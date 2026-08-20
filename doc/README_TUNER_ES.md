# GPSDO Tuner

Una consola de escritorio para ajustar el lazo en vivo y observar lo que hace:
tres gráficas que se desplazan, una pestaña por grupo de parámetros y un cuadro
de comandos manuales para todo lo que las pestañas no cubren.

Es una **ayuda al ajuste**, no un instrumento de medida — lee *Limitaciones*
más abajo antes de sacar conclusiones de lo que muestra.

---

## Requisitos

**Python** 3.9 o posterior, más cuatro paquetes:

```
pip install PySide6 pyqtgraph pyserial tzdata
```

| Paquete | Para qué |
|---------|----------|
| PySide6 | la interfaz de usuario Qt |
| pyqtgraph | las gráficas en vivo |
| pyserial | la comunicación con la placa |
| tzdata | solo el botón **Generate tz_table.h** |

`tzdata` es opcional si nunca pulsas ese botón, y en Linux o macOS el sistema ya
proporciona los mismos datos de zonas. En Windows es la única fuente, ya que el
sistema no incluye base IANA. Actualízalo después con `pip install -U tzdata`.

Ejecuta con `python gpsdo_tuner.py`, o haz doble clic en Windows: la ventana de
consola que se abre se minimiza automáticamente a la barra de tareas y sigue
disponible por si hay que leer un traceback.

---

## Correspondencia de versiones

El tuner lleva un `TOOL_VERSION` que sigue la versión de firmware para la que se
escribió. Al conectar lee la versión de la propia placa y compara:

- **coinciden** — la barra de estado muestra `connected — firmware vX.YZ`
- **discrepan** — la barra de estado y el monitor en bruto lo indican

Una discrepancia no es fatal y el tuner seguirá hablando con la placa, pero es
de esperar que algunos campos se lean raro o que se rechacen comandos: un tuner
antiguo no conoce la telemetría nueva, y uno nuevo puede enviar verbos que la
placa nunca ha oído. Usa el par que se distribuyó junto.

---

## Pestañas

| Pestaña | Función |
|---------|---------|
| **LTIC (algo 10)** | PID por etapa del lazo de fase de tres etapas, más la calibración del detector |
| **LTIC-Lars (algo 11)** | Los parámetros del PI continuo (`LG`, `LD`, `LTC`, …) |
| **Multi-level (algo 12)** | Los dos escalares (`MG`, `MR`) y los once límites de fase por nivel |
| **FA damping** | Ventana de promediado de amortiguación, por etapa |
| **PID algo 3-9** | Kp / Ki / Kd / I_LIMIT para los algoritmos en el dominio de la frecuencia |
| **Calibration** | `LC`, `CT` y las constantes del detector |
| **Raw monitor** | Todo lo que envía la placa, sin analizar |
| **Help** | La referencia completa de comandos del firmware |

Todos los grupos de parámetros se leen al conectar, así que los paneles arrancan
poblados en vez de vacíos.

---

## Gráficas

Tres paneles, actualizados una vez por segundo. Lo que muestran los dos
superiores depende del algoritmo que reporte la placa:

| | Algoritmos 10 / 11 (LTIC) | Algoritmo 12 | Algoritmos 0-9 |
|---|---|---|---|
| Superior | Fase `dph` (ns) | Error de fase `ph` (ns) | Deriva aprendida (LSB) |
| Central | `Vphase` del detector (V), con guías de banda | Tensión de control `Vctl` (V) | Tensión de control `Vctl` (V) |
| Inferior | Error de frecuencia (Hz) | Error de frecuencia (Hz) | Error de frecuencia (Hz) |

Solo los lazos LTIC tienen detector de fase, así que bajo cualquier otro
algoritmo esos dos paneles quedarían vacíos toda la sesión. En su lugar se
reorientan a otras magnitudes y los títulos se ajustan solos — no hay nada que
configurar.

El algoritmo 12 recibe su propia pareja en lugar de tomar prestada ninguna de las
otras: no usa la realimentación anticipativa autoaprendida, así que la traza de
deriva saldría plana, y su fase viene directamente del detector y no a través de
un filtro de lazo, de modo que no es la misma magnitud que dibuja `dph`. Las
guías de banda del detector desaparecen siempre que el panel central muestra en
su lugar una tensión de control.

