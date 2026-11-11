"""Minimal RTMP peers for measuring the stream mode relay (tests only).

  python -I rtmp_peers.py sink PORT OUT.flv [CHUNK] [MODE]
      A "platform": accepts a publisher on 127.0.0.1:PORT, answers the
      handful of commands a publisher sends, announces CHUNK (default 4096)
      as its chunk size, and writes what it receives to OUT.flv. It prints
      whether the publisher's handshake carried FFmpeg's (Flash Player's)
      digest, its flashVer, the chunk size it announced and the largest
      chunk it sent. MODE:
        pings    also sends a ping request every 2 s and a 4-byte window
                 acknowledgement size (the publisher must acknowledge);
                 prints how many pings were answered
        kick     10 s into the first connection, an error status
                 (NetStream.Publish.Failed) and a close; takes the
                 publisher's next connections (OUT.2.flv, ...)
        dropown  closes any connection whose connect says "FMSc" (the
                 relay's own RTMP, OBS's flashVer) without an answer; takes
                 the next connections (the relay falls back to FFmpeg's
                 client, "Lavf")
  python -I rtmp_peers.py publish URL KEY SRC.flv
      An "OBS": publishes SRC.flv in real time (like ffmpeg -re) the way OBS's
      librtmp does: 4096-byte chunks, one send per chunk.

ffmpeg's own RTMP server announces 128-byte chunks, and ffmpeg's RTMP client
mirrors whatever the server announces, so ffmpeg on both ends makes the relay
read and write 128-byte chunks: about 6000 of them per second at 6 Mbps. OBS
and the big ingest servers use 4096 or more. docs/STREAM_MODE.md, "CPU".
"""
import hashlib
import hmac
import socket
import struct
import sys
import threading
import time


# ------------------------------------------------------------------ AMF0 --
def amf_str(s):
    b = s.encode()
    return b'\x02' + struct.pack('>H', len(b)) + b


def amf_num(n):
    return b'\x00' + struct.pack('>d', n)


def amf_null():
    return b'\x05'


def amf_obj(d):
    out = b'\x03'
    for k, v in d.items():
        kb = k.encode()
        out += struct.pack('>H', len(kb)) + kb
        out += amf_str(v) if isinstance(v, str) else amf_num(v)
    return out + b'\x00\x00\x09'


def amf_read(b, i):
    """One AMF0 value at b[i]: (value, next index)."""
    t = b[i]
    if t == 0:
        return struct.unpack('>d', b[i + 1:i + 9])[0], i + 9
    if t == 1:
        return bool(b[i + 1]), i + 2
    if t == 2:
        n = struct.unpack('>H', b[i + 1:i + 3])[0]
        return b[i + 3:i + 3 + n].decode('utf-8', 'replace'), i + 3 + n
    if t == 5 or t == 6:
        return None, i + 1
    if t in (3, 8):
        i += 1 if t == 3 else 5
        obj = {}
        while True:
            n = struct.unpack('>H', b[i:i + 2])[0]
            key = b[i + 2:i + 2 + n].decode('utf-8', 'replace')
            i += 2 + n
            if n == 0 and b[i] == 9:
                return obj, i + 1
            obj[key], i = amf_read(b, i)
    raise ValueError('amf type %d' % t)


