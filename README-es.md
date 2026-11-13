*[English version](README.md)*

# Trigglow Dynamic Delay for OBS

Plugin nativo de OBS Studio que retrasa lo que ven tus espectadores un número configurable de
segundos, **vídeo y audio juntos**, desde el dock, un atajo nativo de OBS, obs-websocket o un
Stream Deck, **sin que tu directo se reconecte nunca.** Sin app externa, sin panel web, sin proceso
aparte: todo vive dentro del propio proceso de OBS, y tu stream va directamente de tu ordenador a tu
plataforma.

**Estado: 1.0.0. Estable en Windows. Las versiones de macOS y Linux están en alpha: compilan y
pasan la CI, pero aún no se han usado en un directo.**

## A qué se aplica el delay

El dock tiene un solo panel y una sola opción para esto, en Opciones > Avanzado.

**A todo OBS (por defecto).** Como en la 0.7: el plugin guarda en RAM tu escena en directo ya
renderizada (comprimida con el HEVC por hardware de tu GPU en Windows o en MJPEG, sin comprimir en
macOS) y la reproduce de 1 a 600 s más tarde en una escena auxiliar. Mientras se acumula el delay se
muestra tu escena de carga (si no eliges ninguna, un vídeo de Trigglow "Poniendo delay"); después el
Programa muestra la escena con delay, así que **tu directo y tu grabación van los dos con delay**.
Los segundos cambian en directo y el output de streaming no se toca nunca, así que funciona con
cualquier salida y cualquier servicio. Lo que va con delay son tus escenas con delay (hasta 8, un
solo búfer para todas): cambia entre ellas desde el dock, los atajos o el Stream Deck con el delay
activo; cualquier otra escena se ve en directo. Gasta más RAM y CPU (tabla de abajo). Detalles: `docs/SPEC-es.md`.

**Solo a la emisión (avanzado: "Aplicar el delay solo a la emisión (la grabación y la vista previa
no se retrasan)", Windows).** El modo stream, nuevo en la 1.0.0: el plugin guarda el stream que OBS
ya ha codificado y lo envía a tu plataforma más tarde. No se decodifica ni se vuelve a codificar
nada, así que casi no gasta CPU, y tú eliges dónde esperan los segundos retenidos: en RAM (lo
predeterminado en un SSD, sin escribir nada en disco) o en un archivo temporal cifrado en tu disco
(lo predeterminado en un disco duro, unos 11-16 MB de RAM con cualquier delay hasta 600 s, medido a
6 Mbps). A cambio, el delay solo lo recibe tu plataforma: la grabación, el Replay Buffer, la cámara
virtual y la vista previa de OBS siguen en directo, y no hay escena de carga. Con él:

- De 1 a 600 s de delay, que activas, quitas, subes o bajas en directo, sin reconectar. Al
  activarlo, los espectadores vuelven a ver los últimos segundos, nunca nada más nuevo. Subirlo es
  al instante en cuanto el directo lleva ese tiempo (con Memoria, hasta tu preset o regla por
  escena más alto).
- Todas las escenas van con delay, así que puedes cambiar de escena con el delay activo.
- "Borrar lo que aún no se ha emitido": los segundos que aún esperan no se envían nunca, el stream
  salta al directo y el delay vuelve solo.
- Delay por escena (activado en tu escena de juego, quitado en la de charla) y comprobaciones de los
  ajustes de OBS que lo empeoran (el intervalo de fotogramas clave, el Retardo de la transmisión
  propio de OBS).
- Al parar el directo, se siguen enviando los segundos que los espectadores aún no han visto
  (opcional).

Necesita la salida RTMP o RTMPS propia de OBS y funciona con cualquier servicio que la use (Twitch,
YouTube, Kick, TikTok, servidores personalizados). Con Enhanced Broadcasting, SRT, RIST o WHIP el
dock dice que ese directo no puede usarlo: desmarca la opción para retrasar todo OBS. Detalles:
`docs/STREAM_MODE-es.md`.

Las dos tienen la insignia de delay para los espectadores, tres presets, atajos, obs-websocket y
Stream Deck.

## Cómo funciona

```
A todo OBS (por defecto, modo escena, todas las plataformas)

    escenas con delay -> filtros de delay de vídeo/audio -> ring en RAM (HEVC o MJPEG) -> escena auxiliar
                    -> Programa de OBS -> encoders de OBS -> tu servicio y tu grabación
                       (el output no se toca)

Solo a la emisión (modo stream, avanzado)

  escenas de OBS -> encoder de OBS -> salida RTMP de OBS --rtmp://127.0.0.1:<puerto>--> relay (en OBS)
                                            |                                               |
                     grabación, Replay Buffer, cámara virtual               historial de paquetes en RAM
                     (en directo, sin tocar)                                (sin decodificar ni codificar)
                                                                                            |
                                                              el emisor envía el paquete p en
                                                              llegada(p) + delay, por RTMP/RTMPS
                                                              -> tu servicio de OBS (Twitch, YouTube...)
```

- **`StreamDelayController`** (`src/stream-delay-controller.*`) escucha los eventos del frontend de
  OBS. Si has elegido "solo a la emisión", cuando un directo va a empezar y es apto (el `rtmp_output` de OBS, RTMP o RTMPS, sin Enhanced
  Broadcasting), lee el servidor y la clave de tu servicio, arranca el relay y apunta **esa salida**
  al relay con un servicio privado. Tus ajustes de emisión guardados no cambian nunca.
- **`StreamRelay`** (`src/stream-relay.*`, avformat de FFmpeg, solo Windows) escucha solo en
  `127.0.0.1`, guarda los paquetes FLV según llegan, sin decodificarlos, y lleva el emisor
  hacia tu servicio de OBS, con su conexión, sus reintentos y su estado.
- **`PacketHistory` / `SinkScheduler`** (`src/packet-delay-core.*`, puros y con tests) deciden qué
  envía el emisor: un cambio de delay empalma en el keyframe adecuado más cercano y reescribe los
  timestamps, así que la plataforma ve un único stream continuo.
- **`BufferModeController`** (`src/buffer-mode-controller.*`) lleva el modo escena: los filtros de
  delay, el ring buffer, la escena auxiliar, la cuenta atrás de llenado y la escalera de
  degradación.
- **`DelayRouter`** (`src/delay-router.hpp`, `src/delay-targets.*`) es la única entrada para el
  botón del dock, los atajos y obs-websocket: cada orden va al modo elegido en el dock
  (`src/apply-to.*`), y solo a él.
