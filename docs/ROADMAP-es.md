*[English version](ROADMAP.md)*

# Roadmap — Trigglow Dynamic Delay

## v0.2.0 (MVP — entregado)
- **Modo buffer** sin reconexión: el output de streaming no se toca nunca, en ningún momento, por
  ningún motivo — el plugin retrasa vídeo y audio acumulándolos en RAM del sistema y cambiando el
  Programa de OBS internamente entre la escena en directo, una escena de carga opcional y una
  escena wrapper delayed.
- Enable / Disable / Toggle Delay, con la escena en directo obligatoria y la escena de carga
  opcional.
- Calidad mínima seleccionable (480p/720p/1080p); el tiempo pedido se acorta en vez de bajar nunca
  de esa calidad, si el presupuesto de RAM no da para ambos.
- Estimación de ajuste de buffer en vivo y presupuesto de RAM detectado, mostrados en el dock y
  actualizados según el usuario ajusta segundos o calidad, con un aviso no bloqueante antes de
  Enable si la petición completa no cabe.
- Vídeo y audio delayed juntos, en sincronía.
- Buffer de RAM liberado automáticamente al pulsar Disable.
- Estado `Inactive` / `Filling` (con cuenta atrás en vivo) / `Active` / `Error` visible en un dock
  nativo de OBS.
- 3 hotkeys nativas de OBS (Toggle obligatoria, Enable/Disable opcionales), utilizables
  directamente desde Stream Deck vía su propia acción "System: Hotkey".
- Persistencia de settings por perfil de OBS.
- Manejo de errores sin crashear (sin escena en directo elegida, escena en directo que no resuelve
  a nada, etc.).
- Windows como plataforma prioritaria; macOS/Linux compilan en verde en CI pero todavía no se han
  probado en directo.

## v0.3.0 (entregado — 2026-08-27)
- **Compresión** real del buffer: cada slot del ring ahora se codifica en MJPEG (todo-intra) al
  capturarlo y se decodifica al reproducirlo, integrando FFmpeg para ambas direcciones
  (`obs_video_encoder_create` solo alimenta el propio pipeline de output de OBS, y libobs no
  expone ninguna API pública de decodificación — también se investigó y descartó una vía de
  compute shader de GPU para v0.2.0, ya que libobs no tiene API de compute/dispatch en su interfaz
  gráfica pública). Fallback automático a NV12 sin comprimir siempre que el codec no esté
  disponible o un frame falle.
- Los slots del ring reservan RAM según un tamaño presupuestado (comprimido) en vez del techo
  crudo completo, creciendo solo según lo que cada frame necesita. Medido en vivo a 30s@1080p:
  ~2,9GB con el buffer activo, frente a los ~6,3GB de antes de comprimir.
- Solo Windows + Linux por ahora — macOS no tiene ningún build estático de FFmpeg de confianza
  equivalente disponible y sigue funcionando exactamente igual que en v0.2.0 (sin comprimir).
- Sigue abierto: medir el ratio de compresión real contra gameplay real prolongado (el único dato
  de hoy vino de una escena de carga estática, no representativo); el crecimiento de RAM de un
  slot es permanente durante la sesión una vez ocurre (no baja hasta Disable()).
- `main` ahora es una rama protegida (PR obligatorio, incluso para el mantenedor); `develop` es
  donde ocurre el trabajo en curso.

## v0.4.0 (entregado — 2026-09-07)
- **Reportar un problema**: un diálogo nativo en el dock que crea un ticket de soporte real en
  trigglow.com (categoría `dynamic_delay`) y adjunta automáticamente el log actual de OBS — sin
  que el usuario tenga que buscarlo a mano. Nuevo `HttpsPostMultipartFile()` sobre el cliente
  WinHTTP ya existente (`src/win-http.cpp`) para la subida del adjunto.
- **Chequeo de actualizaciones dentro del plugin**: al cargar, compara la última release de
  GitHub de este repositorio contra la versión en ejecución (público, sin autenticación) y
  muestra un aviso en el dock si hay una más nueva — silencioso ante cualquier fallo o si ya está
  actualizado, nunca como un error.
