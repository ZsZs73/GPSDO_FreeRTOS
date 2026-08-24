# Registro de cambios — GPSDO FreeRTOS

[English](CHANGELOG_EN.md) | [Polski](CHANGELOG_PL.md) | **Español**

📖 [Inicio del proyecto](../README.md) · Volver al [README](README_ES.md)

Todos los cambios notables de este proyecto se documentan aquí.

Proyecto de **J. M. Niewiński** — <https://github.com/jmnlabs/GPSDO_FreeRTOS>
Basado en **GPSDO v0.06c** de André Balsa
(<https://github.com/AndrewBCN/STM32-GPSDO>), port a FreeRTOS y algoritmos
3–10 del autor, con Claude AI como asistente de programación y diseño de PCB
por Scrachi (foro EEVBlog).

El sufijo de versión `-rtos` marca el linaje del port a FreeRTOS.

> **Nota sobre la traducción.** Las entradas desde v0.95 en adelante están
> íntegramente en español y se mantienen al día. Las anteriores están traducidas
> en su mayor parte, pero conservan pasajes en inglés: son párrafos técnicos
> detallados sobre defectos ya corregidos, y traducirlos mecánicamente produciría
> un texto peor que el original. Se dejan como están antes que estropearlos.
>
> Si algún pasaje antiguo resulta poco claro, la versión inglesa
> (`CHANGELOG_EN.md`) es la referencia completa.

---

## [v1.05-rtos SJ] — sin publicar

Build para Dave (Solder_Junkie), EEVblog: v1.05 más el retroporte de la
escritura no bloqueante del informe (`doc/v105-usb-cdc-nonblocking.patch`),
con la configuración de su hardware (OLED SSD1306, sin LTIC / PICDIV /
GPS-TIMING / INA219 — los algoritmos 10–12 quedan fuera de la compilación).

### Corregido
- **La escritura no bloqueante del informe silenciaba la telemetría de 1 Hz
  por USB CDC.** El guard original preguntaba `availableForWrite()` una vez y
  descartaba el informe ENTERO cuando devolvía menos que su tamaño. Por USB
  CDC eso es todos: la cola TX de `USBSerial` mide 64 × 2 = **128 bytes**
  (valores por omisión de stm32duino 2.12.0) y el informe ocupa 400+. El
  arranque y la CLI seguían funcionando — así que el build que debía curar
  «enchufas el USB y la pantalla se congela» habría entregado una placa sin
  congelación… y sin telemetría. La escritura va ahora por trozos del tamaño
  que el puerto declara, con un presupuesto de 25 ms; `room == 0` significa
  «llena, espera», nunca escribir a ciegas. La cola TX CDC se amplía además a
  1 KB (`-DCDC_TRANSMIT_QUEUE_BUFFER_PACKET_NUMBER=16` en `build_opt.h`,
  macro con `#ifndef` en la librería USBDevice, verificado en el core 2.12.0).
  El parche lleva la versión corregida.

---

## [v1.05-rtos] — 2026-08-20

El algoritmo 12, hecho funcionar. Salió en v1.04 con la aritmética correcta y
cinco fallos independientes en la maquinaria que la rodea, cada uno de los cuales
ocultaba al siguiente. El lazo mantiene ahora la fase en 5–8 ns RMS a lo largo de
23 horas con un único rearmado del picDIV, frente a los 10–23 ns de la mejor
referencia anterior — y cada corrección de abajo se simuló antes de grabarla,
porque los dos cambios de este proyecto que salieron solo con razonamiento
resultaron ambos equivocados.

### Corregido
- **El estimador de ruido solo podía descender.** La puerta de valores atípicos
  era `dp_lim = 5*sigma`, leída de la propia estimación que alimentaba: en cuanto
  sigma era pequeña, toda diferencia lo bastante grande como para elevarla se
  rechazaba por atípica. Medido el 14.08 — `sig` marcó exactamente 2 ns durante
  las 1020 muestras de una serie, y con los límites por nivel derivados de ella
  la jerarquía quedó clavada en el suelo de 100 unidades: 79 de 80 correcciones
  dispararon en el nivel 0. Un acumulador multinivel que nunca abandona el nivel
  cero no lo es. La puerta es ahora absoluta (300 ns), y los atípicos reales para
  los que existía — diferencias tomadas a través de un hueco NOPH/SYNC/rearmado
  — se excluyen estructuralmente con una bandera de contigüidad en lugar de
  estadísticamente. Sigma tiene un suelo de 5 ns, por debajo del cual este
  detector no resuelve honestamente.
- **El término de frecuencia tenía el signo invertido.** Llevaba `+polarity`,
  copiado de la rama de frecuencia del algoritmo 11 — pero esa rama lee TIM2, y
  el propio comentario del algo 11 en este firmware recoge el hallazgo de
  hardware de que TIM2 y el detector LTIC tienen orientación opuesta en este
  cableado. La pendiente `f_nss` no es una lectura de TIM2: es la derivada de los
  mismos valores del acumulador que producen el término de fase, del mismo
  sensor. Una magnitud y su propia derivada temporal, medidas por un solo sensor,
  no pueden requerir signos de realimentación opuestos. El `cvPWM` de Alan
  coincide: pasa fase y pendiente por una sola conversión y las suma. Con la
  planta medida en vez de supuesta (+319,5 µHz/LSB, de regresar la media de 100 s
  del PWM contra la media de frecuencia de 100 s impresa, correlación 0,999 a
  retardo cero), el signo antiguo daba `d(phase_rate) = +0,4·f_ns`. Eso es
  realimentación positiva.
- **`s_mla_wait` nunca se ponía a cero.** Aparecía exactamente dos veces en el
  fichero, en su declaración y en el `++` de la prueba de abandono, y nunca
  volvía a cero. Así que unos cinco minutos después de arrancar superaba 300 y la
  prueba de abandono disparaba en el mismo segundo en que se alzaba la bandera,
  lo que mataba las dos cosas que esa bandera controla: la corrección por cruce
  por cero y la supresión de nuevas correcciones mientras un deslizamiento aún
  lleva la fase a casa. La serie que lo encontró: `zc` = 7 en 76 minutos, todos
  dentro de los cinco primeros, y 1072 de 1174 correcciones exactamente a dos
  segundos, que es la cadencia desnuda del nivel 0 sin nada que la frene. El
  mecanismo de cruce por cero, por tanto, nunca había funcionado más allá de los
  primeros minutos de ninguna serie desde que se introdujo.
- **El rescate FLL era un lazo bang-bang y no podía ser otra cosa.** Su paso era
  `-f*lsb_per_hz*0,10` limitado a ±64, que satura en |f| = 0,256 Hz, mientras que
  la puerta que tenía debajo solo abría a 0,3 Hz — la parte proporcional no podía
  actuar nunca. Accionado una vez por segundo desde una media de 100 s, unos 50 s
  de retardo, eso da 64 LSB/s × 50 s = 3200 LSB de recorrido antes de que la
  medida responda: 1,0 Hz de sobrepasamiento. Ambas cifras están en los registros
  (462 de 655 pasos consecutivos exactamente ±64; PWM barriendo 12 845 LSB;
  `f100` oscilando de −1,29 a +1,55 Hz). Ahora aplica la corrección calculada
  entera una vez y espera lo que necesite refrescarse la media de la que
  provino, a dos velocidades: la media de 10 s mientras el error es grande, la de
  100 s cuando es pequeño, con la espera siempre igual a la ventana en uso. La
  puerta pasó de 0,3 Hz a 0,05 Hz, porque por encima de 0,147 Hz la fase recorre
  toda la banda del detector de ±940 ns dentro de un solo horizonte de 64 s — la
  puerta antigua dejaba una zona muerta de 0,147 a 0,3 Hz en la que el lazo de
  fase no conseguía una mirada lo bastante larga y el FLL daba su trabajo por
  hecho.
- **`instant_offset` desbordaba.** `FREQ_LOWER`/`FREQ_UPPER` admiten ±500 Hz y el
  campo era `int8_t`, así que todo lo que pasara de ±127 alimentaba basura a
  cualquier puerta que lo leyera. Ahora `int16_t`, en los tres ficheros que lo
  tocan — la estructura, la conversión que lo rellena y la instantánea que lo
  copia. Arreglar solo uno habría compilado limpiamente dejando el desbordamiento
  en su sitio.
- **Las ramas de frecuencia y de FLL tomaban el signo de un menos incrustado.**
  Correcto únicamente porque `LPOL` vale −1 en esta placa; realimentación
  positiva en una placa con `LPOL +1`. Ambas toman ahora la `polarity` de la
  placa, que aquí evalúa idéntico y en otras partes correcto.
- **El dithering escribía solo una de sus dos tablas DMA.** El doble búfer
  alterna en cada pasada, así que la otra tabla — todavía con el código anterior
  — se reproducía hasta la escritura siguiente, y la salida alternaba entre el
  código viejo y el nuevo a unos 3 Hz (una pasada son 2^(24−N) periodos de
  portadora = 167,8 ms sea cual sea N). Un filtro de dos polos a 0,8 Hz atenúa
  3 Hz solo 14×. Ahora se rellenan ambas tablas, bajo un mutex, porque
  `pwm24_write()` se alcanza tanto desde ControlTask como desde CliTask y dos
  rellenos concurrentes de una misma tabla se entrelazan en una reproducción rota
  de 168 ms. Escribir la tabla que el DMA está leyendo es seguro por
  adelantamiento: el relleno escribe una entrada cada pocos microsegundos donde
  el DMA consume una cada 81,9 µs.
- **`PO` y `AO` no admitían el valor cero.** La comprobación de rango era
  `v >= −3000 && v <= 3000 && v != 0.0f`, de modo que el único valor que un
  usuario querrá con más probabilidad era el único rechazado. Rangos corregidos a
  ±5000 Pa y ±3000 m, y ambos llevan ya sus unidades en la ayuda y en el eco.

- **La placa no siempre arrancaba en frío, y el raíl de 3,3 V nunca fue la
  causa.** `ubx_poll_svin_nav()` llamaba a `vTaskDelay()` incondicionalmente. Su
  gemela `ubx_poll_svin()` lleva la guarda y el comentario que la explica —
  *"before vTaskStartScheduler() this must not be vTaskDelay(): calling it with
  no scheduler hangs the system"* — y la corrección entró en una de las dos y no
  en la otra.

  Antes de que exista el planificador, `vTaskDelay()` escribe a través de
  `pxCurrentTCB`, que todavía es `NULL`, así que la placa entra en hard fault y
  el manejador por defecto gira con las interrupciones desactivadas: sin salida,
  sin watchdog, nada salvo el botón de reset. Los ganchos de fallo de FreeRTOS
  añadidos en v1.04 no pueden atraparlo — necesitan un núcleo en marcha.

  Se escondía detrás del orden de llamadas en `gpsdo_gps_init()`: NAV-SVIN sólo
  se consulta cuando TIM-SVIN no respondió dentro de su ventana de 500 ms. Un
  receptor que ya está funcionando responde y la placa arranca — es el caso tras
  un reset, porque el receptor conserva su propia alimentación. Uno que todavía
  está arrancando no responde, y la placa se queda muerta. Ese es el caso del
  encendido en frío, y esa asimetría es la razón por la que esto pareció durante
  mucho tiempo una alimentación que se hundía mientras subía el raíl.

  Encontrado en un registro con cuatro arranques consecutivos, cada uno
  terminando tras `UBX: CFG-NAV5 ACK` y antes de `LEA-T: starting survey-in`, con
  la causa de reset leyendo `PIN/NRST` todas las veces — el botón del operador.
  El decodificador imprime `POWER-ON/BROWN-OUT` y una línea sobre revisar el raíl
  de 3V3 cuando la culpa es de la alimentación, y no apareció ni una sola vez.
  Un fallo de alimentación no se detiene cuatro veces en la misma línea de
  código.
- **Los dígitos de frecuencia no seguían el lock del algoritmo 12.** La lógica
  del color tiene una rama autoritativa para los lazos que publican un estado en
  vivo, y el algoritmo 12 no estaba en la lista — así que caía en la rama de las
  medias de frecuencia, que es precisamente lo que el comentario sobre esa rama
  dice que el verde no debe ser.

  Medido sobre una serie de 2,99 h: el lazo y el color discreparon en el
  **15,0%** de las muestras, y en todos esos casos el lazo estaba LOCKED con los
  dígitos BLANCOS, nunca al revés. El lazo alcanzó LOCK a los 108 s y los dígitos
  se pusieron verdes a los 1041 s. Quince minutos de un oscilador disciplinado
  con aspecto de no estarlo, en cada arranque, porque hasta que se llena la media
  de 1000 s esa rama no tiene con qué juzgar y `locked` es falso por
  construcción.

  El algoritmo 12 toma ahora su color de su propia tendencia, como el 10 y el 11.
  `CORR` y `ZC` cuentan como lock: son estados de un segundo que significan que
  el lazo está haciendo su trabajo, el mismo razonamiento por el que no
  reinician `s_mla_quiet`. Sin eso los dígitos parpadearían en blanco una vez por
  corrección — dieciséis veces en las tres horas medidas. La concordancia es
  ahora del 100%.
- **La guarda de «eco rancio» estaba puesta en medio conteo.** Retira el lock
  basado en la media larga cuando la media de 10 s se ha desviado, y el umbral
  era ±50 mHz. Pero la media de 10 s es un conteo de ciclos durante diez
  segundos, así que su granularidad es 0,1 Hz: a lo largo de tres horas tomó
  exactamente tres valores — −100, 0 y +100 mHz — y nada intermedio. Un umbral de
  50 mHz no significaba entonces «dentro de 50 mHz», sino «el contador debe leer
  exactamente 10 000 000», y un conteo en cualquier dirección mataba el verde.
  Eso es el 8,3% de las muestras asentadas y el 46% de la discrepancia anterior.
  Ahora ±0,15 Hz: un conteo completo más medio de margen, de modo que un temblor
  de un conteo pasa y una pérdida real de disciplina — muchos conteos, que es
  para lo que existe la guarda — sigue sin pasar. Los algoritmos 0-9 llevan la
  misma guarda y reciben la misma corrección.

### Añadido
- **Una puerta por nivel sobre el término de frecuencia.** El ruido propio de la
  pendiente es `sd(f_nss) = sigma · 2^((1−3L)/2)`, así que en el nivel 0 es
  1,41·sigma de ruido puro escalado por 12,5 LSB por ns/s, frente a los 0,39 LSB
  por ns del término de fase — una ventaja de 32:1 a favor de la magnitud
  equivocada. El registro mostró lo que eso compraba: el 46% de las correcciones
  chocaba contra el límite de ±470, una de ellas con la fase leyendo exactamente
  0 ns y la corrección a fondo de escala. El término se usa ahora desde el nivel
  3 hacia arriba, donde esa misma estimación está promediada sobre pares de 16 s
  y vuelve a ser una medida.
- **Un ajuste fino de frecuencia desde TIM2.** Cuando la media de 100 s indica
  más de 0,03 Hz, la componente de frecuencia de la corrección se toma de esa
  medida en lugar de la pendiente del acumulador. En régimen permanente está
  dormido — en una simulación de 10 h no disparó ni una vez — y ese es
  precisamente el punto: atrapa las excursiones de frecuencia que de otro modo
  sacarían la fase de la banda del detector, de manera que el lazo nunca tiene
  que readquirir. La serie de 23 h que estableció las cifras de arriba registró
  **un solo** rearmado del picDIV, frente a 121 con los mismos ajustes sin él.
- **`configUSE_MUTEXES` e `INCLUDE_xTaskGetSchedulerState`**, fijados
  explícitamente. El cerrojo del dithering necesita ambos, este proyecto no
  fijaba ninguno, y si la configuración por defecto de la librería los habilita o
  no es algo que no se deja al azar: una macro ausente es un error de compilación
  y no una sorpresa en ejecución.

- **Los 8 bits bajos del dithering llegan por fin al lazo.** v1.04 sacó la salida
  de 24 bits y dijo, en este mismo registro, que todavía no daba al lazo pasos
  más finos: cada algoritmo llamaba a `gpsdo_dac_write16()`, que desplazaba el
  valor a los 16 bits altos para que los ajustes guardados conservaran su
  tensión, y el byte bajo era siempre cero. Ya no lo es.

  La fracción pertenece a `gpsdo_dac.cpp`, no al lazo de control, y ahí está todo
  el diseño. El valor de control se escribe desde 21 sitios — los barridos `CT` y
  `LC`, las rampas de adquisición, el gobierno en holdover, `SP` y el propio lazo
  — y veinte de ellos son gruesos a propósito: un barrido que acaba en 30720,4 en
  lugar de en 30720 no es un barrido mejor, es uno cuyo punto de referencia nadie
  sabe enunciar. Toda escritura gruesa borra la fracción como efecto secundario
  de pasar por `gpsdo_dac_write16()`, de modo que ningún llamante tiene que
  acordarse. Guardar la fracción en el lazo habría significado veinte sitios que
  debían saber ponerla a cero — que es exactamente la clase de fallo para la que
  se creó ese único punto de escritura.

  Lo que compra, sobre la planta medida aquí: un paso de 16 bits son unos 320 µHz,
  o sea 3,2e-11 de 10 MHz — más grueso que los 4e-12 que se midió al lazo
  manteniendo a lo largo de 10 000 s. Llegaba ahí ditherando entre códigos
  contiguos de una corrección a la siguiente, lo cual funciona pero deja la
  tensión de control cazando. Con la fracción conservada, una corrección menor
  que un paso se aplica en lugar de truncarse, y el paso pasa a 1,25e-13.

  El truncamiento que desaparece era además sesgado: `(int32_t)` redondea hacia
  cero, así que toda corrección perdía parte de sí misma en la misma dirección —
  lo que el lazo lee como un error de ganancia de hasta un sexto en las
  correcciones de 6 LSB que se ven en operación normal.

  Por encima de la capa DAC no cambió nada. `gpsdo_dac_last16()` sigue devolviendo
  un `uint16_t` liso, de modo que las pantallas, la línea de telemetría y el
  anillo en flash ven exactamente lo que veían antes, y el bloque de ajustes sigue
  guardando 16 bits: una restauración arranca con fracción cero y cede como mucho
  1,25e-13, por debajo de lo que este hardware puede mostrar.
- **`DAC` — una orden que dice qué es realmente la tensión de control.** La ruta
  de salida y, para el dithering, su frecuencia portadora y la RAM de las tablas;
  el código en tres vistas — 24 bits, los 16 bits redondeados que usan las
  pantallas y el anillo en flash, y el valor fraccionario exacto con su diferencia
  respecto al redondeado; el Vctl medido; y el tamaño de paso en ambas anchuras,
  en µHz y como fracción de 10 MHz. Un código de 24 bits que no es múltiplo de 256
  es la prueba de que quien gobierna el pin es la ruta fina, y por eso se imprimen
  las tres vistas y no una; la orden dice además con todas las letras si la ruta
  fina está activa o si la salida la está redondeando.

  Las cifras de paso necesitan la ganancia de la planta, que sólo `CT` puede
  aportar. Sin ella la orden lo dice, en vez de imprimir un número derivado de un
  valor por defecto. Figura también en la pestaña Help del tuner.

- **`MF` y `MFT` — los límites por nivel reciben su propia fuente, elegida con
  independencia de la ganancia.** Ambos compartían un solo `if`, así que `MG 0`
  significaba «ganancia de CT **y** límites de la fórmula de ruido» y `MG > 0`
  «ganancia a mano **y** límites a mano». No hay razón para que estén soldados:
  la ganancia pertenece al OSCILADOR — es LSB por ns, y otro OCXO tiene otra
  sensibilidad de Vctl — mientras que los límites pertenecen al RUIDO DE FASE que
  ve la placa, que es propiedad del emplazamiento y del receptor. «Ganancia
  medida, límites puestos a mano», que es lo que quiere una instalación ruidosa,
  no podía expresarse en absoluto.

  `MF 0` sigue a `MG` como hasta ahora y es el valor por defecto, así que nada se
  mueve mientras nadie lo pida. `MF 1` mantiene la tabla guardada, `MF 2` la
  fórmula de ruido, `MF 3` la tabla medida de abajo. Ambos ajustes caben en tres
  bytes de relleno que el bloque de algo-12 ya tenía, de modo que la disposición,
  el tamaño y `SETTINGS_VER` quedan intactos y un bloque guardado por una versión
  anterior sigue cargando — se lee como 0/0, que es exactamente el comportamiento
  que tenía aquella versión.
- **`MF 3` — límites por nivel medidos en lugar de extrapolados.** La fórmula es
  `thr[L] = 8·σ·√(2^L)·√10`, y `√(2^L)` afirma que la fase es BLANCA, de modo que
  promediar 2^L muestras reduce el test como 2^(L/2). Medido en dos placas de
  este diseño — mismo PCB, mismo OCXO, habitaciones distintas — el exponente es
  **0,95 y 1,03**, no 0,50. Promediar no compra casi nada aquí, porque lo que
  importa es una deriva lenta (autocorrelación 0,96 a 60 s, 0,64 a 300 s) y no el
  ruido muestra a muestra. El error se agrava con el nivel: la fórmula subestima
  la dispersión real unas 5 veces en el nivel 0 y más de 100 en el nivel 10, así
  que su tabla cae 32 veces a lo largo de la jerarquía donde la propia fase cae
  1,3.

  De modo que el exponente se mide. Cada nivel guarda el valor cuadrático medio
  de su propio estadístico de test, un ajuste por mínimos cuadrados de log2(sd)
  frente al nivel da amplitud y exponente a la vez, y la tabla se construye del
  ajuste. Ajustar A TRAVÉS de los niveles, en lugar de fiarse de cada uno por
  separado, es lo que lo hace utilizable pronto: el nivel 8 se evalúa una vez
  cada 512 s y necesitaría medio día para tener varianza propia, pero los niveles
  bajos se pueblan en minutos y el ajuste extrapola.

  Con ello desaparecen cinco números fijados a mano: el exponente 0,5, el
  multiplicador `8.0` (ahora el cuantil normal para la tasa de disparos por ruido
  que enuncia `MFT` — que es el trabajo que el 8 hacía a mano, porque la
  jerarquía prueba el nivel 0 mil veces más a menudo que el 10), la propagación
  de ruido blanco `√10`, el suelo de σ en 5 ns — propiedad de este detector, no
  de la aritmética — y el suelo de 100 unidades. Queda un número con significado
  físico: cuánto tiempo entre correcciones disparadas sólo por ruido.

  El exponente se limita a [0,5; 1,0] y eso es física, no gusto. Por debajo de
  0,5 el promediado quitaría más de lo que el ruido blanco permite; por encima de
  1,0 la dispersión crece más deprisa que plana-en-ns, lo que es una RAMPA de
  fase y no una placa más ruidosa — y dejar que una rampa suba el umbral es el
  fallo ya registrado en este archivo, donde sigma trepó de 165 a 746 ns y el
  lazo se congeló.

  Verificado reproduciendo la aritmética del propio firmware sobre los registros
  de ambas placas: la tabla del taller sale en 74 ns cayendo hasta 14, que es
  donde esa placa quedó ajustada a mano después de que el modo automático
  resultara inestable, y la placa de casa reproduce su propio comportamiento
  asentado. En una serie de tres horas la de casa ajustó **α = 1,00** y corrigió
  en los niveles 5 a 9 — la primera vez que esta jerarquía usa más de uno o dos
  de sus niveles.

  **No es automáticamente mejor.** En la placa de casa, donde la tabla mucho más
  ceñida de la fórmula casualmente encajaba con un emplazamiento tranquilo, la
  tabla medida duplica el RMS de fase (mediana de 11,6 ns frente a 5,5 ns sobre
  una ventana de la misma longitud) porque corrige un tercio de veces. Las dos
  tablas hacen preguntas distintas — la fórmula pregunta si una desviación supera
  el ruido de medida, la tabla medida si es inusual para esta placa — y cuál de
  las dos acierta depende del emplazamiento. Para eso está `MF`.

### Cambiado
- `LOCK` en el campo de tendencia significa ahora que la jerarquía está tranquila
  **y** que la frecuencia de TIM2 se mantiene dentro de 0,05 Hz, contado sobre
  segundos tranquilos consecutivos y no sobre `s_mla_count`, que se reinicia en
  cada corrección y era un mal indicador de cuánto hacía que había pasado algo.

- **`GPSDO_PWM_DITHER` está activado en la configuración que se distribuye.**
  Salió en v1.04 desactivado, mientras se comprobaba la ruta de salida; cerrada
  ya la ruta fina y con una serie de 23 horas detrás, «desactivado» deja de ser
  el valor por defecto honesto. Comentarlo sigue devolviendo al PWM sencillo de
  16 bits, y el pin, el filtro y el cableado son los mismos en ambos casos.
- **La disposición de campos del panel 320×240 coincide ya con la del 480×320.**
  Este manual viene diciendo desde v0.93 que la pantalla de trabajo se diseña una
  vez y se escala, y eso era cierto de la geometría y no del contenido: los dos
  paneles se habían ido separando campo a campo. qErr sube a la fila de Alt, junto
  a los datos de fix a los que pertenece; AHT y el campo de fase intercambian
  columnas, de modo que los sensores ambientales comparten la columna izquierda y
  los eléctricos la derecha; Vcc y Vdd ocupan juntos la fila que quedó libre. El
  panel pequeño muestra todo lo que muestra el grande.

  Todo campo que combinaba una etiqueta con un valor de anchura variable se partió
  en dos. Una sola cadena anclada a la derecha fija la unidad y arrastra la
  etiqueta de lado a medida que cambia la anchura de los dígitos — en qErr se veía
  como una etiqueta que se movía una vez por segundo. Etiqueta y valor son ahora
  ranuras distintas con relleno distinto: la etiqueta sujeta el borde izquierdo de
  la columna, el valor conserva el anclaje derecho y sólo cambia el hueco entre
  ambos. Igual para `dph` y para la corriente del INA.
- **La fuente 2 es proporcional, y esta disposición se había calculado a 8 px por
  carácter.** Contrastado con la propia tabla de anchuras de la librería, eso
  exagera las cadenas del panel pequeño en torno a una quinta parte — `Vph:1.951V`
  mide 70 px, no 80. El error no era académico: es lo que había costado la
  etiqueta `dph` y lo que mantenía Vcc en dos decimales donde el 480 muestra tres.
  Ambas cosas han vuelto. Los campos de la derecha comparten ahora una única línea
  de alineación en x=314 — aquella a la que Vdd ya estaba anclado — de modo que
  qErr, dph, la corriente del INA y Vdd forman una columna en lugar de cuatro
  casi-aciertos. El relleno de cada campo es ahora la anchura medida de su propia
  forma más ancha, y no la de la lectura de hoy, y los rellenos de una fila la
  embaldosan exactamente, así que ningún fondo puede borrar el borde del vecino.

### Créditos
- **Alan Cashin** (MIS42N en el foro EEVBlog) aparece ya acreditado donde el
  trabajo es suyo: en `V`, en la cabecera de la ayuda, en la pantalla About del
  sintonizador y en la tabla de créditos de los tres manuales. El algoritmo 12,
  la corrección por cruce por cero, el PWM con dithering y la idea de
  autoevaluación `CS` proceden todos de su Budget GPSDO. Hasta ahora figuraba
  como «dither / DAC discussion», lo que se quedaba bastante corto.

### Medido
Veintitrés horas, umbrales automáticos, `MR 9`, dithering a 13 bits:

| | esta serie | mejor anterior |
|---|---|---|
| fase RMS, ya asentada | **5–8 ns** | 10–23 ns |
| \|fase\| < 10 ns | **86,7%** de las muestras | — |
| rearmados del picDIV | **1** | 121 |
| niveles de corrección alcanzados | **5–6 típico, hasta 8** | 0 |
| frecuencia a 10 000 s | **4e-12** | 1,4e-11 |
| intervalo entre correcciones | 254 s | 130 s |

`NOPH` tres veces en 82 572 muestras; `FLL` una. La presión ambiente cayó 4 hPa a
lo largo de la serie y el lazo no reaccionó.

---

## [v1.04-rtos] — 2026-08-12

### Añadido
- **`GPSDO_PWM_DITHER` — tensión de control de 24 bits a partir de un PWM corto con dithering.**
  Idea de Alan Cashin (MIS42N): haz correr el PWM con menos bits de los que
  necesitas y varía el ciclo de trabajo de un periodo al siguiente, de modo que
  sea la media la que lleve el resto.

  La ganancia es la PORTADORA, no los bits de más. El rizado hay que filtrarlo por
  debajo de un paso de salida, y lo difícil que eso resulte depende de la
  separación entre la portadora y el codo del filtro: el PWM de 16 bits a 2 kHz
  admite un codo de 0,7 Hz y una constante de tiempo de 230 ms, mientras que el
  dithering de 13 bits a 12,2 kHz admite 4,2 Hz y 38 ms. El retardo del filtro
  entra en el lazo directamente como desfase, así que un filtro seis veces más
  corto vale más que la resolución.

  Alan hace el dithering en una interrupción de temporizador porque un PIC no
  tiene DMA. Aquí serían 12 000 interrupciones por segundo compitiendo con la
  captura del 1PPS — la única interrupción que no puede retrasarse. Pero el patrón
  para un valor constante es periódico, así que se calcula una vez en una tabla y
  se reproduce por DMA hacia el registro de comparación: 0,012% de CPU a 13 bits,
  y nada de ello dentro de una interrupción. La media es exacta por construcción
  — la tabla contiene exactamente Y entradas de valor X+1 entre 2^(24-N).

  El mismo pin que antes (PB9, TIM4 CH4), así que el filtro y el cableado
  existentes no cambian. TIM4_UP gobierna DMA1 Stream 6 Channel 2; el tic de 2 Hz
  está en TIM9 y la cadena del 1PPS en TIM2/TIM3, de modo que no se perturba nada
  más. Dos búferes en modo doble búfer por hardware hacen que un cambio de valor
  nunca produzca un glitch en el pin.

  Desactivado por defecto. Cuesta 8 KB de RAM a 13 bits, 16 KB a 12.

  **Lo que todavía no da** es un paso más fino para el lazo: cada algoritmo llama
  a `gpsdo_dac_write16()`, que desplaza el valor a los 16 bits altos para que los
  ajustes existentes conserven su tensión. Los 8 bits bajos esperan a un lazo que
  llame a `gpsdo_dac_write24()`.
- **Corrección en el paso por cero, del diagrama de flujo de Alan.** Tras una
  corrección por límite que cambia la frecuencia, la fase sigue moviéndose en la
  dirección que ya llevaba: barre a través de cero, sale por el otro lado y
  normalmente vuelve a incumplir el límite — así que el lazo corrige, se pasa,
  corrige de vuelta y se asienta despacio.

  El instante en que la fase cruza cero es especial. El error de fase es nulo,
  pero el error de frecuencia que la llevó hasta allí sigue presente; cancelar el
  error de frecuencia exactamente entonces deja al oscilador con la frecuencia
  correcta Y sin error de fase, en lugar de en un estado hacia el que el lazo
  tiene que iterar.

  Medido contra los propios registros de Alan del mismo diseño: su lazo corrige
  cada 506 segundos donde este corregía cada 130. La mayor parte de esa diferencia
  es precisamente esta prueba, que él describe como esencial y que aquí faltaba.

  Se informa como `zc=` en la telemetría; la tendencia muestra `ZC` en el momento
  en que actúa.
- **Algoritmo 12 — acumulador multinivel.** Según el diseño de Alan Cashin
  (MIS42N en EEVblog). Cualquier otro lazo aquí tiene una única constante de
  tiempo, y esa constante es un compromiso que nadie gana: medido contra una
  referencia de rubidio, `LTC 60` es hasta 1,58× mejor por encima de 800 s
  mientras que `LTC 240` es hasta 1,44× mejor entre 10 y 400 s. Hay que elegir.

  Este algoritmo no elige. Las lecturas se acumulan en niveles — el nivel n abarca
  2^n segundos — y la corrección se aplica en el nivel **más bajo** cuyo error
  supere su límite. Un error grande actúa en dos segundos; uno pequeño espera a un
  promedio más largo. No hay `LTC` que ajustar.

  Los niveles salen del patrón de bits del contador de segundos, no de una matriz
  de búferes: once niveles, de 2 s a 2048 s, por 22 bytes.

  **La entrada es la fase en nanosegundos del detector LTIC.** La primera versión
  alimentaba el error de cuenta de TIM2 en hercios enteros y estaba ciega: un
  oscilador disciplinado se sitúa muy por debajo de 1 Hz, así que ese campo leía
  cero en el 83% y el 95% de las muestras en dos ejecuciones. La fase se integra
  donde una cuenta de frecuencia de un segundo no lo hace. Alan preguntó por qué
  se habían citado 100 ns cuando un TIC resuelve 1 ns; tenía razón, era la
  resolución del contador y no la del detector.

  **La prueba de frecuencia se ha eliminado**, siguiendo el consejo del propio
  Alan: *«Fue un experimento... lo que queremos es un sistema estable donde las
  pruebas siempre pasen. Así que la prueba de frecuencia es innecesaria.»*

  Nuevos comandos `MG`, `MR`, `MLP` y `ML`, guardados con `ES ALGO12`. Los límites
  por nivel son editables y persistentes porque solo **uno** se dedujo alguna vez
  — 125 ns a 128 s, de la especificación original de 10 MHz ±0,01 Hz. Alan
  describe el resto como arbitrarios.
- **Informe de la causa del reinicio al arrancar.** `RCC->CSR` se lee y decodifica
  antes de que se ejecute cualquier otra cosa, de modo que un reinicio
  intermitente ya no parece idéntico venga de una caída de tensión, del pin de
  reset o de un reinicio por software. Añadido después de que una placa se
  reiniciara repetidamente en el mismo punto de la configuración del GPS sin forma
  de saber cuál era.
- **Ganchos de fallo de FreeRTOS y un `STM32FreeRTOSConfig.h` propio.**
  `configCHECK_FOR_STACK_OVERFLOW` y `configUSE_MALLOC_FAILED_HOOK` valen 0 por
  defecto, así que una pila desbordada corrompe en silencio a su vecina y
  `configASSERT` queda atrapado en un `for(;;)` con las interrupciones
  deshabilitadas — un panel blanco muerto y nada en la consola. Así se
  presentaron exactamente los tres últimos fallos: una pila de CLI demasiado
  pequeña para la escritura del anillo de flash, un grupo de eventos NULL leído
  antes de arrancar el planificador, y una estructura declarada en una rama muerta
  que aun así agrandó el marco de la tarea de pantalla.

  La anulación activa ambos ganchos y redefine `configASSERT` para imprimir
  fichero y línea antes de detenerse. Los ganchos nombran la tarea culpable en la
  consola USB y parpadean el LED, de modo que el siguiente se identifica solo en
  segundos. `Serial` a secas, no `OUT_SERIAL`: un gancho no debe tocar un mutex ni
  un flujo Bluetooth que podría ser lo que ha fallado.

  Autoría del fichero: GLM-5.2, adoptado aquí prácticamente tal cual.

### Corregido
- **Los umbrales del algoritmo 12 ahora se miden, no se heredan.** Se tomaron del
  diseño de Alan y se escalaron por la razón entre pasos de contador, que es la
  magnitud equivocada: lo que un umbral debe superar es el **ruido** de la medida
  de fase, y ese difiere entre montajes por razones que un tamaño de paso no
  captura. Medido en esta placa: fase media −1 ns con una desviación típica de
  462 ns — el oscilador estaba bien ajustado y toda esa dispersión era ruido,
  mientras el umbral del nivel 0 estaba en 462 ns. El 41% de las muestras lo
  cruzaba. 620 correcciones en 1685 segundos, la jerarquía reiniciándose cada
  2,7 s y sin alcanzar nunca el nivel 2.

  El firmware estima ahora el ruido de fase de forma continua y fija con él el
  umbral de cada nivel. Al hacerlo apareció un segundo error: el umbral se aplica
  a la expresión de prueba |3b − a|, cuya desviación es sigma·√(2^L)·√10, y no a
  la fase media, cuya desviación es sigma/√N. Usar la segunda dejaba el umbral 4,5
  veces demasiado bajo en el nivel 0 y peor arriba. Seis sigma sobre la magnitud
  correcta lleva el intervalo entre correcciones a alrededor de un minuto, frente
  a los 256 s en los que se asienta el diseño de Alan.

  `ML` informa del ruido medido y de si los límites lo siguen. La telemetría lo
  lleva como `sig=`. Poner `MG` por encima de cero detiene el autoajuste.
- **El algoritmo 12 ignoraba la polaridad del detector y confundía nanosegundos
  con hercios.** Dos fallos en la misma conversión, hallados juntos en un registro.

  `LPOL -1` no se aplicaba en absoluto — el algoritmo 11 multiplica su término de
  fase por `-polarity` y este no — así que en una placa así cada corrección iba en
  sentido contrario. Y la fase media, en nanosegundos, se multiplicaba por
  LSB-por-hercio como si fueran la misma magnitud: anular P ns en T segundos exige
  P/(100·T) Hz a 10 MHz, de modo que la corrección salía 100·T veces demasiado
  grande, de 200× en el nivel 0 a 102 400× en el nivel 9.

  Realimentación positiva cuatro órdenes de magnitud demasiado fuerte describe
  bien lo que mostró el registro: 6000 cuentas de oscilación del PWM en 148
  correcciones.
- **`MG` y `MR` se aceptaban y guardaban pero nunca se leían.** Los comandos
  funcionaban, el afinador los enviaba, `ML` los listaba, y el algoritmo no usaba
  ninguno. Ambos están ahora conectados.
- **El algoritmo 12 exige ahora el detector LTIC y se detiene en lugar de
  adivinar.** Estaba escrito para recurrir a integrar el error de cuenta de TIM2
  en placas sin detector. Esa vía era activamente destructiva: la cuenta está
  cuantizada a hercios enteros y lee cero en un oscilador disciplinado, así que
  integrarla producía un paseo aleatorio de ruido de cuantización en vez de fase.
  Medido: 6000 cuentas de oscilación del PWM en 148 correcciones, con el detector
  contra el raíl el 58% del tiempo y la fase informada clavada en 0.

  `LA 12` ahora rechaza sin `GPSDO_LTIC`, y cuando el detector está montado pero
  no lee, el algoritmo se detiene y deja trabajar al puente del picDIV.
- **El algoritmo 12 ahora arma el picDIV.** No lo hacía, y el fallo era silencioso:
  con la rampa contra el raíl el detector nunca devuelve una lectura válida, así
  que el código recurría a integrar la cuenta y volvía a estar ciego — justo lo
  que pasar al detector debía evitar. El mismo puente con espera que el algoritmo 11.
- **El afinador dejó de leer nada de la placa.** `STATE_HINT` se añadió a
  `TelemetryParser` pero se referenciaba como `self.STATE_HINT` desde
  `GpsdoTuner`, una clase distinta. Cada línea de telemetría lanzaba entonces
  `AttributeError`, así que ninguna respuesta llegaba a su absorbedor y ni los
  campos de calibración de `LL` ni la tabla de límites del algoritmo 12 se
  rellenaban. Dos síntomas, un solo fallo.

  El manejador va ahora envuelto: un error de análisis cuesta una línea, no la
  recepción entera.
- **`MG` y `LG` respondían igual, con `gain=`.** El absorbedor de Lars corre
  primero y capturaba la respuesta del algoritmo 12. El firmware responde ahora
  `m_gain=` y `m_run_level=`.
- **La tabla de límites del afinador mostraba ceros.** Nunca se leía: la consulta
  de parámetros pedía solo los escalares. Ahora se lee al conectar y el envío se
  niega mientras alguna fila siga a cero.
- **La placa no arrancaba: sin LED, sin consola, nada.** `setup()` escribe el DAC
  tres veces antes de que se ejecute `xEventGroupCreate()`, y la compuerta de las
  nuevas estadísticas leía `xSysEvents`, todavía NULL. Además el marco de pila se
  dimensiona al compilar, así que una estructura en una rama que nunca se ejecuta
  reserva su espacio igualmente; veinte bytes desbordaron la tarea de pantalla y
  murió antes de `tft.init()`.

### Cambiado
- **`SETTINGS_VER` 4 → 5** para el bloque del algoritmo 12, **con migración**. Un
  bloque v4 se acepta, sus campos se aplican y los valores del algoritmo 12 quedan
  por defecto. Rechazarlo habría descartado un PID, un LC y una zona horaria que
  funcionaban solo porque se añadió un algoritmo.

## [v1.03-rtos] — 2026-08-01

Construido sobre v1.01. Los experimentos de v1.02 — un DAC delta-sigma en PB5 y
el soporte del core STM32duino 3.0.0 — no se trasladan: el primero se midió y no
entregaba lo prometido, el segundo colgaba la placa en hardware real. v1.01 sigue
siendo la base probada, con dos añadidos.

### Corregido
- **Un reinicio en caliente ya no reinicia un survey-in terminado.** El receptor
  conserva su propia alimentación y su propio estado a través de `RB`, así que un
  survey completado antes del reinicio sigue siendo válido: la posición que
  estableció no se ha movido. El firmware ordenaba antes un survey nuevo de todos
  modos, descartando un resultado que costó minutos y sacando al módulo del Time
  Mode mientras repetía trabajo ya hecho. `gpsdo_gps_init()` consulta ahora primero
  TIM-SVIN y omite el arranque cuando el receptor informa valid=1 con active=0 —
  Time Mode con un survey terminado detrás. Se informa como *already in Time Mode
  from an earlier survey*.

  Esto exigió hacer `ubx_poll_svin()` seguro de llamar antes del planificador:
  cedía con `vTaskDelay()` sin condición, lo que cuelga el sistema cuando no hay
  planificador en marcha. Ahora usa `delay()` en ese caso, el mismo patrón que ya
  empleaba el lector de ACK.
- **La placa no arrancaba: sin LED, sin consola, nada.** `setup()` escribe el DAC
  tres veces — el 127 inicial, el PWM recuperado y el valor por defecto — antes de
  que se ejecute `xEventGroupCreate()`. Las nuevas estadísticas de corrección
  cuelgan de la ruta de escritura del DAC, y su compuerta leía `xSysEvents`, que
  en ese punto seguía siendo NULL. Pasar NULL a `xEventGroupGetBits()` dispara
  `configASSERT` y detiene el procesador, así que el fallo ocurría antes del primer
  parpadeo y no dejaba nada en la consola que lo explicara. La compuerta comprueba
  ahora NULL primero; esas escrituras tempranas son órdenes, no correcciones, así
  que excluirlas es correcto además de seguro.

### Añadido
- **`CS` — estadísticas de corrección, el lazo evaluándose a sí mismo.** El
  algoritmo 11 se validó contra un patrón de rubidio en el banco de otra persona;
  casi nadie que construya esto tiene uno, y sin él quedan la palabra del autor y
  un indicador de enganche. La corrección que aplica el lazo es el error que acaba
  de observar, así que el tamaño de esas correcciones dice si la disciplina
  funciona — y la referencia es el GPS, de modo que no existe nada mejor con lo que
  comparar la frecuencia. El firmware ya calculaba estos números y los descartaba.

  Informa del RMS de la corrección sobre las últimas **100, 1 000, 10 000 y
  100 000 correcciones**, en cuentas de DAC y — una vez que `CT` ha medido la
  pendiente del oscilador — en frecuencia fraccional, directamente comparable con
  una cifra de ADEV. También el sesgo constante, no nulo cuando el lazo sigue una
  deriva real y no ruido.

  Las ventanas cuentan correcciones y no segundos porque el ritmo de corrección
  depende del algoritmo: el algoritmo 11 corrige una vez por segundo, el 10 una vez
  por `LIV`. Etiquetarlas en minutos habría significado una cosa con un algoritmo y
  sesenta veces eso con el otro — el mismo número describiendo dos periodos
  distintos. `CS` mide el intervalo real e imprime lo que abarcan las ventanas en
  tiempo de reloj, para que el lector no tenga que calcularlo. A una corrección por
  segundo, 100 000 cubre unas 28 horas.

  Son pesos exponenciales, no ventanas duras: alrededor del 63% del peso cae dentro
  de N correcciones y el 95% dentro de 3N. Eso cuesta cuatro multiplicaciones-suma
  por corrección y nada de memoria, mientras que un búfer de 100 000 muestras
  ocuparía la mayor parte de la RAM para responder lo mismo sin mejorarlo.

  Se cuenta solo con el lazo enganchado y sin calibración en curso: la rampa de
  adquisición, los tres saltos de `CT` y el barrido de `LC` son órdenes, no
  correcciones, y uno solo dominaría la media horaria mucho después de terminar.
  Los algoritmos 0-9 no tienen estado de enganche sobre el que basar la compuerta y
  quedan excluidos, cosa que `CS` dice en lugar de informar de un número sin
  significado definido.

  **La advertencia está en la salida, en la cabecera y en el README:** mide si el
  LAZO ESTÁ ASENTADO, no si la SALIDA ES BUENA. Un detector ruidoso hace que el
  lazo persiga ruido; las correcciones crecen, `CS` las informa fielmente, y el
  oscilador estaba bien hasta que el lazo lo empeoró. Nada medido desde dentro del
  lazo puede ver eso.

  La idea es de Alan (MIS42N en EEVblog), cuyo propio diseño se apoya justamente en
  esto y por eso no necesita un patrón secundario.
- **`GPSDO_DAC_EXT` — DAC SPI externo, planeado, no implementado.** Activarlo da un
  error de compilación deliberado: `dac_ext.cpp` es un esqueleto sin dispositivo
  elegido. El PWM de 16 bits da unos 50 µV por paso a 3,3 V, cerca de 2,7e-11
  fraccional en un oscilador de 5,3 Hz/V; un integrado de 18 bits con una
  referencia diseñada para el trabajo alcanza unos 17 µV, cerca de 9e-12, sin
  retardo de filtro dentro del lazo.

  No hace falta SPI por hardware, ni está disponible — SPI1 pertenece al TFT y
  todos los pines de SPI2 de este encapsulado están ocupados — pero el DAC se
  escribe una vez por segundo, así que moverlo por software cuesta microsegundos.
  Pines propuestos: PB0, PB2 y PB4, elegidos para evitar PB6/PB7, que parecen
  libres pero son los pines por defecto de I2C1 que reclama `Wire.begin()`; un DAC
  ahí rompería los sensores y la pantalla de reloj.

### Cambiado
- **Las 23 llamadas a `analogWrite(PIN_VCTL_PWM, ...)` pasan ahora por
  `gpsdo_dac_write16()`.** Añadir una segunda ruta de salida editando cada una
  habría invitado a olvidar alguna, y una llamada olvidada es el peor tipo de fallo
  aquí: el lazo dirigiría bien casi siempre y daría un salto cada vez que se tomara
  la ruta antigua. Añadir un DAC significa ahora rellenar una función.

## [v1.01-rtos] — 2026-07-29

> **Compila con el core STM32duino 2.12.0 o anterior.** El core 3.0.0 (23 de
> julio de 2026) despliega ArduinoCore-API, lo que elimina `ltoa()` y convierte
> `HardwareSerial` en una interfaz abstracta — ambos se usan aquí — y, más
> importante, deja a TFT_eSPI sin poder inicializar el panel (pantalla en blanco,
> el CLI no se ve afectado). Los dos primeros son menores y podrían condicionarse
> por versión; el tercero reside en la biblioteca. Detalles en el README.

Versión hito: fusiona la rama de persistencia por flash-ring con la rama del
algoritmo 11 (LTIC-Lars). El algoritmo 11 se basa en el controlador GPSDO
original de lazo PI continuo del difunto **Lars Walenius**, compartido con la
comunidad time-nuts; se amplía aquí con la autocalibración y la adquisición
descritas abajo, en su memoria.

### Añadido
- **Algoritmo 11 "LTIC-Lars"** — un único lazo PI continuo (sin máquina de estados
  ACQ/DPLL/LOCK), que disciplina el OCXO desde la fase del TIC por hardware.
  Seleccionable con `LA 11`; tendencia LFQ (guiado por frecuencia) / LPH (fase) /
  LLK (enganchado). Ajustable en vivo con LG/LD/LTC/LFD/LTO/LPL/LPF/LTK/LTR.
- **Autocalibración por CT para el algoritmo 11.** gain vale por defecto 0 = auto:
  el lazo deriva su escala de frecuencia de la K medida por CT (Hz por LSB de PWM),
  la misma constante que usa el algo 10, así que una CT calibra también el lazo de
  Lars. Un LG no nulo lo sobrescribe con una escala manual.
- **Adquisición guiada por frecuencia** con un término proporcional dominante y
  autofrenante, un límite de paso y anti-windup, para que un arranque en frío
  enganche sin la fuga ni la oscilación de ±2 Hz vistas durante el desarrollo.
- **Puente de captura de fase picDIV**: cuando la frecuencia está asentada pero la
  fase sigue saturada, se rearma el picDIV una vez para llevar la fase a la ventana
  del detector, donde la rama de fase completa el enganche.
- **Tuner: versionado y emparejado con el firmware.** Las herramientas llevan
  ahora un TOOL_VERSION que sigue la versión del firmware, y el tuner lee la
  versión de la placa al conectar: una discrepancia se informa en la barra de
  estado y en el monitor, en vez de manifestarse como campos que se leen raro. La
  ventana principal se abre maximizada con el splash encima, y en Windows la
  consola que aparece al hacer doble clic en el script se minimiza a la barra de
  tareas (solo cuando pertenece al tuner — una terminal abierta por el operador se
  deja en paz).
- **Tuner: pestaña de ayuda y splash de inicio.** El tuner incorpora una pestaña
  Help con la referencia completa de comandos agrupada por tema y un splash de tres
  segundos que anima dos senoides desfasadas convergiendo en una — la misma metáfora
  de enganche que la pantalla de arranque del TFT (un clic lo omite). Además lee
  todos los grupos de parámetros al conectar (LTIC, FA, PID 3-9, Lars) en lugar de
  solo dos, y el algoritmo 11 tiene su propia pestaña junto al algoritmo 10.
- **Persistencia por flash-ring para el algoritmo 11.** Todos los parámetros
  g_lars se guardan en el flash-ring junto a los ajustes LTIC (SETTINGS_VER 2);
  `ES LTIC` guarda ambos. Sin EEPROM en ningún sitio — persistencia 100% flash-ring.

### Cambiado
- **`LC` avisa cuando se ejecuta antes que `CT`.** No son independientes: `LC`
  necesita la pendiente en Hz por LSB que mide `CT`, y sin ella recurre a un valor
  genérico. El fallo es silencioso, no evidente — una placa informó ns_per_volt
  1592,8 antes de `CT` y 921,2 después, un factor de 1,7, sin nada en la primera
  ejecución que lo insinuara. `LC` ahora lo indica de entrada y continúa igual, y
  el README expone el orden explícitamente.
- **Cada ajuste indica ahora si se ha guardado.** Las preferencias que no tocan el
  lazo de control — zona horaria (`TZ`/`TO`/`LT`), offsets de sensores (`PO`/`AO`) y
  los indicadores de arranque y survey-in (`WU`/`SPL`/`SV`) — se guardan solas, y la
  respuesta nombra el grupo escrito. El ajuste del lazo sigue siendo manual, y su
  respuesta nombra el comando exacto que lo conservaría, p. ej.
  `[not saved — run 'ES LTIC' to keep it]`, así que nunca hay que adivinar el grupo.
  `SET_FLAGS` lleva `SAW` y `LRN` junto a los indicadores de arranque, así que un
  guardado automático los confirma también; el mensaje enumera todo el grupo en
  lugar de ocultarlo. Un valor rechazado se informa como tal —
  `[not saved — value out of range; accepted range shown above]` — en vez de
  ofrecer un comando `ES` para un cambio que nunca ocurrió.
- **`LT` ahora es persistente.** El comando estaba implementado pero no tenía campo
  en el bloque de ajustes, así que la elección UTC/local no sobrevivía a un
  reinicio. Añadido al grupo de zona horaria (SETTINGS_VER 4).
- **`CT` guarda su resultado automáticamente.** Igual que `LC`, la calibración de
  tres minutos escribe ahora sus coeficientes en el flash-ring al terminar con
  éxito, en lugar de confiar en que el operador recuerde `ES PID`. Solo se escribe
  el grupo PID, así que el ajuste del lazo en curso queda intacto.
- **Etiquetas de tendencia del algoritmo 11 renombradas** a ACQ / PLL / LOCK, en
  línea con el vocabulario del algoritmo 10, para que pantallas, CLI y tuner se
  lean de forma coherente. El algo 11 muestra PLL donde el algo 10 muestra DPLL,
  lo que sigue distinguiéndolos en un registro.
- **La telemetría Learn informa de lo que realmente controla cada lazo**: el algo
  11 muestra modo de ganancia / escala / fase filtrada, el algo 10 su máquina de
  estados, los algos 3-9 conservan las cifras LRN. qErr permanece en cada línea
  (compartido por ambas ramas LTIC).
- **El mensaje de CT** indica ahora que ajusta los algos 3-9 más LTIC 10 y 11.
- **Wrappers de persistencia renombrados** eeprom_* → persist_*, para reflejar que
  el almacenamiento es el flash-ring, no EEPROM; los nombres dejan de confundir.

### Corregido
- **El algoritmo 10 podía congelarse durante un enganche sano.** La protección
  contra fugas se disparaba solo con el detector saturado más un error de
  frecuencia grande — pero ese es el estado normal de un OCXO frío o alejado al
  principio de la adquisición, y congelarlo ahí elimina el único camino de vuelta,
  ya que el término de frecuencia es precisamente lo que lleva al oscilador a la
  ventana del detector. Una ejecución observada recorrió 3855 LSB durante un
  enganche DPLL perfectamente sano y quedó congelada a medio camino. Ambas
  protecciones exigen ahora además que el error haya dejado de mejorar durante
  varios ciclos (LTIC_RUNAWAY_STALL), algo que una fuga real por polaridad
  invertida provoca y una adquisición sana nunca. El umbral de saturación vuelve a
  tomarse de la calibración LC en lugar de unos 3,28 V fijos válidos para una sola placa.
- **`LIV` estaba limitado a 30 s.** Tanto el CLI como el lazo recortaban el
  intervalo de corrección LOCK a 30, y el lazo saltaba a 5 s ante un valor fuera de
  rango — así que pedir un lazo más lento daba en silencio el más rápido. Restaurado
  a 1..600 s, recortando al límite más cercano. Importaba de inmediato: un probador
  comparando LIV 30 con LIV 60 habría visto rechazado el 60.
- **Los ajustes nunca se guardaban realmente.** La cabecera del slot guardaba la
  longitud de los datos en un solo byte, así que todo lo que superara 255 B se
  truncaba: un bloque de ajustes de 324 bytes se registraba como 68. Los datos en
  sí se escribían bien y el CRC los cubría, así que nada parecía mal — pero cada
  lectura devolvía una longitud truncada, dejando la cola del bloque recuperado
  con lo que hubiera en la pila. De ahí venía el extraño `temp_coeff=-1`, y en
  cuanto la longitud se comprobó de forma estricta la lectura rechazó el registro y
  la placa arrancaba con los valores por defecto. El campo de longitud es ahora de
  16 bits (cabecera de slot 4 B → 6 B, datos 506 B → 504 B) y el magic del anillo
  se incrementa para que un anillo antiguo se reformatee en vez de decodificarse
  como basura. Esto afectaba a la rama flash-ring desde el principio — el bloque
  del propio GML ya medía 292 B, también por encima del límite.
- **Desbordamiento de pila al escribir el flash-ring.** Guardar los ajustes
  necesita unos 1,4 KB de pila — `fr_write()` construye una imagen de slot de 512
  bytes más una copia de verificación de 512 bytes, y `settings_store` añade un
  bloque de ~324 B — pero la tarea CLI tenía 1 KB y la de control 1,5 KB. La tarea
  CLI se desbordaba sobre su vecina y la placa imprimía la confirmación de guardado
  y luego se colgaba con la pantalla congelada. Ambas pilas se amplían con margen
  (CLI 1 KB → 3 KB, control 1,5 KB → 3,25 KB; 4 KB más de RAM de 128 KB). El riesgo
  es anterior al trabajo de guardado automático — `ES` estaba igual de expuesto —
  pero el guardado automático lo hizo fácil de alcanzar.
- **`EW` informaba del sector de flash equivocado.** El anillo siempre ha estado en
  el sector 7 (0x08060000, el último sector, para que el firmware conserve el
  máximo espacio contiguo por debajo), pero el mensaje de `EW` llevaba fijo
  «sector 6, 0x08040000» — el único sitio que mira un operador era el único que
  mentía. El mensaje lee ahora la dirección de la implementación mediante las
  nuevas funciones `flash_ring_sector_no()` / `flash_ring_base_addr()`, así que ya
  no puede desviarse. Los documentos de puesta en marcha tenían las mismas cifras
  obsoletas y se han corregido en los tres idiomas: el techo del firmware es
  393216 B (384 KB), no 262144 B, y el rango de borrado con J-Link para limpiar el
  anillo es 0x08060000-0x0807FFFF, no el rango del sector 6, que habría dejado el
  anillo intacto.
- **LC ya no descarta su propia convergencia.** El bucle de anulación de tasa
  paraba tras tres intentos y, si aún no había entrado en la banda de aceptación,
  recurría a `saved_pwm + offset` — que asume que el PWM guardado está en el punto
  de enganche. Ejecutado antes de que el oscilador esté cerca de 10 MHz esa
  suposición es muy falsa: una ejecución observada convergió -244, -57, -16 ns/s
  (a un paso de la banda), lo descartó y muestreó a un PWM que corría a -244 ns/s,
  donde la fase cruza toda la ventana del detector entre publicaciones. Cada
  armado del picDIV caía en la saturación y la calibración abortaba. El bucle tiene
  ahora seis intentos y, al agotarlos, conserva el PWM dirigido en vez de volver
  al inicio.
- **FA / FAD / FAL restaurados.** La ventana de promediado del término de
  amortiguación por estado (y el término `damp_e_freq` que alimenta en el
  algoritmo 10) existía en v0.97 pero no en la rama flash-ring, así que se perdió
  en la fusión. Restaurada, y ahora guardada en el flash-ring en lugar de EEPROM.
- **Los registros de ajustes verifican su longitud.** `settings_recall` y
  `settings_save_partial` aceptaban cualquier registro de dos bytes o más en un
  bloque de pila, así que un registro más corto que la estructura actual dejaba la
  cola como basura de pila — y un guardado parcial la reescribía. Ambos ponen a
  cero el bloque primero y exigen el tamaño exacto.
- **settings_store.cpp ya compila.** Leía tres variables globales que no podía
  ver — g_pressure_offset, g_altitude_offset (definidas en gpsdo_control.cpp, sin
  cabecera propia) y g_qerr_enable (declarada en ubx_timtp.h, que no estaba
  incluido). Se añadieron el include y los dos extern locales, siguiendo el patrón
  que usa el resto del proyecto.
- **Comando LT implementado.** La ayuda siempre documentó `LT 0|1` y las rutas de
  pantalla e informes siempre leyeron g_show_local_time, pero el manejador del CLI
  nunca se escribió, así que el verbo no hacía nada en silencio. Ahora conmuta e
  informa UTC frente a hora local, tal como promete la ayuda.
- **El dph del puerto serie coincide ahora con el panel.** La fila del TFT restaba
  el sawtooth del receptor pero el informe serie no, así que el mismo instante se
  leía distinto en cada uno — todo un sawtooth de diferencia (~±10 ns en un LEA-6T,
  más en un M8T). La ruta serie también lo resta ahora, como su propio comentario
  ya afirmaba.
- **CR (reinicio en frío) ahora sí borra el ring.** persist_erase() llama al nuevo
  flash_ring_wipe(), que borra y reformatea físicamente el sector del ring, de modo
  que un reinicio en frío vuelve de verdad a los valores por defecto en lugar de
  solo marcar el estado como obsoleto.

## [v0.95-rtos] — 2026-07-16

### Añadido
- **Zonas horarias con horario de verano, en todo el mundo.** `TZ Adelaide`
  basta para que el reloj sea correcto, incluido su desfase de media hora y su
  DST del hemisferio sur. Los nombres de ciudad se aceptan solos: son únicos en
  toda la base IANA, así que la región es opcional (`TZ Australia/Adelaide`
  también funciona) y no importan las mayúsculas.

  La regla también puede escribirse completa:
  `TZ ACST-9:30ACDT,M10.1.0,M4.1.0/3`. Esa forma importa cuando un gobierno
  cambia las reglas y el firmware aún no lo sabe: el usuario lo corrige desde
  la CLI en lugar de esperar una versión.

  407 zonas y 88 reglas integradas, generadas desde la tzdata del sistema por
  `tools/gen_tz_table.py`. La base IANA completa ocupa ~2 MB, cuatro veces toda
  la flash de este MCU, y su valor real está en actualizarse varias veces al
  año, algo que un GPSDO sin internet no puede aprovechar. La cadena POSIX TZ a
  la que se reduce cada zona ocupa 4–44 bytes y recoge el mismo comportamiento
  actual. Coste: ~7 KB de flash.
- **`H TZ`** — primera página de ayuda por comando.
- **`TO` acepta minutos**: `TO 9:30`, `TO -3:30`, `TO 5:45`.
- **Vcc en pantalla (480×320).** Pedido por Dan Wiering. El raíl de 5 V ya se
  medía pero no tenía hueco; la celda `Alt` cede su mitad derecha y los campos se
  reagrupan: `qErr` sube junto a `Alt` (es lo que el receptor informa de su
  propio 1PPS) y `Vcc` ocupa el sitio que deja junto a `Vdd`, de modo que las
  alimentaciones quedan juntas. `Vdd` recupera su segundo decimal. Solo en 480:
  en 320 `Alt` y `qErr` piden ~168 px y la celda tiene 148.
- **`Vdd` solo se veía en compilaciones con LTIC.** Vive en la fila de fase, y la
  fila entera estaba tras `#ifdef GPSDO_LTIC`. Ahora los raíles quedan fuera de
  esa guarda: `Vcc` y `Vdd` se muestran siempre; solo el campo de fase sigue
  condicionado.

### Corregido
- **Reportado por Dan Wiering: la zona automática no detectaba el DST en
  Australia Meridional.** Dos errores distintos, solo uno visible. `TO A`
  deduce la zona de la longitud y aplica la regla europea, así que fuera de
  Europa no daba DST alguno. Pero además devolvía horas enteras, y Adelaide es
  UTC+9:30, así que el reloj erraba media hora incluso en invierno. India
  (+5:30), Nepal (+5:45), Terranova (−3:30) y Chatham (+12:45) sufrían el mismo
  error silencioso. `TZ <zona>` resuelve ambos; `TO A` sigue disponible sin
  cambios.
- **La lectura de frecuencia saltaba lateralmente en el panel 320×240.** v0.94
  quitó el ancho de campo de `dtostrf`; una cadena que pierde un carácter se
  recentra igualmente. El ancho de campo ha vuelto, como estaba desde v0.89. El
  panel 480×320 no se toca.
- **Los raíles laterales desaparecían junto a la frecuencia.** El sprite borra
  toda su banda y solo dibujaba la línea separadora superior. Ahora también
  lleva los raíles. Ambos paneles.
- **`CT` mostraba «Tune 0s» durante toda su ejecución.** No inicializaba la
  cuenta atrás, a diferencia de `C` y `LC`. Son 185 s.
- **`qErr` se desplazaba en el panel 480×320.** Anclar toda la cadena a la
  derecha fijó la unidad pero arrastró la etiqueta `qErr:` con los dígitos.
  Etiqueta y valor son ahora dos campos: la etiqueta pegada al borde izquierdo
  del hueco, el valor anclado a la derecha en 364.

### Cambiado
- **`dph` mostraba un número seguro mucho después de que el detector dejara de
  medir.** `ns_per_volt` es una pendiente local alrededor del ancla en 0.632·Vsat;
  pasada Vsat el pulso de parada ha fallado la ventana y el condensador carga
  hasta la alimentación. `dph` muestra ahora `ovf` fuera del 15–85% de Vsat.
  Vsat se recupera de `zero_offset`, que es 0.632·Vsat por construcción.
- **`dph` en pantalla nunca restaba el diente de sierra.** La pantalla calculaba
  la fase por su propia vía y se saltaba la corrección que el lazo aplica en
  `ltic_phase_error_ns()`: ~14 ns de dispersión 1σ medidos en el equipo. Ahora
  también la resta. Importa sobre todo fuera del algo 10, que es donde `dph` es
  la única vista de la fase real. El campo `qErr` deja de estar condicionado al
  algo 10.
- **Cada columna tiene una línea de alineación derecha (480×320).** La izquierda
  termina donde «hPa» en la fila BMP, la derecha donde «ns» en la fila de fase.
  `Vct`, `% rH` y la corriente del INA se anclan ahora a esas líneas. Las líneas
  se miden con `textWidth()` en el primer uso en vez de fijarse como constantes.
- **Las filas de sensores se agrupan por columna (480×320).** BMP y AHT ocupan
  la columna izquierda y los campos eléctricos la derecha: la fase arriba y los
  raíles justo debajo. `AHT` y `Vph`/`dph` intercambian su sitio.
- **`dPh:` pasa a `dph:`**, para coincidir con `Vph:`. Cambiado a la vez en el
  TFT y en el informe serie, que debían coincidir.
- **El aviso de survey-in pasa de la cabecera a la barra de estado.** Parpadeaba
  entre el nombre y el reloj, y en el panel de 480 no aparecía en absoluto. Ahora
  se añade a lo que la barra ya dice: `DISCIPLINED  FIX OK SURVEY` (`SV` en 320).
  La barra repinta todo su fondo antes de dibujar, así que la palabra no puede
  quedar recortada por el relleno de un campo vecino, y ahí no necesita parpadear
  para verse. La condición no cambia: aparece tras el timeout del survey-in si el
  receptor sigue midiendo, y desaparece al llegar el Time Mode.
- `g_time_offset` (int8, horas) pasa a `g_time_offset_min` (int16, minutos).
  `g_tz_auto` (bool) pasa a `g_tz_mode` (manual / auto-EU / POSIX).

### EEPROM
- Bloque de zona horaria en `[234..284]`.
- **Los ajustes existentes se migran automáticamente, sin reset de fábrica.**
- **Volver a una versión anterior es unidireccional**: v0.94 leerá un desfase
  sensato en horas enteras, pero una regla `TZ` no cabe allí y se perderá.

### Documentación
- Movida a [`doc/`](../doc/); los archivos en inglés llevan sufijo `_EN` para que
  los tres idiomas se nombren igual. El `README.md` raíz es ahora un índice breve
  que GitHub muestra en la página del proyecto.
- Las guías de puesta en marcha del flash-ring eran huérfanas; ahora llevan la
  misma navegación de idiomas que el resto.
- Su cifra de presupuesto de flash llevaba cinco versiones sin actualizar
  (~170 KB en v0.90). Ahora indica 216976 B (212 KB) en v0.95, ~44 KB de
  margen — medido, no estimado. La guía advierte además de que el porcentaje
  del IDE cuenta sobre los 512 KB completos: «41%» es en realidad el 83%.

### Notas
- `tz_table.h` se genera. Vuelve a ejecutar `tools/gen_tz_table.py` cuando se
  actualice tzdata.
- Africa/Casablanca y Africa/El_Aaiun degradan a su desfase estándar con aviso:
  su DST sigue el ramadán, que el formato POSIX no puede expresar.

---
## [v0.94-rtos] — 2026-07-15


### Corregido
- **El campo de frecuencia en 320×240 seguía dibujándose con las fuentes GFX.**
  La v0.93 devolvió el panel pequeño a las fuentes clásicas, pero la corrección
  solo llegó a la ruta de dibujo directo — y esa ruta nunca se ejecuta, porque
  los sprites se crean en *ambos* paneles, no solo en el de 480×320. La rama de
  sprite seguía con `GF_FREQ`/`GF_STATUS` codificados, así que la lectura (y
  `no signal`) se seguía renderizando en FreeMono. Ahora pasa por las mismas
  macros `TFT_FONT_*` que todo lo demás.
- **La frecuencia temblaba lateralmente en el panel 480×320.** La lectura
  estaba centrada, así que cualquier cambio de longitud movía todos los
  caracteres: la ventana de promediado cambia los decimales, y
  10000000.0000 → 9999999.9999 pierde un carácter entero, repartiendo el
  centrado esa diferencia entre ambos extremos. La lectura se ancla ahora por
  su borde derecho en x=464, elegido para que la nominal `10000000.0000 Hz`
  (16 caracteres × 28 px de ancho fijo = 448 px) siga cayendo justo en el
  centro, dejando 16 px de aire a cada lado. «Hz» ya no se mueve; solo los
  dígitos. Los mensajes de estado siguen centrados — usan la fuente
  proporcional, donde no hay columnas que alinear.

### Cambiado
- **El marco es blanco en ambos paneles.** Además de igualar al panel grande,
  esto es lo que permite que el sprite de datos de 1 bit lleve el marco él
  mismo: ese sprite tiene exactamente dos colores (blanco y fondo), así que el
  marco azul marino no podía dibujarse *dentro* de él y había que repintarlo en
  el panel tras cada envío. El blanco significa que marco y texto salen ahora
  juntos en una transferencia atómica, en ambos tamaños. El separador de la
  cabecera se movió al sprite de frecuencia por la misma razón (su paleta de
  4 bits ya contiene blanco).
- **El splash ya no usa las fuentes GFX.** Era el último reducto de GFX en el
  panel pequeño, lo que obligaba a quien actualizara desde la v0.92 a añadir
  `LOAD_GFXFF` a `User_Setup.h` o ver el subtítulo reducirse a una sola «p» —
  un fallo críptico a cambio de una mejora cosmética. El subtítulo usa ahora la
  fuente clásica 4 (que lleva el alfabeto completo — las fuentes 6/8 son las
  que no tienen letras) y los créditos la fuente 1 en ambos paneles. **Una
  compilación 320×240 solo necesita ahora `LOAD_GLCD`, `LOAD_FONT2` y
  `LOAD_FONT4`**; `LOAD_GFXFF` se requiere únicamente para la de 480×320. Las
  macros huérfanas `GF_TITLE`/`GF_SUB`/`GF_CREDIT` y la rama muerta de 320 del
  bloque `GF_*` desaparecen con ello.
- Las etiquetas de la barra de estado bajan 2 px en el panel 320×240. Van en
  mayúsculas, así que el espacio para descendentes al pie de la caja del glifo
  está vacío y el centrado geométrico se lee alto; el desplazamiento centra lo
  que el ojo realmente ve. El panel 480×320 no cambia.
- Subida de versión a v0.94-rtos, incluidas las cabeceras de cada archivo (que
  aún indicaban v0.92).

## [v0.93-rtos] — 2026-07-14

### Corregido
- **Las cuentas atrás iban más lentas que el reloj.** El calentamiento del OCXO
  y las calibraciones medían sus segundos con `vTaskDelay(1000)`, que duerme
  *durante* un segundo en vez de *hasta* el siguiente — así que las lecturas del
  ADC, las impresiones por serie y cualquier apropiación se sumaban encima, y la
  cifra mostrada se retrasaba respecto al tiempo real (tanto más cuanto más
  cargado el sistema). Ambas usan ahora `vTaskDelayUntil`, que absorbe el tiempo
  de trabajo y mantiene cada paso como un segundo real. La cuenta atrás de
  calibración además se paraba en 1 en lugar de llegar a 0.
- **Un survey-in que sobrevive a su ventana de monitorización ya no es
  invisible.** Cuando salta el tiempo de seguridad, el firmware deja de
  consultar pero el receptor sigue haciendo el survey («continuing anyway» en el
  registro) — y con la banda de frecuencia de vuelta mostrando la frecuencia,
  nada en pantalla lo decía. Un `SURVEY` de pulso lento se sitúa ahora en la
  cabecera entre la versión y el reloj, y se apaga solo cuando el receptor
  informa de Time Mode (`HDOP: TIME`), que es la señal real de finalización del
  survey.
- **`qErr` dejaba caracteres residuales en el panel ILI9488** (visto como
  `qErr: -1.6 nsss`). El relleno de texto del campo era de 55 unidades de autor
  (~82 px), mientras que el valor más ancho, `qErr: -21.3ns`, necesita ~104 px
  en FreeSans 9pt — TFT_eSPI solo repinta el fondo bajo el relleno, así que la
  cola de la cadena anterior, más larga, sobrevivía. Relleno ampliado a 75
  unidades (~112 px), que cubre el texto y sigue despejando el campo `Vdd`
  anclado a la derecha.
- **Vctl / Vcc / Vdd marcaban 0,000 V durante todo el calentamiento del OCXO.**
  Esas medias del ADC se muestrean en el bucle principal de la tarea de control,
  pero `do_warmup()` se ejecuta *antes* de entrar en ese bucle y solo dormía —
  así que nada las llenaba. La cuenta atrás del calentamiento ahora muestrea los
  mismos tres canales cada segundo, igual que ya hace `wait_secs_pwm()` durante
  la calibración.
- **La lectura de frecuencia quedaba a la derecha del centro y saltaba de
  lado.** El valor se formateaba con `dtostrf(..., 14, ...)`, rellenándolo por
  la izquierda hasta 14 caracteres; `MC_DATUM` centraba luego la cadena
  *incluyendo* esos espacios invisibles, así que los dígitos visibles quedaban
  ~40 px a la derecha del centro — y como el número de espacios varía con la
  ventana de promediado (1–4), la lectura se desplazaba al cambiar la precisión.
  Se elimina el ancho de campo: `GF_FREQ` es FreeMonoBold, que ya mantiene los
  dígitos en columnas fijas, así que el relleno no aportaba nada. Con él
  desaparece el apaño del espacio final en el panel de 480.

### Cambiado
- **Los paneles 320×240 vuelven a las fuentes clásicas en la pantalla de
  trabajo.** v0.92 pasó todos los paneles a las fuentes libres GFX; en 480×320
  fue una mejora clara, pero a 320×240 los tipos proporcionales son demasiado
  anchos para una maquetación pensada para las numéricas — los valores se salían
  de sus columnas hacia la vecina y el divisor central cortaba lo que
  desbordaba. Tampoco había un tipo menor al que recurrir (FreeSans empieza en
  9 pt; por debajo solo está el ilegible TomThumb de 3×5). El panel pequeño usa
  ahora la fuente 2 para la cabecera y la rejilla, la 4 para la barra de estado
  y la 1 ×3 (ancho fijo) para la frecuencia, mientras que el splash mantiene GFX
  en ambos paneles. Las macros `TFT_FONT_*` eligen esto en tiempo de compilación
  — sigue habiendo una maquetación, no dos. El divisor central de columnas es
  ahora solo de 480 (a 320 no hay sitio), y el marco vuelve al azul marino en el
  panel pequeño.
- **Las regiones vivas de la pantalla usan doble búfer con sprites.** La
  cabecera, la banda de frecuencia y el área de datos se renderizan cada una en
  un `TFT_eSprite` en RAM y se envían al panel en una sola transferencia SPI
  continua, en vez de borrar el panel con `setTextPadding` y dibujar encima. Ese
  borrar-y-luego-dibujar se veía como un parpadeo una vez por segundo, sobre
  todo en el panel 480×320, donde limpia 2,4× los píxeles. Las paletas lo
  abaratan (4 bits cabecera/frecuencia, 1 bit datos; ~25 KB en total en el panel
  grande). Si `createSprite()` falla con un montón fragmentado, cada banda
  recurre al dibujo directo — vuelve el parpadeo pero nada se rompe; el registro
  de arranque indica qué ruta está activa.
- **Los mensajes de estado ahora se escriben completos y nombran qué
  calibración corre.** `WARMUP 285s` → `OCXO warmup 285s`, `SVIN 120s 5m` →
  `Survey 120s +/-5m`, y el ambiguo `CAL 245s` pasa a ser `Calibrate`, `Tune` o
  `LTIC cal` — C, CT y LC tardan tiempos muy distintos, así que una cuenta atrás
  a secas decía poco al operador. Ambos paneles. Ojo: las dos cifras son de
  distinta naturaleza: el calentamiento y las calibraciones cuentan hacia abajo,
  mientras que el survey-in cuenta hacia arriba (el receptor informa del tiempo
  transcurrido, y la finalización depende también de la precisión, así que una
  cifra de «restante» sería una conjetura).
- **`SPI_FREQUENCY 40000000` es ahora el ajuste documentado** (el README decía
  27 MHz mientras `gpsdo_config.h` ya indicaba 40). El SPI1 del F411 llega hasta
  50 MHz, así que 40 deja margen; importa sobre todo en el panel 480×320, donde
  un envío de sprite es una única transferencia cuya duración escala con el
  reloj. Baja a 27 MHz si los cables largos dan problemas.
- **Las líneas de créditos del splash ganan interlineado en el panel 480×320.**
  El hueco de 12 unidades de autor escala allí a solo ~16 px, y los créditos son
  FreeSans 9pt (~13 px de alto), así que ambas líneas se fundían visualmente. El
  panel grande usa ahora un hueco de 16 unidades (~21 px, interlineado ~1,6×);
  el panel 320×240 mantiene 12, que encaja con su fuente 6×8.
- `dPh:` y `qErr:` en el ILI9488 pierden el espacio antes de su unidad `ns`.
- Incremento de versión a v0.93-rtos.

## [v0.92-rtos] — 2026-07-12


### Cambiado
- **Splash simplificado y proporciones de la pantalla de operación afinadas.**
  Se eliminó el gran título verde «GPSDO» de la pantalla de inicio; el subtítulo
  «GPS Disciplined OCXO» ahora aparece elevado en la parte superior, como en el
  diseño original 320×240. En la pantalla de operación, el texto de la cabecera
  se redujo al tamaño de la fuente de datos, la barra de estado inferior se
  redujo a la mitad de su altura con una fuente de estado más pequeña, y el
  espacio recuperado pasó a un mayor interlineado entre las filas de telemetría
  (paso de fila 17→20 autoral) para que la rejilla respire. La fuente de datos
  se mantiene en FreeSans 9pt.
- **Todo el texto del TFT migrado a las fuentes libres Adafruit GFX (GFXFF).**
  La cabecera, el gran indicador de frecuencia, la rejilla de datos, la barra de
  estado y el título/subtítulo de la pantalla de inicio se dibujan ahora con
  FreeSans / FreeMono en lugar de las clásicas fuentes GLCD numéricas. Esto
  corrige un error de larga data en el que las cadenas con letras dibujadas con
  las fuentes numéricas (6/8, que solo contienen `0-9 . : - a p m`) se reducían a
  un único glifo — de forma más visible el subtítulo «GPS Disciplined OCXO» que
  aparecía como una sola «p», y la etiqueta de la barra de estado que quedaba en
  blanco sobre una barra de color. Una capa de fuentes por rol y por panel
  (`GF_DATA` / `GF_HEAD` / `GF_STATUS` / `GF_TITLE` / `GF_SUB` / `GF_FREQ` en
  `gpsdo_config.h`) selecciona automáticamente FreeSans 9/12 pt, FreeSansBold
  12/18/24 pt y FreeMonoBold 18/24 pt para los paneles 320×240 y 480×320, de modo
  que el mismo código de diseño sirve para ambos. La frecuencia grande usa
  FreeMonoBold para que sus dígitos mantengan ancho fijo y no se desplacen al
  cambiar el valor.
- **Requiere `#define LOAD_GFXFF` en `User_Setup.h`** (ver README). Las antiguas
  líneas `LOAD_FONT2/4/6/8` ya no son necesarias; `LOAD_GLCD` se conserva solo
  para las dos líneas de créditos en letra pequeña de la pantalla de inicio.
- **Diseño de la pantalla de operación recalculado geométricamente para
  480×320.** Los límites de las bandas (frecuencia, rejilla, sensores, estado)
  se recalcularon para que las filas más altas de las fuentes proporcionales
  nunca crucen un separador en ninguno de los paneles, ambas columnas de datos
  llenan todo el ancho con un tenue divisor central, y la barra de estado llena
  toda la banda hasta el borde inferior de la pantalla (sin franja de color
  muerta bajo la etiqueta). Los valores de la rejilla se anclan con datum
  derecho, de modo que los anchos cambiantes quedan fijos en lugar de desplazarse.
  Verificado en el panel ILI9486 480×320.

### Corregido
- **Eliminado el texto obsoleto «aún no implementado / phase A» de la CLI y la
  telemetría.** `LA` con un valor incorrecto decía "0..9 (10=LTIC, not yet
  available)", `LL` imprimía "(loop not yet implemented — phase A)", y la
  ayuda/comentarios aún describían el algo 10 como una vista previa sin
  implementar. El algoritmo 10 disciplina el lazo desde hace muchas versiones;
  ahora todo describe el lazo de fase de 3 etapas ACQ→DPLL→LOCK en
  funcionamiento. (`Vdd:` en el TFT también ganó un espacio antes de su valor
  para coincidir con las demás etiquetas.)
