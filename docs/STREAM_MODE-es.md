*[English version](STREAM_MODE.md)*

# Modo stream (v1.0.0)

El modo stream retrasa el stream que OBS **ya ha codificado**, en vez de los frames renderizados.
Es el modo por defecto desde la v1.0.0 allí donde puede funcionar (Windows y, en alfa, macOS y
Linux); el modo escena (modo buffer, `docs/SPEC-es.md`) se queda para los casos que el modo stream
no cubre.

## Por qué casi no cuesta nada

El modo escena, en cada frame: renderiza, lee la imagen de la GPU, la comprime en RAM (MJPEG o el
HEVC de la GPU), la descomprime más tarde, la vuelve a renderizar, y después OBS la codifica para
el stream. Su RAM es el buffer a una calidad cercana a la de origen: de ~190 MB a ~2,3 GB para
30-60 s (tabla del README).

El modo stream guarda los paquetes que el propio encoder de OBS ha producido para el stream y los
envía más tarde. No se decodifica ni se vuelve a codificar nada; la CPU solo copia bytes. El
historial es el bitrate del stream por los segundos guardados (45 MB por minuto a 6 Mbps), y casi
todo espera en un archivo temporal cifrado en el disco, así que la RAM que ocupa es más o menos la
misma con cualquier delay (ver Memoria y disco). Su CPU es de 1 a 1,5 s por minuto de stream con
Twitch, Kick, Facebook o YouTube (ver CPU).

## Memoria y disco

- **Almacenamiento contiguo.** Los bytes de los paquetes van seguidos en bloques de 1 MiB
  (`PayloadStore`, `payload-store.*`), un registro por paquete: sus flags, sus datos laterales y su
  contenido, exactamente lo que devolvió el demuxer de FFmpeg, así que la plataforma recibe los
  mismos bytes. Los metadatos del planificador son 48 bytes por paquete. Antes, cada paquete era
  una reserva de memoria propia de FFmpeg, que costaba un 10-13 % encima del historial (abajo: +490
  MB de RAM para 446 MB de historial, +106 MB para 93 MB).
- **El volcado a disco.** En RAM solo quedan los segundos más recientes (4 s más dos intervalos de
  fotogramas clave: el directo y los delays cortos nunca tocan el disco, y bajar el delay empalma
  dentro de la RAM) y lo que cada destino va a enviar a continuación (3 s por delante). Los bloques
  llenos más antiguos van a un archivo temporal y su RAM se libera. Un bloque que un destino está a
  punto de enviar se vuelve a leer en el hilo de E/S propio del relay antes de que toque, nunca con
  el bloqueo que usan los emisores; un emisor cuyos bytes aún no han vuelto espera en vez de enviar.
- **Cifrado.** AES-256 en modo contador, con una clave y un nonce aleatorios creados para cada
  directo y guardados solo en la memoria de OBS; el contador sale del número del bloque, que nunca
  se repite. El AES es el del sistema: CNG (bcrypt) en Windows, CommonCrypto en macOS y el OpenSSL
  que el build de Linux ya enlaza para RTMPS. Ninguna dependencia nueva.
- **Dónde y cuánto tiempo.** En la carpeta temporal del sistema (en Linux, `$TMPDIR`, `/var/tmp` o
  `/tmp`, la primera que no sea un tmpfs en RAM), un archivo `trigglow-dd-spill-*.tmp`. Se borra al terminar el directo, y también si
  OBS se cierra de golpe: se borra al cerrarse en Windows y se desvincula nada más crearlo en Linux y
  macOS. El siguiente directo borra cualquiera que haya dejado un corte de luz. El espacio liberado se
  reutiliza y el archivo se recorta cuando el historial encoge.
- **Uso de disco.** El bitrate por 600 s mientras emites en modo stream (unos 450 MB a 6 Mbps, 600
  MB a 8 Mbps), escritos y, con delays más largos que la ventana de RAM, leídos de nuevo al bitrate
  del stream. El volcado siempre deja 1 GiB libre en ese disco.
- **Cuando no se puede usar el disco** (no hay sitio, falla una escritura o una lectura, un bloque
  tarda más de 2 s, o hay 48 bloques esperando a escribirse), el volcado se detiene para ese
  directo: lo que hay en disco se vuelve a leer y el historial se queda en RAM, como antes del
  volcado, con el bitrate por los segundos guardados (ver Cambiar el delay en directo). Queda en el
  log y el directo sigue. Solo se descarta un bloque que no se pueda volver a leer, con sus
  paquetes; los destinos vuelven a empalmar en el siguiente keyframe.