- El **dock** (`src/settings-ui.*`, `src/settings-ui-stream.cpp`) es un solo panel: el estado, un
  solo botón "Activar delay", el delay, las escenas y las opciones plegadas, donde la casilla
  avanzada cambia a solo a la emisión.
- **Inicio de sesión** (`src/auth-*`, `src/api-client.*`, `src/win-http.*`, `src/token-store.*`): la
  cuenta gratuita que necesitas para activar el delay, por WinHTTP en Windows y libcurl en macOS y
  Linux.

El mapa completo de archivos está en [Estructura del proyecto](#estructura-del-proyecto).

## Versiones anteriores

La 1.0.0 es la primera versión estable; todo lo anterior era Early Access. En resumen: la 0.3
comprimió el buffer de RAM (MJPEG), la 0.3.3 añadió la puerta de cuenta gratuita, la 0.4 trajo
"Reportar un problema", el aviso de actualizaciones y las traducciones, la 0.5 la insignia de delay,
la 0.6 el soporte de 1440p60, la escalera de calidad adaptativa, Trigi y el nuevo inicio de sesión,
y la 0.7 el HEVC por hardware de la GPU para el buffer en Windows (más o menos la mitad de RAM) y
las ubicaciones de plugins de OBS 33. Todos los cambios, con cifras: `CHANGELOG-es.md`.

Antes de cambiar el modo escena, lee `docs/SPEC-es.md` (cómo funciona el modo buffer, y por qué se
probó y se descartó cambiar en directo el delay propio de OBS) y `docs/BUILD_VALIDATION.md` (qué se
verificó y qué no). Para el modo stream, `docs/STREAM_MODE-es.md`.

## Requisitos de uso

**OBS Studio 31.1.0 o posterior**, en todas las plataformas. El plugin se compila contra las
fuentes de OBS 31.1.1 (ver `buildspec.json`; desde OBS 32.0, OBS rechaza un plugin compilado contra
un OBS *más nuevo* que el que está corriendo), y desde la v0.7.1 se niega a cargar en un OBS más
antiguo, con `requires OBS Studio 31.1.0 or newer, running X` en el log de OBS; el instalador de
Windows también lo comprueba. Probado en directo hasta OBS **32.2.2** en Windows. OBS 33 (en
release candidate) queda soportado por las ubicaciones de instalación de la v0.7.1, pero aún no se
ha probado en directo. El mínimo se define en un único sitio, el `obsCompat` de `buildspec.json`.

| Plugin | OBS mínimo | Probado hasta | Win | macOS | Linux |
|---|---|---|---|---|---|
| 1.0.0 | 31.1.0 | 32.2.2 | Instalador, `.zip` (ubicación de OBS 33) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.2 | 31.1.0 | 32.2.2 | Instalador, `.zip` (ubicación de OBS 33) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.1 | 31.1.0 | 32.2.2 | Instalador, `.zip` (ubicación de OBS 33) | `.pkg` | `.deb`, `.rpm`, `.pkg.tar.zst`, `.tar.xz` |
| 0.7.0 | 31.1.0 | 32.2.2 | Instalador, `.zip` ("Legacy" en OBS 33) | `.pkg` | `.deb`, solo Ubuntu 24.04 con OBS 32.2 |

Linux: un único build x86_64 para Ubuntu 24.04 LTS (con el OBS del PPA de OBS), Ubuntu 26.04 LTS y
26.10, Fedora, Arch y sus derivadas (`.deb`, `.rpm`, `.pkg.tar.zst`), y un `.tar.xz` por usuario
para el resto, con Qt 6.4 o posterior. Debian 13 trae OBS 30.2, por debajo del mínimo: el `.deb` no
se instala hasta que haya un OBS 31.1 o posterior. El OBS de Flatpak no está soportado. Detalles en
`docs/INSTALL_GUIDE-es.md`.

**Resolución y fps.** Cualquier lienzo: el buffer del delay sigue la resolución de **salida** de OBS
(`Ajustes → Vídeo → Resolución de salida (escalada)`) y nunca la supera, así que un lienzo mayor que
la salida no le cuesta nada extra al plugin. Las salidas 1080p y **1440p hasta 60 fps** están
soportadas desde la v0.6.0 (cifras en `CHANGELOG-es.md`, medidas fuera de OBS en una CPU de 8
núcleos/16 hilos). En un equipo que no llega, el buffer baja a menos resolución (nunca por debajo de
la calidad mínima que elijas) y, como último paso, a la mitad de fps de captura, en vez de
sobrecargar OBS o cortar el delay; mientras tanto el dock muestra un aviso ámbar.

**Memoria solo a la emisión (modo stream).** Tú eliges dónde vive el búfer del delay (dock: Opciones > Búfer del
delay):

- **Memoria:** todo se queda en RAM y no se escribe nada en disco: tu bitrate por el delay, o por tu
  preset o regla por escena más alto si es mayor (45 MB por minuto a 6 Mbps). Subir el delay es al
  instante hasta ahí.
- **Disco:** en RAM solo quedan los segundos más recientes y lo que está a punto de enviarse. El
  plugin guarda los últimos 600 s (así cualquier subida es al instante) en un archivo temporal
  cifrado en la carpeta temporal del sistema (la clave solo existe en la memoria de OBS), que se
  borra al terminar el directo. Todo tu directo se escribe en disco mientras emites: unos 2,8 GB por
  hora a 6 Mbps, 225-600 MB en disco a la vez (tu bitrate por 600 s).
- **Automático** (lo predeterminado): Memoria si tu carpeta temporal está en un SSD, para ahorrarle
  horas de escrituras constantes, y Disco en un disco duro. Si no se puede saber el tipo de unidad,
  Memoria. El dock muestra lo que detectó, por ejemplo "Automático (Memoria, SSD detectado)", y
  siempre puedes elegir la otra opción. Un cambio se aplica a tu próximo directo.

La RAM que añade, medida a 6,2 Mbps con el relay por sí solo (detalles en `docs/STREAM_MODE-es.md`):

| Delay | Memoria | Disco: RAM | Disco: en disco |
|---|---|---|---|
| 10 s | unos 50 MB* | 13 MB | 440 MB |
| 120 s | 96 MB | 15 MB | 440 MB |
| 600 s | 454 MB | 13 MB | 440 MB |

\* Con los presets por defecto (10, 30 y 60 s) Memoria guarda 60 s; calculado a partir del bitrate,
no medido. Si Disco no puede usar el disco (lleno, con fallos o demasiado lento), el delay sigue en
RAM como con Memoria. El dock muestra la RAM y el disco en uso y el bitrate mientras emites.

**Memoria con todo OBS (modo escena, por defecto).** El presupuesto de RAM del buffer es la mitad de la RAM del sistema,
entre 1 GB y 24 GB (en macOS y Linux, donde todavía no se detecta la RAM, 2 GB fijos). Lo que ocupa de verdad depende
de lo que haya en pantalla y de cómo se guarde. Medido fuera de OBS con contenido tipo gameplay a
60 fps (solo el buffer; el audio suma unos 11 MB por fuente con sonido para 30 s):

| Salida | 30 s, HEVC por hardware (Windows, v0.7.0) | 30 s, MJPEG | 60 s, HEVC por hardware | 60 s, MJPEG |
|---|---|---|---|---|
| 1080p60 | ~190 MB | ~400 MB | ~290 MB | ~750 MB |
| 1440p60 | ~325 MB | ~640 MB | ~520 MB | ~1200 MB |
| 2160p60 | ~590 MB | ~1250 MB | ~920 MB | ~2300 MB |

Las cifras por hardware son de una GPU AMD (AMF); NVENC de NVIDIA aún no está medido. MJPEG es lo
que usan Linux, Windows sin encoder de hardware y el ajuste de soporte `buffer_codec`. Si los
segundos que pides no caben a tu calidad mínima, se guardan menos segundos (el dock te lo dice antes
de activarlo). La compresión necesita FFmpeg, que va incluido en Windows y Linux; en macOS el buffer
se guarda sin comprimir. Detalles en `docs/FAQ-es.md`.

**Una cuenta gratuita de trigglow.com.** Activar el delay necesita que inicies sesión una vez desde
el dock (en tu navegador; el plugin sigue siendo 100% gratis), en los dos modos. Desde la 1.0.0 el
inicio de sesión funciona también en macOS y Linux (con la libcurl del sistema); antes era solo de
Windows. Ver `docs/ACCOUNT_GATE-es.md`.

**Para solo a la emisión (modo stream):** Windows, macOS o Linux (en alpha en macOS y Linux), la salida RTMP o RTMPS propia de OBS (los ajustes de "Emisión" de
siempre, con cualquier servicio RTMP o un servidor rtmp:// o rtmps:// personalizado) y Enhanced
Broadcasting desactivado. Se recomienda un intervalo de fotogramas clave de 2 s
(`Ajustes → Salida`): el delay se mueve en pasos de ese tamaño.

## Uso de la red y privacidad

- **En cada arranque de OBS**: una comprobación de tu inicio de sesión con `api.trigglow.com` (más
  una renovación cuando toca) y una comprobación de la última release de este repositorio en
  `api.github.com`. Nada más se ejecuta en segundo plano.
- **Solo cuando lo usas**: iniciar y cerrar sesión, preguntar a Trigi y enviar un reporte, todo a
  `api.trigglow.com`. El plugin solo abre sus propias páginas fijas de www.trigglow.com y este
  repositorio en GitHub, en tu navegador.
- **Cómo**: solo HTTPS, sin seguir redirecciones, el inicio de sesión solo se envía a
  `api.trigglow.com`, se guarda cifrado para tu usuario de Windows (en macOS y Linux, en un archivo
  que solo puede leer tu usuario, modo 0600) y nunca se escribe en el log de OBS, igual que los
  códigos de inicio de sesión, los IDs y lo que escribes a Trigi.
- **Solo a la emisión** (modo stream) envía tu stream directamente de tu ordenador al servicio que tengas en OBS: el
  relay solo escucha en `127.0.0.1`, lee tu servidor y tu clave de OBS al empezar el directo sin
  guardarlos en ningún otro sitio, y no hay ningún servidor de Trigglow por medio. Los segundos más
  antiguos del directo esperan en tu ordenador en un archivo temporal cifrado (su clave solo existe
  en la memoria de OBS), que se borra al terminar el directo.
- **Trigi** recibe lo que escribes y, solo si lo marcas, un pequeño conjunto de cifras y estados
  técnicos de OBS (nunca nombres, rutas, URLs ni claves), que ves antes de enviarlo. **El log de
  OBS nunca se envía a la IA**; solo se adjunta a un ticket si marcas su casilla, cosa que puedes
  hacer después de verlo en **Ver qué se envía**. Tu usuario de Windows, URLs, claves, emails e IPs
  van ocultos, pero pueden seguir apareciendo nombres de escenas, fuentes, dispositivos de audio y
  ventanas. Las conversaciones se guardan hasta 12 meses y puedes borrarlas antes en trigglow.com.
  Detalles en `docs/FAQ-es.md` y en la
  [Política de Privacidad](https://www.trigglow.com/legal/privacy).

## Requisitos de compilación

Basado en la plantilla oficial [`obsproject/obs-plugintemplate`](https://github.com/obsproject/obs-plugintemplate),
sin modificar su sistema de build (`cmake/`, `CMakePresets.json`, `.github/`) salvo para activar
`ENABLE_FRONTEND_API` y `ENABLE_QT` y registrar los archivos fuente nuevos.

| Plataforma | Herramientas |
|---|---|
| Windows | Visual Studio 17 2022, CMake ≥ 3.30.5 |
| macOS | Xcode 16.0, CMake ≥ 3.30.5 |
| Linux (Ubuntu 24.04) | CMake ≥ 3.28.3, Ninja, GCC 13, `nasm`, `jq`, Qt 6.4, libcurl (`libcurl4-openssl-dev`): `.github/scripts/linux/install-build-deps.sh` |

macOS enlaza la libcurl del sistema (la 7.79 de macOS 12 está contemplada). **Windows es la
plataforma prioritaria para la 1.0.0**: macOS y Linux compilan en verde en CI con el propio sistema
de build de la plantilla, pero no se han probado en directo en esta entrega, y el inicio de sesión
con libcurl lo ha compilado la CI pero aún no se ha ejecutado en un Mac o un Linux reales.

## Compilar (Windows)

```powershell
git clone <tu-repo>/obs-trigglow-dynamic-delay.git
cd obs-trigglow-dynamic-delay
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

La primera configuración descargará automáticamente las fuentes de OBS y las dependencias
pre-compiladas (`obs-deps`, Qt6) que declara `buildspec.json` — no necesitas instalar OBS ni Qt tú
mismo por separado. Requiere conexión a internet la primera vez.

## Compilar (macOS / Linux)

```bash
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
```

En Linux el plugin enlaza su propio FFmpeg estático y compila libobs desde las fuentes de OBS
fijadas en el primer configure, igual que el CI en un contenedor `ubuntu:24.04`:

```bash
sudo .github/scripts/linux/install-build-deps.sh   # paquetes de Ubuntu 24.04
.github/scripts/build-ffmpeg.sh                   # FFmpeg de buildspec.json, LGPL, MJPEG y relay
.github/scripts/linux/build.sh RelWithDebInfo      # deja el resultado en release/stage/usr
.github/scripts/linux/package.sh                   # .deb, .rpm, .pkg.tar.zst, .tar.xz
```

## Instalar el plugin

**Windows: usa el instalador.** Descarga `obs-trigglow-dynamic-delay-1.0.0-windows-x64-setup.exe`
desde la [última release](https://github.com/TrigglowStudios/trigglow-dynamic-delay/releases/latest) y
ejecútalo — instala el plugin en la carpeta de plugins de OBS para todos los usuarios y saca de la
carpeta de OBS una instalación de la v0.7.0 o anterior. Todavía no
está firmado, así que Windows SmartScreen avisará antes de dejarlo ejecutar. **Guía
completa con capturas de cada paso, incluido el aviso de SmartScreen: `docs/INSTALL_GUIDE-es.md`.**
Instálalo antes de salir en directo, no a mitad de un stream — necesita OBS cerrado para
sobrescribir sus archivos.

**Linux: usa el paquete de tu distribución.** `obs-trigglow-dynamic-delay-1.0.0-x86_64-linux-gnu.deb`
(Debian, Ubuntu, Mint, Pop!_OS: `sudo apt install ./<archivo>.deb`), `.rpm` (Fedora, openSUSE:
`sudo dnf install ./<archivo>.rpm`) o `.pkg.tar.zst` (Arch, Manjaro, EndeavourOS:
`sudo pacman -U <archivo>.pkg.tar.zst`), que instalan lo que necesitan. En cualquier otra
distribución: el `.tar.xz` y su `./install.sh`, solo para tu usuario.

**macOS:** el `.pkg` de la misma release.

**Dónde quedan los archivos (v0.7.1):**

| Plataforma | OBS 33 y posteriores | OBS 31 y 32 (ubicación legacy) |
|---|---|---|
| Windows | `C:\ProgramData\obs-studio\plugins\obs-trigglow-dynamic-delay\obs-trigglow-dynamic-delay.dll`, con `data\` al lado | `...\obs-trigglow-dynamic-delay\bin\64bit\obs-trigglow-dynamic-delay.dll`, el mismo `data\` |
| Linux, paquete | `<libdir>/obs-modules/plugins/obs-trigglow-dynamic-delay.so`, `/usr/share/obs/obs-modules/plugins/obs-trigglow-dynamic-delay/` | enlaces simbólicos en `<libdir>/obs-plugins/` y `/usr/share/obs/obs-plugins/` |
| Linux, `.tar.xz` | `~/.local/share/obs-studio/plugins/obs-trigglow-dynamic-delay/` | enlaces simbólicos en `~/.config/obs-studio/plugins/obs-trigglow-dynamic-delay/` |
| macOS | `~/Library/Application Support/obs-studio/plugins/obs-trigglow-dynamic-delay.plugin` | la misma |

`<libdir>` es `/usr/lib/x86_64-linux-gnu` (`.deb`), `/usr/lib64` (`.rpm`) o `/usr/lib`
(`.pkg.tar.zst`). OBS 33 carga la primera columna y se salta la copia legacy; OBS 31 y 32 solo
conocen la segunda. En Windows cada copia lleva sus propias DLL de FFmpeg al lado.

**Instalación manual en Windows:** extrae el `.zip` en `C:\ProgramData\obs-studio\plugins\`
(contiene la carpeta `obs-trigglow-dynamic-delay`) y reinicia OBS. No copies nada en
`C:\Program Files\obs-studio`: OBS 33 marca ahí los plugins como "Legacy" y OBS 34 no los carga.

## Usar el plugin

Abre OBS. Si el dock **"Trigglow Dynamic Delay"** no aparece, abre el menú `Paneles` (View → Docks) y actívalo. Inicia
sesión una vez desde el dock con tu cuenta gratuita de Trigglow.

El dock es un solo panel. De arriba abajo: el **estado**, el único botón **Activar delay** /
**Desactivar delay**, la tarjeta **Delay** (1-600 s, -5/+5 y tus tres presets como atajos rápidos),
tus **escenas** y, plegadas, **Presets con atajo** y **Opciones** (la calidad mínima y la
estimación de RAM, la insignia de delay para los espectadores, con o sin los segundos, y su esquina,
y **Avanzado**). La
versión y **Reportar un problema** están abajo del todo.

### A todo OBS (por defecto; todas las plataformas)

1. Elige tus **escenas con delay** (la primera es obligatoria — las escenas con tu contenido real;
   **Añadir escena** para tener hasta 8) y, opcionalmente, una **escena de carga** para mostrar mientras se acumula el delay en vez de
   quedarte en la escena en directo sin delay (si no eliges ninguna, sale un vídeo de Trigglow
   "Poniendo delay": "Trigglow (por defecto)" en el selector). Pon el **Delay** (1-600 s) y, en
   Opciones, la **calidad mínima** (480p, 720p, 1080p, 1440p (2K) o 2160p (4K)) por debajo de la
   cual el vídeo con delay no baja para ahorrar RAM (tampoco se guarda nunca por encima de la
   resolución de salida de OBS: con una salida de 720p el buffer es 720p aunque el mínimo sea
   1080p). Opciones muestra una estimación en vivo de si tus segundos caben de verdad en la RAM a
   esa calidad *antes* de activar el delay, y se abre sola cuando no caben: entonces el tiempo de
   buffer se acorta en vez de bajar más la calidad, pero tu elección nunca se bloquea. Las escenas
   y la calidad se bloquean mientras el delay se prepara o está activo.
2. Pulsa **Activar delay** (o un atajo, o el Stream Deck). El dock muestra **Preparando delay** con
   una barra de progreso y una cuenta atrás mientras se ve tu escena de carga; en cuanto se llena el
   buffer pasa a **Delay activo** y el Programa muestra el vídeo y el audio con delay, en
   sincronía: tu directo y tu grabación van los dos con delay. **El output de streaming en sí no se
   toca en ningún momento**: los espectadores nunca ven un corte ni una reconexión, lo pulses antes
   o durante un directo.
3. Cambia los segundos con el delay activo: al bajarlos se salta hacia delante al momento; al
   subirlos la imagen se congela hasta 3 s, y por encima de eso se ve la escena de carga solo
   durante los segundos de más.
4. Deja la vista con delay en el Programa y cambia entre tus escenas con delay con los botones del
   dock (salen con el delay activo), los atajos "Escena con delay 1" a "8" o el Stream Deck: tu
   audiencia ve el cambio cuando el delay llega a él, imagen y sonido juntos. También puedes elegir
   una escena con delay en la lista de escenas de OBS, pero OBS muestra antes su transición sin
   delay un momento. Una escena que no es de ellas se ve sin delay; para cambiar a cualquier escena
   libremente, usa solo a la emisión.

**Desactivar delay** libera el buffer de RAM al instante y muestra en directo la escena que estaba
en el delay (si lo desactivas mientras aún se prepara, la escena que estaba antes de activarlo).

A todo OBS gestiona por su cuenta unos pocos objetos de tu colección de escenas: las escenas
**Trigglow Delay Buffer (auto)** y **Trigglow Delay Scenes (auto)** (guarda tus escenas con delay;
el filtro de vídeo va en ella), los filtros **Trigglow Buffer Mode Delay (auto)** y `Trigglow
Audio Delay (auto) - <fuente>`, la imagen de la insignia **Trigglow Delay Badge (auto)** y, si no
eliges escena de carga, la escena **Trigglow Delay Loading (auto)**. La insignia la puedes mover o
escalar en OBS y se queda donde la pongas; los demás no los renombres ni los borres. Solo a la
emisión no añade nada a tu colección de escenas (su insignia es una fuente privada en un canal de
salida de OBS).

### Solo a la emisión (avanzado; Windows)

Marca **Aplicar el delay solo a la emisión (la grabación y la vista previa no se retrasan)** en
**Opciones → Avanzado**. Se bloquea mientras hay un delay activo ("Desactiva el delay para
cambiarlo."), y si lo marcas en pleno directo se aplica desde tu próximo directo. Desaparecen los
selectores de escena, la calidad mínima y la estimación de RAM, y aparece lo propio de la emisión:
a dónde envía, **Borrar**, **Ajustes que revisar**, **Delay por escena**, el **Búfer del delay**
(Opciones) y enviar el resto al parar.

1. Empieza a emitir como siempre. Con el delay quitado estás en directo; pulsa **Activar delay**
   cuando quieras y los espectadores ven el stream esos segundos tarde. Si lo activas antes de
   empezar a emitir, el directo empieza esos segundos tarde en la plataforma ("Preparado").
2. Cambia los segundos en directo: al subirlos se rebobina (los espectadores vuelven a ver los
   últimos segundos) y al bajarlos se salta hacia delante. **Desactivar delay** vuelve al directo.
   En cuanto el directo lleva ese tiempo, cualquier subida hasta 600 s es al instante (lo dice la
   línea **Subidas al instante** del dock; si no se puede usar el disco, hasta tu preset o regla por
   escena más alto).
3. **Borrar** (lo que aún no se ha emitido) cuando se te cuela algo en pantalla: los segundos que
   aún esperan en el delay no se envían nunca, el stream salta al directo y el delay vuelve solo
   pasados sus segundos.
4. Opcional: **Delay por escena** (activa el delay con sus propios segundos, o lo quita, al cambiar
   a una escena) y **Al parar el directo, enviar los segundos que los espectadores aún no han
   visto** (activado por defecto; **Dejar de enviar ya** lo corta).

El dock muestra el estado ("Delay desactivado", "Preparado", "Delay activo: tus espectadores ven el
directo 30 s tarde", "Enviando el resto"...), a dónde envía y el estado de la conexión, la RAM y el
disco usados y el bitrate, y los ajustes de OBS que conviene cambiar. El delay se mueve en pasos de
tu intervalo de fotogramas clave: ponlo en 2 s en `Ajustes → Salida`. Si un directo no puede usarlo
(Enhanced Broadcasting, SRT, WHIP...), el dock dice por qué: desmarca la opción para retrasar todo
OBS.

### Atajos

En `Ajustes → Atajos`, busca "Trigglow". Manejan el modo elegido en el dock, y solo ese:

| Atajo | A todo OBS (por defecto) | Solo a la emisión |
|---|---|---|
| Trigglow: Activar/desactivar Dynamic Delay | activa o quita el delay | activa o quita el delay |
| Trigglow: Activar Dynamic Delay | activa el delay | activa el delay |
| Trigglow: Desactivar Dynamic Delay | quita el delay | quita el delay |
| Trigglow: Volver al directo (quitar delay) *(nuevo)* | quita el delay | quita el delay |
| Trigglow: Delay +5 s / Delay -5 s *(nuevos)* | cambia el delay en directo (1-600) | cambia el delay en directo (1-600) |
| Trigglow: Borrar lo que aún no se ha emitido *(nuevo)* | nada | borra |
| Trigglow: Preset de delay 1 / 2 / 3 *(nuevos)* | los segundos del preset, con el delay activado | los segundos del preset, con el delay activado |
| Trigglow: Escena con delay 1 ... 8 *(nuevos)* | pone esa escena con delay (en el orden del dock) en el delay | nada |

Los tres atajos originales mantienen su nombre interno, así que las teclas que ya tenías asignadas
se conservan. Con OBS en inglés se llaman "Trigglow: Toggle Dynamic Delay", "Trigglow: Enable
Dynamic Delay" y "Trigglow: Disable Dynamic Delay".

### obs-websocket

El plugin registra un vendor de obs-websocket 5, **`trigglow-dynamic-delay`**, cuando obs-websocket
está cargado (viene con OBS). Envía `CallVendorRequest` con `vendorName: "trigglow-dynamic-delay"`
y uno de estos `requestType`:

| Petición | Datos | Qué hace |
|---|---|---|
| `GetStatus` | — | solo devuelve el estado |
| `EnableDelay` / `DisableDelay` / `ToggleDelay` | — | lo mismo que los atajos |
| `SkipToLive` | — | vuelve al directo (quita el delay) |
| `SetDelay` | `{"seconds": N}` | fija los segundos (1-600); 0 o menos se ignora |
| `AdjustDelay` | `{"delta": N}` | suma N segundos (en negativo, los baja) |
| `DumpDelay` | — | borra lo que aún no se ha emitido (solo a la emisión); `dumped` dice si había algo que descartar |
| `ApplyPreset` | `{"index": 0-2}` | preset 1-3 |
| `SetDelayedScene` | `{"sceneName": "..."}` o `{"slot": 1-8}` | todo OBS, con el delay activo: pone esa escena con delay en el delay; `selected` dice si lo hizo |

Las peticiones van al modo elegido en el dock (`SetDelayedScene`, siempre a todo OBS). Cada respuesta lleva `ok` y el estado: `mode`
(`scene` para todo OBS, `stream` para solo a la emisión), `state` (modo stream: `idle`,
`live`, `delayed`, `draining`; modo escena: `inactive`, `filling`, `active`, `error`), `delayOn`,
`delaySeconds`, `appliedSeconds` (lo que reciben ahora los espectadores, 0 en directo),
`behindSeconds` y `ramMegabytes` (modo stream), `delayedScenes` (`[{"sceneName"}]`, la lista de todo
OBS) y `delayedScene` (la que está en el delay, vacío si está desactivado). El evento del vendor **`StatusChanged`** lleva los
mismos campos salvo `behindSeconds` y `ramMegabytes`, y se envía cuando cambia alguno de ellos.

## Mapear un botón de Stream Deck

Dos formas:

- **El plugin de Stream Deck de Trigglow** habla con OBS por obs-websocket (el vendor de arriba) y
  muestra el estado del delay en sus teclas. Sus teclas de Dynamic Delay (activar/desactivar,
  volver al directo, presets, +/-) se están añadiendo a ese plugin; ver `docs/STREAM_DECK-es.md`.
- **Cualquier Stream Deck, sin plugin:** añade la acción **"System" → "Hotkey"** y haz que envíe la
  combinación de teclas que asignaste a uno de los atajos de arriba. OBS tiene que estar abierto y
  escuchando el atajo global (con algunas configuraciones, también con el foco).

Pulsarlo en directo es seguro con cualquiera de las dos opciones: sin reconexión ni corte visible. Más detalle y
solución de problemas: `docs/STREAM_DECK-es.md`.

## Estructura del proyecto

```
src/
  plugin-main.cpp                 → obs_module_load/unload, wiring de todos los componentes, los
                                     archivos de ajustes (buffer-mode-settings.json,
                                     stream-mode-settings.json)
  stream-delay-controller.{hpp,cpp} → modo stream de la v1.0.0, lado de OBS: si el directo es apto,
                                     apuntar la salida de emisión al relay, activar o quitar el
                                     delay y sus segundos, borrar, presets, reglas por escena, la
                                     insignia, las comprobaciones de ajustes
  stream-relay.{hpp,cpp}           → el relay (avformat de FFmpeg, Windows): escucha RTMP en
                                     127.0.0.1, el emisor hacia tu servicio de OBS, reintentos, envío al parar
  packet-delay-core.{hpp,cpp}      → puro: PacketHistory y SinkScheduler (empalmes, reescritura de
                                     timestamps, recorte, suelo del borrado), con tests
  settings-ui-stream.cpp           → lo del dock propio de solo a la emisión (a dónde envía,
                                     Borrar, ajustes que revisar, delay por escena, búfer)
  apply-to.{hpp,cpp}               → puro: a qué se aplica el delay (todo OBS / solo a la
                                     emisión), su ajuste y la migración desde builds de prueba 1.0
  delay-router.hpp                 → puro: el botón del dock, los atajos y obs-websocket hacia el
                                     modo elegido y solo a él, con tests
  delay-targets.{hpp,cpp}          → los dos modos como destinos del router
  websocket-vendor.{hpp,cpp}       → el vendor de obs-websocket trigglow-dynamic-delay (peticiones,
                                     evento StatusChanged)
  core-host.{hpp,cpp}, core-main.cpp → el build solo de núcleo (TRIGGLOW_CORE_ONLY): el modo escena
                                     controlado desde los ajustes de una fuente, para hosts sin el
                                     frontend de OBS
  buffer-mode-controller.{hpp,cpp} → la máquina de estados (Inactive/Filling/Active/Error) que
                                     orquesta el modo buffer — la única dueña del estado
  obs-frontend-bridge.{hpp,cpp}    → única capa que toca obs-frontend-api.h + el cableado de
                                     filtros/escenas
  video-delay-filter.{hpp,cpp}     → el filtro de delay de vídeo: captura, conversión I420 en la
                                     GPU, lectura, reproducción, su pequeño pool fijo de objetos GPU
                                     y la telemetría perf check / pipeline check
  codec-pipeline.{hpp,cpp}         → hilos de compresión/descompresión, el hilo de preparación del
                                     códec de almacenamiento y el hilo liberador, fuera del hilo
                                     gráfico de OBS, con test de estrés en tests/
  storage-codec.hpp                → la interfaz del códec de almacenamiento (encoder asíncrono,
                                     decoder), el códec sin comprimir, utilidades NV12 y la
                                     contabilidad de cadenas de los códecs inter-frame
  hw-storage-codec.{hpp,cpp}       → v0.7.0, solo Windows: códec de almacenamiento HEVC/H.264 por
                                     hardware (NVENC, AMF, Quick Sync) con decode D3D11VA, y su
                                     sondeo
  storage-codec-select.{hpp,cpp}   → puro: el ajuste oculto buffer_codec, las familias de encoders
                                     de OBS y el orden de los encoders de hardware, con tests
  storage-codec-policy.{hpp,cpp}   → puro: cuándo funciona el códec por hardware y cuándo el buffer
                                     vuelve a MJPEG (protección frente a "Encoding overloaded"),
                                     con tests
  annexb.{hpp,cpp}                 → recorrido Annex B puro: parámetros en banda, con tests
  mjpeg-codec.{hpp,cpp}            → MJPEG sobre el FFmpeg vendorizado, una instancia por hilo
  frame-ring.{hpp,cpp}             → ring de cabeceras de slot con máquina de estados y, por
                                     paquete, códec, key, cadena y seq, con tests
  frame-arena.{hpp,cpp}            → arena por bloques para los frames comprimidos, con tests
  degradation-policy.{hpp,cpp}     → degradación automática pura (hilos, peldaños de resolución,
                                     mitad de fps), con tests
  pipeline-health.hpp              → lo que lee el dock sobre la degradación del buffer
  perf-stats.{hpp,cpp}             → series de percentiles de tamaño fijo para la telemetría
  buffer-fit.{hpp,cpp}             → aritmética pura de dimensionado del buffer (límite de
                                     resolución de salida, suelo de calidad, presupuesto de RAM),
                                     con tests unitarios en tests/
  encoder-check.{hpp,cpp}          → decisiones puras sobre los encoders de OBS: cuáles ven el
                                     lienzo principal, el aviso de "encoder por encima del buffer"
                                     en el log y el techo de los encoders sobre el buffer (v0.7.0),
                                     con tests
  audio-delay-filter.{hpp,cpp}     → ring de delay de audio por fuente hoja, sincronizado con el
                                     vídeo
  hardware-info.{hpp,cpp}          → consulta de RAM total del sistema, usada para dimensionar el
                                     presupuesto del buffer, y las GPU presentes (fabricante, para
                                     el códec de almacenamiento por hardware)
  scene-combo-box.{hpp,cpp}        → combo box del dock poblado a partir de la lista de escenas de
                                     OBS
  settings-ui.{hpp,cpp}            → el dock Qt, un solo panel para los dos modos (estado, el
                                     único botón, delay, escena en directo/de carga, opciones con
                                     calidad, estimación de ajuste y la casilla avanzada, cuenta
                                     atrás de llenado, aviso de degradación, aviso de actualización)
  ui-tokens.{hpp,cpp}              → tokens de color puros derivados de la paleta de OBS, con el
                                     contrato WCAG AA probado en tests/
  ui-theme.{hpp,cpp}               → la hoja de estilo única de la UI, las fuentes de marca
                                     empaquetadas y los iconos SVG recoloreados (src/resources/),
                                     compartidos por el dock y el diálogo
  overlay-badge.{hpp,cpp}          → pinta el overlay "DELAY 30 s" del directo como un único PNG
                                     (QPainter)
  overlay-layout.{hpp,cpp}         → colocación pura de ese overlay en el lienzo, con tests
  auto-names.{hpp,cpp}             → nombres de la escena, filtros y overlay que gestiona el plugin
                                     y su migración desde los nombres "(no tocar)" de v0.5.x, con
                                     tests
  report-bug-dialog.{hpp,cpp}      → diálogo "Reportar un problema": primero Trigi si está
                                     disponible, después el formulario de ticket (con cuenta o
                                     de invitado), con el log de OBS redactado solo si el usuario
                                     lo marca después de verlo
  trigi-page.{hpp,cpp}             → la página de Trigi en ese diálogo: aviso, datos técnicos,
                                     conversación, estados y el paso a un ticket
  trigi-widgets.{hpp,cpp}          → la vista de la conversación (nunca carga nada) y sus burbujas
  trigi-states.{hpp,cpp}           → decisiones puras de estado, botón y cuenta atrás, con tests
  chat-text.{hpp,cpp}              → saneado puro y mini parser de marcado de las respuestas de
                                     Trigi (texto escapado, negrita, código y listas), con tests
  support-client.{hpp,cpp}         → llamadas de Trigi y de tickets, respuestas y errores, con tests
  support-service.{hpp,cpp}        → la parte Qt del soporte: hilos en segundo plano, una petición
                                     de Trigi a la vez, elecciones locales, líneas de log
  support-diagnostics.{hpp,cpp}    → los datos técnicos opcionales de OBS (solo números, booleanos
                                     y valores fijos, en menos de 1 KB), con tests
  diagnostics-collector.{hpp,cpp}  → rellena esa instantánea desde OBS y el plugin
  bug-report.{hpp,cpp}             → localiza el log actual de este OBS (solo Windows por ahora:
                                     el que lleva una marca que el plugin escribe al cargar), lo
                                     lee y lo redacta en memoria, y borra las copias antiguas de
                                     %TEMP%
  log-redact.{hpp,cpp}             → redacción pura, línea a línea, del log de OBS, con tests
  update-checker.{hpp,cpp}         → compara la última release de GitHub de este repositorio contra
                                     la versión en ejecución
  background-tasks.{hpp,cpp}       → trabajos en segundo plano cancelables (inicio de sesión, Trigi,
  cancel-token.{hpp,cpp}             tickets, comprobación de actualizaciones) con un cierre acotado
  ui-tasks.{hpp,cpp}                 cuando OBS descarga el plugin, con tests; ui-tasks los une al
                                     bucle de eventos de Qt
  auth-manager.{hpp,cpp}           → la puerta de login de cuenta gratuita (ver
                                     `docs/ACCOUNT_GATE-es.md`), en hilos en segundo plano
  auth-client.{hpp,cpp}            → protocolo de inicio de sesión: vinculación v2, renovación,
                                     cierre de sesión, el cambio único desde la v0.5.x y las
                                     reglas de 401/403, con tests
  token-store.{hpp,cpp}            → el inicio de sesión del plugin, cifrado con la API de
                                     protección de datos de Windows; en macOS y Linux, un archivo
                                     que solo puede leer tu usuario (0600), con tests
  api-client.{hpp,cpp}             → peticiones solo a api.trigglow.com, errores, Retry-After y
                                     las páginas fijas que puede abrir el plugin, con tests
  json.{hpp,cpp}                   → lector/escritor JSON estricto y acotado, con tests
  win-http.{hpp,cpp}               → el único camino HTTPS endurecido (sin redirecciones, TLS 1.2+,
                                     solo puerto 443, timeouts, tope de tamaño) para toda llamada
                                     de red: WinHTTP en Windows, libcurl en macOS y Linux
  i18n.hpp                         → helper Str() (obs_module_text()) — todo el texto visible de
                                     este plugin pasa por aquí; ver data/locale/*.ini
  hotkeys.{hpp,cpp}                → registro de los 18 atajos nativos de OBS, a través de
                                     DelayRouter
  logging.{hpp,cpp}                → wrapper de logging con prefijo de componente
  delay-controller.{hpp,cpp}       → lógica heredada del modo reconexión del diseño original
                                     abandonado (ver `docs/SPEC-es.md` §6) — presente en el repo pero
                                     nunca instanciada por plugin-main.cpp; no se envía en ningún
                                     build actual
data/
  locale/en-US.ini, es-ES.ini,     → las cadenas traducidas del plugin (ver src/i18n.hpp); OBS
  pt-BR.ini, zh-CN.ini               elige la de su idioma, y el inglés para las claves que falten
  images/, videos/                 → el fotograma de reserva de la insignia y los vídeos de carga
                                     por defecto
  fonts/                           → Figtree y Geist Mono empaquetadas + sus licencias OFL
  licenses/                        → avisos de los componentes de terceros (iconos Lucide, DLL de FFmpeg)
tests/
  buffer_fit_tests.cpp             → tests unitarios de buffer-fit (desactivados por defecto:
                                     -DTRIGGLOW_BUILD_TESTS=ON, después ctest)
  encoder_check_tests.cpp          → tests unitarios de encoder-check (igual)
  ui_tokens_tests.cpp · auto_names_tests.cpp · overlay_layout_tests.cpp ·
  frame_arena_tests.cpp · frame_ring_tests.cpp · degradation_policy_tests.cpp ·
  storage_codec_tests.cpp · storage_policy_tests.cpp → igual, para esos módulos
  pipeline_stress_tests.cpp        → test de estrés de concurrencia de ring + arena + pipeline de
                                     códec
  pipeline_gop_stress_tests.cpp · adversarial_gop_tests.cpp → lo mismo con un códec inter-frame
                                     falso (fake-gop-codec.hpp): cadenas, pérdidas, evacuaciones,
                                     cesiones, pausas de render
  hw_storage_codec_tests.cpp       → el códec de almacenamiento por hardware sobre el FFmpeg real,
                                     con fallos inyectados (Windows, builds locales con el FFmpeg
                                     del plugin)
  json_tests.cpp · log_redact_tests.cpp · chat_text_tests.cpp · support_diagnostics_tests.cpp ·
  api_client_tests.cpp · token_store_tests.cpp · auth_client_tests.cpp ·
  support_client_tests.cpp · trigi_states_tests.cpp → cliente de cuenta, Trigi y tickets, con un
                                     sustituto de red guionizado (fake-transport.hpp)
  background_tasks_tests.cpp       → cancelación y cierre acotado de los trabajos en segundo plano
  packet_delay_core_tests.cpp      → empalmes, timestamps, recorte y borrado del modo stream
  apply_to_tests.cpp               → a qué se aplica el delay, la migración desde builds de prueba
                                     1.0 y el router siguiendo la casilla del dock
  relay_harness.cpp                → arnés de extremo a extremo del relay: entra un stream RTMP
                                     local, cambios de delay, un borrado, y se comprueba la salida
                                     (manual, Windows)
tools/fonts/                       → cómo se generan los subconjuntos de las fuentes empaquetadas
docs/
  SPEC-es.md               → especificación técnica completa (empieza por aquí)
  INSTALL_GUIDE-es.md       → guía de instalación paso a paso con capturas (SmartScreen,
                             asistente del instalador, activar el panel en OBS)
  BUILD_VALIDATION.md       → qué se verificó de verdad en esta entrega, y qué no
  ROADMAP-es.md             → qué trajo cada versión, qué viene después y qué queda fuera de alcance
  STREAM_MODE-es.md         → modo stream: cómo funciona, controles, límites, qué está verificado
  STREAM_DECK-es.md         → guía detallada de Stream Deck, atajos y obs-websocket
  FAQ-es.md · TROUBLESHOOTING-es.md
CMakeLists.txt, CMakePresets.json, buildspec.json, cmake/  → sistema de build de la plantilla oficial
```

## Licencia y repositorio

GPL-2.0-or-later, heredada de `obsproject/obs-plugintemplate` (ver `LICENSE`) — no es solo una
formalidad: `libobs` es GPL-2.0, así que enlazar contra ella arrastra la misma licencia a este
plugin, lo que obliga a publicar el código fuente de cualquier binario distribuido.

Por eso este proyecto vive en **su propio repositorio, separado del repositorio principal de
Trigglow** (que permanece privado) — publicar solo el plugin aquí cumple la GPL sin exponer el resto
del producto. Detalle completo de esta decisión en `docs/SPEC-es.md` §9.

`main` es una rama protegida en GitHub: las fusiones requieren un pull request, incluso para el
propio dueño del repositorio (sin pushes directos, sin force-push, sin borrar la rama). El trabajo
del día a día ocurre en `develop`; los PRs van de `develop` a `main`.

### Recursos de terceros

El plugin incluye algunos componentes de terceros, cada uno con su licencia (todas compatibles con
distribuirlos junto a un binario GPL-2.0):

| Recurso | Dónde | Licencia |
|---|---|---|
| **Figtree** (SemiBold, Bold), subconjunto | `data/fonts/Figtree-*.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-Figtree.txt` |
| **Geist Mono** (Medium), subconjunto | `data/fonts/GeistMono-Medium.ttf` | SIL Open Font License 1.1, `data/fonts/OFL-GeistMono.txt` |
| Iconos de la interfaz (trazados de Lucide, como en la web de Trigglow) | compilados desde `src/resources/icons/` | ISC (partes MIT, Feather), `data/licenses/LICENSE-lucide.txt` |
| Bibliotecas de **FFmpeg** (avcodec, avutil, swresample, y avformat para el modo stream), build LGPL de BtbN fijada en `buildspec.json`, solo Windows | DLL junto a la del plugin (`bin/64bit` en el paquete), sin modificar | LGPL 3.0 o posterior, con componentes MIT/BSD/Apache (libvpl, AMF, ffnvcodec); `data/licenses/LICENSE-ffmpeg.txt`, con la etiqueta de la build y su código fuente |

Las fuentes son instancias estáticas, en subconjunto, de las fuentes variables originales de
[google/fonts](https://github.com/google/fonts) (`ofl/archivo`, `ofl/geistmono`); `tools/fonts/`
explica cómo se generan. La OFL exige que su texto acompañe a las fuentes, así que cada `OFL-*.txt`
va junto a su fuente en el paquete y en el instalador. Las fuentes solo las usan el dock, el diálogo
y el overlay del plugin (la fuente de la interfaz de OBS nunca cambia); sin ellas, todo el texto usa
la fuente de OBS.
