"""Which RTMP chunk size an ingest server announces (docs/STREAM_MODE.md, "CPU").

  python -I probe_ingest.py URL [--publish]

Connects to URL (rtmp:// or rtmps://), handshakes, sends connect and
createStream, and prints what the server sends for a few seconds. No stream key
is used. With --publish it also publishes to the key "invalid-probe", which the
server rejects. FFmpeg's RTMP client keeps 128-byte chunks unless the server
announces a size ("SetChunkSize N" in the output).
"""
import os
import socket
import ssl
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rtmp_peers as rp  # noqa: E402


def main():
    url = sys.argv[1]
    publish = '--publish' in sys.argv[2:]
    scheme, rest = url.split('://', 1)
    hostport, app = rest.split('/', 1)
    host, port = (hostport.split(':') + [('443' if scheme == 'rtmps' else '1935')])[:2]
    s = socket.create_connection((host, int(port)), timeout=8)
    if scheme == 'rtmps':
        s = ssl.create_default_context().wrap_socket(s, server_hostname=host)
    c = rp.Conn(s)
    s.sendall(b'\x03' + bytes(1536))
    c.take(1 + 1536 + 1536)
    s.sendall(bytes(1536))
    rp.command(c, 0, 'connect', 1, rp.amf_obj({'app': app, 'type': 'nonprivate', 'flashVer': 'FMLE/3.0',
                                                'tcUrl': scheme + '://' + hostport + '/' + app}))
    s.settimeout(4)
    seen = []
    end = time.time() + 8
    try:
        while time.time() < end:
            before = c.in_chunk
            mtype, _, _, payload = c.read_message()
            if c.in_chunk != before:
                seen.append('SetChunkSize %d' % c.in_chunk)
            if mtype != 20:
                seen.append('message type %d' % mtype)
                continue
            name, txn, args = rp.parse_command(payload)
            seen.append('%s %s' % (name, txn))
            if name == '_result' and txn == 1:
                if publish:
                    rp.command(c, 0, 'releaseStream', 2, rp.amf_null(), rp.amf_str('invalid-probe'))
                    rp.command(c, 0, 'FCPublish', 3, rp.amf_null(), rp.amf_str('invalid-probe'))
                rp.command(c, 0, 'createStream', 4, rp.amf_null())
            elif name == '_result' and txn == 4 and publish:
                rp.command(c, int(args[-1]), 'publish', 5, rp.amf_null(), rp.amf_str('invalid-probe'),
                           rp.amf_str('live'))
    except Exception as e:  # the server closing or going quiet ends the probe
        seen.append('end: %s' % type(e).__name__)
    print(url, '->', '; '.join(seen))


main()
