*[English version](STREAM_MODE.md)*

# Modo stream (v1.0.0)

El modo stream retrasa el stream que OBS **ya ha codificado**, en vez de los frames renderizados.
Es el modo por defecto desde la v1.0.0 allí donde puede funcionar (Windows); el modo escena (modo
buffer, `docs/SPEC-es.md`) se queda para los casos que el modo stream no cubre.

## Por qué casi no cuesta nada

El modo escena, en cada frame: renderiza, lee la imagen de la GPU, la comprime en RAM (MJPEG o el
HEVC de la GPU), la descomprime más tarde, la vuelve a renderizar, y después OBS la codifica para
el stream. Su RAM es el buffer a una calidad cercana a la de origen: de ~190 MB a ~2,3 GB para
30-60 s (tabla del README).

El modo stream guarda los paquetes que el propio encoder de OBS ha producido para el stream y los
envía más tarde. No se decodifica ni se vuelve a codificar nada; la CPU solo copia bytes. La RAM es
el bitrate del stream por los segundos guardados:

| Bitrate | 30 s | 60 s | 120 s (lo guardado por defecto) | 600 s |
|---|---|---|---|---|
| 3 Mbps | 11 MB | 22 MB | 45 MB | 225 MB |
| 6 Mbps | 22 MB | 45 MB | 90 MB | 450 MB |
| 8 Mbps | 30 MB | 60 MB | 120 MB | 600 MB |

Las herramientas que reenvían el stream fuera de OBS funcionan igual. Una cifra de "0-15 MB" para
una de ellas solo puede ser su consumo base o un delay casi nulo: un stream de 6 Mbps guardado
durante 60 s son 45 MB, lo guarde quien lo guarde.

## Cómo funciona

1. Con `OBS_FRONTEND_EVENT_STREAMING_STARTING` (después de que OBS cree la salida de emisión y
   antes de que arranque), `StreamDelayController` comprueba que el directo es apto: el
   `rtmp_output` de OBS, protocolo RTMP o RTMPS, sin Enhanced Broadcasting (multipista). Lee el
   servidor y la clave con `obs_service_get_connect_info()` (el servidor de ingesta ya resuelto,
   incluido "auto").
2. Arranca `StreamRelay` (avformat de FFmpeg, Windows): un listener en `127.0.0.1:<puerto libre>` y
   un emisor hacia el servicio del streamer.
3. Apunta **esa salida** al relay con un servicio `rtmp_custom` privado. El servicio guardado del
   streamer no se toca; OBS lo vuelve a poner en la salida en cada arranque.
4. OBS publica en el relay; el relay separa el FLV (sin decodificar) en `PacketHistory`
   (`packet-delay-core.*`) y marca cada paquete con su hora de llegada.
5. El `SinkScheduler` del emisor envía el paquete *p* cuando `now >= arrival(p) + delay`,
   remultiplexado a FLV por RTMP/RTMPS (TLS de SChannel), y la conexión con la plataforma se abre
   cuando toca enviar el primer paquete (un directo que empieza con delay empieza esos segundos
   más tarde en la plataforma).

### Cambiar el delay en directo

El emisor **empalma**: salta al keyframe de vídeo más reciente que tenga al menos *delay* de
antigüedad y reescribe los timestamps para que el siguiente frame vaya un intervalo de frame
después del último enviado. Subir el delay salta hacia atrás: los espectadores vuelven a ver los
últimos segundos, nunca nada más nuevo. Quitarlo salta hacia delante. La plataforma ve un único
stream continuo: sin reconexión ni cabecera nueva. El paso es el intervalo de fotogramas clave
(se recomiendan 2 s); el dock avisa cuando está en automático o pasa de 4 s (ver Comprobación de
ajustes).

El historial siempre llega `max(delay, guardado)` segundos atrás más dos intervalos de fotogramas
clave, así que subir el delay hasta "Guardar para subidas al instante" (120 s por defecto) no
necesita esperar. Si pides más de lo que tiene el historial, el emisor espera hasta que el keyframe
guardado más antiguo tenga la antigüedad suficiente (`SpliceInfo::shortHistory`).

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
  escena, segundos guardados (10-600), envío al parar, la insignia.
- Atajos (`Ajustes → Atajos` de OBS): Activar/desactivar, Activar, Desactivar (mismos nombres que
  antes), Volver al directo, Delay +5 s, Delay -5 s, Borrar, Preset 1-3. Pasan por `DelayRouter`:
  al modo stream cuando este lleva los controles, al modo escena si no (ahí, +/-5 y los presets
  cambian los segundos del siguiente Activar, limitados a 1-60, un preset además activa el delay, y
  Borrar no hace nada).
- obs-websocket 5, vendor `trigglow-dynamic-delay`: `GetStatus`, `EnableDelay`, `DisableDelay`,
  `ToggleDelay`, `SkipToLive`, `SetDelay {seconds}`, `AdjustDelay {delta}`, `DumpDelay`,
  `ApplyPreset {index}` (0-2); evento `StatusChanged`. Para el plugin de Stream Deck de Trigglow;
  detalles en `docs/STREAM_DECK-es.md`.

La puerta de cuenta gratuita se aplica a Activar igual que en el modo escena.

## Lo que aún no cubre

- macOS y Linux: el FFmpeg del plugin ahí todavía no tiene avformat, así que el modo stream no está
  disponible y se usa el modo escena.
- Enhanced Broadcasting (vídeo multipista) y los servicios SRT, RIST y WHIP: modo escena.
- El delay por escena solo existe en el modo stream: el modo escena sigue retrasando la única
  escena en directo elegida en el dock.

## Estado de la verificación

- `packet-delay-core` tiene tests unitarios (`tests/packet_delay_core_tests.cpp`): paso directo en
  vivo, rebobinado al activar con el delay mantenido y timestamps monótonos, salto hacia delante al
  desactivar, historial corto, empalme antes del primer keyframe, recorte, solo audio.
- El relay y la integración con OBS compilan en Windows; **todavía no** se ha hecho un directo a
  una plataforma real. Pendiente antes de la release: Twitch (RTMP), Kick (RTMPS), YouTube (HEVC
  por enhanced RTMP), un cambio de delay y un OBS parado a mitad de delay, mirando el reproductor de
  la plataforma.