- **Las animaciones del spinner LED (calentamiento / survey-in / calibración)
  iban ~5× demasiado lento y a saltos.** La tarea de pantalla despierta con la
  notificación PPS de 1 Hz, pero los spinners avanzan de fotograma cada 200 ms
  — así que con el despertar cada 1100 ms solo avanzaban una vez por segundo. La
  tarea ahora despierta ~cada 150 ms mientras hay una animación activa (y
  mantiene el ritmo lento de 1100 ms si no, ya que el reloj solo cambia una vez
  por segundo). Para que el despertar más rápido no reenvíe segmentos idénticos
  por el TM1637 bit-bangeado por software (~5–8 ms por escritura), una pequeña
  caché de escritura (`tm_set`) omite la transferencia cuando el patrón no
  cambia. Es una corrección de planificación/caché — sin DMA; DMA sigue siendo
  un paso futuro aparte para la ruta TFT SPI.
- **Un suelo de amortiguación elevado no surtía efecto tras reflashear — el
  enganche oscilaba LOCK↔DPLL.** El multiplicador de amortiguación se persiste
  en el anillo Flash (datos vivos) y se restaura al arrancar. La Flash escrita
  por un build con el suelo antiguo de 0,30 recargaba por tanto damp = 0,30
  incluso tras subir el suelo a 0,45, y como damp solo se adapta en cruces del
  ciclo límite, se quedaba ahí atascado — el lazo corría al 30 % de autoridad,
  la fase subía por encima del umbral de enganche y el lazo cazaba LOCK↔DPLL
  cada ~30 s (visto en hardware). El damp restaurado se acota ahora a la banda
  legal actual al cargar (anillo Flash y EEPROM), así que un reflasheo surte
  efecto de inmediato. Los límites de damp se movieron a la cabecera compartida
  para que el almacenamiento y el aprendiz coincidan.