Medido con `tests/relay_harness.cpp` (el relay en su propio proceso, con ffmpeg haciendo de OBS y
de plataforma; bytes privados por encima del proceso antes del directo, 18,9-21,1 MB) con un
stream x264 de 6,2 Mbps (1280x720 a 30 fps, B-frames, keyframes cada 2 s, AAC a 160 kbps), Windows
11, 2026-11-11:

| Delay | Historial guardado (antes / ahora) | RAM antes | Solo bloques contiguos | RAM ahora | de ella, el historial | En disco ahora |
|---|---|---|---|---|---|---|
| 10 s | 127 s, 93 MB / 607 s, 446 MB | +106 MB | +96 MB | +13 MB | 11 MB | 440 MB |
| 120 s | 127 s, 93 MB / 607 s, 446 MB | +103 MB | +96 MB | +15 MB | 13 MB | 440 MB |
| 600 s | 607 s, 446 MB / 607 s, 446 MB | +490 MB | +453 MB | +13 MB | 12 MB | 440 MB |
| 600 s, solo RAM (sin volcado) | 607 s, 446 MB | | | +454 MB | 449 MB | 0 |

"+N MB" es cuánto creció el proceso (bytes privados) respecto a sí mismo antes del directo. "Antes"
es el relay sin estos cambios, con "Guardar para subidas al instante" en su valor por defecto (120
s); "Solo bloques contiguos" es el primer cambio sin el volcado a disco. Dentro de OBS, el historial
es lo único del modo stream que crece con el delay.

Las herramientas que reenvían el stream fuera de OBS guardan los mismos bytes: un stream de 6 Mbps
guardado durante 60 s son 45 MB, estén donde estén. El modo stream los guarda en disco, cifrados, y
en RAM solo lo que está a punto de enviarse.

## CPU

La CPU del relay es casi toda del sistema operativo moviendo los bytes de RTMP por los sockets, así
que lo que cuenta es cuántas lecturas y escrituras necesita el stream a cada lado:

- **Desde OBS.** OBS envía chunks de 4096 bytes, una escritura por chunk (`obs-outputs`:
  `rtmp-stream.c` pone `m_outChunkSize = 4096`; librtmp escribe cada chunk con su cabecera). A 6
  Mbps el listener de FFmpeg del relay lee unos 190 chunks por segundo.
- **Hacia la plataforma.** El relay tiene su propia capa RTMP para los destinos `rtmp://` y
  `rtmps://` (`src/rtmp-wire.*`, `src/rtmp-publisher.*`). Anuncia chunks de 4096 bytes a todos los
  servidores, como OBS, y escribe cada lote de paquetes que toca enviar en unas pocas escrituras
  grandes (de hasta 32 KB, a través del buffer de E/S de FFmpeg), anuncie el servidor el tamaño que
  anuncie. El muxer FLV de FFmpeg sigue construyendo las etiquetas (cabeceras de códec, RTMP
  mejorado) y la conexión sigue siendo el TCP o el TLS de FFmpeg (SChannel en Windows, con sus
  comprobaciones de nombre de host y certificado), así que solo es nuevo el troceado RTMP de en
  medio. El handshake, `connect` (con el `flashVer` de OBS), `releaseStream`, `FCPublish`,
  `createStream` y `publish` siguen al cliente de FFmpeg.
- **Antes** (el modo stream hasta este cambio), el stream lo enviaba el cliente RTMP de FFmpeg
  (FFmpeg 9.0.2, `rtmppkt.c`). Escribe cada chunk con dos llamadas a `send()` (el chunk y luego la
  cabecera de un byte del siguiente), cada una tras un `select()`, y se queda en chunks de 128 bytes
  salvo que el servidor anuncie un tamaño. Twitch, Kick (Amazon IVS) y Facebook anuncian 4096 bytes
  al conectar el relay; YouTube (RTMP y RTMPS) no anuncia nada, así que FFmpeg se quedaba en 128
  bytes: unos 6000 chunks y 12.000 envíos diminutos por segundo a 6 Mbps, unos 10 s de CPU por
  minuto por cada destino de YouTube. FFmpeg no tiene opción ni API pública para cambiarlo.