### Span y Follow

**Span** fija cuánta historia se ve: 1 min, 5 min, 15 min, 1 h o *all*. Con un
span seleccionado la traza se desplaza hacia la izquierda a escala constante, en
lugar de que el eje se estire para cubrir todo el búfer.

**Follow live** mantiene la ventana anclada a la muestra más reciente. Arrastra
o usa la rueda sobre cualquier gráfica y se desmarca sola, cediendo el eje al
ratón para poder recorrer todo el búfer; vuelve a marcarla — o cambia el Span —
para saltar de nuevo al modo en vivo.

**Clear plots** descarta todas las muestras almacenadas y reinicia el eje de
tiempo a cero — útil tras un arranque fallido y más rápido que reiniciar la
herramienta, lo que además cortaría la conexión. Borra los búferes además de las
trazas, así que después no vuelve a aparecer nada.

**About** reproduce la animación de arranque, sin más motivo que el placer.

---

## Limitaciones

**El historial está limitado a 30 horas.** El tuner guarda 108 000 muestras al
ritmo de telemetría de 1 Hz. Eso cubre una adquisición completa de 24 horas con
margen, pero todo lo más antiguo se descarta a medida que llegan datos nuevos y
no se puede recuperar. No se escribe nada en disco.

**Las gráficas no son un registrador.** Los datos graficados viven solo en
memoria y se pierden al cerrar la ventana. Para lo que quieras conservar usa
**Start logging** (pestaña Raw monitor): escribe cada línea recibida en
`gpsdo_AAAA-MM-DD_HH-MM-SS.log` junto al script, con búfer por líneas, de modo
que una ejecución que termine mal deja igualmente datos utilizables. Ojo: captura
el *texto crudo de telemetría*, no las series graficadas — para ADEV y
comparaciones largas contra una referencia, pasa ese archivo a TimeLab o similar.

**Generate tz_table.h** reconstruye la tabla de zonas horarias del firmware a
partir de los datos IANA de esta máquina y escribe `tz_table.h` junto al script.
Sustituye al antiguo `gen_tz_table.py`, así que el tuner es ya el único script
que mantener.

Los datos de zonas provienen de la base del sistema en Linux/macOS, o del
paquete `tzdata` de Python — que es como funciona en Windows, donde el sistema
no incluye base IANA alguna. Si el botón informa de que no hay datos, ejecuta
`pip install tzdata`; para actualizarlos después, `pip install -U tzdata`. La
cabecera generada anota de qué versión de IANA procede, cuando puede
determinarse.

> La propia IANA publica *fuentes* que requieren el compilador `zic`, así que
> descargar directamente de ellos no ayudaría — el paquete `tzdata` son los
> mismos datos ya compilados.

**El panel Raw monitor guarda solo las últimas ~2500 líneas** (menos de cinco
minutos al ritmo de telemetría). Es un límite de visualización, no de registro:
una vez iniciado el registro el archivo recibe todo, independientemente de lo que
el panel siga mostrando.

**La resolución es el ritmo de telemetría.** Una muestra por segundo, así que
cualquier cosa más rápida que unos 2 s es invisible: un ciclo límite rápido o el
jitter pulso a pulso no aparecerán, y lo que ves ya viene promediado desde el
firmware.

**Una conexión cada vez.** El puerto serie es exclusivo. Cierra antes cualquier
otro terminal sobre el mismo puerto, y recuerda que el tuner lo retiene mientras
está abierto.

**Las gráficas se fían de la placa.** Los valores se extraen del texto de
telemetría tal como llega. Si el firmware reporta una cifra obsoleta o errónea,
el tuner la dibuja fielmente — no verifica nada de forma cruzada.

**Las escrituras no son persistentes.** Fijar un parámetro lo cambia solo en
RAM. Los parámetros de ajuste del lazo requieren un `ES` explícito (la respuesta
nombra el comando exacto); las preferencias se guardan solas y lo indican.

---

*Parte de GPSDO FreeRTOS — [manual del firmware](README_ES.md) · [changelog](CHANGELOG_ES.md) · [repositorio](https://github.com/jmnlabs/GPSDO_FreeRTOS)*