- **El TFT ahora muestra qErr, y la fase recibe una etiqueta `dPh:`.** En algo
  10 con SAW activo, el hueco derecho de la fila de sensores encabeza con el
  diente de sierra del receptor `qErr:…ns` y sigue con `Vdd:` acortado a 1
  decimal. Ambos se dibujan por separado — qErr a la izquierda, Vdd anclado al
  borde derecho de la pantalla — así que Vdd ya no se desplaza lateralmente
  cuando qErr cambia de ancho. Con SAW apagado se muestra solo Vdd a plena
  precisión, aún anclado a la derecha. La fase LTIC de la izquierda se etiqueta
  `dPh:±…ns` (sin espacio tras `Vph:`) para una lectura más clara y coherente;
  el informe serie usa la misma etiqueta `dPh:` tras `Vphase:` para que
  coincidan. qErr y dPh usan un campo de ancho fijo con signo (signo siempre visible,
  magnitud justificada a la derecha), así que los dígitos y las unidades se
  quedan quietos en vez de saltar de lado al cruzar cero o cambiar de número de
  cifras.
### Corregido
- **LOCK podía perder el enganche con un OCXO a la deriva — ahora lleva un
  término de frecuencia suave.** En la rama normal de LOCK la ruta de frecuencia
  estaba desactivada (freq_term = 0), así que la única defensa contra una deriva
  real del OCXO era el lento feed-forward de deriva. En hardware caliente se
  retrasaba mucho y la fase se salía de la ventana de enganche (11 → −425 ns en
  51 s, luego LOCK→DPLL→ACQ). LOCK aplica ahora un término ligero de 0,1×Kp —
  suficiente para cancelar la deriva viva en cada actualización, lo bastante
  suave para no inyectar ruido de TIM2 en un enganche silencioso. Se combina con
  el feed-forward más rápido (abajo). Análisis de causa raíz: GML-5.2.