Los mensajes del servidor (peticiones de ping, acuses de recibo, un estado de error) los lee el
propio hilo del destino entre escrituras, nunca durante una: la conexión pasa a no bloqueante solo
para esa lectura, como mucho cada 100 ms (cada 250 ms si no se está enviando nada), que es lo que
hace el propio cliente de FFmpeg tras cada escritura. Se responden los pings, se envían los acuses
según la ventana del servidor, y un estado de error (o que el servidor cierre) termina la conexión,
que se reconecta como cualquier otro fallo. avio.h no tiene llamada para ese cambio, así que el
relay pone el indicador a través de los primeros campos del contexto de conexión de FFmpeg, tras
comprobar que lo es (su nombre de clase, la URL con la que se abrió, sus indicadores).

El cliente de FFmpeg sigue para otras URL y como respaldo: si esa comprobación falla (un FFmpeg
compilado de otra forma), o si un servidor corta dos veces seguidas la preparación RTMP del relay
antes de `publish` (cierra, no responde o envía algo ilegible; una parada, un error de red o un
rechazo no cuentan), ese destino usa el cliente de FFmpeg el resto del stream, y el log lo dice. Un
servidor que cuelga justo después de `publish` se toma como una clave de stream incorrecta, no como
un fallo del RTMP del relay (Twitch, YouTube y Kick cierran la conexión con una clave incorrecta,
también con el cliente de FFmpeg); el error ahora lo dice.

Medido con `tests/relay_harness.cpp` (el relay en su propio proceso; CPU solo de ese proceso) y los
pares de `tools/relay-cpu/` (`rtmp_peers.py` publica como OBS y hace de plataforma, que anuncia 4096
bytes o nada), un stream x264 de 6,2 Mbps (1920x1080 30 fps, CBR, `-g 60 -bf 2`, AAC 160 kbps),
300 s, con el delay puesto cuando el stream de OBS llega al relay. Windows 11, Ryzen 7 7700X,
11-11-2026, cuatro relays a la vez. Segundos de CPU por minuto de stream mientras OBS y el destino
envían a la vez (entre paréntesis, ciclos de CPU exactos, en millones por segundo; los segundos de
CPU son el muestreo de Windows por tic de reloj, los ciclos son la cifra más estable):

| Delay | Antes, plataforma que anuncia 4096 bytes (Twitch, Kick, Facebook) | Antes, plataforma que no anuncia nada (YouTube) | Ahora, 4096 bytes | Ahora, nada (YouTube) |
|---|---|---|---|---|
| 0 s | 0,9 s (88) | 11,1 s (772) | 1,0 s (78) | 0,6 s (74) |
| 10 s | 1,4 s (91) | 10,2 s (769) | 1,5 s (77) | 0,9 s (76) |
| 120 s | 1,6 s (97) | 10,4 s (778) | 1,6 s (86) | 0,9 s (90) |
| 120 s, solo OBS (antes de que pase el delay) | 0,7 s (41) | 0,7 s (39) | 0,9 s (46) | 0,3 s (54) |
| 120 s, solo envío (vaciando tras parar) | 0,9 s (73) | 9,6 s (693) | 0,9 s (57) | 0,8 s (56) |

La versión anterior, ejecutada otra vez junto a estas (10 s, sin tamaño anunciado): 11,2 s (832).
Con la plataforma enviando además una petición de ping cada 2 s y una ventana de acuses de 4 bytes
(10 s, sin tamaño anunciado): 1,3 s (83), los 153 pings respondidos. Así que el modo stream gasta
ahora de 1 a 1,5 s de CPU por minuto de stream (un 2-3 % de un núcleo) con Twitch, Kick, Facebook o
YouTube; un destino de YouTube ya no suma una sexta parte de un núcleo.

Los 14-20 s de CPU por minuto de `docs/BENCHMARK-buffer-storage.md` salían de usar ffmpeg como OBS y
como plataforma: el servidor RTMP de ffmpeg anuncia 128 bytes y el cliente de ffmpeg lo copia, así
que los dos sockets del relay llevaban chunks de 128 bytes (el hilo de entrada se despertaba unas
10.000 veces por segundo; 12-13 s de CPU por minuto con el cliente de FFmpeg enviando).

Lo que se comprobó y no es el coste (con el cliente de FFmpeg enviando):

- **Los hilos del propio relay.** Muestreados por hilo (muestras de pila, `QueryThreadCycleTime`,
  cambios de contexto, 60 s con delay 10, pares tipo OBS y plataforma de 4096 bytes): el envío, 70
  millones de ciclos por segundo, el 85 % en el kernel dentro de `send()`/`select()`; el listener,
  46 millones, un tercio en el kernel; el hilo de E/S del volcado, 5 millones. Todo el tiempo en
  modo usuario junto (incluidos el demuxer y el muxer de FFmpeg) es de unos 0,4 s de CPU por
  minuto. Con una plataforma de 128 bytes, solo el envío eran unos 700 millones de ciclos por
  segundo, el 96 % en el kernel.
