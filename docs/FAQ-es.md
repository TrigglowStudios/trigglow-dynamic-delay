*[English version](FAQ.md)*

# FAQ — Trigglow Dynamic Delay

## Modo stream (v1.0.0, Windows)

**¿Qué diferencia hay entre el modo stream y el modo escena?**
El modo stream (nuevo en la 1.0.0 y el modo por defecto en Windows) retrasa el directo que OBS **ya
ha codificado**: el plugin apunta la salida de emisión de OBS a un relay dentro de OBS, en
`127.0.0.1`, guarda los paquetes en RAM y los envía a tu plataforma pasado el delay. El modo escena
(el único antes de la 1.0.0, ahora "Modo escena (compatibilidad)" en el dock) guarda en RAM tu
escena en directo ya renderizada y la reproduce en una escena auxiliar, antes de que OBS la
codifique. El modo stream gasta mucha menos RAM y CPU, retrasa todas las escenas, deja la grabación
en directo y te deja cambiar el delay en pleno directo; el modo escena funciona con cualquier salida
y en todas las plataformas. Detalles: `docs/STREAM_MODE-es.md`.

**¿Por qué el modo stream usa tan poca RAM?**
Guarda lo que el propio encoder de OBS ya ha producido para el directo, y nada más: no se decodifica
ni se vuelve a codificar nada, la CPU solo copia bytes. La RAM es tu bitrate por los segundos
guardados: unos 45 MB por minuto a 6 Mbps. Por defecto guarda 120 s ("Guardar para subidas al
instante"), así que el delay se puede subir hasta ahí en directo sin esperar: unos 90 MB a 6 Mbps.
El modo escena guarda frames con una calidad cercana a la de tu fuente, y por eso ocupa de ~190 MB a
~2,3 GB para 30–60 s.

**¿Qué directos pueden usar el modo stream?**
Los que salen por la salida RTMP propia de OBS, por RTMP o RTMPS: Twitch, YouTube, Kick, TikTok y
cualquier servidor rtmp:// o rtmps:// personalizado, con Enhanced Broadcasting desactivado. (Aún)
no: Enhanced Broadcasting, los servicios SRT, RIST y WHIP, ni macOS y Linux (su FFmpeg todavía no
tiene la parte que necesita el relay). En esos casos el dock dice por qué y el modo escena funciona
como siempre.

**¿Se reconecta el directo al activar, quitar, subir o bajar el delay?**
No. El relay mantiene una sola conexión con tu plataforma y empalma: activar el delay (o subirlo)
salta atrás a un keyframe anterior, así que los espectadores vuelven a ver los últimos segundos,
nunca nada más nuevo; quitarlo (o bajarlo) salta hacia delante. Las marcas de tiempo se reescriben
para que la plataforma vea un único directo continuo. Si activas el delay antes de empezar a emitir,
el directo empieza en la plataforma esos segundos tarde.

**¿Por qué el dock me pide poner el intervalo de keyframes en 2 s?**
El delay solo puede moverse de keyframe en keyframe, así que el intervalo entre keyframes es el paso
con el que se mueve. En automático (0) la mayoría de encoders usan 4–10 s. El dock avisa cuando está
en automático o por encima de 4 s; cámbialo en `Ajustes → Salida` (modo de salida Avanzado,
"Intervalo de fotogramas clave"). 2 s es además lo que piden la mayoría de plataformas.

**El dock avisa de que el Retardo de emisión propio de OBS está activado. ¿Por qué?**
`Ajustes → Avanzado → Retardo de la transmisión` suma su retardo fijo encima de este. Desactívalo y usa el
de Dynamic Delay.

**¿Puedo emitir a varias plataformas a la vez?**
Sí. Añade destinos en el dock (un nombre, un servidor rtmp:// o rtmps:// y su clave): reciben el
mismo directo retrasado, de la misma y única codificación. Cada uno se conecta, reintenta (a los 2,
4, 8 y 15 s) y muestra su estado por su cuenta, así que si uno falla los demás siguen. Sus claves se
guardan en este ordenador, en el `stream-mode-settings.json` del plugin, igual que OBS guarda tu
propia clave.

**¿Qué hace "Borrar lo que aún no se ha emitido"?**
¿Se ha visto algo que no debía con el delay activo? Púlsalo (en el dock, con un atajo o por
obs-websocket): los segundos que aún esperan en el delay no se envían nunca. El directo salta al
momento actual en más o menos un intervalo de keyframes, y el delay vuelve solo pasados sus segundos
más 3 s, rebobinando solo hasta lo que vino después del borrado. No hace nada con el delay quitado
ni en modo escena.

**¿Qué son los presets y "Delay por escena"?**
Tres presets (10, 30 y 60 s por defecto) que ponen sus segundos y activan el delay, desde el dock,
un atajo u obs-websocket. "Delay por escena" activa el delay (con sus propios segundos o los
actuales) o lo quita cuando cambias a una escena: activado en tu escena de juego, quitado en la de
charla. Las reglas se aplican cuando OBS cambia de escena y cuando empieza un directo, solo en modo
stream.

**¿Qué pasa si paro el directo con el delay activo?**
Por defecto el plugin sigue enviando los segundos que los espectadores aún no han visto y después
para ("Directo parado: enviando los últimos 30 s"). **Dejar de enviar ya** lo corta, y puedes
desactivarlo con "Al parar el directo, enviar los segundos que los espectadores aún no han visto".
Empezar un directo nuevo o cerrar OBS también lo corta.

**¿Por qué OBS muestra 0 frames perdidos en modo stream?**
Ahora OBS envía el directo al relay de tu propio ordenador, así que su contador de congestión solo
ve ese enlace local. El dock muestra en su lugar el estado y la tasa de cada destino, y un destino
que se queda más de 4 s atrás (subida lenta) vuelve a saltar al delay en el siguiente keyframe en
vez de dejar que el delay crezca.

**¿Está probado el modo stream?**
Su núcleo (los empalmes, las marcas de tiempo, el recorte y el borrado) tiene tests unitarios, y el
relay se comprobó de principio a fin en una máquina local con un directo de prueba, con cambios de
delay y un borrado incluidos. **Todavía no se ha usado en un directo a una plataforma real**
(Twitch, Kick, YouTube); es el siguiente paso antes de darlo por verificado
(`docs/STREAM_MODE-es.md`, "Estado de la verificación"). El modo escena sigue disponible si algo
falla.

## General

**¿Se corta o reconecta el stream al activar, desactivar o usar el delay?**
No — nunca, en ningún momento, por ningún motivo, en ninguno de los dos modos. El modo stream
mantiene una sola conexión con tu plataforma y empalma el directo retrasado (ver arriba). El modo
escena no llama en absoluto al ajuste de delay nativo del output de streaming de OBS. En su lugar, dos
filtros de OBS acumulan vídeo y audio en un anillo de RAM del sistema, y el plugin cambia el
Programa de OBS entre tu escena en directo, una escena de carga opcional y una escena "wrapper"
delayed, todo internamente — el output de streaming en sí nunca ve nada de esto. Detalle técnico
completo, incluyendo por qué se probó y se abandonó un diseño anterior basado en reconexión, en
`docs/SPEC-es.md` §2–§3.

**¿El audio también se retrasa, en sincronía con el vídeo?**
Sí. Vídeo y audio usan el mismo delay configurado en segundos, así que vuelven a estar en
sincronía — el audio usa un filtro aparte adjunto a cada fuente individual con audio dentro de tu
escena en directo (OBS no permite adjuntar un filtro de audio directamente a una escena), pero
ambos funcionan con el mismo valor de `delaySeconds`. Desde la v0.3.2, esto sigue siendo cierto
incluso si el presupuesto de RAM obliga al vídeo a guardar de verdad menos tiempo del pedido (ver la
siguiente pregunta) — el audio se ajusta automáticamente a lo que el vídeo realmente puede entregar,
en vez de quedarse por delante. Desde la v0.6.0 ambos usan exactamente el mismo delay en segundos
enteros también en ese caso (antes podían quedar hasta ~1 s separados), y el dock y el overlay del
directo muestran esa misma cifra. Ver `docs/SPEC-es.md` §3.2/§3.3.

**¿Cómo interactúan los ajustes "Delay (segundos)" y "Calidad mínima"?**
De forma independiente, por diseño — eliges ambos libremente. La calidad mínima (de 480p a 2160p
(4K)) marca hasta dónde puede bajar el plugin la resolución del buffer para ahorrar RAM: nunca baja
de ahí, y en su lugar acorta el tiempo real de buffer si los segundos pedidos no caben en el
presupuesto de RAM detectado en tu máquina a esa calidad. Eso sí, desde la v0.6.0 nunca eleva el
buffer por encima de la resolución de salida de OBS (Ajustes → Vídeo → Resolución de salida
(escalada)): con una salida de 720p el buffer es 720p aunque elijas 1080p, porque OBS no tiene nada
por encima de su salida que emitir. Desde la v0.7.0, cuando todos los encoders que ven tu lienzo
(emisión, grabación, pistas de Enhanced Broadcasting) codifican por debajo de la salida, el buffer
se guarda al tamaño del mayor de ellos: una salida 1440p que se emite con un encoder reescalado a
1080p, sin grabar nada por encima de 1080p, guarda el buffer a 1080p. También es el suelo de la
reducción automática de calidad cuando tu equipo no llega (ver más abajo). El dock muestra una
estimación en vivo, no bloqueante, de lo que una combinación concreta va a lograr realmente
(segundos y resolución) mientras ajustas cualquiera de los dos, antes incluso de pulsar Enable. Ver
`docs/SPEC-es.md` §3.5.

**¿Qué pasa si el delay + calidad que pido no caben en RAM?**
Recibes un aviso claro en el dock *antes* de activarlo — nunca falla en silencio ni crashea. El
plugin simplemente guarda menos segundos de los pedidos a la calidad elegida en vez de bajar nunca
de esa calidad; bajar los segundos o la calidad mínima te devuelve al tiempo completo pedido.

**¿Por qué no puedo cambiar el delay mientras está activo?**
Desde la v0.6.0 las escenas, los segundos de delay y la calidad mínima se bloquean mientras el delay
se prepara o está activo, con una línea que dice "Desactiva el delay para cambiarlo." Antes, los
segundos seguían editables y cada tecla redimensionaba el buffer en pleno directo. Desactiva el
delay, cámbialo y vuelve a activarlo; se llena de nuevo con los segundos nuevos.

**¿Cuánto delay máximo soporta?**
El spinner del dock acepta 1–60 segundos como rango de sensatez de la interfaz. El límite real es
lo que quepa en el presupuesto de RAM detectado en tu máquina (aproximadamente la mitad de la RAM
total del sistema, con un suelo de 1GB y un techo de 24GB) a la calidad elegida — si 60s a tu
calidad elegida no caben, la estimación en vivo del dock te lo dirá antes de activarlo, y el plugin
guardará tantos segundos como quepan de verdad en su lugar.

**¿Comprime el buffer? ¿Eso baja la calidad?**
Sí. Solo cambia cómo se guardan en memoria los frames del buffer: la calidad mínima que elijas (de
480p a 2160p (4K)) funciona exactamente igual.
- **Windows con encoder de vídeo por hardware (v0.7.0):** los frames se guardan como vídeo HEVC (o
  H.264 en una GPU sin HEVC) con el encoder de tu GPU (NVENC de NVIDIA, AMF de AMD o Quick Sync de
  Intel), y se decodifican en la GPU. Un keyframe cada medio segundo y los frames intermedios
  guardados como diferencias: por eso ocupa más o menos la mitad de RAM que MJPEG. Además la imagen
  se parece más a la original: en nuestras mediciones los frames guardados vuelven con un PSNR-Y de
  ~45 dB, frente a ~40 dB con MJPEG.
- **Windows sin encoder por hardware, y Linux:** cada frame se comprime por separado con MJPEG, como
  desde la v0.3.0, visualmente casi sin pérdidas. El gameplay real comprime unas 11–15x, aunque
  depende de lo que haya en pantalla.
- **macOS:** el buffer se guarda sin comprimir.

La compresión y la descompresión se ejecutan en hilos propios del plugin (y en el motor de vídeo de
la GPU con el encoder por hardware), no en el hilo gráfico de OBS, y si un frame no se puede
comprimir se guarda sin comprimir. Ver `docs/SPEC-es.md` §4 y §4.8.

**¿Cuánta RAM ocupa el buffer del delay?**
Depende de la resolución de salida, del delay, de lo que haya en pantalla y de cómo se guarde.
Medido fuera de OBS con contenido tipo gameplay a 60 fps, solo el buffer:

| Salida | 30 s, HEVC por hardware (Windows, v0.7.0) | 30 s, MJPEG | 60 s, HEVC por hardware | 60 s, MJPEG |
|---|---|---|---|---|
| 1080p60 | ~190 MB | ~400 MB | ~290 MB | ~750 MB |
| 1440p60 | ~325 MB | ~640 MB | ~520 MB | ~1200 MB |
| 2160p60 | ~590 MB | ~1250 MB | ~920 MB | ~2300 MB |

- La columna por hardware se midió con una GPU AMD (AMF). **NVENC de NVIDIA aún no está medido**;
  sus bytes por frame, y con ellos la RAM, pueden variar algo.
- Frente a MJPEG, el códec por hardware ocupa entre un 49 % y un 62 % menos según la resolución y el
  delay. La v0.6.0 ocupaba algo más que la columna de MJPEG: unos 675 MB a 1440p60 con 30 s, ~1,2 GB
  con 60 s.
- Suma unos 11 MB por fuente de audio con sonido para 30 s de delay (22 MB para 60 s).
- El contenido que cambia mucho en cada frame comprime peor, con los dos códecs.
- Para ver lo que ocupa de verdad tu buffer, busca `arena=…MB committed` en las líneas `pipeline
  check` del log de OBS: son los frames guardados, la mayor parte (los códecs y las reservas de
  frames suman unos 50 MB con MJPEG y 110 MB con el códec por hardware a 1440p). `codec=`, en la
  misma línea, dice cómo se guarda (`docs/TROUBLESHOOTING-es.md`, "Cómo leer las líneas del log").

**¿El buffer usa el encoder de vídeo de mi GPU? ¿Puede provocar "Encoding overloaded"?**
En Windows, sí: desde la v0.7.0 el buffer usa el encoder por hardware de la GPU cuando lo hay, y
también su decoder. Está hecho para que el encoder de tu stream vaya siempre primero. Cuando el
buffer codifica en el mismo motor que alguno de los encoders de OBS (los dos en NVENC, por ejemplo):
- si ese motor empieza a saturarse (sube la latencia del encoder del buffer), el buffer baja un
  peldaño de resolución, nunca por debajo de tu calidad mínima, y no vuelve a subir en 5 minutos; el
  dock muestra su aviso ámbar "Calidad del buffer reducida";
- si OBS se salta aunque sea un frame por retraso de codificación, o una de sus salidas se para por
  un error del encoder, el buffer vuelve a MJPEG al momento, que no usa el motor para nada. El delay
  no se corta ni se mueve; el buffer solo ocupa más RAM hasta que desactivas el delay;
- cuando OBS empieza a emitir, a grabar o a usar el Replay Buffer, el buffer le cede antes su sesión
  NVENC a esa salida y la recupera unos segundos después.

Si el buffer codifica en otro motor (una iGPU mientras emites con la gráfica dedicada, o un stream
con x264), no puede provocar retraso de codificación y nada de esto tiene que actuar. Esta
protección se probó con una GPU AMD; **NVENC aún no se ha validado dentro de un OBS real**. Si
sospechas del buffer, fuerza MJPEG (siguiente pregunta) y compara.

**¿Puedo hacer que el buffer use MJPEG, como antes de la v0.7.0?**
Sí, con el ajuste oculto `buffer_codec`: `"buffer_codec": "mjpeg"` en el `buffer-mode-settings.json`
del plugin, editado con OBS cerrado. Tiene que ser exactamente ese texto en minúsculas; cualquier
otro valor mantiene el de por defecto, `"auto"` (hardware donde funcione, MJPEG si no), y el log
dice por qué. Está pensado para soporte o para descartar el códec por hardware en un problema: MJPEG
ocupa más o menos el doble de RAM y gasta más CPU. Pasos en `docs/TROUBLESHOOTING-es.md`, "Ajustes
ocultos para soporte".

**OBS muestra "Encoding overloaded" (sobrecarga al codificar) en cuanto activo el delay. ¿Por qué?**
Hasta la v0.5.0, la compresión y descompresión de los frames del buffer se ejecutaba dentro del hilo
gráfico de OBS, que tiene 16,7 ms por frame a 60 fps, y el buffer se guardaba a la resolución del
lienzo: con un lienzo mayor que la salida (por ejemplo lienzo 2560x1440, salida 1920x1080) ese
trabajo se comía la mayor parte del frame y OBS se quedaba atrás. **La v0.6.0 lo corrige**: el buffer
nunca se guarda por encima de la resolución de salida de OBS, y la compresión se ejecuta en hilos
propios del plugin, así que lo que queda en el hilo gráfico de OBS es uno o dos milisegundos por
frame. Actualiza a la última versión; si bajaste el lienzo para esquivarlo, puedes volver a subirlo.
Desde la v0.7.0 el buffer también puede usar el encoder de vídeo de la GPU, pero le cede el paso al
encoder de OBS (preguntas anteriores). Si aun así sigue pasando, mira la sección "Encoding
overloaded" de `docs/TROUBLESHOOTING-es.md`.

**¿Solo funciona a 1080p? ¿Y a 2K (1440p)?**
Funciona a cualquier resolución — el buffer sigue la resolución de salida de OBS. Para el
rendimiento lo que cuenta es la resolución y los fps de *salida*, no el lienzo:
- **Lienzo mayor que la salida** (p. ej. lienzo 1440p, salida 1080p): bien desde la v0.6.0, mismo
  coste que una salida 1080p.
- **Salida 1440p a 30 o 60 fps:** soportada desde la v0.6.0. En nuestro benchmark fuera de OBS a
  1440p60 (una CPU de 8 núcleos/16 hilos) el trabajo del plugin en el hilo gráfico de OBS es de
  ~1,2–1,7/1,6–2,1 ms p50/p95 por frame, y la compresión y la descompresión ocupan en total menos de
  un núcleo de CPU en sus propios hilos.
- **Calidad mínima 1440p (2K) o 2160p (4K):** en el dock desde la v0.6.0. Con una salida 1440p y un
  mínimo de 1440p el buffer nunca baja de 1440p, ni para ahorrar RAM ni cuando tu equipo no llega
  (entonces solo se captura a la mitad de fps, con una salida a 48 fps o más): si los segundos no
  caben en la RAM, el delay se acorta, igual que con 1080p. Por encima de tu salida no cambia nada:
  con una salida 1080p el buffer sigue a 1080p. Para ver la RAM que ocupa de verdad el buffer, busca
  `arena=…MB committed` en las líneas `pipeline check` del log de OBS (cada 10 s con el delay en
  marcha): a 1440p60 con 30 s y gameplay, espera unos 200–250 MB con el códec por hardware (`ratio=`
  de unas 40–60x) y 600–900 MB con MJPEG (unas 11–15x); los códecs y las reservas de frames suman
  encima unos 50 MB (MJPEG) a 110 MB (hardware). El plugin reserva margen para hasta ~1,9 GB con el
  5x que supone para MJPEG, o ~0,8 GB con el 12x del códec por hardware una vez que ha funcionado en
  esta sesión de OBS, y el dock avisa antes de activarlo si eso no cabe.
- **Si tu equipo no llega** con tus ajustes, el plugin baja la resolución del buffer (nunca por
  debajo de tu calidad mínima) y, como último paso, sus fps de captura, en vez de dejar que OBS se
  quede atrás o de cortar el delay. El dock te lo dice con un aviso ámbar (siguiente pregunta).

**¿Qué significa el aviso ámbar "Calidad del buffer reducida" del dock?**
Tu equipo no daba para el buffer del delay a calidad completa, así que el plugin lo aligeró: primero
añade hilos de compresión (eso no muestra aviso), después guarda el buffer a menos resolución, nunca
por debajo de tu calidad mínima, y como último paso captura un frame de cada dos (solo con una salida
a 48 fps o más). Desde la v0.7.0 también baja un peldaño cuando el motor de vídeo de la GPU que el
buffer comparte con el encoder de OBS se satura (el aviso da entonces como causa comprimir los
fotogramas, y no vuelve a subir en 5 minutos). El aviso dice qué se redujo y por qué. Los segundos
de delay no cambian nunca y el directo no se corta; los frames repetidos, si los hay, se ven como un
tirón breve. Cuando baja la carga, vuelve a subir solo tras 30 s con margen de sobra, y el aviso
desaparece. Qué hacer, si hace falta algo: `docs/TROUBLESHOOTING-es.md`.

**¿Afecta a la grabación local o al Replay Buffer?**
**En modo stream, no**: la grabación, el Replay Buffer y la cámara virtual siguen en directo
mientras los espectadores reciben el directo retrasado. Lo único que comparten es la insignia de
delay, si la muestras: se pinta sobre el programa de OBS, así que con el delay activo también sale
en la grabación.

**En modo escena, indirectamente sí**, y merece la pena entenderlo bien: el modo buffer funciona
cambiando lo que muestra el Programa de OBS, y tanto la grabación local como el Replay Buffer
capturan ese mismo output del Programa — así que si estás grabando (o tienes el Replay Buffer
activo) mientras el delay está `Active`, lo que se guarda es también el contenido delayed, igual que
lo que ven los espectadores. En modo escena no hay forma de retrasar el stream sin retrasar también
la grabación/Replay Buffer; para eso, usa el modo stream.

**La escena y los filtros del plugin tienen nombres nuevos. ¿Tengo que hacer algo?**
Desde la v0.6.0 terminan en "(auto)" en vez de "(no tocar)": **Trigglow Delay Buffer (auto)**,
**Trigglow Buffer Mode Delay (auto)**, `Trigglow Audio Delay (auto) - <fuente>` y **Trigglow Delay
Badge (auto)**. Tus colecciones de escenas se renombran solas. Solo tienes que actualizar las
acciones de Stream Deck, obs-websocket u otro control remoto que usen esos objetos **por su
nombre**; los atajos del plugin no se ven afectados. Si después vuelves a la v0.5.0 o anterior, esa
versión crea su propio juego "(no tocar)" junto al "(auto)": borra el que no uses.

**¿Qué es la insignia que ven los espectadores durante el delay?**
Una sola imagen que pinta el propio plugin: desde la v1.0.0, una placa redondeada con el logo de
Dynamic Delay, "DELAY" y tus segundos, y "Dynamic Delay by Trigglow" debajo, igual en todos los
idiomas. Solo se ve con el delay activo. En modo escena es la fuente **Trigglow Delay Badge
(auto)**: **Mostrar overlay de delay en el directo** en el dock la quita, el dock elige la esquina,
y puedes moverla y escalarla en OBS (se queda donde la pongas). En modo stream se pinta sobre el
programa de OBS en su propio canal de salida (hasta 600 s): **Mostrar la insignia de delay a los
espectadores** y la esquina están en la sección del modo stream, y no se puede mover a mano.

**¿Qué provoca el estado `Error`?**
Cualquier cosa que de otro modo dejaría al plugin en una condición rota pero silenciosa: no haber
elegido ninguna escena en directo, o que la escena en directo elegida ya no resuelva a nada (por
ejemplo, si se borró). El dock muestra un mensaje claro explicando qué falla en vez de colgarse o
crashear.

**¿Funciona con Twitch/YouTube/Kick/cualquier plataforma?**
Sí. El modo escena trabaja enteramente sobre el propio output de Programa de OBS, así que la
plataforma da igual. El modo stream funciona con cualquier plataforma a la que OBS emita por RTMP o
RTMPS (Twitch, YouTube, Kick, TikTok, servidores personalizados); los servicios SRT y WHIP usan el
modo escena.

**¿Necesito un panel web o una cuenta de Trigglow para usarlo?**
Panel web no: todo el control vive dentro de OBS. Desde la v0.3.3 sí necesitas iniciar sesión una
vez con una cuenta gratuita de trigglow.com para poder activar el delay, en cualquiera de los dos
modos (el inicio de sesión se hace en tu navegador, y el plugin sigue siendo 100% gratis) — ver
`docs/ACCOUNT_GATE-es.md`. Desde la v1.0.0 el inicio de sesión (y con él el delay y Trigi) funciona
también en macOS y Linux, no solo en Windows.

**¿Cómo funciona el inicio de sesión y qué guarda el plugin?**
El inicio de sesión siempre se hace en trigglow.com, en tu navegador: el plugin nunca te pide la
contraseña. Pulsa **Iniciar sesión** en el dock; muestra un código ("La página debe mostrar el
código KD7F-29QM."), y la página de trigglow.com muestra el mismo código junto con la app, su
versión y cuándo se pidió. Confirma solo si los códigos coinciden y lo has iniciado tú; **No he sido
yo** lo cancela. A partir de ahí el plugin guarda su propio inicio de sesión, que solo sirve para lo
que hace el plugin (comprobar tu cuenta, Trigi y tus propios tickets de Dynamic Delay). En Windows
va cifrado con la protección de datos de Windows para tu usuario de Windows; en macOS y Linux se
guarda en un fichero que solo tu usuario puede leer (modo 0600), sin cifrar, igual que OBS guarda tu
clave de retransmisión. Tu email no se guarda. **Cerrar
sesión** en el dock también lo desconecta en trigglow.com, y también puedes desconectarlo desde la
web: Ajustes → Sesiones activas → Apps conectadas.

**Tengo la v0.5.x o anterior. ¿Tengo que actualizar?**
Sí. El inicio de sesión de la v0.5.x y anteriores se apaga cuando se publica la v0.6.0: desde ese
momento esas versiones ya no pueden iniciar sesión en Trigglow, y las sesiones que ya tienen dejan
de funcionar tras un periodo de transición, así que el delay deja de funcionar en ellas. Instala la
última versión encima de la tuya, con OBS cerrado. En el primer arranque tu inicio de sesión pasa
solo al formato nuevo; si el dock te pide iniciar sesión, hazlo una vez. Tus escenas y ajustes se
conservan.

**El dock o "Reportar un problema" me dice que actualice el plugin. ¿Qué hago?**
"Esta versión del plugin ya no puede iniciar sesión. Actualiza el plugin." en el dock, "Actualiza el
plugin para usar Trigi" en "Reportar un problema" o "Actualización necesaria" en la página de inicio
de sesión de trigglow.com significan lo mismo: tu versión es demasiado antigua para lo que intentas
hacer. Descarga la última versión (**Descargar la actualización** en el diálogo, el aviso de
actualización del dock o la
[última release](https://github.com/TrigglowStudios/trigglow-dynamic-delay/releases/latest)) e
instálala con OBS cerrado; tus ajustes y tu inicio de sesión se conservan. Mientras tanto puedes
seguir enviando un reporte desde el formulario del diálogo o desde
[trigglow.com/support](https://www.trigglow.com/support).

**¿Qué es Trigi?**
El asistente de soporte con IA de Trigglow, el mismo que en trigglow.com, ahora dentro del plugin:
abre **Reportar un problema** en el dock y pregúntale por Dynamic Delay (cómo configurarlo, qué dice
el dock, rendimiento, inicio de sesión). Responde en el propio diálogo. Es una IA y puede
equivocarse, así que revisa lo que te diga antes de cambiar tu configuración. Si no lo resuelve,
**Crear ticket con esta conversación** se la envía al equipo con el contexto, y siempre puedes
saltarte a Trigi y escribir directamente al equipo. Trigi está disponible con la sesión iniciada (en
macOS y Linux desde la v1.0.0); si no, el diálogo se abre en el formulario de ticket. La primera vez, un aviso breve
("Antes de empezar con Trigi") explica todo esto, y no se envía nada hasta que lo aceptas y mandas tu
primera pregunta.

**¿Qué ve Trigi y qué no?**
Ve lo que escribes y, solo si marcas **Incluir datos técnicos de OBS**, un pequeño conjunto de
cifras y estados que se envían una vez al empezar la conversación: versiones de OBS y de Windows,
renderizador, fabricante de la GPU, RAM total, familia del codificador, el estado, los segundos y la
calidad mínima del delay, tamaño del lienzo y de la salida, fps, los frames retrasados y saltados de
OBS, la salud del buffer y los códigos de problemas recientes del plugin. **Ver qué se envía**
muestra los datos exactos antes de enviarlos. La casilla está desmarcada hasta que la marques. Trigi
nunca recibe tu log de OBS (ni completo, ni en partes, ni resumido), y los datos técnicos nunca
incluyen nombres de escenas, fuentes ni filtros, rutas, URLs, claves ni datos de tu emisión, tu
nombre de usuario o tu email, el nombre de tu GPU, IDs de hardware ni IPs. Trigglow y un proveedor
de IA indicado en la [Política de Privacidad](https://www.trigglow.com/legal/privacy) procesan tus
mensajes y los datos técnicos. No escribas contraseñas, claves de stream, URLs de overlay ni datos
de pago. El log de OBS solo va con un ticket si marcas su casilla, cosa que puedes hacer después de
verlo en **Ver qué se envía**. Las rutas con tu usuario de Windows, URLs, claves, emails e IPs van
ocultas; aun así, pueden seguir apareciendo nombres de escenas, fuentes, dispositivos de audio y
ventanas, así que revísalo ahí antes.

**¿Cuánto tiempo se guardan mis conversaciones con Trigi y cómo las borro?**
Las conversaciones con Trigi y los datos técnicos que elijas enviar se guardan hasta 12 meses y
después se borran. El historial que ves en OBS solo está en memoria y desaparece al cerrar el
diálogo (**Borrar conversación** solo limpia esa vista). Para borrarlas antes, en trigglow.com ve a
Ajustes → Privacidad y datos → Conversaciones con Trigi → **Eliminar mis conversaciones con Trigi**,
o pídeselo a soporte. Una conversación que convertiste en ticket se queda en el hilo de ese ticket,
como el resto del ticket.

**¿Qué conexiones de red hace el plugin?**
En cada arranque de OBS, una comprobación de tu inicio de sesión con api.trigglow.com (más una
renovación cuando toca) y una comprobación de la última release de este repositorio en
api.github.com. Todo lo demás solo ocurre cuando lo usas: iniciar sesión, cerrar sesión, preguntar a
Trigi y enviar un reporte. Todo por HTTPS, el inicio de sesión solo se envía a api.trigglow.com, y
el plugin solo abre sus propias páginas fijas de www.trigglow.com y GitHub. En modo stream tu
directo va de tu ordenador directamente a tu plataforma y a los destinos que hayas añadido; no hay
ningún servidor de Trigglow por medio.

**¿Necesito el plugin de Stream Deck de Trigglow?**
No. Los diez atajos de OBS del plugin (activar/desactivar, activar, desactivar, volver al directo,
+5 s, -5 s, borrar, tres presets) funcionan con la acción propia del Stream Deck "System: Hotkey".
El plugin de Stream Deck de Trigglow añade teclas que muestran el estado del delay, por
obs-websocket (vendor `trigglow-dynamic-delay`, que también puede usar cualquier herramienta de
obs-websocket). Ver `docs/STREAM_DECK-es.md`.

**¿Es un producto terminado?**
En Windows, sí: la **v1.0.0** es la primera versión estable, pensada para un uso real en directo,
con un alcance deliberadamente acotado (ver `docs/ROADMAP-es.md` para lo que viene después). Las
versiones de macOS y Linux siguen en alpha: compilan y pasan la CI, pero aún no se han usado en un
directo, y solo tienen el modo escena. El modo stream es nuevo en la 1.0.0 y aún no se ha usado en
un directo a una plataforma real (ver "¿Está probado el modo stream?" más arriba). Una zona de
riesgo — un crash de GPU observado bajo un diseño anterior — que se cree reducida pero todavía no
reconfirmada específicamente (`docs/SPEC-es.md` §7).