- **Eliminado el ciclo límite en ACQ (oscilación de PWM de ±150 LSB).** El
  algoritmo 10 tomaba su error de frecuencia de avg10 (cuantización de 0,1 Hz);
  por Kp (~1550 LSB/Hz) producía saltos de PWM de ±150 LSB en un ciclo de ~10 s,
  que impedían que la fase se asentara bajo el umbral de enganche y ralentizaban
  la adquisición. Ahora usa avg100 (0,01 Hz), 10× más fino, y la adquisición se
  asienta limpiamente. Análisis: GML-5.2.
- **El feed-forward de deriva ahora hace bootstrap tras el enganche.** Su primera
  ventana de aprendizaje era lenta (30 s), así que una deriva rápida tras el
  enganche escapaba antes de que se moviera. Ahora corre tres ventanas rápidas de
  8 s con un paso mayor justo tras el enganche (absorbiendo la deriva en
  ~10–20 s) y luego se relaja al régimen silencioso de 30 s.
- **Suelo de amortiguación subido 0,30 → 0,45.** El aprendiz podía amortiguar
  tan fuerte que el lazo tenía solo el 30 % de autoridad de corrección y no podía
  seguir la deriva; 0,45 aún amortigua un ciclo límite pero conserva autoridad
  para seguir.