- **Localización real**: todo el texto visible (dock, diálogo de reportar un problema, nombres de
  filtro, descripciones de hotkeys, mensajes de estado del buffer) pasa ahora por el sistema de
  idiomas propio de OBS (`obs_module_text()`) en vez de literales en español fijos en el código —
  `data/locale/en-US.ini`/`es-ES.ini` ahora están totalmente traducidos en vez de tener una sola
  clave `PluginName` cada uno. Antes, un usuario con OBS en cualquier idioma que no fuera español
  veía igualmente el plugin 100% en español.
- **Rediseño del dock**: secciones agrupadas en tarjetas, paleta de colores de estado semántica
  consistente, y controles de delay/calidad uno junto al otro.
- **RAM medida por primera vez, no asumida**: una línea de log `compression check`
  (`src/video-delay-filter.cpp`) reveló un ratio de compresión MJPEG real de ~11-15x sobre
  gameplay en vivo, muy por encima del 3x asumido de forma conservadora desde v0.3.0 —
  `kAssumedCompressionRatio` reajustado a 5.0 con esos datos, más la eliminación de una
  asignación de memoria por cada frame en la codificación. Medido en vivo a 30s/1080p60: la
  memoria total del proceso de OBS bajó de ~2.8GB a ~2.1GB.

## v0.5.0 (entregado — 2026-10-04)
- **Overlay "Delay Ns"** (insignia con logo + texto, al 40% de tamaño) sobre la salida retrasada
  mientras está Activo, con checkbox y selector de cuatro esquinas en el dock.
- **Un solo botón Activar/Desactivar** y un **dock responsivo** (área con scroll + filas que se
  apilan cuando es estrecho).
- **Delay de vídeo por reloj real.** Un log de producción mostró el ring por número de frames a
  48-90 slots/s con 60 fps (delay real ~20-38s frente a 30s exactos del audio). Ahora se captura
  un slot por celda de 1/fps y la reproducción elige por hora de captura; el audio también sigue
  timestamps.
- **La escena envoltorio contiene siempre la escena en directo elegida**, y los filtros que
  quedaron encendidos al cerrar OBS estando Activo se apagan al cargar.
- **Fix de empaquetado del instalador de Windows:**
- **El instalador de Windows nunca incluía `es-ES.ini`.** Encontrado probando en directo en una
  instalación de OBS configurada en español: el plugin parecía atascado en inglés sin importar el
  idioma de OBS. La sección `[Files]` de `installers/windows/trigglow-dynamic-delay-setup.iss` solo
  listaba `en-US.ini` — un descuido de empaquetado anterior a esta sesión, invisible desde que
  v0.4.0 convirtió `es-ES.ini` en una traducción real por primera vez (antes tenía una sola clave
  placeholder, así que un archivo faltante no cambiaba nada visible para el usuario).
  `obs_module_text()` cae de vuelta al idioma por defecto siempre que el `.ini` del idioma pedido no
  esté en disco, lo cual se ve idéntico a "este plugin no soporta español" desde el lado del
  usuario. Ahora incluye ambos. Es el siguiente release después de v0.4.0 (actualmente en
  producción) — lo bastante pequeño como para no esperar a las features no relacionadas de abajo.

