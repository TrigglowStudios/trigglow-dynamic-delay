*[English version](ACCOUNT_GATE.md)*

# Puerta de cuenta gratuita

Trigglow Dynamic Delay es, y seguirá siendo, 100% gratis: sin suscripción, sin versión de pago y sin
ninguna función bloqueada tras un pago. Desde la v0.3.3, activar el delay (los botones del dock, los
atajos, Stream Deck y obs-websocket, en el modo stream y en el modo escena) exige haber iniciado
sesión al menos una vez con una cuenta gratuita de trigglow.com. El objetivo es que se conozca
Trigglow, no monetizar ni captar leads: Trigglow tiene otros productos gratuitos (con límites) para
streamers, y la mayoría de quienes instalan este plugin desde el directorio de recursos de OBS
nunca sabrían que existen.

## Lo que esto NO hace

- El plugin nunca pide ni guarda contraseñas, emails ni ningún dato personal (ver "Cómo funciona el
  inicio de sesión" más abajo). El plugin solo guarda su propio token opaco, que no sirve para nada
  más que las pocas llamadas que hace el plugin.
- No se bloquea ninguna configuración del plugin (escenas, segundos de delay, calidad mínima,
  presets, reglas por escena). Solo se bloquea activar el delay (Activar,
  Activar/desactivar, un preset o una regla por escena que lo active). Quitarlo, volver al directo y
  Borrar nunca se bloquean.
- Los datos de cuenta que se recogen aquí no se usan para marketing ni se revenden a terceros sin un
  consentimiento aparte y explícito. Es un compromiso de producto y de la política de privacidad de
  Trigglow, no algo que imponga este código, pero es la razón de que la puerta sea tan acotada.

## Cómo funciona el inicio de sesión

1. `AuthManager::StartLogin()` (`src/auth-manager.hpp`/`.cpp`, protocolo en `src/auth-client.hpp`)
   llama a `POST /auth/plugin/start` en trigglow.com, que devuelve un código corto (como
   `KD7F-29QM`), la página de confirmación que hay que abrir y un secreto que nunca sale del plugin.
   Esta llamada (y todas las demás llamadas HTTPS de esta función) pasa por `src/win-http.hpp`
   (WinHTTP, la API nativa de Windows, en Windows; libcurl en macOS y Linux desde la v1.0.0, con las
   mismas reglas: HTTPS solo por el puerto 443, TLS 1.2+, sin redirecciones, cuerpo con tope, CR/LF
   rechazados en las cabeceras, cancelable, nada en el log) en vez de por `QNetworkAccessManager` de
   Qt. El comentario de esa cabecera explica por qué: el plugin de backend TLS de Qt va atado a la
   versión exacta de Qt que ya está cargada en el proceso (el Qt que trae OBS, no el más antiguo
   contra el que se compila este plugin), y si se incluye uno que no coincide, se rechaza en silencio
   al ejecutarse ("No functional TLS backend was found", visto en directo el 2026-09-01). Las
   redirecciones nunca se siguen, así que el token del plugin no se puede desviar a otro host.
2. El plugin abre la página en el navegador del sistema (`QDesktopServices::openUrl`), nunca en un
   navegador integrado ni en un formulario dentro de OBS, y muestra el código en el dock. La página
   muestra el mismo código, el nombre de la app tal como lo conoce trigglow.com (no un texto sacado
   del enlace), su versión y su sistema, hace cuánto se hizo la petición y, cuando parece venir de
   una red distinta a la del navegador, un aviso: confirma solo si los códigos coinciden. "No he
   sido yo" en la página cancela la petición. "Copiar enlace" en el dock sirve si el navegador no se
   abrió.
3. Inicias sesión (o creas una cuenta gratuita) en la página de inicio de sesión normal de
   trigglow.com, la misma que usa la propia web (email y contraseña u OAuth de Kick). El plugin no
   participa en ese paso.
4. Mientras el dock espera, el plugin consulta `POST /auth/plugin/token` al intervalo que indica el
   servidor, hasta que la petición caduca. El secreto viaja en el cuerpo de la petición, nunca en
   una URL, y ni la página, ni el código, ni el secreto se escriben nunca en el log de OBS.
5. Una vez confirmado, el plugin recibe su propio token. Solo sirve para las llamadas propias del
   plugin (comprobación de cuenta, renovación, cierre de sesión, Trigi y los tickets de soporte del
   propio usuario), se renueva solo cuando el servidor lo indica, y "Cerrar sesión" lo revoca en el
   servidor además de borrarlo en local (la copia local se borra aunque no haya conexión). También
   puedes revocarlo desde trigglow.com: Ajustes → Sesiones activas → Apps conectadas.
6. En Windows el token se guarda cifrado con la API de protección de datos de Windows, solo para el
   usuario de Windows actual, aparte de los ajustes del plugin (`trigglow-auth.dat` en la carpeta de
   configuración del plugin). Si no se puede descifrar (otro usuario de Windows, un perfil
   restaurado), se borra y el dock vuelve a "Iniciar sesión". En macOS y Linux (v1.0.0) el mismo
   archivo **no va cifrado**: es un archivo que solo tu usuario puede leer y escribir (modo 0600,
   puesto antes de escribir nada en él), igual que OBS guarda tu clave de retransmisión en su
   `service.json`, con una marca que rechaza un archivo que no sea del plugin. El token solo sirve
   para las llamadas propias del plugin, y Cerrar sesión (o las apps conectadas de trigglow.com) lo
   revoca. El email no se guarda. "Reportar un problema" nunca lee, adjunta ni menciona el token.

Los inicios de sesión hechos con la versión 0.5.x o anterior se cambian una sola vez, en silencio,
por un token del plugin en el primer arranque de la 0.6.0, y el antiguo archivo de inicio de sesión
en texto plano se sobrescribe y se borra. Hasta que ese cambio sale bien (por ejemplo, sin
conexión), el inicio de sesión antiguo sigue permitiendo activar el delay.

El inicio de sesión de la 0.5.x y anteriores se apaga en trigglow.com cuando se publica la 0.6.0: a
partir de ahí esas versiones ya no pueden iniciar sesión, y las sesiones que ya tienen dejan de
funcionar tras un periodo de transición, durante el que la 0.6.0 todavía puede cambiarlas. Por eso
actualizar es obligatorio para seguir usando el delay; en la 0.6.0 el dock dice "Esta versión del
plugin ya no puede iniciar sesión. Actualiza el plugin." si trigglow.com llega a indicar lo mismo
para ella.

En cada arranque de OBS, `AuthManager::Init()` hace una sola llamada `GET /auth/plugin/me` para
confirmar que el token guardado sigue valiendo (más una renovación cuando toca): ni un bucle ni un
latido en segundo plano. Si no se puede llegar a trigglow.com, el plugin sigue con la sesión
iniciada y activar el delay sigue funcionando; si el servidor dice que la cuenta está suspendida o
no está activa, activar se bloquea y el dock dice por qué. Fuera de una espera activa de
`StartLogin()`, el plugin no hace ninguna otra llamada de red relacionada con esta función.

El inicio de sesión (y con él activar el delay y Trigi) solo funcionó en Windows de la 0.6.0 a la
0.7.3. Desde la v1.0.0 funciona también en macOS y Linux, con la libcurl del sistema (CMake enlaza
`CURL::libcurl`; el `.deb` depende de `libcurl4t64 | libcurl4`, el paquete de Arch de `curl`, y el
`.rpm` la toma de lo que necesita el binario; la libcurl 7.79 de macOS 12 está contemplada). Este
camino lo compila la CI, pero aún no se ha ejecutado en un Mac o un Linux reales. El guardado
previsto en el llavero o el almacén de secretos del sistema no se hizo: ver el paso 6.

## Puntos de control

Uno por modo, los dos configurados desde la misma comprobación en `plugin-main.cpp`:

- **Modo escena:** `BufferModeController::SetAuthorizationCheck()`
  (`src/buffer-mode-controller.hpp`). El botón Activar delay del dock (`settings-ui.cpp`) y todos
  los atajos u órdenes de obs-websocket que van al modo escena acaban en
  `BufferModeController::Enable()`/`Toggle()`, así que controlarlo ahí los cubre todos.
- **Modo stream:** `StreamDelayController::SetAuthorizationCheck()`
  (`src/stream-delay-controller.hpp`), comprobado en `StreamDelayController::Enable()`, por donde
  pasan la sección del modo stream del dock, los presets, las reglas por escena y los atajos y
  órdenes de obs-websocket redirigidos (`src/delay-router.cpp`).