### Cambiado
- **El anclaje de calibración LC es ahora universal — 0,632·Vsat, derivado por
  placa.** El detector es una rampa de carga RC V(φ) = Vsat·(1 − e^(−φ/τ)); el
  punto φ = τ, donde V = 0,632·Vsat, es la misma altura fraccional en todo
  detector exponencial sin importar Vsat. LC recupera ahora Vsat con un ajuste
  1-D (linealizar −ln(1 − V/Vsat) frente a t, elegir el Vsat de menor residuo) y
  ancla ahí. El 1,85 V fijo anterior solo funcionaba porque los detectores de
  Marek y Dan Wiering tienen ambos Vsat ≈ 2,93 V; un detector con otro Vsat
  habría perdido la banda. LC se autoadapta ahora por placa sin configuración, y
  `LTIC_ZERO_ANCHOR_V` queda retirado. Verificado en ejecuciones registradas:
  Vsat recuperado al ~0,3 %, los anclajes concuerdan al ~0,8 % entre ejecuciones.
  Física y derivación: GML-5.2.
- **El subtítulo del splash** ahora dice `GPS Disciplined OCXO` (espacio, no guión).
- **Calibración LC — punto de trabajo anclado + ns/V por pendiente local (Opción D).**
  El detector de fase de rampa es exponencial (1k/1n, τ≈1 µs), así que ns/V no es
  constante a lo largo de la rampa, y un promedio de todo el tránsito (range/span)
  variaba ~15–20 % entre ejecuciones — según dónde el arm del picDIV aparcara la
  fase. ns/V se toma ahora de la pendiente LOCAL dV/dt en una ventana de ±0,20 V
  alrededor de un punto de trabajo fijo (LTIC_ZERO_ANCHOR_V = 1,85 V). zero_offset
  se ancla en ese punto — el centro repetible de la rampa, lejos de las zonas
  muertas del detector medidas por Dan Wiering (la caída del Schottky + pull-down
  por debajo de ~0,05 V, y el riel/wraparound del ADC cerca de 3,3 V). Si un
  barrido nunca cruza la banda de anclaje, el código recurre al antiguo promedio
  range/span y lo indica.

  Hallazgos de banco en varias ejecuciones LC con resolución de 1 s:
  * El anclaje es exacto — ejecuciones consecutivas sitúan zero_offset en
    1,8500 V cada vez.
  * La dispersión de ns/V entre ejecuciones cayó de ~15–20 % (antiguo promedio
    range/span) a unos pocos por ciento. Con ambas ejecuciones barridas a la
    MISMA tasa es ~2,8 %; el residuo lo domina la cuantización de la tasa de
    barrido, no el ajuste de pendiente — avg100 resuelve la tasa a 1 ns/s, así
    que una etiqueta «−5» vs «−6» lleva ±0,5 ns/s y las bandas de confianza de
    ambos ns/V se solapan. Esto no perjudica a LOCK: el lazo usa el ns/V exacto
    que midió, a la tensión en la que realmente trabaja.
  * La ventana de ajuste se amplió a ±0,20 V (LTIC_ANCHOR_WIN_V): más puntos en
    la banda (~70 vs ~35) promedian el ruido del ADC, reduciendo la dispersión a
    igual tasa de ~5,9 % con ±0,10 V a ~2,8 %.
- **Registro de diagnóstico LC por segundo.** Durante el barrido de muestreo, LC
  imprime ahora una línea `t=/V=/n=` por segundo, haciendo visible toda la rampa
  en una captura (se usó para derivar la Opción D).

### Corregido
- **El informe serie se imprimía dos veces por segundo en RD/RH con fix del GPS.**
  vDisplayTask recibe notificación de dos fuentes de ~1 Hz — el relé de frecuencia
  (por PPS) y el parser del GPS (por sentencia de tiempo) — así que con fix se
  despertaba dos veces por segundo y emitía dos líneas de informe. La línea serie
  se controla ahora por un cambio del contador de PPS, de modo que se imprime
  exactamente una por segundo; la pantalla sigue refrescándose en cada
  notificación. Reportado por Dan Wiering.
- **Ortografía del nombre en los agradecimientos** corregida a «Wiering» (a
  petición del autor).
- **La lectura de fase `Vph` del TFT era código muerto, y erróneo si se activaba.**
  Dependía de la constante de compilación `LTIC_NS_PER_VOLT` (0 por defecto, así
  que el valor en ns nunca aparecía una vez calibrado el detector) y, de haberse
  fijado, calculaba `V × ns_per_volt` desde 0 V en vez de relativo a `zero_offset`.
  Ahora usa los `g_ltic.ns_per_volt` y `zero_offset` MEDIDOS por LC, mostrando una
  fase con signo `(V − zero_offset) × ns/V` que coincide con el error del lazo, o
  solo los voltios cuando no está calibrado.
- **CT rechazaba OCXOs de span estrecho (mejores).** El chequeo de ganancia
  del planta limitaba K a 0,1 mHz/LSB, pero un span EFC estrecho es deseable —
  menos Hz/LSB significa más resolución y es una vía hacia E-12. El equipo de
  Dan Wiering mide 0,048 mHz/LSB (~1,05 V de span EFC) y se rechazaba
  erróneamente. Límite inferior bajado a 0,02 mHz/LSB; ahora solo se rechazan
  ejecuciones de ruido/sin-GPS.
- **Correcciones del diseño ILI9488 (480×320) — a partir de fotos de usuarios,
  aún no verificadas en un panel.** Los primeros usuarios Dan Wiering y lucido
  enviaron fotos de sus montajes 480×320. Se abordaron varios problemas a
  partir de esas imágenes sin un ILI9488 a mano: (1) la fuente del cuerpo se
  sobre-escalaba — TFT_F mapeaba fuente 2→4 (creciendo 1.63× mientras las filas
  escalan solo 1.33×), así que las líneas se solapaban verticalmente y la barra
  de estado se salía de la pantalla; la fuente del cuerpo se mantiene ahora en
  2. (2) La fila del sensor BMP se acortó (temperatura y presión a 1 decimal)
  para que los glifos escalados más anchos no invadan la columna AHT. (3) A las
  instrucciones de `User_Setup.h` les faltaba `LOAD_FONT8`, que necesita la
  lectura de frecuencia — sin ella esa línea queda en blanco. Son correcciones
  «al mejor criterio» a partir de fotografías; un pase final de geometría
  seguirá cuando haya un panel ILI9488 a mano. Los paneles pequeños 320×240 no
  se ven afectados (TFT_F es identidad ahí).
- **LOCK podía perder el enganche con un OCXO a la deriva (rebote LOCK→DPLL→ACQ).**
  Con una deriva de frecuencia real (~8,5 ns/s medidos en hardware caliente), la
  fase se salía de la ventana de enganche — 11 → −425 ns en 51 s — mientras la
  corrección era demasiado débil para seguirla: el aprendiz de amortiguación
  había tocado suelo en 0,30 (corrección al 30 % de autoridad) y el
  feed-forward de deriva aún recogía su primera ventana de 30 s, así que nunca
  se movió antes de perder el enganche. Dos cambios: el suelo de amortiguación
  se subió 0,30 → 0,45 (conserva autoridad para seguir la deriva sin dejar de
  amortiguar un ciclo límite), y el feed-forward ahora hace BOOTSTRAP tras el
  enganche — tres ventanas rápidas de 8 s con un paso mayor absorben la deriva
  en ~10–20 s, luego se relaja al régimen lento y silencioso de 30 s. Simulado
  sobre la deriva registrada, la fase ahora se mantiene cerca de −125 ns en vez
  de escaparse. Los montajes estables de baja deriva (p. ej. con referencia Rb)
  no se ven afectados — el bootstrap converge de inmediato y el suelo más alto
  sigue siendo amortiguación neta.
- **Fase en ns añadida al informe serie**, tras `Vphase:`, una vez que LC ha
  calibrado el detector — `(V − zero_offset) × ns/V`, la misma convención que
  el lazo y la fila del TFT.
- **El anclaje de LC es ahora el punto medio medido de la rampa, no 1,85 V fijo.**
  El anclaje de pendiente local estaba fijado a la banda del detector de Marek;
  un equipo cuya rampa barre otro rango (el de Dan va más bajo, ~1,3 V) perdía
  la ventana de anclaje por completo y recurría al promedio range/span (resultado
  "débil"). El anclaje es ahora `vlow + span/2` del barrido real, usando la
  constante `LTIC_ZERO_ANCHOR_V` solo cuando cae dentro de la banda barrida. LC
  se autoadapta por placa.
- **La presión en el TFT podía invadir la columna AHT.** La presión BMP se
  imprimía con 2 decimales (`1013.25hPa`), que con presiones de 4 dígitos se salía
  de la columna izquierda. Reducida a 1 decimal (`1013.2hPa`), igual que el
  informe serie.
- **Rebote de LOCK en arranque en caliente (desperdiciaba ~1 min de los ~8 min
  de arranque a lock).** Un LOCK/DPLL persistido se reanudaba mientras la lectura
  de fase fuera válida (en la rampa), aunque estuviera lejos de zero_offset —
  p. ej. Vphase ≈2,09 V frente a un anclaje de 1,85 V (~260 ns de desvío). LOCK
  se enganchaba, DPLL juzgaba la fase demasiado lejos un minuto después y caía
  hasta ACQ, así que el pull-in completo corría igualmente tras un desvío inútil.
  La protección de arranque ahora degrada un LOCK/DPLL persistido a ACQ salvo que
  la fase sea válida Y esté dentro de la ventana ACQ de zero_offset. El arranque
  en frío no se ve afectado (el estado por defecto es ACQ); un arranque en
  caliente genuinamente centrado sigue reanudando LOCK de inmediato.

---

## [v0.90-rtos]

### Añadido
- **Búfer en anillo en Flash con nivelado de desgaste para datos "vivos".** La
  deriva/amortiguación aprendidas, la calibración LC y el último PWM se
  auto-guardan ahora en un sector de Flash dedicado (sector 6, 0x08040000,
  128 KB) como un anillo de slots de 32 bytes. Cada guardado programa el
  siguiente slot vacío; el sector se borra solo cuando el anillo da la vuelta
  (una vez cada 4095 guardados), así que a 100 guardados/día el Flash dura del
  orden de mil años. Cada slot lleva un CRC y un número de secuencia; un slot
  escrito a medias (corte de energía) falla el CRC y se usa el anterior válido.
  Una cabecera con firma + versión de formato hace el firmware robusto frente a
  borrado de chip completo, programación solo por sectores, primer arranque y
  restos de basura en Flash por igual (un sector ajeno o en blanco se detecta y
  reinicializa).
- **Auto-guardado con histéresis.** Los datos vivos se escriben solo cuando se
  han asentado en un nuevo nivel: la deriva cambió > 8 LSB o la amortiguación
  > 0,03, Y han pasado al menos 20 min desde el último guardado. Una calibración
  `LC` exitosa guarda de inmediato.
- **Comando `FR 0|1`** (guardado con `ES`, activo por defecto) conmuta el búfer
  en anillo en tiempo de ejecución — sin flag de compilación, así que sin
  sorpresas de caché de compilación. `FR 0` detiene toda actividad del anillo.
- **Comando `EW`** muestra diagnósticos de desgaste del Flash: ciclos de borrado
  y slots usados.
- **Corrección de diente de sierra (qErr) para LTIC (`SAW 0|1`).** Los receptores
  de temporización u-blox generan el 1PPS dividiendo un reloj interno, así que
  cada pulso cae hasta un periodo de reloj lejos del tiempo GPS real — un error
  de cuantización por pulso que el receptor reporta como `qErr` en UBX-TIM-TP.
  Un sniffer pasivo parsea ese mensaje (qErr es un campo con signo de 32 bits en
  picosegundos, en el mismo offset en LEA-6T, LEA/NEO-M8T y ZED-F9T, así que un
  solo parser sirve para todos) y la ruta de fase del TIC lo resta, eliminando
  el diente de sierra de granularidad del receptor y dejando el error propio del
  OCXO. En un LEA-6T (21 ns de granularidad) este es el término de fase de corto
  plazo dominante. TIM-TP se habilita automáticamente al iniciar el GPS; `SAW`
  conmuta la corrección (guardada con `ES`, desactivada por defecto) y muestra
  qErr en vivo.

### Cambiado
- **`ES` ya no sobrescribe los valores aprendidos/de calibración con el anillo
  activo.** Con `FR 1`, la calibración (ns_per_volt, zero_offset, range_ns,
  centre_v) y la deriva/amortiguación aprendidas pertenecen exclusivamente al
  anillo; `ES` escribe solo ajustes genuinos (ganancias PID, umbrales, flags).
  Con `FR 0`, `ES` sigue guardando esos valores vivos en EEPROM como respaldo, y
  `eeprom_recall()` los siembra al arrancar, de modo que migrar una EEPROM
  antigua conserva su calibración.

### Corregido
- **`LC` ya no pelea con el lazo de disciplina.** Ejecutar `LC` mientras el
  algoritmo 10 disciplinaba activamente permitía que el lazo moviera el PWM al
  mismo tiempo que el barrido de calibración, de modo que ambos se corrompían —
  la tasa de barrido medida salía a ±1 ns/s y el rango como valores absurdos
  (1502 / 3518 ns), que la comprobación física rechazaba correctamente. El lazo
  de control se suprime ahora siempre que hay una calibración activa
  (`g_calib_active`), así que `LC` puede ejecutarse en cualquier momento,
  incluso bajo `LA 10`.
- **Rutas de PWM seguras durante la calibración.** La misma protección cubre
  ahora también el pilotaje de holdover térmico del algoritmo 9 y los comandos
  manuales de PWM (`up1`/`up10`/`dp1`/`dp10`/`SP`), que se rechazan con un
  mensaje claro mientras corre `LC`/`CT`, de modo que ninguna ruta pueda
  perturbar un barrido en curso.
- **Que `LC` no dé la vuelta ya no se marca como fallo.** Un detector que no da
  la vuelta dentro del barrido ahora pasa con buena pendiente/centro/span y se
  auto-guarda; solo un resultado genuinamente débil (span diminuto o centro
  fuera de banda) se señala, con el motivo específico. Los mensajes ya no piden
  al usuario ejecutar `ES` tras `LC` — un `LC` exitoso auto-guarda en el anillo
  Flash (esto son datos vivos). `CT` sigue pidiendo `ES`, ya que ajusta valores
  PID.

### Créditos
- Atribución afinada: André Balsa acreditado como autor de v0.06c, la
  inspiración del port a RTOS. Enlace del repositorio corregido.

---

## [v0.89-rtos]

### Añadido
- **Ayuda de lazo auto-aprendida (`LRN`), compartida por el algoritmo 7 y LTIC.**
  Dos aprendices lentos y pasivos — informados por las trazas nocturnas
  referenciadas a Rb de Dan Wiering (un diente de sierra de fase de ~9000 s
  ±80 ns, un bache de ADEV en la constante de tiempo del lazo, y deriva de
  8E-12/día): (1) un **feed-forward de deriva** que estima la pendiente media de
  fase del OCXO en ventanas de 30 s y añade un término de PWM para cancelarla,
  de modo que el lazo deja de perseguir un objetivo móvil y la fase se aplana;
  (2) una **adaptación de amortiguación** que vigila los cruces por cero del
  error de fase y baja la ganancia de corrección ante sobreoscilación, la sube
  cuando va lento — aplanando el bache de ADEV en la constante de tiempo del
  lazo. Ambos corren SOLO en lock, se actualizan como mucho cada 30 s, y están
  fuertemente acotados (feed-forward ±400 LSB, amortiguación 0,5–1,5) de modo
  que una mala estimación no pueda desestabilizar el lazo; ninguno inyecta
  excitación. `LRN 1|0` activa/desactiva (activo por defecto), `LRN R` restaura
  a la teoría, `LRN` a secas imprime el estado en vivo; los valores aprendidos
  se guardan con `ES` (EEPROM 222–230) y se recuperan al arrancar. El informe
  serie muestra una línea `Learn:` en vivo (deriva, pendiente, amortiguación,
  periodo/amplitud del ciclo límite observado).
- **El aprendizaje cubre ahora todos los algoritmos de disciplina (3–10), no
  solo 7/8.** Un único envoltorio `lrn_apply()` alimenta el acumulador de fase y
  el error de frecuencia propios de cada lazo a los aprendices; la NN (algo 9),
  al no tener acumulador de fase explícito, usa solo amortiguación. El estado de
  `LRN` se comparte entre algoritmos.

### UI / Pantalla
- **TFT en color reelaborado para claridad y un poco de vida.** Formato de
  etiquetas consistente con un solo espacio en todo (`Alt: 144m`, `PWM:...`,
  `Uptime: ...`); los campos de valor se alinean ópticamente en la fuente
  proporcional. Un marco azul marino (a juego con la cabecera) enmarca ahora el
  área de datos, con los tres separadores unidos por rieles laterales. La
  frecuencia se vuelve verde en lock. Etiqueta `DATE:` añadida.
- **Splash de arranque refinado**: título a la altura de la frecuencia, dos ondas
  de oscilador que aparecen desfasadas, derivan hasta coincidir y se fusionan en
  una sola onda verde con un halo que crece y se desvanece, seguido de una lista
  de detección de hardware con scroll (ventana de altura fija, los créditos se
  quedan en su sitio).
- **Comando `SPL 0|1`** (guardado con `ES`, 1 por defecto) conmuta la animación
  de arranque. `SPL 0` muestra solo el título y los créditos durante dos
  segundos — para los indiferentes al arte.

---

## [v0.88-rtos]

### Corregido
- **El campo de frecuencia del TFT ya no conserva restos de dígitos tras los
  mensajes CAL/WARMUP/SVIN.** Los mensajes de ocupado y la frecuencia grande
  usan alturas de texto distintas, así que el relleno de texto borraba solo la
  banda de la fuente actual; ahora todo el campo se limpia en cada transición
  ocupado↔normal.

### Eliminado
- **Soporte del puente SPI→T6963C eliminado** (un experimento): `T6963C_Bridge.h`,
  su sección de tarea de pantalla, bloque de configuración y referencias cruzadas
  han desaparecido.

### Documentación
- READMEs (EN/PL/ES) actualizados con el conjunto de funciones LTIC v0.5x–v0.88
  (auto-calibración LC, ganancias auto-ajustadas, ruta de mediana del ADC,
  protección anti-fuga, WU, animaciones LED, color de lock fiable) y una nueva
  sección sobre soporte de TFT en color: cualquier panel TFT_eSPI a 320×240 o
  480×320 con los pasos de configuración.

---

## [v0.87-rtos]