- **Esperas.** No hay esperas activas: `condition_variable::wait_for` de MSVC redondea hacia arriba
  a milisegundos enteros, cada envío duerme hasta que toca su siguiente paquete (100-200
  despertares por segundo), y un despertar cuesta unos 50.000 ciclos aquí. El hilo de E/S del
  volcado se despierta 20 veces por segundo (4 millones de ciclos por segundo).
- **Por paquete:** una reserva y una copia de paquete de FFmpeg por paquete enviado (unos 80 por
  segundo), el muxing FLV, el estado una vez por segundo, ningún log. Nada de eso aparece en las
  muestras.
- **`TCP_NODELAY` desactivado** en el socket del destino (Nagle junta las escrituras pequeñas de
  FFmpeg): de 9,8 a 8,3 s de CPU por minuto con chunks de 128 bytes, sin cambio con 4096. No
  compensa la latencia extra, así que sigue activado (con las escrituras grandes del relay da igual
  en un sentido u otro).

Sin hacer: una plataforma real con una clave de stream válida (Twitch, YouTube y Kick, RTMP y
RTMPS; ver Estado de la verificación). RTMPS hacia un servidor que no anuncia nada ya no necesita
una medida aparte: el relay hace las mismas escrituras grandes, y por tanto los mismos pocos
registros TLS, con cualquier servidor.

## Cómo funciona

1. Con `OBS_FRONTEND_EVENT_STREAMING_STARTING` (después de que OBS cree la salida de emisión y
   antes de que arranque), `StreamDelayController` comprueba que el directo es apto: el
   `rtmp_output` de OBS, protocolo RTMP o RTMPS, sin Enhanced Broadcasting (multipista). Lee el
   servidor y la clave con `obs_service_get_connect_info()` (el servidor de ingesta ya resuelto,
   incluido "auto").
2. Arranca `StreamRelay` (avformat de FFmpeg): un listener en `127.0.0.1:<puerto libre>` y un
   emisor hacia el servicio del propio streamer.
3. Apunta **esa salida** al relay con un servicio `rtmp_custom` privado. El servicio guardado del
   streamer no se toca; OBS lo vuelve a poner en la salida en cada arranque.
4. OBS publica en el relay; el relay separa el FLV (sin decodificar) en `PacketHistory`
   (`packet-delay-core.*`) y marca cada paquete con su hora de llegada. Sus bytes van al
   `PayloadStore` del historial y, cuando tienen más de unos segundos, al archivo de volcado cifrado.
5. El `SinkScheduler` del emisor envía el paquete *p* cuando `now >= arrival(p) + delay`,
   remultiplexado a FLV por RTMP/RTMPS (TLS según el sistema, ver Plataformas), y la conexión con
   la plataforma se abre cuando toca enviar el primer paquete (un directo que empieza con delay
   empieza esos segundos más tarde en la plataforma).

### Plataformas

| | FFmpeg | TLS para RTMPS | AES del volcado a disco |
|---|---|---|---|
| Windows | el build compartido LGPL de BtbN (`avformat-63.dll` junto al plugin) | SChannel | CNG (bcrypt) |
| Linux | el build estático LGPL del plugin, enlazado dentro | el OpenSSL 3 del sistema (`libssl.so.3`, dinámico; una dependencia del paquete) | el mismo OpenSSL (`libcrypto.so.3`) |
| macOS | el build estático LGPL del plugin, enlazado dentro, universal (arm64, x86_64) | Secure Transport (Security.framework) | CommonCrypto |

El FFmpeg de Linux y macOS sale de `.github/scripts/build-ffmpeg.sh` con el pin `ffmpeg-source` de
buildspec.json (tag firmado, commit fijado), y solo con lo que necesita el relay: el demuxer y el
muxer FLV, los protocolos RTMP, RTMPS, TCP y TLS, y los parsers de H.264, HEVC, AV1 y AAC. Sus
símbolos están ocultos, así que nunca chocan con el avformat que carga OBS; la CI lo comprueba
(`package.sh` con `nm -D` en Linux, el paso "Check Plugin Binary" del job de macOS con `nm`). El
build solo del núcleo (core-only) no tiene relay.

