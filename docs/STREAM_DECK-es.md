*[English version](STREAM_DECK.md)*

# Trigglow Dynamic Delay + Stream Deck

Hay dos formas de controlar el delay desde un Stream Deck (o desde cualquier otro controlador):

- **Atajos de OBS**, con la propia acción "Hotkey" del Stream Deck. Funciona con cualquier software
  de Stream Deck y sin instalar nada más, pero la tecla no puede mostrar el estado del delay.
- **obs-websocket**, el control remoto que trae OBS. El plugin registra el vendor
  `trigglow-dynamic-delay` (v1.0.0). El plugin de Stream Deck de Trigglow lo usa para sus teclas de
  Dynamic Delay, que muestran el estado del delay en la propia tecla; cualquier herramienta que hable
  obs-websocket 5 también puede usarlo.

Las dos funcionan en el modo stream y en el modo escena: el plugin manda cada orden al modo stream
cuando este lleva el directo actual (o el siguiente), y al modo escena si no.

## Con atajos

### Paso 1: asignar el atajo en OBS

1. `Ajustes → Atajos`.
2. Escribe "Trigglow" en el filtro de arriba.
3. Verás estas entradas:
   - **Trigglow: Activar/desactivar Dynamic Delay** (la que casi todo el mundo quiere)
   - **Trigglow: Activar Dynamic Delay**, **Trigglow: Desactivar Dynamic Delay**
   - **Trigglow: Volver al directo (quitar delay)**
   - **Trigglow: Delay +5 s**, **Trigglow: Delay -5 s**
   - **Trigglow: Borrar lo que aún no se ha emitido** (solo en el modo stream)
   - **Trigglow: Preset de delay 1**, **2** y **3**
4. Haz clic en el campo junto al que quieras usar y pulsa la combinación de teclas (p. ej.
   `Ctrl+Alt+D`). Evita combinaciones que ya uses para otra cosa.

Los tres primeros existían antes de la 1.0.0 y mantienen su nombre interno, así que las teclas que
ya tenías asignadas se conservan. Con OBS en otro idioma los nombres salen traducidos (en inglés,
"Trigglow: Toggle Dynamic Delay"...).

### Paso 2: mapear el botón en el Stream Deck

1. Abre el software de Stream Deck (Elgato).
2. En el panel de acciones, busca la categoría **System** y arrastra la acción **Hotkey** a un
   botón.
3. Configúrala con la misma combinación de teclas que en el paso 1.
4. Opcional: cambia el icono y el título del botón a algo que reconozcas, como "Delay".

### Paso 3: probarlo

1. Con OBS abierto (no hace falta estar en directo para la primera prueba), pulsa el botón.
2. El dock "Trigglow Dynamic Delay" cambia de estado:
   - **Modo stream**: "Delay preparado: tu próximo directo empieza 30 s tarde" antes de emitir;
     mientras emites, "Delay activo: tus espectadores ven el directo 30 s tarde" o "En directo, sin
     delay".
   - **Modo escena**: "Preparando delay" con una cuenta atrás mientras se llena el buffer, y
     después "Delay activo".
3. Hazlo en directo sin miedo: en ninguno de los dos modos el stream se reconecta ni se corta.

### Notas

- Si tu Stream Deck envía la pulsación al sistema en vez de a una ventana, OBS tiene que poder
  recibir atajos globales. OBS registra sus atajos a nivel de sistema en Windows y macOS, así que
  normalmente funciona con OBS en segundo plano, pero los ajustes de accesibilidad o de permisos de
  tu sistema pueden cambiarlo.
- Una combinación que también uses en otro sitio puede disparar las dos cosas. Elige una poco
  habitual.

## Con obs-websocket

obs-websocket viene con OBS (`Herramientas → Ajustes del servidor WebSocket`: actívalo y apunta el
puerto y la contraseña). El plugin registra su vendor cuando OBS lo carga; el log de OBS dice
`obs-websocket vendor "trigglow-dynamic-delay" registered`, o `obs-websocket not available` cuando
obs-websocket no está.

