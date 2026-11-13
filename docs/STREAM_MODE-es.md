*[English version](STREAM_MODE.md)*

# Modo stream (v1.0.0)

El modo stream retrasa el stream que OBS **ya ha codificado**, en vez de los frames renderizados.
En el dock es la opción avanzada **"Aplicar el delay solo a la emisión (la grabación y la vista
previa no se retrasan)"** (Opciones > Avanzado), desmarcada por defecto: por defecto el delay se
aplica a todo OBS, con el modo escena (modo buffer, `docs/SPEC-es.md`), como en la 0.7. El modo
stream funciona allí donde existe su relay (Windows y, en alfa, macOS y Linux).

## Un panel, dos motores

El dock es un solo panel con un solo botón "Activar delay". La casilla elige el motor, guardado como
`apply_to` (`"all"` o `"stream"`) en `stream-mode-settings.json` (`src/apply-to.*`):

- **A todo OBS** (`"all"`, por defecto, también si falta la clave o no es uno de los dos valores):
  el modo escena. Mientras se llena el buffer se ve la escena de carga, y después el Programa, y por
  tanto el directo y la grabación, van con delay. El dock muestra los selectores de escena en
  directo y de carga, la calidad mínima y la estimación de RAM.
- **Solo a la emisión** (`"stream"`): el modo stream. El dock oculta eso y muestra a dónde va el
  directo, Borrar, la comprobación de ajustes, el delay por escena, el búfer del delay y el envío al
  parar.