### Cambiar el delay en directo

El emisor **empalma**: salta al keyframe de vídeo más reciente que tenga al menos *delay* de
antigüedad y reescribe los timestamps para que el siguiente frame vaya un intervalo de frame
después del último enviado. Subir el delay salta hacia atrás: los espectadores vuelven a ver los
últimos segundos, nunca nada más nuevo. Quitarlo salta hacia delante. La plataforma ve un único
stream continuo: sin reconexión ni cabecera nueva. El paso es el intervalo de fotogramas clave
(se recomiendan 2 s); el dock avisa cuando está en automático o pasa de 4 s (ver Comprobación de
ajustes).

Con el volcado a disco, el historial siempre llega 600 s atrás (el máximo del delay) más dos
intervalos de fotogramas clave más 1 s: cuesta disco, no RAM, y cualquier subida en directo es al
instante en cuanto el directo lleva ese tiempo. Solo en RAM (si no se pudo usar el disco) llega el
delay o el preset o la regla por escena con delay más alto, lo que sea mayor, más el mismo margen
(`KeepSeconds()` y `RetentionNs()` en `packet-delay-core.hpp`). La línea "Subidas al instante" del
dock dice cuál se aplica. Si pides más de lo que tiene el historial (al principio de un directo, o
por encima de los presets solo en RAM), el emisor espera hasta que el keyframe guardado más antiguo
tenga la antigüedad suficiente (`SpliceInfo::shortHistory`).

### Problemas de red

- Si la conexión con la plataforma no puede conectar o enviar, el emisor reintenta con espera
  creciente (2, 4, 8, 15 s) y una línea de tiempo nueva, con el delay.
- Si el emisor se queda más de 4 s por detrás de su delay (subida lenta), vuelve al delay en el
  siguiente keyframe, en vez de dejar que el delay crezca (`resyncs` en el estado).
- La gestión de congestión de OBS solo ve el loopback, así que el contador de frames perdidos de
  OBS se queda en 0 en el modo stream; en su lugar, el dock muestra el estado y la velocidad de la
  conexión.

### Al parar

Cuando OBS para, el relay sigue enviando lo que los espectadores aún no han visto (los últimos
*delay* segundos), salvo que "Al parar el directo, enviar los segundos que los espectadores aún no
han visto" esté desactivado; el dock ofrece "Dejar de enviar ya". Un directo nuevo, o cerrar OBS, lo
corta.

### Borrar

"Borrar lo que aún no se ha emitido" (dock, atajo, `DumpDelay` por obs-websocket) pone en el
historial un suelo de empalme en el momento actual (`PacketHistory::SetSpliceFloor`) y el delay a
0, con el lock que toma el emisor antes de `Due()`. El emisor vuelve a empalmar en el primer
keyframe que llega después del suelo (en menos de un intervalo de fotogramas clave), así que los
segundos que aún esperaban en el delay no se envían nunca. El controlador vuelve a subir el delay
pasados sus segundos más 3 s; el empalme no puede bajar del suelo, así que solo rebobina hasta lo
que llegó después del borrado. Comprobado con el arnés: borrado a los 25 s con un delay de 10 s, en
directo en ~2 s, el delay vuelve, timestamps continuos y la salida se decodifica limpia.

### Reglas por escena y presets

Las reglas asocian una escena a "activar delay (con sus segundos)" o "quitar delay", y se aplican
con `OBS_FRONTEND_EVENT_SCENE_CHANGED` y al empezar un directo. Tres presets (10/30/60 s por
defecto) fijan los segundos y activan el delay, cada uno con su atajo y con `ApplyPreset {index}`
por obs-websocket.

## Lo que sigue en directo

La grabación, el Replay Buffer y la cámara virtual leen el Programa de OBS, que el modo stream
nunca toca: mantienen la imagen en directo. Todas las escenas van con delay, así que cambiar de
escena con el delay activo es seguro (el modo escena solo retrasa la escena elegida en el dock).

## La insignia de delay

Mientras el delay está activo y el directo pasa por el relay, la insignia "DELAY N s" (los mismos
PNG que en el modo escena, hasta 600 s) se dibuja en el canal de salida 61 de libobs, sobre el
Programa de OBS, después de la escena. Los espectadores la reciben con la imagen retrasada. Como está
en el Programa, la grabación (y todo lo que tome el Programa) también la muestra mientras el delay
está activo. Activarla o no ("Mostrar la insignia de delay a los espectadores") y la esquina están en
el dock y se guardan.