Envía `CallVendorRequest` con `vendorName: "trigglow-dynamic-delay"`, un `requestType` y, cuando
haga falta, `requestData`:

| requestType | requestData | Qué hace |
|---|---|---|
| `GetStatus` | — | solo devuelve el estado |
| `EnableDelay` | — | activa el delay (necesita la cuenta gratuita, como el dock) |
| `DisableDelay` | — | quita el delay |
| `ToggleDelay` | — | activa o quita |
| `SkipToLive` | — | vuelve al directo: quita el delay |
| `SetDelay` | `{"seconds": 45}` | fija los segundos: 1-600, en directo en los dos modos. 0 o menos se ignora |
| `AdjustDelay` | `{"delta": -5}` | suma los segundos (en negativo, los baja), con los mismos rangos |
| `DumpDelay` | — | modo stream: los segundos que los espectadores aún no han visto no se envían nunca; `dumped` es false cuando no había nada que descartar (delay quitado, sin emitir o modo escena) |
| `ApplyPreset` | `{"index": 0}` | preset 1 (`0`), 2 (`1`) o 3 (`2`): sus segundos, con el delay activado. Igual en el modo escena |
| `SetDelayedScene` | `{"sceneName": "Chat"}` o `{"slot": 2}` | todo OBS (v1.0.0), con el delay activo: pone esa escena con delay en el delay; tu audiencia ve el cambio cuando el delay llega a él. `selected` es false si el delay está desactivado o la escena no es una de las escenas con delay |

El `responseData` de cada respuesta lleva `ok: true` y el estado:

| Campo | Significado |
|---|---|
| `mode` | `stream` o `scene` |
| `state` | modo stream: `idle`, `live`, `delayed`, `draining`; modo escena: `inactive`, `filling`, `active`, `error` |
| `delayOn` | el delay está activado (o preparado para el próximo directo) |
| `delaySeconds` | los segundos fijados |
| `appliedSeconds` | lo que reciben ahora los espectadores (0 en directo) |
| `behindSeconds` | modo stream: la antigüedad de lo que envía el relay, con precisión de 0,1 s |
| `ramMegabytes` | modo stream: la RAM guardada, con precisión de 0,1 MB |
| `delayedScenes` | las escenas con delay de todo OBS, `[{"sceneName": "..."}]`, en el orden del dock |
| `delayedScene` | la que está ahora en el delay (vacío con el delay desactivado) |
| `dumped` | solo en la respuesta de `DumpDelay` |
| `selected` | solo en la respuesta de `SetDelayedScene` |

El plugin también emite el evento de vendor **`StatusChanged`** (suscríbete a los eventos de
vendor) con `mode`, `state`, `delayOn`, `delaySeconds` y `appliedSeconds`, solo cuando cambia
alguno de ellos. Mientras el modo stream se pone al día tras una subida, `behindSeconds` cambia sin
evento: consulta `GetStatus` (una vez por segundo basta) si quieres mostrarlo.

Petición de ejemplo (obs-websocket 5, op 6):

```json
{
  "op": 6,
  "d": {
    "requestType": "CallVendorRequest",
    "requestId": "dd-1",
    "requestData": {
      "vendorName": "trigglow-dynamic-delay",
      "requestType": "SetDelay",
      "requestData": { "seconds": 45 }
    }
  }
}
```

Las peticiones se ejecutan en el hilo de la interfaz de OBS, como los botones del dock, y la
respuesta llega cuando la orden ya se ha aplicado.

## El plugin de Stream Deck de Trigglow

El plugin de Stream Deck de Trigglow está recibiendo teclas de Dynamic Delay basadas en este vendor:
activar/desactivar, volver al directo, un preset (que opcionalmente activa el delay) y +/- un paso,
cada una mostrando el estado del delay. Se conecta a obs-websocket con el host, el puerto y la
contraseña que pongas en sus ajustes. Hasta que salga esa versión, usa los atajos de arriba.

Sin probar todavía: esas teclas contra un OBS real con Dynamic Delay 1.0.0 y un Stream Deck físico
(se comprobaron contra un obs-websocket simulado).