# ------------------------------------------------------------- chunking --
class Conn:
    def __init__(self, sock):
        self.s = sock
        self.buf = bytearray()
        self.in_chunk = 128
        self.out_chunk = 128
        self.max_chunk = 0  # Largest chunk payload read.
        self.prev = {}  # csid -> [ts, length, type, stream, delta]
        self.partial = {}  # csid -> bytearray
        self.lock = threading.Lock()

    def need(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d

    def take(self, n):
        self.need(n)
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def read_message(self):
        """Next complete message: (type, stream id, timestamp, payload)."""
        while True:
            b0 = self.take(1)[0]
            fmt, csid = b0 >> 6, b0 & 0x3F
            if csid == 0:
                csid = 64 + self.take(1)[0]
            elif csid == 1:
                lo, hi = self.take(2)
                csid = 64 + lo + hi * 256
            p = self.prev.setdefault(csid, [0, 0, 0, 0, 0])
            new_msg = csid not in self.partial or len(self.partial[csid]) == 0
            if fmt <= 2:
                ts = int.from_bytes(self.take(3), 'big')
                if fmt <= 1:
                    p[1] = int.from_bytes(self.take(3), 'big')
                    p[2] = self.take(1)[0]
                if fmt == 0:
                    p[3] = struct.unpack('<I', self.take(4))[0]
                if ts == 0xFFFFFF:
                    ts = struct.unpack('>I', self.take(4))[0]
                if fmt == 0:
                    # FFmpeg's reader: a later fmt 3 header adds this field.
                    p[0] = ts
                    p[4] = ts
                else:
                    p[4] = ts
                    p[0] += ts
            else:
                if p[4] >= 0xFFFFFF:
                    self.take(4)
                if new_msg:
                    p[0] += p[4]
            part = self.partial.setdefault(csid, bytearray())
            n = min(self.in_chunk, p[1] - len(part))
            self.max_chunk = max(self.max_chunk, n)
            part += self.take(n)
            if len(part) >= p[1]:
                payload = bytes(part)
                part.clear()
                if p[2] == 1:
                    self.in_chunk = struct.unpack('>I', payload[:4])[0] & 0x7FFFFFFF
                    continue
                return p[2], p[3], p[0], payload

    def send_message(self, csid, mtype, stream, ts, payload):
        """fmt 0 header, then the payload in out_chunk pieces: each chunk with
        its header in one send (librtmp's way)."""
        ext = ts >= 0xFFFFFF
        hdr = bytes([csid & 0x3F]) + (min(ts, 0xFFFFFF)).to_bytes(3, 'big') + len(payload).to_bytes(3, 'big') + \
            bytes([mtype]) + struct.pack('<I', stream) + (struct.pack('>I', ts) if ext else b'')
        cont = bytes([0xC0 | (csid & 0x3F)]) + (struct.pack('>I', ts) if ext else b'')
        with self.lock:
            off = 0
            first = True
            while first or off < len(payload):
                piece = payload[off:off + self.out_chunk]
                self.s.sendall((hdr if first else cont) + piece)
                off += len(piece)
                first = False

    def set_chunk(self, size):
        self.send_message(2, 1, 0, 0, struct.pack('>I', size))
        self.out_chunk = size


def command(conn, stream, name, txn, *args):
    conn.send_message(3, 20, stream, 0, amf_str(name) + amf_num(txn) + b''.join(args))


def parse_command(payload):
    name, i = amf_read(payload, 0)
    txn = None
    if i < len(payload):
        txn, i = amf_read(payload, i)
    rest = []
    while i < len(payload):
        try:
            v, i = amf_read(payload, i)
        except Exception:
            break
        rest.append(v)
    return name, txn, rest


# ------------------------------------------------------------------ sink --
PLAYER_KEY = b'Genuine Adobe Flash Player 001'


def c1_digest_ok(c1):
    """FFmpeg's client (and Flash Player) sign C1 with an HMAC at an offset
    bytes 8-11 select (rtmpproto.c, rtmp_handshake_imprint_with_digest)."""
    pos = sum(c1[8:12]) % 728 + 12
    want = hmac.new(PLAYER_KEY, c1[:pos] + c1[pos + 32:], hashlib.sha256).digest()
    return want == c1[pos:pos + 32]


def sink(port, out_path, chunk, mode=''):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(('127.0.0.1', port))
    ls.listen(1)
    print('LISTENING', port, flush=True)
    n = 0
    while True:
        if n > 0:
            if mode not in ('kick', 'dropown'):
                break
            ls.settimeout(30)
        try:
            s, _ = ls.accept()
        except socket.timeout:
            break
        n += 1
        serve(s, out_path if n == 1 else '%s.%d.flv' % (out_path[:-4], n), chunk, mode, n)


def serve(s, out_path, chunk, mode, n):
    pings = mode == 'pings'
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    c = Conn(s)
    c0c1 = c.take(1537)
    print('CONN %d HANDSHAKE version=%d digest=%s' % (n, c0c1[0], 'ok' if c1_digest_ok(c0c1[1:]) else 'none'),
          flush=True)
    s.sendall(b'\x03' + bytes(1536) + c0c1[1:])
    c.take(1536)
    pongs = set()
    pinged = [0]
    acks = [0]
    stop = threading.Event()

    def pinger():
        while not stop.wait(2.0):
            pinged[0] += 1
            try:
                c.send_message(2, 4, 0, 0, struct.pack('>HI', 6, pinged[0]))
            except OSError:
                return
    out = open(out_path, 'wb')
    out.write(b'FLV\x01\x05\x00\x00\x00\x09\x00\x00\x00\x00')
    tags = 0
    try:
        while True:
            mtype, stream, ts, payload = c.read_message()
            if mtype == 20:
                name, txn, args = parse_command(payload)
                if name == 'connect':
                    flash = args[0].get('flashVer', '') if args and isinstance(args[0], dict) else ''
                    print('CONN %d CONNECT flashVer=%r' % (n, flash), flush=True)
                    if mode == 'dropown' and 'FMSc' in flash:
                        print('CONN %d DROPPED' % n, flush=True)
                        break
                    # With pings, a tiny window: the publisher must acknowledge.
                    c.send_message(2, 5, 0, 0, struct.pack('>I', 4 if pings else 5000000))
                    c.send_message(2, 6, 0, 0, struct.pack('>IB', 5000000, 2))
                    c.set_chunk(chunk)
                    command(c, 0, '_result', txn, amf_obj({'fmsVer': 'FMS/3,0,1,123', 'capabilities': 31}),
                            amf_obj({'level': 'status', 'code': 'NetConnection.Connect.Success',
                                     'description': 'ok', 'objectEncoding': 0}))
                elif name == 'createStream':
                    command(c, 0, '_result', txn, amf_null(), amf_num(1))
                elif name == 'publish':
                    command(c, stream, 'onStatus', 0, amf_null(),
                            amf_obj({'level': 'status', 'code': 'NetStream.Publish.Start', 'description': 'ok'}))
                    if pings:
                        threading.Thread(target=pinger, daemon=True).start()
                elif name in ('releaseStream', 'FCPublish'):
                    command(c, 0, '_result', txn, amf_null(), amf_null())
                elif name in ('deleteStream', 'FCUnpublish', 'closeStream'):
                    pass
            elif mtype == 4 and len(payload) >= 6 and struct.unpack('>H', payload[:2])[0] == 7:
                pongs.add(struct.unpack('>I', payload[2:6])[0])
            elif mtype == 3:
                acks[0] += 1
            elif mtype in (8, 9, 18):
                if mode == 'kick' and n == 1 and mtype != 18 and ts >= 10000:
                    command(c, stream, 'onStatus', 0, amf_null(),
                            amf_obj({'level': 'error', 'code': 'NetStream.Publish.Failed', 'description': 'kicked'}))
                    print('CONN %d KICKED at ts=%d' % (n, ts), flush=True)
                    time.sleep(0.2)
                    break
                if mtype == 18 and payload.startswith(amf_str('@setDataFrame')):
                    payload = payload[len(amf_str('@setDataFrame')):]
                out.write(bytes([mtype]) + len(payload).to_bytes(3, 'big') + (ts & 0xFFFFFF).to_bytes(3, 'big') +
                          bytes([(ts >> 24) & 0xFF]) + b'\x00\x00\x00' + payload +
                          struct.pack('>I', 11 + len(payload)))
                tags += 1
    except (EOFError, ConnectionError, OSError):
        pass
    stop.set()
    out.close()
    s.close()
    print('CONN %d SINK DONE tags=%d chunk_in=%d max_chunk=%d pings=%d pongs=%d acks=%d' %
          (n, tags, c.in_chunk, c.max_chunk, pinged[0], len(pongs), acks[0]), flush=True)


# --------------------------------------------------------------- publish --
def flv_tags(path):
    with open(path, 'rb') as f:
        data = f.read()
    i = struct.unpack('>I', data[5:9])[0] + 4
    while i + 11 <= len(data):
        mtype = data[i]
        size = int.from_bytes(data[i + 1:i + 4], 'big')
        ts = int.from_bytes(data[i + 4:i + 7], 'big') | (data[i + 7] << 24)
        payload = data[i + 11:i + 11 + size]
        if len(payload) < size:
            break
        yield mtype, ts, payload
        i += 11 + size + 4


def publish(url, key, src):
    rest = url.split('://', 1)[1]
    hostport, app = rest.split('/', 1)
    host, port = (hostport.split(':') + ['1935'])[:2]
    s = socket.create_connection((host, int(port)))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    c = Conn(s)
    s.sendall(b'\x03' + bytes(1536))
    s0s1s2 = c.take(1 + 1536 + 1536)
    s.sendall(s0s1s2[1:1537])
    c.set_chunk(4096)
    tc = '%s/%s' % (url.split('://')[0] + '://' + hostport, app)
    command(c, 0, 'connect', 1, amf_obj({'app': app, 'type': 'nonprivate', 'flashVer': 'FMLE/3.0', 'tcUrl': tc}))
    command(c, 0, 'releaseStream', 2, amf_null(), amf_str(key))
    command(c, 0, 'FCPublish', 3, amf_null(), amf_str(key))
    command(c, 0, 'createStream', 4, amf_null())
    stream = None
    while stream is None:
        mtype, _, _, payload = c.read_message()
        if mtype == 20:
            name, txn, args = parse_command(payload)
            if name == '_result' and txn == 4:
                stream = int(args[-1])
            elif name == '_error':
                raise SystemExit('rejected: %r' % (args,))
    command(c, stream, 'publish', 5, amf_null(), amf_str(key), amf_str('live'))
    started = False
    while not started:
        mtype, _, _, payload = c.read_message()
        if mtype == 20:
            name, txn, args = parse_command(payload)
            started = name == 'onStatus'

    # The server's acks and pings are read and dropped.
    def drain():
        try:
            while True:
                c.read_message()
        except Exception:
            pass
    threading.Thread(target=drain, daemon=True).start()

    t0 = time.perf_counter()
    first_ts = None
    sent = 0
    for mtype, ts, payload in flv_tags(src):
        if mtype == 18:
            c.send_message(4, 18, stream, 0, amf_str('@setDataFrame') + payload)
            continue
        if first_ts is None:
            first_ts = ts
        due = t0 + (ts - first_ts) / 1000.0
        wait = due - time.perf_counter()
        if wait > 0:
            time.sleep(wait)
        c.send_message(4 if mtype == 8 else 6, mtype, stream, ts, payload)
        sent += 1
    command(c, stream, 'FCUnpublish', 6, amf_null(), amf_str(key))
    command(c, stream, 'deleteStream', 7, amf_null(), amf_num(stream))
    time.sleep(0.5)
    s.close()
    print('PUBLISH DONE tags=%d' % sent, flush=True)


if __name__ == '__main__':
    if sys.argv[1] == 'sink':
        sink(int(sys.argv[2]), sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 4096,
             sys.argv[5] if len(sys.argv) > 5 else '')
    else:
        publish(sys.argv[2], sys.argv[3], sys.argv[4])