## Comprobación de ajustes

Al empezar un directo, el controlador revisa los ajustes de OBS y el dock lista lo que empeora el
modo stream (`StreamDelayController::CheckObsSettings`):

- el intervalo de fotogramas clave del encoder del stream en automático (0, a menudo 4-10 s) o por
  encima de 4 s: el delay se mueve en pasos de ese tamaño;
- el Retardo de la transmisión propio de OBS (`Ajustes → Avanzado`) activado: su retardo fijo se
  suma a este.

## Controles

- Dock: activar/quitar, volver al directo, segundos (1-600, +/-5), borrar, tres presets, delay por
  escena, hasta dónde una subida es al instante (solo lectura; sustituye al ajuste "Guardar para
  subidas al instante", cuyo `keep_seconds` guardado se sigue leyendo y escribiendo sin cambios),
  envío al parar, la insignia, adónde va el directo (solo lectura: `Ajustes → Emisión` de OBS). Las
  estadísticas muestran la RAM y el disco que ocupa el historial.
- Atajos (`Ajustes → Atajos` de OBS): Activar/desactivar, Activar, Desactivar (mismos nombres que
  antes), Volver al directo, Delay +5 s, Delay -5 s, Borrar, Preset 1-3. Pasan por `DelayRouter`:
  al modo stream cuando este lleva los controles, al modo escena si no (ahí los segundos también
  cambian en directo, 1-600, un preset además activa el delay, y Borrar no hace nada).
- obs-websocket 5, vendor `trigglow-dynamic-delay`: `GetStatus`, `EnableDelay`, `DisableDelay`,
  `ToggleDelay`, `SkipToLive`, `SetDelay {seconds}`, `AdjustDelay {delta}`, `DumpDelay`,
  `ApplyPreset {index}` (0-2); evento `StatusChanged`. Para el plugin de Stream Deck de Trigglow;
  detalles en `docs/STREAM_DECK-es.md`.

La puerta de cuenta gratuita se aplica a Activar igual que en el modo escena.

## Lo que aún no cubre

- Enhanced Broadcasting (vídeo multipista) y los servicios SRT, RIST y WHIP: modo escena.
- El delay por escena solo existe en el modo stream: el modo escena sigue retrasando la única
  escena en directo elegida en el dock.

## Estado de la verificación

- `packet-delay-core` tiene tests unitarios (`tests/packet_delay_core_tests.cpp`): paso directo en
  vivo, rebobinado al activar con el delay mantenido y timestamps monótonos, salto hacia delante al
  desactivar, historial corto, empalme antes del primer keyframe, recorte, solo audio, borrado, y
  empalmes en un stream con B-frames (lo que usa x264 por defecto) que conservan el orden de
  presentación.
- De extremo a extremo en un OBS 33.0.1 real en Windows 11 (copia portable, x264 CBR 2500 kbps,
  1280x720 a 30 fps, keyframes cada 2 s), manejado por obs-websocket 5, con un listener RTMP local
  de FFmpeg haciendo de plataforma (2026-11-11). Un reloj en código Gray de 12 bits dibujado en la
  escena midió el delay que reciben los espectadores a partir de las horas de llegada a la
  plataforma; cada salida se revisó con ffprobe/ffmpeg (timestamps monótonos, ningún hueco de
  más de ~150 ms en un empalme, se decodifica limpia):
  - Modos de salida Avanzado y Simple; con el Retardo de la transmisión propio de OBS encima (los
    delays se suman).
  - El paso directo en vivo no tiene un coste medible: 1,4 s del lienzo de OBS a la plataforma, lo
    mismo que sin el relay. Delay activado, subido, bajado, quitado, presets, borrado (en directo en
    un keyframe, el delay vuelve pasados sus segundos + 3 s), la regla por escena, cada petición
    vendor y cada atajo (TriggerHotkeyByName): el delay medido es el pedido más esos 1,4 s.
  - Parar con el delay activo envía los últimos segundos y termina; un directo nuevo durante el
    envío al parar lo corta; cerrar OBS a mitad de directo o del envío al parar sale en ~0,3 s.
  - Cambios de escena con delay (12, hasta 0,3 s de separación, delay de 10 s): cada cambio llegó
    a los espectadores 11,4-11,6 s después, el delay se mantuvo en 11,37-11,53 s en todos los
    frames, ningún frame mostró una escena que no estuviera puesta en el momento de su contenido,
    no salió nada más nuevo que el delay y no hubo cortes.
  - 19 cambios en directo, hacia arriba y hacia abajo, en un mismo directo (atajos +/-5, los tres
    presets, `SetDelay`, `AdjustDelay`, quitar y activar, 3-40 s): cada uno medido a menos de
    0,04 s del delay pedido, una sola conexión con la plataforma todo el tiempo, timestamps
    continuos.
  - Delays largos con "Guardar para subidas al instante" a 600 s (antes de sustituirlo): 300, 120 y 600 s mantenidos al
    frame (medidos 300,0, 120,1 y 600,0 s), y de vuelta a 30 s en directo. Subir por encima del
    historial guardado (30 -> 300 s a los 76 s de directo, 120 -> 600 s a los 436 s) espera: no
    llega nada (ni vídeo ni audio) a la plataforma durante los segundos que faltan (232,6 s y
    172,5 s aquí), y después el directo sigue con el delay nuevo. La plataforma de prueba local
    mantuvo la conexión; una plataforma real puede cortar una publicación que no envía nada
    durante minutos (no comprobado). Cuando el directo ya lleva los segundos guardados, las subidas
    son repeticiones al instante.
  - RAM, antes del volcado a disco y del almacenamiento contiguo: el historial es el bitrate del
    stream por los segundos guardados (192 MB para 605 s a 2,66 Mbps; ahí deja de crecer). El
    working set de OBS creció lo que ocupa el historial más ~15 % (693 -> 906 MB) y volvió a su
    tamaño en reposo tras el directo. Los +275 MB al empezar a emitir son del propio OBS (igual con
    el modo stream desactivado). No se ha vuelto a medir en un OBS real desde entonces (ver la tabla
    de Memoria y disco para el relay por sí solo).
  - 12 ciclos de arrancar y parar, la mitad con todos los núcleos ocupados: OBS siempre llegó al
    relay (listener listo en 5-50 ms).
  - La puerta de cuenta gratuita rechaza Activar, los presets y Activar/desactivar sin sesión
    iniciada (un interruptor de compilación solo para pruebas, `TRIGGLOW_TEST_SKIP_ACCOUNT_GATE`,
    que la CI rechaza, la saltó para el resto).
  - La insignia se dibuja mientras el delay está activo. Forma parte de la imagen retrasada, así
    que los espectadores ven el valor que había cuando se renderizó el frame: durante los primeros
    *delay* segundos tras un cambio, el valor anterior (o ninguno, justo después de Activar).
- Arreglado en estas pruebas: en el modo de salida Avanzado, OBS ponía su propio servicio en la
  salida después de `STREAMING_STARTING`, así que el directo se saltaba el relay (ahora el
  servicio del frontend se cambia por el del relay durante la llamada de arranque y se restaura
  justo después); los empalmes con B-frames se reproducían hacia atrás; parar antes de que OBS
  llegara al relay, o una plataforma caída, dejaba el envío al parar sin terminar nunca; la espera
  de 150 ms al listener es ahora una comprobación de que escucha (si no, el directo sale sin el
  relay; en Linux y macOS sigue siendo una espera fija de 150 ms).
- Almacenamiento contiguo, volcado a disco y su retención (2026-11-11), Windows 11, con
  `tests/relay_harness.cpp` y un stream x264 de 6,2 Mbps con B-frames (`-bf 2 -g 60`, CBR, AAC):
  - Tests unitarios: `payload_store_tests` (bloques y recorte; la máquina de estados del volcado
    con un disco simulado: ida y vuelta, recorte y truncado, una escritura fallida, una lectura
    fallida, un disco lento, cola acumulada, archivo lleno), `spill_file_tests` (el vector NIST SP
    800-38A CTR-AES256, ida y vuelta, nada del texto en claro en disco, un flujo de clave distinto
    por bloque y por archivo, borrado al cerrar, truncado, archivos que quedaron) y la retención y
    los enganches del volcado en `packet_delay_core_tests`. Los tres también compilan sin avisos y
    pasan en Linux (GCC 13, OpenSSL 3, un contenedor Ubuntu 24.04).
  - Cambios en directo `4:3,10:8,18:2,24:0` y una subida a 300 s leída de vuelta del disco
    (`5:10,330:300,360:20,375:0`): cada paquete enviado es idéntico byte a byte a un paquete del
    origen (sha256, tamaño y flags, `ffprobe -show_packets -show_data_hash`), los empalmes caen en
    los mismos keyframes que con el relay antes del cambio, DTS de vídeo estrictamente creciente,
    ningún hueco de más de 200 ms, se decodifica limpio. Dos ejecuciones en tiempo real nunca
    coinciden paquete a paquete, ni siquiera dos del mismo build (el audio que se salta alrededor de
    un empalme y el frame tras el que va dependen de lo que tocaba en ese instante), así que las
    salidas se compararon con el origen.
  - Un volcado limitado a 70 MB volvió a la RAM al llenarse (queda en el log), leyó de vuelta los
    bloques volcados y siguió; la salida siguió idéntica byte a byte y sin huecos. Matar el proceso
    a mitad de directo borró su archivo de volcado al instante.
- CPU (11-11-2026), con los montajes de CPU (arriba), 10 relays a la vez: los cambios en directo
  `4:3,10:8,18:2,24:0` y una subida de 10 a 300 s en t=200 (190 s repetidos desde el volcado), con
  ffmpeg y con los pares tipo OBS, y las pruebas estables a 0, 10 y 120 s contra los dos tipos de
  plataforma: cada paquete enviado es idéntico byte a byte a un paquete del origen (sha256), DTS de
  vídeo estrictamente creciente, ningún salto de DTS de más de 200 ms (el mayor: 34 ms en vídeo, 96
  ms en audio), decodifica sin errores. El tamaño de chunk de los ingest públicos se leyó con un
  connect RTMP anónimo (sin clave de stream, sin publicar nada).
- La capa RTMP propia del relay (11-11-2026, Windows 11): `rtmp_wire_tests` (AMF0 en los dos
  sentidos, chunks escritos y leídos de vuelta a 128, 4096 y 60000 bytes, alimentados byte a byte,
  cabeceras comprimidas de un servidor intercaladas, Set Chunk Size, Abort, marcas de tiempo
  extendidas, entrada malformada y aleatoria, etiquetas FLV a mensajes con `@setDataFrame`, URL de
  servidor). Con `tests/relay_harness.cpp` y los pares tipo OBS, contra una plataforma que anuncia
  4096 bytes y otra que no anuncia nada: las pruebas estables a 0, 10 y 120 s, los cambios en
  directo `4:3,10:8,18:2,24:0`, una subida de 10 a 300 s en t=200 (190 s repetidos desde el
  volcado), una plataforma que hace ping cada 2 s con una ventana de acuses de 4 bytes (153 de 153
  pings respondidos), una plataforma que termina el stream con un estado de error a los 10 s (el
  relay reconectó 2 s después) y otra que corta el RTMP del relay tras `connect` (dos veces, y luego
  siguió el cliente de FFmpeg). La plataforma vio el digest de handshake que envía el cliente de
  FFmpeg, y chunks de 4096 bytes del relay. Cada salida: cada paquete idéntico byte a byte a un
  paquete del origen (sha256), DTS de vídeo estrictamente creciente, ningún salto de DTS de más de
  200 ms (el mayor: 34 ms en vídeo, 91 ms en audio), decodifica sin errores. Contra los ingest
  reales con una clave no válida (`invalid-probe`, sin publicar nada): Twitch (RTMP y RTMPS), YouTube
  (RTMP y RTMPS) y Kick (RTMPS) completaron el TLS, el handshake, `connect` y `createStream`, y
  cerraron la conexión tras `publish`, igual que con el cliente de FFmpeg. Aún sin probar: un stream
  con una clave válida en cualquiera de ellos.
- Aún sin probar: una plataforma real (Twitch por RTMP, Kick por RTMPS, YouTube con HEVC por
  enhanced RTMP) mirando su reproductor, el dock a mano (también la línea nueva "Subidas al
  instante"), el volcado dentro de un OBS real, el volcado en marcha en macOS (aquí no se compila) y
  Linux (solo tests unitarios), un disco lento (USB, HDD) o casi lleno en la práctica, y OBS
  reconectando al relay a mitad de directo (el relay escucha una sola vez: si se cae la conexión de
  OBS con él, el directo termina en vez de continuar).
- El relay y la integración con OBS compilan en Windows, Linux y macOS. En Linux, el relay en sí
  funcionó de extremo a extremo con `tests/relay_harness.cpp` (ffmpeg haciendo de OBS y de
  plataforma) contra el FFmpeg estático del plugin: RTMP y RTMPS (certificado y nombre de host
  comprobados por OpenSSL), el delay 0 -> 6 -> 0 s en directo y el envío al parar. macOS y Linux
  aún no se han probado en un OBS real.