El botón del dock, los atajos, obs-websocket y el Stream Deck pasan todos por `DelayRouter`
(`src/delay-router.hpp`), que manda cada orden solo al motor elegido:
`StreamDelayController::HandlesControls()` es verdadero cuando se ha elegido "solo a la emisión" y
el relay existe aquí (`ControlsGoToStream()`); un directo que no puede usar el relay no pasa los
controles al modo escena, sino que el dock dice por qué. La casilla se bloquea mientras el delay de
cualquiera de los dos motores está activo o enviando el resto ("Desactiva el delay para
cambiarlo."); si se marca con OBS ya emitiendo, se aplica desde el siguiente directo. Al cambiarla,
el motor nuevo empieza con los segundos que muestra el dock; la insignia (activada o no, y su
esquina) se escribe en los dos. Al cargar, el motor que no está elegido toma los segundos y la
insignia del elegido.

Las builds de prueba de la 1.0 guardaban `mode_enabled` (true por defecto, "Usar el modo stream").
La 1.0 nunca se publicó, así que esa clave se ignora y se borra al guardar, y esas instalaciones
empiezan con todo OBS (`apply_to_tests` cubre la migración y el router).

## Por qué casi no cuesta nada

El modo escena, en cada frame: renderiza, lee la imagen de la GPU, la comprime en RAM (MJPEG o el
HEVC de la GPU), la descomprime más tarde, la vuelve a renderizar, y después OBS la codifica para
el stream. Su RAM es el buffer a una calidad cercana a la de origen: de ~190 MB a ~2,3 GB para
30-60 s (tabla del README).

El modo stream guarda los paquetes que el propio encoder de OBS ha producido para el stream y los
envía más tarde. No se decodifica ni se vuelve a codificar nada; la CPU solo copia bytes. El
historial es el bitrate del stream por los segundos guardados (45 MB por minuto a 6 Mbps). El
streamer elige dónde espera: en RAM, o casi todo en un archivo temporal cifrado en el disco, donde
la RAM que ocupa es más o menos la misma con cualquier delay (ver Memoria y disco).

## Memoria y disco

**Dónde vive el búfer del delay** lo elige el streamer (dock: Opciones > Búfer del delay, guardado
como `buffer_storage` en `stream-mode-settings.json`), como el ajuste "memoria / disco" del búfer de
un grabador:

- **Memoria:** todo el historial se queda en RAM y no se escribe nada en disco: el bitrate por los
  segundos guardados (45 MB por minuto a 6 Mbps). Llega hasta el delay o el preset o la regla por
  escena con delay más alto (ver Cambiar el delay en directo), así que subir por encima de eso espera
  a los segundos que faltan.
- **Disco:** el volcado de abajo. La RAM se queda en 11-16 MB con cualquier delay y toda subida hasta
  600 s es al instante, pero todo el directo se escribe en disco mientras emites (unos 2,8 GB por
  hora a 6 Mbps), y se vuelve a leer con delays largos.
- **Automático** (el valor por defecto, también para ajustes guardados antes de existir la opción):
  Memoria si la carpeta temporal está en un SSD, Disco si está en un disco duro mecánico. Un SSD gana
  poco con el volcado en los delays habituales (60 s de historial son 45 MB de RAM) y a cambio
  recibiría horas de escrituras continuas; un disco duro no se desgasta al escribir, y un equipo con
  uno es más probable que vaya justo de RAM. Una unidad cuyo tipo no se puede saber (unidad de red,
  un adaptador USB que no lo transmite, un error) cuenta como SSD. El dock dice lo detectado:
  "Automático (Memoria, SSD detectado)".
- La unidad se comprueba al cargar OBS (para el dock) y otra vez al empezar cada directo, y queda en
  el log una vez por directo (`delay buffer: memory (choice auto, temp folder drive ssd)`), nunca con
  la ruta. Un cambio en directo se aplica al siguiente; el dock lo indica.
- **Detección** (`drive-kind.*`, la unidad de la carpeta del volcado): en Windows, los discos del
  volumen responden a `IOCTL_STORAGE_QUERY_PROPERTY` con `StorageDeviceSeekPenaltyProperty` (si hay
  penalización de búsqueda es un disco duro), y `StorageDeviceTrimProperty` como segunda señal
  cuando eso no se puede leer (TRIM significa SSD; sin TRIM, por sí solo, no demuestra nada). Un
  volumen sobre varios discos es HDD si lo es alguno. A las unidades de red y los discos en RAM no
  se les pregunta. No hacen falta permisos de administrador. En Linux,
  `/sys/dev/block/<major:minor>/queue/rotational` del dispositivo de la carpeta (su disco si es una
  partición; para un sistema de archivos con número de dispositivo anónimo, como btrfs, el origen
  del montaje en `/proc/self/mountinfo`). En macOS, el "Medium Type" de IOKit ("Solid State" /
  "Rotational") en las características del dispositivo, buscado hacia arriba en el registro desde
  el disco BSD de la carpeta (pasando por los contenedores APFS hasta la unidad física).

El resto de esta sección es cómo se guarda el historial y cómo funciona Disco:

- **Almacenamiento contiguo.** Los bytes de los paquetes van seguidos en bloques de 1 MiB
  (`PayloadStore`, `payload-store.*`), un registro por paquete: sus flags, sus datos laterales y su
  contenido, exactamente lo que devolvió el demuxer de FFmpeg, así que la plataforma recibe los
  mismos bytes. Los metadatos del planificador son 48 bytes por paquete. Antes, cada paquete era
  una reserva de memoria propia de FFmpeg, que costaba un 10-13 % encima del historial (abajo: +490
  MB de RAM para 446 MB de historial, +106 MB para 93 MB).
- **El volcado a disco** (Disco). En RAM solo quedan los segundos más recientes (4 s más dos intervalos de
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
- **Uso de disco.** El bitrate por 600 s mientras emites con Disco (unos 450 MB a 6 Mbps, 600
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
| 600 s, solo RAM (Memoria) | 607 s, 446 MB | | | +454 MB | 449 MB | 0 |

"+N MB" es cuánto creció el proceso (bytes privados) respecto a sí mismo antes del directo. "Antes"
es el relay sin estos cambios, con "Guardar para subidas al instante" en su valor por defecto (120
s); "Solo bloques contiguos" es el primer cambio sin el volcado a disco. Dentro de OBS, el historial
es lo único del modo stream que crece con el delay.

Las herramientas que reenvían el stream fuera de OBS guardan los mismos bytes: un stream de 6 Mbps
guardado durante 60 s son 45 MB, estén donde estén. El modo stream los guarda en disco, cifrados, y
en RAM solo lo que está a punto de enviarse.

## Cómo funciona

1. Con `OBS_FRONTEND_EVENT_STREAMING_STARTING` (después de que OBS cree la salida de emisión y
   antes de que arranque), si se ha elegido "solo a la emisión", `StreamDelayController` comprueba que el directo es apto: el
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

Con Disco (el volcado), el historial siempre llega 600 s atrás (el máximo del delay) más dos
intervalos de fotogramas clave más 1 s: cuesta disco, no RAM, y cualquier subida en directo es al
instante en cuanto el directo lleva ese tiempo. Con Memoria, o si no se pudo usar el disco, llega el
delay o el preset o la regla por escena con delay más alto, lo que sea mayor, más el mismo margen
(`KeepSeconds()` y `RetentionNs()` en `packet-delay-core.hpp`). El "Subidas al instante: N s" del
dock, junto a "Búfer del delay", dice cuál se aplica. Si pides más de lo que tiene el historial (al principio de un directo, o
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
escena con el delay activo es seguro (a todo OBS solo se retrasa la escena en directo elegida en el
dock).

## La insignia de delay

Mientras el delay está activo y el directo pasa por el relay, la insignia "DELAY N s" (los mismos
PNG que en el modo escena, hasta 600 s) se dibuja en el canal de salida 61 de libobs, sobre el
Programa de OBS, después de la escena. Los espectadores la reciben con la imagen retrasada. Como está
en el Programa, la grabación (y todo lo que tome el Programa) también la muestra mientras el delay
está activo. Activarla o no ("Mostrar la insignia de delay a los espectadores") y la esquina están en
las Opciones del dock, un solo control para los dos motores, y se guardan.

## Comprobación de ajustes

Al empezar un directo, el controlador revisa los ajustes de OBS y el dock lista lo que empeora el
modo stream (`StreamDelayController::CheckObsSettings`):

- el intervalo de fotogramas clave del encoder del stream en automático (0, a menudo 4-10 s) o por
  encima de 4 s: el delay se mueve en pasos de ese tamaño;
- el Retardo de la transmisión propio de OBS (`Ajustes → Avanzado`) activado: su retardo fijo se
  suma a este.

## Controles

- Dock (con "solo a la emisión" marcado): el único botón de activar/quitar, segundos (1-600, +/-5,
  los presets como atajos rápidos), borrar, los tres presets (Presets con atajo), delay por escena, el búfer del delay (Automático / Memoria / Disco) con hasta dónde una subida es al
  instante (solo lectura; sustituye al ajuste "Guardar para subidas al instante", cuyo `keep_seconds` guardado se sigue leyendo y escribiendo sin cambios),
  envío al parar, la insignia, adónde va el directo (solo lectura: `Ajustes → Emisión` de OBS). Las
  estadísticas muestran la RAM y el disco que ocupa el historial.
- Atajos (`Ajustes → Atajos` de OBS): Activar/desactivar, Activar, Desactivar (mismos nombres que
  antes), Volver al directo, Delay +5 s, Delay -5 s, Borrar, Preset 1-3. Pasan por `DelayRouter`,
  solo al motor elegido en el dock (a todo OBS los segundos también cambian en directo, 1-600, un
  preset además activa el delay, y Borrar no hace nada).
- obs-websocket 5, vendor `trigglow-dynamic-delay`: `GetStatus`, `EnableDelay`, `DisableDelay`,
  `ToggleDelay`, `SkipToLive`, `SetDelay {seconds}`, `AdjustDelay {delta}`, `DumpDelay`,
  `ApplyPreset {index}` (0-2); evento `StatusChanged`. Para el plugin de Stream Deck de Trigglow;
  detalles en `docs/STREAM_DECK-es.md`.

La puerta de cuenta gratuita se aplica a Activar igual que en el modo escena.

## Lo que aún no cubre

- Enhanced Broadcasting (vídeo multipista) y los servicios SRT, RIST y WHIP: el dock dice que ese
  directo no puede usarlo; a todo OBS (lo predeterminado) funciona con ellos.
- El delay por escena solo existe con "solo a la emisión": a todo OBS sigue retrasando la única
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
- La elección del búfer del delay (2026-11-11), Windows 11:
  - Tests unitarios: `drive_kind_tests` (Automático con unidades SSD, HDD y desconocidas, las
    opciones explícitas, valores guardados y ajustes de antes de existir la opción, las señales de
    Windows y varios discos, el análisis de `rotational` y `mountinfo` en Linux, el tipo de medio
    en macOS).
  - Detección en este equipo (dos SSD NVMe, ningún disco duro): C: y E: SSD, la carpeta temporal
    (en C:) SSD; letras de unidad que no existen, desconocida.
  - En un OBS 33.0.1 portable: un archivo de ajustes sin `buffer_storage` se cargó como Automático
    y se volvió a guardar como `auto`; un directo a un receptor RTMP local con Automático dejó en
    el log `delay buffer: memory (choice auto, temp folder drive ssd)` y no creó archivo de
    volcado, y uno con Disco dejó `disk (choice disk, ...)` y el archivo de volcado existía mientras
    se emitía. El dock entre 260 y 380 px en inglés, español, portugués y chino: nada cortado;
    "Subidas al instante" va junto a "Búfer del delay" o debajo cuando no cabe, y la etiqueta larga
    de Automático acaba en "..." cuando el desplegable es demasiado estrecho.
- Aún sin probar: una plataforma real (Twitch por RTMP, Kick por RTMPS, YouTube con HEVC por
  enhanced RTMP) mirando su reproductor, el volcado en marcha en macOS (aquí no se compila) y
  Linux (solo tests unitarios), la detección de la unidad en marcha en macOS y Linux y con un
  disco duro, una unidad USB o una carpeta temporal en red, un disco lento (USB, HDD) o casi lleno
  en la práctica, y OBS
  reconectando al relay a mitad de directo (el relay escucha una sola vez: si se cae la conexión de
  OBS con él, el directo termina en vez de continuar).
- El relay y la integración con OBS compilan en Windows, Linux y macOS. En Linux, el relay en sí
  funcionó de extremo a extremo con `tests/relay_harness.cpp` (ffmpeg haciendo de OBS y de
  plataforma) contra el FFmpeg estático del plugin: RTMP y RTMPS (certificado y nombre de host
  comprobados por OpenSSL), el delay 0 -> 6 -> 0 s en directo y el envío al parar. macOS y Linux
  aún no se han probado en un OBS real.