### Corregido
- **Cero tiempo muerto antes del muestreo — la preparación se comía toda la
  banda.** El ADC sigue el ritmo bien (1 muestra/s ≈ 8 mV/paso a 9 ns/s); lo que
  fallaba eran los ~60 s de asentamiento y las lecturas d1/d2 entre comandar la
  rampa y la primera muestra. Un offset fijo se suma a cualquier df que el PWM
  guardado ya tenga (medido +9 ns/s en el banco), así que la fase voló
  0,061→2,62 V a través de toda la banda ANTES de que empezara el muestreo, y el
  ajuste solo veía saturación. Ahora LC re-arma el picDIV (arranque
  determinista desde abajo), comanda el offset y empieza a muestrear en ~3 s; la
  tasa exacta se lee DESPUÉS de la pasada desde un avg100 limpio. Si la
  saturación aún llega antes de 10 puntos de ajuste, el offset se reduce a la
  mitad, se re-arma el picDIV y la pasada se reintenta una vez. La medición
  d1/d2 previa al barrido y la maquinaria adaptativa de reducir/aumentar se
  eliminan — la comprobación física y la tasa precisa posterior a la pasada las
  hacen redundantes.

---

## [v0.86-rtos]

### Cambiado
- **LC rediseñado como una única pasada de abajo hacia arriba — sin sondeo de
  dirección, sin inversiones, sin necesidad de dar la vuelta.** Los registros de
  campo demostraron que el arm del picDIV aparca la fase de forma DETERMINISTA
  ~60 ns por encima del punto de sync (Vphase ≈0,061 V tras cada re-arm), que el
  lado negativo por debajo de ese punto está MUERTO (el orden de los flancos se
  invierte, el pulso desaparece — avg100 mostró una deriva real de −3 ns/s
  mientras la tensión no se movía), y que el lado positivo recorre toda la banda
  hasta una saturación suave. Ahora LC lo explota: tras armar, COMANDA un barrido
  positivo de ~+4 ns/s (offset a partir de la K medida), muestrea toda la banda
  en una pasada, y trata la saturación superior sostenida como el FINAL natural
  de la medición en vez de un fallo. La relectura precisa de avg100 (v0.85)
  escala ns/V con exactitud. La inversión de dirección en pleno barrido y su
  maquinaria de reinicio se eliminan.

---

## [v0.85-rtos]

### Corregido
- **La inversión de dirección ahora COMANDA una tasa de barrido en vez de fiarse
  de una lectura ciega — y la fase ya no se aparca en el borde de la banda.** En
  el banco, la iteración de inversión se detenía en un nominal «−1 ns/s» que en
  realidad era ≈0: avg10 cuantiza a 0,1 Hz (d1=0,1000, d2=0,0000 en el registro),
  así que por debajo de 0,1 Hz la lectura es ruido. Con df≈0 la fase se quedaba
  donde el re-arm del picDIV la dejaba (Vphase 0,061 V — el borde inferior de la
  banda, donde pulsos demasiado estrechos apenas cargan el RC), el barrido
  cubría 5 mV, y la comprobación física tenía que abortar. Ahora, cuando el
  signo se invierte entre iteraciones, LC interpola el punto de 10 MHz P0 a
  partir de los dos últimos offsets y fija la rampa en P0 − 0,06 Hz·(LSB/Hz) —
  un −6 ns/s COMANDADO derivado de la K medida, independiente de la lectura
  cuantizada. Al final del barrido (PWM constante en toda la pasada, así que
  avg100 es limpio a resolución de 0,01 Hz) la tasa real se relee y reemplaza a
  la comandada antes de calcular ns/V, de modo que la escala del ajuste es
  exacta.

---

<!-- ============================================================= -->
<!-- Las versiones anteriores a v0.85 aún no están traducidas al    -->
<!-- español. El texto en inglés se conserva abajo verbatim y se    -->
<!-- traducirá en revisiones sucesivas. Para el historial completo  -->
<!-- traducido, véanse CHANGELOG.md (EN) y CHANGELOG_PL.md (PL).     -->
<!-- ============================================================= -->

> **⚠ Traducción pendiente.** Las entradas de v0.84 hacia abajo todavía están
> en inglés; se irán traduciendo en próximas revisiones. El contenido técnico es
> idéntico al de `CHANGELOG.md`.

## [v0.84-rtos]

### Fixed
- **The in-sweep direction flip now re-measures the rate and FORCES the sign
  to change.** v0.83's defences all fired correctly on air (soft-saturation →
  flip → clean restart → bad result rejected), but the flip itself had two
  defects: (1) the fit's ns/V divides by phase_rate, and the pre-flip rate was
  reused after the flip — a guaranteed wrong scale (ns/V=9.09e6 rejected by
  the guard); (2) mirroring the offset around saved_pwm does not change the
  drift sign when saved_pwm sits far from the true 10 MHz point (+70 gave
  +0.100 Hz, −70 still +0.054 Hz — the railing side, just slower). After the
  flip LC now re-measures df, and if the sign has not flipped it pushes the
  offset further by −2·df·(LSB/Hz) from the measured K and re-checks (≤3
  iterations); the glitch-rejection window is rescaled to the new rate.
  Simulated on the exact on-air numbers: one push lands at −0.054 Hz
  (−5.4 ns/s), the wrapping side at an ideal sweep speed.

---

## [v0.83-rtos]

### Fixed
- **`LC` can no longer be fooled by soft RC saturation.** A run with a fast
  initial offset (10 ns/s) let the phase drift into the RC's soft-saturation
  region (2.9-3.27 V — below the 3.28 V rail threshold, so "live"): the linear
  fit ingested flat saturation points (ns/V ×74 too big), the later drop out
  of saturation (a 2.57 V "jump") was accepted as a wrap, and the result
  (range=209204 ns, zero_offset=1.34 V — outside the detector band) even
  PASSED the volt-vs-volt self-consistency. Three band-relative gates close
  this class: (1) **physics gate** — the committed range cannot exceed what
  the sweep could physically cover (~rate × window × 1.5), else params
  unchanged; (2) **wrap-jump endpoints** must lie within the clean fitted
  band ±50%, so a drop out of saturation is not a wrap; (3) **soft-saturation
  skip** — once the fit has shape, samples far outside its band are treated
  like railed ones (skipped; they feed the in-sweep direction-flip logic).
  All three scale from the run's own observations — full-swing 3.3 V
  detectors are unaffected.

### Added
- **Survey-in animation on the LED displays.** An upper-'o' spinner (segments
  A→B→G→F chasing around the digit's top loop), phase-shifted per digit into a
  wave — visually distinct from the warmup's lower-'o' wave.

---

## [v0.82-rtos]

### Fixed
- **ACQ parked the phase half a range away from the handover point — permanent
  ACQ (1401 cycles on air with Δf≈0).** The ACQ pull target was computed as
  `zero_offset + span/2`, a relic from before v0.66 when zero_offset was the
  band's floor; since then zero_offset IS the band middle, so the loop held the
  phase at its own "centre" while the ACQ→DPLL threshold (measured against
  zero_offset) could never be satisfied. One point of truth now: ACQ pulls
  exactly to zero_offset. A fresh `LC` also clears any old `LCV` override
  (which could silently re-introduce the same stalemate from EEPROM).

### Added
- **Warmup animation on the LED displays.** During OCXO warmup every digit of
  the TM1637/HT16K33 shows the lowercase-'o' chasing-segment spinner,
  phase-shifted per digit so the pattern travels across the display like a
  wave (survey-in keeps the dashes).

### Note
- After upgrading, re-run `LC` once: the previous calibration was taken
  through the old 10-second-averaged ADC path and its zero_offset/range are
  blurred; the rebuilt burst-median path (v0.79) gives a sharper measurement.

---

## [v0.81-rtos]

### Fixed
- **Build fix:** `p_eff` was used by the DPLL/LOCK integrator before its
  declaration (v0.79/v0.80 did not compile). The deadband/soft-knee block is
  now computed first, so both the integrator and the phase term see it.
- **Calibration countdown shows the REAL total time.** The counter used to
  restart for every internal wait segment (30 s, 20 s…), so the display never
  reflected the whole procedure. `LC`/`CT` now preload a realistic total and
  adaptive phases (ramp increase, rail-backoff, direction flip, sweep restart)
  top it up as they occur; every exit path clears it.
- **OCXO warmup restored and made a saved setting.** Warmup was silently
  skipped whenever the EEPROM was valid — so it "disappeared" once a
  configuration was saved, and a cold-started OCXO was disciplined while still
  drifting thermally. Warmup now runs by default on every boot and can be
  disabled with the new `WU 0` command (`WU 1` re-enables; state saved by `ES`
  in EEPROM byte 221, fresh-flash default: on).

### Added
- **LED "CAL" + spinner during every calibration.** TM1637 and HT16K33 show
  CAL on the first three digits and, on the fourth, a chasing-segment
  animation (G→C→D→E) tracing a lowercase 'o' — a clear "working" cue.

---

## [v0.80-rtos]

### Fixed
- **The green frequency colour now means a trustworthy, CURRENT lock.** After
  LTIC dropped from LOCK to ACQ, the display stayed green because the 1000-s
  average still read ~10 MHz — an echo of the past, not the present. Rules now:
  for algorithm 10 green comes ONLY from the loop's live LOCK state (no
  average fallback); for algorithms 0-9 the long-window criterion remains but
  must be backed by the fast 10-s average still within ±50 mHz of 10 MHz, so a
  loss of discipline kills the green in ~10 s instead of minutes.

---

## [v0.79-rtos]