## v0.6.0 (entregado — 2026-10-19)
Rendimiento, adaptación y el aspecto de Trigglow, además de Trigi, un inicio de sesión más seguro
y una revisión de seguridad. Se preparó un hotfix de rendimiento v0.5.1 que nunca se publicó; todo
lo que llevaba sale aquí. Lo primero que se reportó en directo: sobrecarga al codificar ("Encoding
overloaded") en cuanto se activaba el delay con un lienzo 2560x1440 y una salida 1920x1080 a 60 fps.
- **Buffer limitado a la resolución de salida de OBS.** El ring se dimensionaba a partir del lienzo,
  así que un lienzo 1440p emitido a 1080p se codificaba/decodificaba en MJPEG a 1440p en el hilo
  gráfico de OBS (14,3/17,4 ms p50/p95 por frame). La calidad mínima pasa a ser un suelo de RAM que
  nunca supera la salida. La aritmética de dimensionado está en `src/buffer-fit.*`, con tests; anchos
  múltiplos de 8 para la lectura en OpenGL. Ajuste oculto de soporte `buffer_output_cap`, y un aviso
  en el log para encoders por encima del buffer.
- **Compresión/descompresión en hilos propios, fuera del hilo gráfico de OBS — salida 1440p60
  soportada.** Uno o dos hilos de compresión, un hilo de descompresión que trabaja por adelantado, un
  hilo liberador de memoria, un ring de cabeceras de slot con máquina de estados y una arena por
  bloques. A 1440p60 el trabajo del plugin en el hilo gráfico pasó de ~14/17 ms a ~1,2–1,7/1,6–2,1 ms
  por frame (p50/p95, microbenchmark fuera de OBS). Interruptor oculto `codec_workers` para volver al
  códec en el hilo gráfico.
- **Degradación automática que nunca corta el delay**: más hilos de compresión, después peldaños de
  menos resolución hasta la calidad mínima, después la mitad de fps de captura; vuelve a subir con
  30 s de histéresis; aviso ámbar en el dock con qué se redujo y por qué. La calidad mínima llega
  hasta 1440p (2K) y 2160p (4K).
- **Menos RAM, nada pesado en el hilo gráfico**: sin relleno de ceros, arena por bloques (~1,9 GB →
  ~0,6–0,8 GB para 30 s a 1440p60 en simulación), memoria liberada en el hilo liberador, códecs y
  objetos de GPU liberados al desactivar, conversión I420 en la GPU, tablas de Huffman estándar, cada
  frame decodificado y subido una sola vez.
- **Un único delay en segundos enteros para vídeo y audio**, también cuando la RAM recorta el ring
  (antes hasta ~1 s de diferencia); intervalo de frame exacto a 59,94 fps; un reinicio del buffer se
  rellena detrás de la escena de carga en vez de mostrar negro; 0 s deja pasar el vídeo.
- **Los filtros de delay que quedaban encendidos al cerrar OBS estando Activo se apagan de verdad**,
  al arrancar y al cambiar de colección de escenas; corregidas la fuga de referencias del filtro y el
  destructor sin contexto gráfico; un archivo de ajustes ilegible se aparta como `.invalid`.
- **Telemetría `perf check` y `pipeline check`** cada 10 s en el log de OBS (sustituye a
  `compression check`).
- **Rediseño de la interfaz alineado con trigglow.com**: dock, diálogo de reporte y overlay del
  directo sobre el tema de OBS, Archivo y Geist Mono empaquetadas (OFL), iconos SVG, contraste WCAG
  AA y foco de teclado, errores de inicio de sesión visibles, los segundos de delay bloqueados
  mientras el delay está en marcha. El overlay es una sola imagen generada, igual en todos los
  sistemas, firmada "By Trigglow", que conserva la posición y el tamaño que le des.
- **Nombres "(auto)"** para la escena, los filtros y el overlay que gestiona el plugin, migrados
  automáticamente.
- **Trigi, el asistente de soporte con IA de Trigglow, en "Reportar un problema"** (Windows, con
  sesión iniciada): aviso de IA y privacidad en el primer uso, datos técnicos de OBS opcionales
  (solo cifras y estados, con vista previa), el log de OBS nunca va a la IA, cada estado de error con
  su siguiente paso, render seguro de las respuestas y la conversación convertida en ticket con un
  resumen.
- **Inicio de sesión v2**: un código que comparar en la página de trigglow.com con "No he sido yo",
  un inicio de sesión propio del plugin y con alcance limitado, cifrado con la API de protección de
  datos de Windows, renovado solo y revocado con Cerrar sesión (o desde las apps conectadas de
  trigglow.com), y un paso automático y único desde la v0.5.x. El inicio de sesión de la v0.5.x y
  anteriores se apaga cuando se publica esta versión, así que actualizar es obligatorio.
- **Tickets con la cuenta**, con asunto opcional y un pie con las versiones; el log de OBS redactado
  en memoria, con vista previa y adjuntado solo con consentimiento; las copias antiguas de `%TEMP%`
  se borran; los invitados pasan al formulario web cuando trigglow.com pide una comprobación humana.
- **Endurecimiento**: un único camino WinHTTP (sin redirecciones, TLS 1.2+, timeouts, tope de
  tamaño, el token solo para api.trigglow.com), solo páginas fijas, sin códigos, URLs ni rutas con
  el usuario en el log de OBS, el aviso de actualización limitado a las releases de este repo, sin
  rutas de compilación en la DLL, y todas las llamadas de red se pueden cancelar, así que OBS se
  cierra limpio aunque Trigi esté respondiendo.
- **Build/CI**: FFmpeg fijado a un build de fin de mes de BtbN, 19 suites de tests unitarios en CI
  en todas las plataformas, un job de ThreadSanitizer, `Qt6::Svg`.

## v0.7.0 (entregado — 2026-10-20)
Una versión de optimización: el buffer del delay ocupa más o menos la mitad de RAM en Windows. La
v0.6.1 prevista (cliente HTTP para macOS y Linux) no se publicó; va a continuación.
- **Códec de almacenamiento por hardware en Windows.** El buffer se guarda como HEVC (H.264 en una
  GPU sin HEVC) con el encoder de la GPU (NVENC, AMF, Quick Sync) y se decodifica con D3D11VA, en
  vez de MJPEG: a 1440p60 pasa de ~675 MB a ~325 MB con 30 s y de ~1,2 GB a ~520 MB con 60 s, el
  PSNR-Y de los frames guardados sube de ~40 a ~45 dB y la CPU del códec baja de ~0,8 a ~0,2–0,3
  núcleos (fuera de OBS, AMF de AMD). MJPEG se queda como puente mientras se prepara un encoder y
  como alternativa. Sin DLL nuevas.
- **Un ring que admite códecs inter-frame**: códec, keyframe, cadena y seq por paquete, un GOP de
  margen, decode desde un keyframe con puesta al día, varios códecs en un mismo ring y una interfaz
  de encoder asíncrona.
- **Protección frente a "Encoding overloaded"** cuando el motor de la GPU se comparte con los
  encoders de OBS: un peldaño menos ante un aviso temprano de latencia (veto de 5 minutos), vuelta a
  MJPEG al momento ante un frame que OBS se salta por retraso de codificación, un error de un
  encoder de OBS o cualquier error del hardware, y la sesión NVENC cedida a una salida que arranca.
- **Dimensionado**: clase de almacenamiento fija por ciclo (12x cuando el códec por hardware ya ha
  funcionado en la sesión, 2,4 veces los frames cuando falta RAM; el 5x de MJPEG en el primer ciclo
  de la sesión), el buffer limitado a la resolución de los encoders del stream cuando todos
  reescalan por debajo de la salida, bloques de arena de 8 MiB con uno de repuesto y como mucho dos
  hilos de slice por contexto MJPEG.
- **Soporte**: ajuste oculto `buffer_codec` (`"auto"` o `"mjpeg"`), `codec=` en `pipeline check`,
  una línea `storage codec check` y `PipelineHealth` v2. Sin UI, textos ni avisos de RAM nuevos.
- **Seguridad y licencias**: sin Quick Sync en un OBS elevado, `get_pipeline_state` limitado al
  tamaño de la estructura de quien llama, `LICENSE-ffmpeg.txt` incluido y una comprobación LGPL de
  la build de FFmpeg en CI.
- **Tests**: 23 suites en CI (estrés y pruebas adversariales con un códec inter-frame falso, la
  política del códec), más el códec por hardware sobre el FFmpeg real en builds locales de Windows.

## v0.7.1 – v0.7.3 (entregado — del 2026-10-20 al 2026-11-11)
- **0.7.1**: las ubicaciones de plugins de OBS 33 (`C:\ProgramData\obs-studio\plugins` en Windows),
  una comprobación de la versión mínima de OBS (31.1.0) y paquetes de Linux para Ubuntu, Fedora y
  Arch.
- **0.7.2**: el núcleo del plugin para Trigglow Studio (`-DTRIGGLOW_CORE_ONLY=ON`) y el aviso de
  actualizaciones en la nueva dirección del repositorio.
- **0.7.3**: también se retrasan las fuentes de audio globales y el audio dentro de escenas anidadas
  o grupos.

Detalles en `CHANGELOG-es.md`.

## v1.0.0 (2026-11-11) — fuera de beta
La primera versión estable en Windows. macOS y Linux siguen en alpha (se compilan y se prueban en
la CI, pero aún no se han usado en un directo).
- **Modo stream (Windows)**: el delay a nivel de output que estaba en "Más adelante" desde el
  2026-09-08. El plugin apunta la salida de emisión de OBS a un relay en `127.0.0.1` y envía a la
  plataforma el stream ya codificado, pasado el delay: sin decodificar, sin una segunda
  codificación, RAM = bitrate × segundos guardados. De 0 a 600 s cambiables en directo sin
  reconectar, todas las escenas con delay, grabación / Replay Buffer / cámara virtual en directo
  y envío de lo pendiente al parar. Detalles:
  `docs/STREAM_MODE-es.md`.
- **Borrar lo que aún no se ha emitido**, **delay por escena** (activado con sus propios segundos,
  o quitado, por escena), **tres presets** con atajos, la **insignia de delay** en el modo stream
  (hasta 600 s) y **comprobaciones de ajustes** (intervalo de fotogramas clave, el Retardo de la
  transmisión propio de OBS). Solo en el modo stream, salvo los presets.
- **Atajos**: Volver al directo, Delay +5 s / -5 s, Borrar y Preset 1–3, junto a los tres de
  siempre (mismos nombres y teclas).
- **Control por obs-websocket**: el vendor `trigglow-dynamic-delay` con `GetStatus`,
  `EnableDelay`, `DisableDelay`, `ToggleDelay`, `SkipToLive`, `SetDelay`, `AdjustDelay`,
  `DumpDelay`, `ApplyPreset` y el evento `StatusChanged`, para el plugin de Stream Deck de
  Trigglow.
- **Inicio de sesión en macOS y Linux** (el cliente HTTP previsto como v0.6.1): libcurl con las
  reglas de WinHTTP (HTTPS solo por el 443, TLS 1.2+, sin redirecciones, cuerpo con tope) y el
  inicio de sesión en un archivo que solo puede leer tu usuario (0600). Con él, activar el delay,
  Trigi y "Reportar un problema" funcionan también ahí.
- **El nuevo diseño de Trigglow** en el dock, una placa de overlay nueva, el nuevo logo de Dynamic
  Delay, el vídeo de carga "Poniendo delay" por defecto y sin la etiqueta "Beta".
- **Traducciones**: portugués (Brasil) y chino (simplificado), además de inglés y español.
- **El modo escena cambia el delay en directo y llega a 600 s**: al bajarlo, los espectadores saltan
  hacia delante al momento; al subirlo, la imagen se congela (o sale la escena de carga por encima de
  3 s) solo durante los segundos de más; nada se rellena desde cero. Los rings de audio cuentan en el
  presupuesto de RAM (`docs/SPEC.md` §3.3, §3.10). Cambiar de escena con delay sigue siendo cosa del
  modo stream (§3.9); el dock lo dice.

## Siguiente
- **Probar el modo stream en directo en plataformas reales** antes de darlo por verificado: Twitch
  (RTMP), Kick (RTMPS), YouTube (HEVC por enhanced RTMP), con cambios de delay, un borrado y un
  OBS parado a mitad de delay, mirando el reproductor de la plataforma. Por ahora solo tiene tests
  unitarios y pruebas con el arnés local del relay (`docs/STREAM_MODE-es.md`, "Estado de la
  verificación").
- **Modo stream en macOS y Linux.** El FFmpeg del plugin ahí se compila sin avformat (y sin una
  pila TLS para RTMPS), así que esas versiones solo tienen el modo escena. Hace falta avformat con
  RTMP y TLS en el build de FFmpeg de Linux y un FFmpeg para macOS, y luego el mismo relay.
- **Enhanced Broadcasting, SRT, RIST y WHIP en el modo stream.** Hoy esos directos pasan al modo
- **Modo escena siguiendo los cambios de escena: un único ring compartido por todas las escenas**
  (evaluado el 2026-11-11, `docs/SPEC.md` §3.9). Un filtro de delay en cada escena de primer nivel,
  solo la escena del Programa (y las fuentes de una transición en curso) capturando en un único ring
  compartido y dibujando desde él, y filtros de audio que escriben silencio mientras su fuente no
  está activa. Hace falta donde no puede ir el modo stream (Enhanced Broadcasting, SRT, WHIP, y
  macOS y Linux hasta que llegue su relay).
  escena: Enhanced Broadcasting son varias pistas en una salida multipista, y SRT, RIST y WHIP no
  son RTMP, así que el relay necesitaría otro emisor para cada uno.
- **Firma de código** (emitida por una CA, Windows y macOS): la solución de verdad para el aviso de
  SmartScreen y el falso positivo de Defender de
  [#10](https://github.com/TrigglowStudios/trigglow-dynamic-delay/issues/10). La solicitud al
  programa gratuito para proyectos de código abierto de
  [SignPath Foundation](https://signpath.org/apply) está en curso. Hasta entonces, el instalador de
  Windows se firma con un certificado autofirmado (`.github/workflows/build-project.yaml`, "Sign
  Windows Installer (self-signed)"), que solo demuestra que lo produjo la CI: no quita el aviso de
  "Editor desconocido" ni el falso positivo. Ese paso se queda como alternativa cuando llegue un
  certificado real.
- **El plugin de Stream Deck de Trigglow con teclas de Dynamic Delay** (activar/desactivar, volver
  al directo, presets, +/-, el estado del delay en la tecla) sobre el vendor de obs-websocket; el
  camino de los atajos nativos se queda como opción sin dependencias.
- **Verificación en directo en macOS/Linux** del modo escena y del nuevo inicio de sesión: los dos
  compilan en verde en CI; esto trata de ejecutarlos y confirmarlos en esas plataformas.
- Modo escena:
  - **Validar el códec de almacenamiento por hardware en NVIDIA dentro de un OBS real**: NVENC
    compartido con el encoder del stream (y Enhanced Broadcasting) sin ningún frame saltado por
    retraso de codificación, NVDEC, la VRAM que ocupa y los bytes por frame frente a las cifras de
    AMD. También Quick Sync, en hardware Intel.
  - **Copia cero en la GPU.** Codificar directamente desde la textura de OBS y decodificar a una
    textura D3D11, para que desaparezcan las copias de cada frame en la CPU: según nuestra
    estimación, otros 50–120 MB menos y menos trabajo en el hilo gráfico de OBS. Tiene más riesgo
    (compartir el dispositivo D3D11 de OBS con FFmpeg entre hilos).
  - **Deduplicar las capturas por la hora de frame de OBS** (`obs_get_video_frame_time()`) en vez
    de por una celda de reloj por intervalo de frame, para que el jitter del render no meta dos
    renders en una celda y se pierda un frame real.
  - **Un render target más pequeño para el keep-alive** mientras se llena el buffer (hoy, una
    textura del tamaño del lienzo cuyo contenido nunca se usa: 14,7 MB a 1440p).
  - Verificar la salida 1440p60 y la degradación dentro de una sesión real de OBS (las cifras salen
    de benchmarks fuera de OBS en CPU y GPU de AMD), y la alineación A/V con un reloj y una palmada.
  - Revisar los ratios de compresión supuestos (5x para MJPEG, 12x para el códec por hardware) con
    las cifras de `ratio` y de la arena de la línea `pipeline check` de más sesiones y juegos.

## Replay Buffer en directo real (resuelto por el modo stream en Windows; el modo escena no cambia)
El modo stream nunca toca el Programa de OBS, así que el Replay Buffer, la grabación y la cámara
virtual mantienen la imagen en directo mientras el stream va con delay. Lo que sigue es el estudio
del 2026-09-08 para el **modo escena**, todavía pausado: ahí los filtros de delay están en las
propias fuentes de la escena en directo, así que cualquier otra salida ve la imagen con delay
mientras el delay está activo.

- **Idea de primera pasada, y por qué está mal.** Esta entrada originalmente proponía el patrón
  "Branch Output" de `exeldro/obs-source-record` tal cual: un `obs_view` + encoder +
  output `"replay_buffer"` dedicados (todo API pública de OBS — `obs_view_create`/`obs_view_add2`,
  `obs_video_encoder_create`, `obs_output_create("replay_buffer", ...)`, leído de la implementación
  real de Source Record) apuntando a la escena en directo real, corriendo en paralelo al output de
  streaming que sigue mostrando la escena wrapper delayed. Eso está mal específicamente para este
  plugin: los filtros son una propiedad del propio objeto fuente, no de quien lo renderiza/lee —
  verificado directamente contra `libobs/obs-source.c` (`obs_source_output_audio()` ejecuta toda la
  cadena de filtros vía `filter_async_audio()` *antes* de que `source_signal_audio_data()` llegue a
  CUALQUIER callback de captura o consumidor; `obs_source_default_render()` funciona igual para
  vídeo). Como `VideoDelayFilter`/`AudioDelayFilter` están adjuntos directamente a los objetos de la
  escena en directo/fuentes hoja (anidar nunca los duplica — ver el comentario de cabecera de
  `EnsureBufferWrapperScene`), un `obs_view` o encoder nuevo apuntando a "la escena en directo real"
  vería la MISMA salida ya delayed que ve el lado de streaming, justo en el momento en que el modo
  buffer pasa a Active — exactamente cuando un replay en directo real más importaría. Source Record
  en sí nunca se topa con esto, porque su objetivo es capturar una fuente *con* los filtros que
  tenga.
- **Lo que arreglarlo de verdad requeriría.** `VideoDelayFilter`/`AudioDelayFilter` ya capturan una
  copia sin retrasar de cada frame/buffer de audio en cada tick, antes de escribirla en su ring de
  delay — el fix correcto es que TAMBIÉN empujen esa misma copia sin retrasar a un `video_output_t`/
  `audio_output_t` dedicado (`video_output_open()`/`video_output_lock_frame()`, igual para audio) que
  un encoder+output de replay buffer nuevo lea, en vez de construir algo aislado al lado. Eso implica
  cambiar los internos de los dos archivos de los que depende la garantía central de delay de este
  plugin — no una feature nueva autocontenida junto a ellos.
- **Por qué pausado en vez de intentado ahora.** La propia historia de `docs/SPEC-es.md` (las entradas
  v0.3.1/v0.3.2 de §3.3, el crash de GPU sin resolver de §7) muestra que este mismo par de archivos
  ya ha sacado a la luz repetidamente bugs sutiles, solo encontrados en directo, incluso con cambios
  acotados y específicos del propio delay. Con el modo stream cubriendo esta necesidad en Windows,
  esto solo importa para macOS y Linux hasta que el modo stream llegue ahí.

## Fuera de alcance por ahora
- **Seguir los cambios de escena del streamer con el delay activo en el modo escena, sin un hueco
  de recarga** (investigado y descartado, 2026-09-08). Los filtros del modo escena solo guardan los
  frames de UNA escena, así que al cambiar a otra habría que volver a llenar desde cero, lo que
  rompe la promesa de "nunca corta"; guardar todas las escenas por adelantado multiplicaría el coste
  de RAM y GPU por el número de escenas. El modo stream lo resuelve (retrasa el stream ya compuesto
  y codificado, así que un cambio de escena es parte de una única señal continua), y ahí el delay
  por escena cubre "un delay distinto por escena".
- **Retrasar todo el programa en el modo escena con una vista o un lienzo privado** (evaluado el
  2026-11-11 para la v1.0.0, `docs/SPEC.md` §3.9). No es seguro: el frontend de OBS cambia
  directamente el canal de salida 0 en las transiciones rápidas, las transiciones propias de cada
  escena y la barra T, sin avisar antes (frames sin delay en cada cambio); sobrescribir la textura
  principal en un callback de render no cubre las pistas reescaladas de Enhanced Broadcasting; y
  libobs mezcla el audio desde el árbol del Programa tal como está ahora, así que los filtros de
  audio por fuente reproducirían el audio con delay de la escena a la que se cambia, filtrando lo que
  escondía una escena de "ahora vuelvo". El camino es el ring compartido de Siguiente.
- Panel web externo como forma principal de control (decisión de producto: todo vive nativo en
  OBS).