### Fixed
- **LTIC ADC path rebuilt — the 10-second moving average was poisoning the
  loop.** The old path took ONE raw ADC read per PPS through a 10-sample
  (=10 s) moving average: ~5 s group delay (the loop corrected on stale data)
  and, worse, pre- and post-wrap voltages blended into phantom mid-levels — the
  loop saw a smooth ~30 ns/s drift that did not physically exist and kicked the
  real phase (LOCK steps up to 152 LSB, LOCK↔DPLL bouncing). Now each PPS slot
  takes a 16-read burst (~1 ms) and its MEDIAN — no cross-second memory, no
  lag, no wrap blending, single-read glitches fall out — plus an outlier gate:
  a jump >25% of the calibrated span must repeat in the next read to be
  believed (real wraps persist; glitches don't). Note: reading the ADC more
  often would add nothing — the detector charges the capacitor once per PPS,
  so phase information is inherently 1 Hz; the burst maximises the quality of
  that one sample.
- **LOCK is gentle by design: deadband + soft knee + step cap.** Inside a
  deadband (range/40, ≥6 ns — the ADC noise floor) the phase error counts as
  zero and the integrator holds; outside, the error ramps from zero (soft
  knee); the final LOCK step is hard-capped at ≈4 mHz (from measured K). Small
  offsets now get proportionally small pushes instead of full-gain kicks.

---

## [v0.78-rtos]

### Fixed
- **First confirmed on-air LOCK with the LTIC three-stage loop.** Two follow-ups:
  (1) the TFT frequency readout now turns green on LTIC LOCK — it only
  recognised the legacy "hit" trend, so the colour would have waited for the
  1000/10000-s averages to reach mHz; (2) the EEPROM recall guard rejected
  algorithm 10 (`algo > 9 → 0`), so a saved LTIC configuration silently
  reverted to algorithm 0 on reboot — now `> 10`. With this, `ES` fully
  preserves the LTIC setup: algorithm 10, the LC calibration and polarity are
  stored, and the loop gains are re-derived by autotune from the stored
  measurements on every entry, so a reboot comes back locked-capable with no
  manual steps.

---

## [v0.77-rtos]

### Fixed
- **State transitions no longer bounce on the stepped detector read.** With the
  frequency finally held (−0.02 Hz), the loop still ping-ponged ACQ↔DPLL: the
  ADC updates the phase voltage in steps, and each step produced a phantom
  50-100 ns/s "slope" that tripped the V-derived slope gates (entry to DPLL
  blocked for 183 cycles; DPLL demoted after 6). All frequency-quality gates in
  the transitions now use TIM2's Δf (immune to the stepping) — ACQ→DPLL at
  |Δf|≤0.05 Hz, DPLL→LOCK at ≤0.03 Hz, demotions at Δf>0.30 / 0.10 Hz — while
  the voltage is used only for phase POSITION. DPLL demotion also gained the
  same 3-strike persistence LOCK already had, so a single stepped read cannot
  demote. Simulated with stepped reads: no false demotions, clean promotion to
  LOCK.

---

## [v0.76-rtos]

### Added
- **Full LTIC auto-tuning — no hand-set coefficients.** `ltic_autotune()`
  derives EVERY loop gain from the two measured hardware constants: K (Hz/LSB
  from CT) and ns/V + range (from LC). Freq loop cancels ~50% of Δf per step;
  phase loop pulls with τ≈20 s; LOCK is 4× gentler; the ACQ threshold becomes a
  quarter of the measured detector range. Runs automatically after each
  successful LC and on entry to algorithm 10, and prints the derived values.

### Fixed
- **ACQ now drives the TIM2 frequency error, not the voltage-derived drift.**
  The stepped detector read goes flat at a band edge (on air: phase parked at
  0.336 V while a real −0.3 Hz offset persisted, with ACQ↔DPLL bouncing) — a
  V-derived slope is blind there; TIM2 is not.
- **Board polarity no longer inverts the frequency path.** K is positive on
  every board (+PWM → +f), so frequency terms take no `pol`; only the phase
  (Vphase) path does. Routing e_freq through pol=−1 had been inverting a
  correct frequency correction in DPLL — a co-cause of the state bouncing.

---

## [v0.75-rtos]

### Fixed
- **ACQ oscillated (±1 Hz swings, twice frozen by the runaway guard) once the
  calibration was finally CORRECT.** The drift gain used a guessed fixed
  multiplier (×60) that had been implicitly tuned against the old, wrongly
  scaled calibration; with the true ns/V the numeric drift grew ~2.3× and the
  loop over-corrected ~1.8× per step — a textbook overshoot oscillation. The
  gain is now derived from the MEASURED OCXO sensitivity (CT stores 0.40/K in
  g_pid[7].Kp, so LSB-per-Hz is recovered as Kp7/0.40) with a 0.5 damping
  factor: ~60% of the error cancelled per step, unconditionally stable on any
  unit, no per-board tuning. The DPLL frequency term (fixed ×1000, ~6× too weak
  on this unit) is scaled from measured K the same way.

---

## [v0.74-rtos]

### Fixed
- **Wrap-jump quality gate — closes the last known way LC could go wrong.** The
  stepped ADC can report a wrap mid-step, yielding a PARTIAL jump; one was
  accepted as the full span (0.122 V on a ~0.33 V detector), which parked
  zero_offset near the floor (0.09 V) and sent the loop chasing a false centre
  until the frequency ran 3 Hz away. A jump now counts only if it starts from a
  live (un-railed) sample AND is ≥80% of the min–max band actually observed;
  partial jumps are named in the log and the observed band (or time
  cross-check) is used instead. `zero_offset` is now ALWAYS the middle of the
  observed band, never derived from the jump position.
- **Operator verdict line.** LC ends with an explicit "PASSED checks — review
  LL, then 'ES'" or "MARGINAL result — prefer re-running LC before 'ES'", so a
  weak calibration is hard to save by accident.

---

## [v0.73-rtos]

### Fixed
- **Runaway guard rebuilt after a real 3 Hz escape reached PWM 63500 — the old
  guard had three false assumptions.** (1) Its baseline re-anchored on every
  un-railed sample, but during a runaway the phase periodically WRAPS (briefly
  un-railed), so the baseline chased the escape and the 6000-LSB trip never
  fired. It now re-baselines only when genuinely healthy (un-railed AND
  |Δf| < 0.25 Hz). (2) An LSB threshold silently assumes the OCXO's Hz/LSB
  sensitivity; the primary criterion is now the measured frequency error
  itself: phase railed AND |Δf| > 0.5 Hz → freeze (a 2000-LSB backstop
  remains). (3) Freezing the step left the DPLL/LOCK integrator winding up,
  ready to slam PWM on recovery — it is re-seeded to the held PWM while
  frozen. Behavioural test: old guard let the simulated escape reach 6.15 Hz;
  the new one freezes at 0.51 Hz.

---

## [v0.72-rtos]

### Fixed
- **Direction flip now happens IN the sweep, where the rail actually shows.**
  v0.71's 8 s pre-check could not catch the wrong direction: in the rail-prone
  direction the phase exits the sync window only after ~a full range of drift —
  tens of seconds into the sweep (the pre-check passed, then 137 samples
  railed). LC now counts consecutive railed samples during the sweep itself;
  a sustained run (≥15 s) is the direction verdict: it flips the offset sign
  (mirrored around the saved PWM), re-arms picDIV, wipes every accumulator and
  restarts the sweep once. Verified in simulation: wrong side rails at 40 s →
  flip at ~54 s → clean sweep from the good side with the full-span wrap jump
  captured. If both directions rail, the existing mostly-railed abort still
  reports it.

---

## [v0.71-rtos]

### Fixed
- **`LC` auto-detects the ramp DIRECTION — the root cause of every railed
  calibration.** Comparing all field runs revealed the pattern: every failed
  cal had a positive df (ramp pushing the frequency above 10 MHz) and the single
  clean one (range=318) had a negative df. On this detector family the phase
  wraps sawtooth-style only when drifting one way; the other way the pulse just
  widens until the RC pins at the 3.3 V rail and stays. The good direction is
  board-dependent, so LC now probes it: after settling it watches the phase for
  ~8 s and, if pinned to a rail, flips the offset sign, re-arms picDIV and
  settles again (aborting cleanly only if BOTH directions rail). The adaptive
  ramp keeps the detected direction. Also verified: algorithm 7 does NOT run
  during LC (the calibration blocks the control task), so loop interference is
  ruled out.

---

## [v0.70-rtos]

### Changed
- **`LC` is fully self-contained: it ignores the previous calibration.** Per a
  good operator principle — you recalibrate precisely because the stored values
  may be wrong — LC no longer inherits anything from EEPROM/g_ltic: the ramp
  target, wrap threshold, glitch window and prep criterion all start from
  neutral assumptions and everything is measured fresh. This ends the poisoning
  cascade where one bad cal (range=6035) mis-steered the next three runs.
- **Single-wrap range measurement.** The voltage JUMP at a wrap (sawtooth top →
  bottom in one sample) IS the full detector span, so one wrap suffices:
  range = |jump| × ns/V. The ramp target drops to one wrap in the window, i.e. a
  much gentler sweep that no longer pushes the phase out of the picDIV sync
  window onto a rail (the failure seen at 9-22 ns/s). Two wraps, when they occur
  naturally, still enable the independent time cross-check.
- **Prep criterion is universal:** waits for a valid, un-railed, steady phase —
  no assumed centre voltage (detector bands legitimately differ between builds).

---

## [v0.69-rtos]

### Fixed
- **`LC` adaptive ramp is now hardware-agnostic and self-limiting.** The v0.68
  log showed a cascade: a poisoned prior cal (range_ns=6035 from a noise fit)
  set an absurd ramp-speed target, the adaptive increase chased it (offset up to
  1120, 15 ns/s), and the fast ramp pushed the phase out of the picDIV sync
  window entirely — the detector pulse went wide and the voltage pinned at the
  rail for the whole sweep ("180 railed samples"). Three hardware-agnostic
  defences (no detector band is assumed; different builds range from ~0.3 V to
  full 3.3 V swings): (1) the stored range only *guides* the ramp target through
  a wide anti-garbage clamp (20..5000 ns); (2) **rail-backoff** — after each
  ramp increase LC watches ~8 s and, if the phase pins to a rail, halves the
  offset back, re-arms picDIV to regain sync, and proceeds at the speed the
  hardware allows; (3) **self-consistency gate** — results are committed only if
  range ÷ slope implies a physically possible voltage span (≤3.3 V), otherwise
  the previous calibration is left untouched (a bad LC can no longer poison the
  next one).

---

## [v0.68-rtos]

### Fixed
- **`LC` no longer produces garbage when the ramp lands near the OCXO's 10 MHz
  point.** A +70 LSB offset can barely detune the OCXO (df=0.01 Hz → 1 ns/s), so
  no real wrap could occur in the window — yet read glitches (the phase voltage
  updates in steps) exceeded the wrap threshold and produced fake "2 wraps", a
  noise-only fit, and absurd results (ns_per_volt=38615, range_ns=6035). Three
  defences added: (1) **adaptive ramp increase** — if the drift is too slow for
  two wraps in the window, the offset is doubled (capped ±4000) and re-settled;
  (2) **time-validated wraps** — a jump sooner than ~half the expected crossing
  time after the previous wrap is a glitch and is ignored; (3) **volt/time
  range cross-check** — the time between two wraps × phase rate gives an
  independent range measure; if it disagrees >2× with the voltage-span measure,
  the slope is suspect and the TIME range wins (ns/V rescaled to match).

---

## [v0.67-rtos]

### Added
- **`LC` now auto-preps before ramping (operator convenience).** Running `LC`
  used to require a manual `LA 7` / `AP` / "wait for the phase to reach centre"
  sequence first; starting with the phase against a rail was the main cause of
  poor calibrations. `LC` now, on its own: (1) arms picDIV to sync to 1PPS if a
  GPS fix is present, then (2) waits up to ~60 s for the phase voltage to settle
  inside the central band of the detector (centre ± ¼ range, held a few seconds)
  before starting the ramp. It prints each step and proceeds with a clear note
  if the phase can't be centred in time. Just run `LC` — no manual prep needed.

---

## [v0.66-rtos]

### Fixed
- **`LC` now measures the FULL detector range (was a fraction, e.g. <75 ns).**
  Two bugs collapsed `range_ns` on a narrow detector: (1) the wrap threshold was
  a fixed 0.5 V — larger than the whole ~0.33 V detector range — so wraps were
  never detected; (2) `range_ns` was taken from the small slice the phase
  happened to sweep during the ramp, not the detector's full unambiguous span.
  `LC` now sweeps until it has seen **two wraps** (one full cycle), tracks the
  true min/max across wraps for the range, and still fits the slope (ns/V) on
  the clean pre-wrap segment. The wrap threshold is now relative to the detector
  span. Ramp/window retuned (offset 70 LSB, 180 s) so both a long clean slope
  segment and two wraps fit. `LC` reports whether it saw 0/1/2 wraps so you know
  if the range is exact, approximate, or a lower bound.

---

## [v0.65-rtos]

### Fixed
- **DPLL corrected too infrequently for a narrow detector (looked "frozen").**
  DPLL only adjusted PWM every 10 s and LOCK every `lock_interval_s`; on a
  narrow detector the phase sweeps its whole range in ~10-15 s of residual
  drift, so between corrections the phase wandered and wrapped while PWM sat
  still (seen as PWM pinned at one value for 114 samples). DPLL now corrects
  every 2 s. This is *not* a schematic error: in every state PWM (via the RC
  filter → EFC) drives the OCXO — Vphase is only the ADC feedback measurement,
  so there is correctly no analog Vphase→EFC path.
- **LOCK interval clamped to a sane range (1..30 s).** A corrupted
  `lock_interval_s` (e.g. the 50373 seen in a log) would have made LOCK correct
  roughly once every 14 hours; it is now bounded at runtime and in the `LIV`
  command so LOCK keeps tracking.

---

## [v0.64-rtos]

### Changed
- **Removed the unreliable polarity auto-probe; polarity is now set manually.**
  The single-cycle probe could not separate the PWM effect from the phase's own
  drift on a narrow, drifting detector, so it repeatedly detected the wrong sign
  (+1 where the board is −1). ACQ now holds and prints a reminder to run
  `LPOL -1` (or `+1`) then `ES` when polarity is unset, and DPLL/LOCK already
  hold when polarity is unknown. Once `LPOL` is set and saved, all three stages
  use it consistently — this is reliable where the probe was not.

---

## [v0.63-rtos]

### Fixed
- **Detected polarity is now shared by all three stages.** The auto-detected
  sign lived in a static local inside ACQ, invisible to DPLL/LOCK, which then
  fell back to +1 and — on a reversed board with polarity unsaved — drove the
  phase to the ceiling rail with PWM climbing and frequency walking away from
  10 MHz. ACQ now writes the detected sign into `g_ltic.polarity`, so every
  stage uses it (and it prints a reminder to `ES`).
- **DPLL/LOCK hold instead of guessing when polarity is unknown.** With no
  established sign they now output zero correction and let the machine fall back
  to ACQ (which probes), rather than assuming +1 and running away.
- **Runaway guard.** If the phase is pinned to a rail while PWM is pushed more
  than ~6000 LSB from where the loop started, the loop freezes and warns once
  ("check LPOL / re-centre") instead of sliding PWM to an extreme and
  undisciplining the OCXO.

### Note
- Save your polarity: after the loop prints "detected …polarity -1", run `ES`
  so it survives a reboot (this was the root cause of the last runaway — the
  sign was set but never saved, so it reverted to auto/one).

---

## [v0.62-rtos]

### Fixed
- **DPLL and LOCK now apply the board polarity (was ACQ-only).** ACQ used the
  detected/forced `LPOL` sign, but DPLL and LOCK did not — so on a reversed
  board they drove the phase the wrong way, shoving Vphase onto the floor rail
  and dropping straight back to ACQ (the phase would centre in ACQ, hand over to
  DPLL, then get pushed to ~0 V and fall back). All three stages now share the
  same polarity, so DPLL/LOCK pull the phase toward centre instead of into a
  rail. With ACQ handover already working (v0.61), this is what lets DPLL hold
  and progress to LOCK.

---

## [v0.61-rtos]

### Fixed
- **ACQ now nulls the phase drift instead of chasing phase position.** With the
  polarity correct (`LPOL -1`) PWM stopped running away, but the phase still
  swept the whole detector and wrapped, so ACQ never met the "in-window + low
  slope" exit. The residual frequency offset (~-0.26 Hz) drove the phase at
  ~26 ns/s across a 318 ns detector — far too fast. ACQ's dominant term now acts
  on the phase DRIFT (dPhase/dt), driving the frequency offset to zero so the
  phase stops moving; a weak centring term parks it mid-range only once the
  drift is already small. Wrap-induced drift spikes (phase jumping >½ range in a
  step) are rejected so they don't corrupt the drift estimate or the
  slope-gated transitions.

---

## [v0.60-rtos]

### Fixed
- **ACQ ran PWM away when the board polarity was reversed.** ACQ walked PWM in a
  fixed direction toward `zero_offset`; on hardware where increasing PWM lowers
  the phase voltage (opposite sign), that drove PWM ever downward while the
  phase wrapped chaotically, so ACQ never settled (observed as a long ACQ hang
  with PWM sliding from ~41000 to ~17000). ACQ now **auto-detects the PWM→phase
  polarity** with a small probe step, then drives toward the target with the
  correct sign. A new `LPOL -1/0/1` command forces the sign (0 = auto).
- **ACQ now centres on the middle of the detector range, not `zero_offset`.**
  On a narrow low-band detector `zero_offset` can sit near the floor (e.g.
  0.097 V), so targeting it kept the phase against the rail (risking latch-up /
  wrap, per Dan's note about choosing mid-scale). ACQ now aims at the range
  middle, overridable with `LCV <volts>`.

### Added
- `LPOL` (PWM→phase polarity) and `LCV` (ACQ centring target) CLI commands,
  both persisted to EEPROM and shown by `LL`.

---

## [v0.59-rtos]

### Changed
- **Phase-slope gating on state transitions (algorithm 10).** On advice from
  Dan (time-nuts), both LTIC state transitions now check the phase SLOPE
  (dPhase/dt), not just the phase magnitude. Since frequency is the first
  derivative of phase, a small slope means the frequency is already close to
  10 MHz — so ACQ→DPLL now requires a wide slope window and DPLL→LOCK a ~5×
  tighter one, preventing a handover while the phase is merely sweeping through
  centre at speed (which would lock the wrong frequency). LOCK also drops back
  to DPLL if the slope grows. This is what makes the frequency land very close
  to nominal at each handover.

---

## [v0.58-rtos]

### Fixed
- **`LC` ramp far too fast for a narrow detector.** On hardware whose detector
  spans only a fraction of the ADC (e.g. ~0.33 V per unambiguous period), the
  old +2000 LSB ramp drove the phase across the whole detector every ~1-2 s, so
  every sample railed or wrapped and `LC` aborted with "mostly railed". The
  default ramp offset is now a gentle 60 LSB (≈4-5 ns/s on a typical OCXO), and
  `LC` adaptively steps the offset down further if the measured drift would
  cross the detector in under ~15 s. The frequency-measurement fix from v0.56
  is confirmed working (real df now reported, e.g. 1.4-2.0 Hz, not the old
  hard-coded 0.6).

---

## [v0.57-rtos]

### Fixed
- **ACQ now actively centres the phase (was frequency-only).** The ACQ stage
  previously corrected only the TIM2 frequency error; once the OCXO was already
  near 10 MHz nothing drove the phase, so it could sit stuck against a detector
  rail forever and never satisfy the ACQ→DPLL exit test (observed as an
  overnight hang with Vphase parked low). ACQ now walks PWM toward the detector
  centre when the reading is railed, and drives proportionally to the phase
  error once it is in-window.
- **Phase centre taken from calibration, not a hard-coded 1.65 V.** Real
  hardware can have a narrow detector band far from mid-ADC (e.g. 0..0.45 V), so
  the loop now centres on the calibrated `zero_offset` (with a coarse 0.22 V
  fallback) instead of assuming 1.65 V. Run `LC` so `zero_offset`/`ns_per_volt`
  reflect the real band.

---

## [v0.56-rtos]

### Fixed
- **`LC` frequency measurement.** The calibration read the 10 s frequency
  average once, immediately after a 10 s settle — on real hardware that window
  had not yet caught up to the forced ramp, so the ramp rate (and therefore
  `ns_per_volt`) came out wrong. `LC` now settles 30 s, then samples the 100 s
  average (steadier, with a 10 s fallback) twice ~5 s apart and averages them.
- **`LC` rail handling.** Samples where the TIC voltage sits at the ADC ceiling
  or floor (phase outside the detector window) are now skipped rather than
  flattening the least-squares fit, and `LC` aborts with a clear message if the
  ramp is mostly railed (telling you to centre Vphase near mid-rail first).
- **Build fix:** removed a duplicate `g_ltic_voltage` extern in
  GPSDO_algorithms.cpp that conflicted with the `gpsdo_state.h` declaration.

---

## [v0.55-rtos]

### Added
- **Algorithm 10 (LTIC three-stage PLL) — the loop is now implemented.**
  `LA 10` disciplines the OCXO from the hardware TIC phase (PA1) through a
  hybrid ACQ → DPLL → LOCK state machine. ACQ is frequency-led (TIM2) to pull
  the OCXO close to 10 MHz so the phase ramps slowly enough to catch; DPLL adds
  the LTIC phase term for fast centring; LOCK is phase-led with slow updates
  every `lock_interval_s` and a hysteresis band for dropping back to DPLL. The
  picDIV is armed automatically on entering ACQ. The loop works in nanoseconds
  when the TIC is calibrated (`LC`), and falls back to a nominal volt-based
  phase with a one-time warning when it is not. The state persists in
  `g_ltic.state`, so a warm reboot (`RB`) resumes mid-sequence rather than
  restarting from ACQ. The trend field shows `ACQ` / `DPLL` / `LOCK`.
- **Third PID set (ACQ).** `LticParams_t` gained an `acq` PID alongside `dpll`
  and `lock`, with its own CLI verbs `AQP` / `AQI` / `AQD` / `AQL` and EEPROM
  storage. `LL` now lists all three sets.

### Changed
- **EEPROM layout extended to 216 bytes (reserved to 224).** The ACQ PID block
  [200..215] was appended under the same `GPSD2` signature with the usual
  NaN/`0xFF` guards, so older saves still load with the ACQ gains defaulting.

---

## [v0.54-rtos]

### Added
- **`LC` — LTIC self-calibration.** Automatically measures the TIC's
  voltage→time slope without any external reference. `LC` forces a small PWM
  offset so the phase ramps linearly, derives the ramp rate from the TIM2
  frequency error (`phase_rate = df / BASE_FREQ × 1e9` ns/s), least-squares
  fits the TIC voltage against time to get `dV/dt`, and computes
  `ns_per_volt = phase_rate / (dV/dt)`. It also records the swept voltage span
  as `range_ns` and a mid-scale `zero_offset`, detecting one wrap to keep a
  single clean ramp segment. Runs in the control task like `CT`, with the same
  safety pattern (PWM saved and restored, range-guarded results, abort on
  no-GPS / too-few-points / singular or flat fit — params left unchanged on
  any failure). Results go to the live LTIC params; review with `LL`, then
  `ES` to save. New config constants `LTIC_CAL_PWM_OFFSET`, `LTIC_CAL_SECS`,
  `LTIC_CAL_MIN_POINTS`. This fills the calibration fields that the phase-A
  loop will need; the loop itself is still not implemented.

---

## [v0.53-rtos]

### Added
- **Warm/cold restart commands `RB` and `CR`.** `RB` does a warm reboot
  (`NVIC_SystemReset()`) keeping the EEPROM, so the still-warm OCXO recalls its
  disciplined state. `CR YES` does a cold restart: erases the EEPROM (back to
  factory defaults — PWM, model, calibration, LTIC params all reset) then
  reboots; the `YES` confirmation is required because it discards the learned
  OCXO model.
- **Algorithm 10 (LTIC) infrastructure — parameters, CLI and EEPROM.** Full
  parameter set, CLI editing and EEPROM persistence for the planned LTIC
  three-stage PLL (ACQ→DPLL→LOCK), so the configuration is ready before the
  loop itself is written ("phase A"). New `LticParams_t` holds TIC calibration
  (ns/V, zero offset, range), two PID sets (wide-band DPLL + narrow-band LOCK),
  state-transition thresholds, the LOCK interval, and the resumable state.
  Fifteen CLI commands set/show these (`LL`, `LNV/LZO/LRN`, `DPP/DPI/DPD/DPL`,
  `LKP/LKI/LKD/LKL`, `LAT/LDT/LIV`). `LA 10` is accepted by the parser but
  reports "not implemented yet" and refuses to select, so the OCXO is never
  left undisciplined. The loop itself is not implemented — that is phase A,
  pending the LTIC hardware.

### Changed
- **EEPROM layout extended to 200 bytes (reserved to 208).** The LTIC block
  [144..207] was added under the **same `GPSD2` signature**; every new field is
  NaN/`0xFF`-guarded, so EEPROM images saved by older firmware load cleanly with
  the LTIC parameters defaulting until set. No migration or re-init needed.

---

## [v0.52-rtos]

### Added
- **LTIC (Lars' TIC) phase-voltage preview.** The TIC voltage on PA1 was
  already sampled and sent over serial telemetry, but had no on-screen
  presence. Added (all gated by `GPSDO_LTIC`, so zero effect on builds without
  the TIC):
  - a **TFT row** showing `Vph:x.xxxV` (and `… NNNns` once calibrated);
  - an **LTIC entry in the boot-splash hardware checklist** (`[x] LTIC phase
    (PA1)` — shown when compiled in, like the TM1637/TFT, since the TIC is
    read-only and cannot be probed);
  - a **`LTIC_NS_PER_VOLT` calibration constant** in the config (0 =
    uncalibrated → volts only). When set to the measured ramp slope, the
    display and the planned phase-discipline algorithm convert volts to ns.
  This is a **preview/telemetry layer only** — the control loop does not yet
  discipline the OCXO from the TIC (planned as a separate phase, a new
  LTIC-based algorithm). OLED/LCD were intentionally left unchanged (their
  layouts are full); Vphase remains available there via serial logging, which
  is what characterising the TIC needs at this stage.

---

## [v0.52-rtos]

### Added
- **LTIC (Lars' TIC) phase-voltage preview layer.** When `GPSDO_LTIC` is
  compiled in, the latched TIC voltage (`g_ltic_voltage`, already sampled on
  PA1 and discharged each PPS) is now surfaced as a preview: a dedicated
  `Vph:` row on the TFT (below the sensor row, shown only with LTIC built in),
  and an `LTIC phase (PA1)` entry in the boot checklist. Serial telemetry
  already carried Vphase. A new `LTIC_NS_PER_VOLT` calibration constant lets a
  future build convert the voltage to a phase in nanoseconds: while it is 0
  (default, uncalibrated) the displays show volts only; once set, the TFT row
  also shows `<n>ns`. This is preview/telemetry only — the control loop does
  not yet discipline on LTIC; that is a planned separate algorithm. OLED/LCD
  layouts are unchanged (both are full); Vphase will be added there when LTIC
  becomes an operational loop input.

---

## [v0.51-rtos]

### Added
- **CLI commands are now case-insensitive.** The command dispatcher compared
  verbs with `strcmp()`, so `LA` worked but `la` did not. Command matching now
  uses a small case-insensitive helper (`cli_ieq`), so any letter case is
  accepted (`LA` / `la` / `La` are equivalent), including the lowercase verbs
  (`up1`, `dp10`, …) and the `KP`/`KI`/`KD`/`IL` family (whose parameter
  letter is also matched case-insensitively). Command arguments are unchanged;
  `TO A` already accepted either case.

### Changed
- **ZED-F9T (Gen9) support is no longer experimental.** The CFG-VALSET
  survey-in path and the NAV-SVIN monitor fallback were tested on real
  hardware by EEVblog user danieljw, so the "experimental / untested" markings
  have been removed from the code, config and READMEs. No code change to the
  F9T path itself — only its status.

---

## [v0.50-rtos]

### Added
- **ZED-F9T (Gen9) timing-receiver support — experimental, untested.** A third
  survey-in path was added alongside the proven LEA-6T / LEA-M8T ones.
  `ubx_start_survey_in()` now also sends a `CFG-VALSET` (0x06 0x8A) frame
  setting the Gen9 configuration keys `CFG-TMODE-MODE` (survey-in),
  `CFG-TMODE-SVIN_MIN_DUR` and `CFG-TMODE-SVIN_ACC_LIMIT` (the latter converted
  from mm to the F9T's 0.1 mm unit). The survey-in monitor gained a parallel
  `NAV-SVIN` (0x01 0x3B) parser and falls back to it when `TIM-SVIN` does not
  answer, since the F9 generation reports survey-in through NAV-SVIN. ⚠️
  Written from u-blox documentation/ubxtool with no F9T on hand — key IDs, the
  0.1 mm unit and the NAV-SVIN payload offsets are NOT verified on hardware.
  The legacy `CFG-NAV5` stationary frame may NAK on an F9T (harmless; the
  survey-in path is independent). The two tested receivers are unaffected:
  TIM-SVIN is still tried first, so LEA-6T / LEA-M8T / NEO-M8T behaviour is
  unchanged. Documented as experimental in the README and config.

### Changed
- **LCD 20×4 splash subtitle** changed from `GPS-Disciplined Osc.` to
  `GPS-Disciplined OCXO`, matching the TFT splash (both 20 chars, full width).

### Notes
- **NEO-M8T** confirmed (by datasheet analysis) fully compatible with the
  existing LEA-M8T path — same M8 silicon + FW3, same CFG-TMODE2 / TIM-SVIN —
  no code change required. Documented in the timing-receiver section.

---

## [v0.49-rtos]

### Fixed
- **Config macro ordering: `OUT_SERIAL` now respects `GPSDO_BLUETOOTH`.** The
  `OUT_SERIAL` routing macro was evaluated near the top of `gpsdo_config.h`,
  *before* `GPSDO_BLUETOOTH` (and several other feature switches) were defined
  further down. As a result `OUT_SERIAL` always resolved to USB `Serial` even
  when Bluetooth was enabled, and a build with `GPSDO_BLUETOOTH` commented out
  could fail to compile depending on what referenced it. All feature switches
  are now grouped together near the top of the file, and macros derived from
  them (`OUT_SERIAL`) are evaluated afterwards in a dedicated "Derived macros"
  section. No functional change to any enabled feature beyond Bluetooth output
  now actually going to Serial2. A scan of the other source files found no
  further define-after-use ordering issues.

### Changed
- **HT16K33 startup pattern unified with the TM1637.** At power-up the
  HT16K33 now shows `----` (segment-G dashes) instead of `oooo`, matching the
  TM1637's startup pattern — both LED clocks signal "alive, waiting for GPS"
  the same way. The `oooo` indicator is retained for the no-fix-during-
  operation case (where the TM1637 also shows `oooo`), so the two displays now
  behave identically in every state.
- **TFT splash credit line** changed from `jmnlabs + with Claude (Anthropic)`
  to `jmnlabs with Claude (Anthropic)` (dropped the `+`).

---

## [v0.48-rtos]

### Added
- **ILI9488 480×320 SPI TFT support (`GPSDO_TFT_ILI9488`).** ⚠️ Untested — no
  panel on hand yet. The existing 320×240 ILI9341/ST7789 operating screen and
  animated splash are shared and auto-scaled to 480×320 at compile time:
  width ×1.5 and height ×1.33 via independent `TFT_SX`/`TFT_SY` macros (the
  panel aspect differs from a pure 1.5×), and TFT_eSPI fonts mapped up one
  size via `TFT_F`. Geometry verified to fit the panel; not yet run on real
  hardware. Set `ILI9488_DRIVER` + `TFT_WIDTH 320`/`TFT_HEIGHT 480` (+
  `LOAD_FONT6`) in TFT_eSPI `User_Setup.h`.
- **SPI→T6963C bridge as a new display backend (`GPSDO_T6963C`).**
  ⚠️ Experimental / untested — backend is complete and compiles, but the link
  is not yet validated on clean hardware (long-wire bring-up showed ringing
  and spurious CS edges; same on the reference master → a signal-integrity
  issue, not firmware). Disabled by default; leave off until tested on
  short, point-to-point wiring.
  Drives a PowerTip PG240128 (240×128 mono) panel through the external
  `T6963C_SPI_bridge` over SPI1 using high-level drawing commands
  (`T6963C_Bridge.h`). Selectable in the config like the other displays;
  mutually exclusive with the TFT (shared SPI1 pins / display slot).
  - Reuses the TFT's SPI1 pins: `SCK PA5`, `MOSI PA7`, `CS PB13`,
    `READY PB12`; frees `PB15` (was TFT_RST).
  - Condensed 240×128 layout mirroring the TFT screen: header (title + LMT
    time), large frequency (LOGISOSO fonts), status row, value rows
    (PWM/Vctl, INA219, sensors) and a survey-in progress bar.
  - Monochrome panel → the lock/holdover colour cue becomes an inverted
    (filled) box around the status word (`LOCK` / `HOLD` / `H-LOST` /
    `NOFIX`).
  - One batched SPI transaction per refresh (single READY wait), with the
    bridge library's auto-split as a safety net; per-field change-cache to
    skip redundant redraws.
  - Static boot splash (logo + subtitle + hardware checklist); no wave
    animation, since batched SPI rendering would make it costly on a small
    mono panel.

---

## [v0.47-rtos]

### Added
- **`SV` CLI command** — enable/disable survey-in (Time Mode) on a timing
  receiver at runtime, stored in EEPROM (byte 143). `SV` shows state, `SV 0`
  disables (stay in nav mode — handy for bench testing), `SV 1` enables;
  `ES` saves, applied at next boot. Defaults to enabled on fresh EEPROM.

### Fixed
- **Survey-in polling no longer stalls the displays.** `ubx_poll_svin()`
  waited up to 1000 ms with a busy `delay()`, starving the higher-priority
  GPS task's siblings — the display task visibly lagged (worst on the
  slower-responding LEA-6T). The poll now uses a ~500 ms window that yields
  with `vTaskDelay()` between reads, so the display task runs normally while
  still reliably catching the module's TIM-SVIN reply (100-200 ms latency).
  NMEA bytes seen while scanning are forwarded to TinyGPS++ so the fix is
  not disrupted. Once a survey has replied, occasional missed polls no
  longer abort the monitor (the survey is in progress); gaps in the
  `svin dur=` sequence are gone.
- **Survey-in now exits reliably when its criteria are met.** Completion is
  declared when EITHER the receiver flags the mean position valid, OR the
  user criteria are met (accuracy ≤ limit AND duration ≥ minimum) — some
  receivers (notably the LEA-6T) reached ~0.45 m well past the minimum but
  left the survey "active", so the old `valid && !active` test never fired.
  The safety backstop is now `3 × SVIN_MIN` (min 600 s) so a slow-converging
  survey on a weak antenna gets a fair chance.
- TIM-SVIN early-survey accuracy of `0xFFFFFFFF` ("no estimate") is clamped
  to 65535 mm instead of overflowing.

### Changed
- **TFT precision**: INA219 now shows bus voltage to 3 decimals and current
  to 2 decimals; the PWM control voltage (Vctl) shows 3 decimals.

### Documentation
- README (EN/PL) notes that survey-in needs a good outdoor antenna with a
  full sky view, and records the field observation that the LEA-6T is more
  sensitive than the LEA-M8T in marginal conditions. Both modules were
  verified completing survey-in and entering Time Mode on a professional
  outdoor (survey-grade) antenna. Corrected a couple of stale source
  comments (EEPROM size 144 B, TIM-SVIN vs NAV-SVIN).

---

## [v0.46-rtos]

### Removed
- **Compile-time OCXO selection (CTI / Vectron) dropped entirely.** The `CT`
  command measures the plant gain and derives all coefficients for whatever
  oscillator is fitted, so per-OCXO defines, PID tables and the
  `DEFAULT_PWM` switch are no longer needed. The loop starts from a
  universal mid-range PWM (32767 ≈ 1.65 V) before the first `CT`.

### Added
- **Multi-variant survey-in start.** The LEA-6T and LEA-M8T accept
  different Time Mode commands (both verified in u-center), so the firmware
  tries each in turn and stops at the first ACK: `CFG-TMODE2` 0x06 0x3D
  (LEA-M8T), then the classic `CFG-TMODE` 0x06 0x1D (LEA-6T, u-blox 6). This
  auto-adapts to either module. If neither is ACKed the module is assumed to
  be already timing and is monitored anyway.

### Fixed
- **TIM-SVIN accuracy was nonsense (showed ~467 km).** The `meanV` field is
  a position *variance* in mm², not a distance — the firmware now takes its
  square root to report a 1-sigma accuracy in mm (verified against u-center:
  18113534 mm² → ~4.3 m). Survey-in duration/accuracy now read sensibly.
- **Boot hang when survey-in actually started (LEA-M8T).** The survey-in
  progress loop ran inside `gpsdo_gps_init()` — before the scheduler — and
  used `vTaskDelay()`, which hangs the system when called before
  `vTaskStartScheduler()`. It never showed on the LEA-6T because that unit
  NAKed the start and skipped the loop; the M8T ACKs it, entered the loop,
  and froze (blue LED stuck). Survey-in now only *starts* in init; progress
  is polled non-blocking from `vGpsTask` after the scheduler runs.
- **Intermittent boot hang / black displays** — `STACK_DISPLAY` raised from
  768 to 1024 words. Font scaling and the OLED clear loop had made 768
  marginal; with no stack-overflow hook this showed as a silent, sometimes-
  boots hang.
- **LEA-M8T timing module now works.** It was stuck in a 3D nav fix
  (HDOP ≈ 1) because the firmware sent it `CFG-TMODE3`, which its firmware
  (TIM 1.10, PROTVER 22) does not support. u-center confirmed the LEA-M8T
  uses the **same** `CFG-TMODE2` / `TIM-SVIN` messages as the LEA-6T. The
  timing path is unified to a single TMODE2 implementation; the separate
  `GPSDO_GPS_LEA6T` / `GPSDO_GPS_LEA8T` options are replaced by one
  `GPSDO_GPS_TIMING`, and the TMODE3 / NAV-SVIN branch is removed.
- **OLED**: the lower half of the big `GPSDO` splash (drawn with a two-row
  font) lingered behind the LMT clock — the panel is now cleared, every row
  blanked, the 2x2 font reset and the row cache invalidated when the splash
  ends. `GPSDO` and the version line are centred; footer uses
  `jmnlabs+Claude`.
- **LCD 20x4**: title/version line shifted right (two leading spaces) so the
  `-rtos` suffix is no longer truncated.
- EEPROM layout header comment corrected (143 bytes, was mislabelled 134).

### Changed
- **TFT**: the white frequency value uses a fixed-width font (font 1,
  size 3) so its digits keep a constant column position; subtitle enlarged
  and changed to `GPS-Disciplined OCXO`; logo, subtitle and the
  converging-wave animation raised; hardware checklist reveals more slowly
  with a lead-in pause so the first items are not missed; footer credit
  uses `+`. Sensor values (BMP/AHT temperature, pressure, humidity) now show
  two decimal places.

---

## [v0.45-rtos]

### Changed
- **TFT splash reworked again** to a phase-lock metaphor: the credits are
  drawn first and persist; two 2px sine waves (blue above, amber below)
  start with a visible phase offset and small vertical gap, then slowly
  converge until they coincide and merge into a single 4px green wave,
  held ~1.8 s. The hardware checklist follows.
- Serial human-readable report now shows `HDOP:TIME` in Time Mode (the
  tab-delimited machine format keeps the numeric value for plotting).

### Removed
- Redundant `SERIAL_*_BUFFER_SIZE` defines in `gpsdo_config.h` (they never
  reached the core anyway). The buffer sizes live solely in `build_opt.h`
  (`RX=256, TX=512`).

---

## [v0.44-rtos]

### Added
- **`build_opt.h`** enlarging the serial RX/TX buffers to 256 bytes
  (`-DSERIAL_RX_BUFFER_SIZE=256 -DSERIAL_TX_BUFFER_SIZE=256`). STM32duino
  applies these to the whole build including the core, which a sketch-level
  `#define` cannot reach. This prevents NMEA sentences being dropped or
  merged at 38400 baud when the GPS task is briefly preempted (the cause of
  the garbled NMEA seen on the LEA-6T).

### Changed
- **TFT boot splash reworked**: two sine waves of different colours (blue
  from the left, amber from the right) converge to the centre and merge
  into a single green 10 MHz wave — a synchronism metaphor — with the
  GPSDO logo and hardware checklist below. Timings stretched for
  readability.

### Notes
- Only GGA + RMC NMEA sentences are kept (GLL/GSA/GSV/VTG disabled), which
  together with the larger buffer keeps the bus well within budget.

---

## [v0.43-rtos]

### Added
- **Time Mode detection / `HDOP:TIME`.** A timing receiver in time-only
  mode keeps a frozen valid position but reports HDOP ≈ 99.99. Instead of
  showing that meaningless number, the displays now show `HDOP:TIME` when a
  valid position coincides with a non-meaningful HDOP (≥ 50.00). New
  `gGps.time_mode` flag.

### Changed
- **Survey-in NAK is handled gracefully.** Some timing modules (e.g.
  surplus units with a stored Time-Mode config) NAK `CFG-TMODE2/3`. The
  firmware no longer treats this as an error — it logs that the module may
  already be timing and continues; runtime Time Mode detection then reports
  the real state.
- Boot splash durations lengthened (TFT ~7 s, OLED/LCD ~4.5 s) so the
  welcome screen can actually be read.

### Fixed
- OLED splash footer no longer clips the last character (`jmnlabs/Claude`,
  spaces around the slash removed to fit the 16-column width).

---

## [v0.42-rtos]

### Fixed
- **Build error in the survey-in code** (`get_ubx_ack` called with
  class/id/timeout instead of the message-buffer pointer it expects). Both
  `ubx_start_survey_in` branches now pass the frame buffer, matching the
  function signature. LEA timing builds compile again.

### Notes
- The u-blox M8 timing module (**LEA-M8T**) is the same generation as the
  8T and uses CFG-TMODE3 / NAV-SVIN — enable `GPSDO_GPS_LEA8T` for it.

---

## [v0.41-rtos]

### Added
- **Animated boot splash on TFT**: a sweeping 10 MHz sine, the GPSDO logo,
  and a hardware checklist reconstructed from the real detection flags
  (modules show `[x]` / `[ ]`), with a discreet `jmnlabs · with Claude
  (Anthropic)` footer. Plays once, then the operating screen is drawn.
- **Boot splash on OLED** (character mode, U8x8): double-size `GPSDO`,
  version, accent line and footer.
- **Boot splash on LCD 20x4**: four-line welcome with title, subtitle and
  footer.

### Fixed
- **TFT did not update PWM / Vctl during calibration.** The display
  returned early after drawing the countdown, freezing the info grid. It
  now falls through so the PWM/Vctl cell keeps updating live during
  `C` / `CT` — matching the OLED behaviour.

---

## [v0.40-rtos]

### Added
- **LEA-6T / LEA-8T timing receiver support** (`GPSDO_GPS_LEA6T` /
  `GPSDO_GPS_LEA8T`). On these modules the firmware runs a survey-in at
  every power-up (CFG-TMODE2 on the 6T, CFG-TMODE3 on the 8T), then the
  receiver switches to a fixed-position time-only solution with a much
  cleaner 1PPS. Survey-in ends when either the minimum duration
  (`GPSDO_SVIN_MIN_SECS`, default 120 s) or the accuracy limit
  (`GPSDO_SVIN_ACC_LIMIT`, default 2000 mm) is met.
- Survey-in progress is shown on every display (`SVIN nnns nnm` on
  OLED/LCD/TFT, dashes on the LED clocks), via the new `g_svin_*` state.
- Position keeps streaming in NMEA throughout Time Mode, so location
  display and automatic timezone (`TO A`) continue to work — using the
  averaged, frozen survey-in position.
- `CHANGELOG.md` and `CHANGELOG_PL.md` are now included in the project archive.

### Notes
- NEO-6M / NEO-8M behaviour is unchanged (neither LEA option defined).

---

## [v0.39-rtos]

### Added
- OCXO warmup is now shown on every display with a live countdown
  (`WARMUP nnn s` on OLED/LCD/TFT, dashes on TM1637/HT16K33), driven by the
  new `g_warmup_active` / `g_warmup_remaining` state.

---

## [v0.38-rtos]

### Fixed
- **Steady-state PWM dither on the phase-locked algorithms (4, 5, 7, 8).**
  The dead-zone now tests the accumulated phase as well as the frequency
  error: when `|e| < 1 mHz` and `|phase| < 5 Hz·s` (≈500 ns) the loop holds
  the PWM and reports `hit`, so a locked oscillator stops being nudged by
  GPS noise every period. Small phase noise is held; real drift is still
  corrected.
- All phase algorithms now actually emit the `hit` trend on lock; FLL
  algorithms (3, 6) gained an equivalent frequency-only lock hold.
- PWM and Vctl readings on the displays now update live **during** `C` /
  `CT` calibration (a new `wait_secs_pwm` publishes PWM and samples the
  Vctl ADC each second while the main loop is busy).

---

## [v0.37-rtos]

### Changed
- `LP 8` and `LP 9` now show where those algorithms actually read their
  gains: algo 8 (hybrid) uses `g_pid[6]` (FLL branch) + `g_pid[7]` (PLL
  branch); algo 9 (NN) uses fixed network weights, so only `NS` / `IL`
  apply. Prevents the empty `g_pid[8]/[9]` from looking "untuned" after
  `CT`.

---

## [v0.36-rtos]

### Added
- Calibration progress shown on all displays: `CAL nnn s` countdown in the
  frequency field (OLED/LCD/TFT) and `CAL` on the LED clocks (TM1637 /
  HT16K33), via `g_calib_active` / `g_calib_remaining`.

---

## [v0.35-rtos]

### Added
- **`CT` (Calibrate & Tune) command.** Measures the plant gain `K` from a
  three-point PWM sweep (1.5 / 2.0 / 2.5 V) with a least-squares fit, finds
  the PWM for exactly 10 MHz, and derives PID coefficients for all
  algorithms from `K` (PLL: `Kp = 0.40/K`; FLL: `Kp = 0.35/K`,
  `Ki = Kp/300`, `Kd = Kp·73`; NN: `max_step = 0.05/K`). Sanity-checked,
  non-destructive; `ES` saves the result.

---

## [v0.34-rtos]

### Changed
- **Two-timescale PLL tuning for "fast capture, gentle phase-hold".** The
  dominant term acts on the frequency error (`Kp ≈ 0.4/K`) for quick,
  overshoot-free capture; small phase terms remove slow drift. A shared
  output stage adds a slew-rate limit (≈12 LSB/step for the PLLs, 40 for
  the hybrid) and a near-lock dead-zone, so a large overnight phase drift
  is spread over several periods instead of one big PWM jump.

---

## [v0.33-rtos]

### Fixed
- **Algorithm 9 (NN) ran away upward.** The previous "trained" weights had a
  large output bias (≈ −0.96 at zero error → constant PWM ramp). Replaced
  with an analytically constructed, bias-free, odd-symmetric network: zero
  input gives exactly zero output.
- **Algorithms 4 / 5 / 7 and the PLL branch of 8 drifted.** They used a
  rolling-window average as a stand-in for phase, which lagged the 10 s
  update by 500–1000 s and wound the integrator up. Replaced with true
  phase accumulation (`phase += (avg10 − 10 MHz)·10 s`, the exact cycle
  count), feeding back with a 10 s lag.
- The `GPS fix acquired` message now distinguishes the first fix after boot
  from a genuine recovery after fix loss.

### Added
- **Automatic timezone (`TO A`).** Local time follows the GPS position: a
  compact European civil-zone rule set plus the EU DST rule, or a solar
  `round(lon/15)` zone elsewhere. `TO <n>` keeps the manual mode. The mode
  is saved to EEPROM (byte 142, now 143 bytes total) and restored at boot.

---

## [v0.32-rtos]

### Fixed
- **Hardware detection report.** Added a robust dual-verification I2C probe
  (address ACK + 1-byte read-back). OLED and HT16K33 were previously
  reported `OK` unconditionally / on an unreliable ACK; they now report
  real presence. TM1637 and TFT are marked `enabled (write-only — not
  verifiable)`.
- **TFT frequency colour.** The green "locked" colour is now derived from
  the actual deviation from 10 MHz (≤1 mHz on the 10000 s window or ≤10 mHz
  on 1000 s), independent of the algorithm — so a locked algo 8 turns green
  too, rather than only on the rarely-emitted `hit` trend.

---

## [v0.31-rtos]

### Added
- **HT16K33 4-digit clock support** (I2C 0x70): a self-contained driver
  (HH:MM with blinking colon, `oooo` when searching), shareable with the
  LCD on the same bus — no extra pins. TM1637 retained.
- Unified startup hardware report: every optional device reports `OK` or
  `not found` in a consistent `HW:` format.
- New hardware architecture diagram in both READMEs (TFT + HT16K33).

---

## [v0.30-rtos]

### Added
- **TFT 240×320 support (ILI9341 / ST7789)** via TFT_eSPI on hardware SPI1
  (SCK PA5, MOSI PA7, RES PB15, DC PB12, CS PB13). Landscape layout: header
  bar, large colour-coded frequency, two-column info grid, sensor row, and
  a colour-coded status bar. Selective per-cell redraw keeps SPI traffic
  low. DisplayTask stack raised to 768 words when the TFT is enabled.
  Both controllers tested on hardware.

---

## [v0.29-rtos]

### Fixed
- **picDIV synchronisation.** Arming is now deferred until a GPS fix is
  present (a stopped divider with no 1PPS on Sync would otherwise hang
  dead); a dedicated flag replaces the millis-timestamp guard (wrap-safe);
  auto-arm after calibration was removed (the loop hasn't converged yet).
  Added clear serial feedback. README documents FLL phase random-walk vs
  PLL phase-lock for long-term 1PPS alignment.

---

## [v0.28-rtos]

### Fixed
- **PWM range with 3.3 V DAC.** The STM32 PWM reaches only 0–3.3 V of the
  0–4 V EFC input (82.5 %), so the accessible tuning is −10…+14.75 Hz (CTI)
  and −20…+13 Hz (Vectron). Default PWM corrected per-OCXO: 32767 (CTI,
  1.65 V midpoint) and 39718 (Vectron, 2.0 V nominal).

---

## [v0.27-rtos]

### Fixed
- **Vectron C4550A1-0213 parameters.** Corrected to its real operating
  point: 5 V supply, 0–4 V EFC, Kv = 10 Hz/V (0.504 mHz/LSB), scale factor
  1.333 vs CTI (gains × 0.75), shared default PWM.

### Changed
- `README_EN.md` renamed to `README.md` (GitHub default); `README_PL.md`
  unchanged.

---

## [v0.26-rtos]

### Added
- **OCXO selection** in `gpsdo_config.h` (`GPSDO_OCXO_CTI_OSC5A2B02` /
  `GPSDO_OCXO_VECTRON_C4550`), with per-OCXO compile-time PID defaults and
  default PWM. Falls back to CTI values if none is selected.
- `SP`, `F`, `C`, `T` documented in the help text and READMEs.

---

## [v0.25-rtos]

### Added
- `g_pressure_offset` (`PO`) and `g_altitude_offset` (`AO`) now saved to and
  restored from EEPROM (bytes 134–141, 142 bytes total).
- `V` command expanded with full author/credit information and GitHub links.

---

## [v0.24-rtos]

### Fixed
- **Bluetooth output.** All runtime messages route through an `OUT_SERIAL`
  macro (Serial2 when `GPSDO_BLUETOOTH` is defined, else USB Serial).

### Added
- Report pause/resume (`RP` / `RR`) to quiet the data stream during
  configuration.
- Algorithm PID parameters saved to EEPROM (signature `GPSD2`).
- Professional file-header documentation across all source files; README
  rewritten from scratch (project description, hardware principle, software
  architecture) in Polish and English; GitHub URL added to every file and
  to the serial banner.

---

## [v0.23-rtos]

### Added
- **Runtime PID tuning over CLI** — `LP`, `KP`, `KI`, `KD`, `IL` for
  algorithms 3–7, `BC` / `BS` for the algo 8 blend, `NS` for the algo 9 NN
  step. Coefficients moved to a global `g_pid[10]` array.

---

## [v0.22-rtos]

### Added
- Yellow LED 4-state machine (off / on / slow pulse = manual holdover /
  fast pulse = auto-holdover) and automatic holdover on GPS fix loss with
  `H` / `A` indicators on OLED and LCD.

---

## [v0.21-rtos]

### Added
- OLED row-0 clock (local time + day of week) after the version splash;
  LCD line-2 date/day rotating view. Day-of-week (Zeller) and local-time
  offset helpers.

---

## [v0.20-rtos]

### Changed
- Unified 4-character trend strings; corrected OLED/LCD frequency
  formatting; build-time guard against LCD + TM1637 together; fixed the
  André Balsa source URL.

---

## [v0.19-rtos]

- First tracked FreeRTOS port baseline: STM32F411CE BlackPill, frequency
  measurement via TIM2 ETR + TIM3 1PPS capture, ring-buffer averaging,
  PWM-DAC discipline loop, GPS/NMEA parsing, OLED / LCD / TM1637 displays,
  optional AHT/BMP/INA sensors, and the initial control algorithms.
