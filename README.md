# Tengen Tetris (NES) → GBA port

Port de **Tetris (NES, Tengen, 1989)** a Game Boy Advance, con el objetivo de
mantener su jugabilidad y sus gráficos lo más fiel posible al original —
controles 1:1, mismas rarezas de rotación, mismo playfield pixel a pixel —
adaptando solo lo que la propia pantalla del GBA obliga a adaptar.

## Por qué es viable un port "1:1"

La pantalla de GBA (240×160 px) no es del mismo tamaño que la de NES
(256×240 px), pero el **campo de juego** de Tengen Tetris — 10 columnas × 20
filas de tiles de 8×8, con el marco trenzado del cartucho a cada lado:
112×160 px en total — tiene *exactamente* los 160 px de alto de la pantalla
del GBA. El playfield entra sin recortar ni escalar, tile por tile, y en las
mismas columnas que en la NES. Lo que no entra 1:1 es el HUD alrededor (al
NES le sobran 160 px de ancho para repartir a los costados; al GBA 144), así
que el plan es: **playfield y jugabilidad idénticos, HUD rediseñado** para
los paneles más angostos.

Un detalle que conviene saber: la pantalla del NES son en realidad **dos**
campos enmarcados iguales, uno a cada lado del banner vertical de TETRIS. El
modo 2P pone un jugador en cada uno; el modo 1P dibuja el panel de puntaje
encima del segundo. Por eso en la NES el campo de 1P queda bien corrido a la
izquierda — que en una pantalla de GBA con un solo jugador se ve raro. Así
que las columnas del cartucho no solo se recortan: se **reordenan**, cada
tramo entero, para dejar el campo justo en el centro (80 px de pantalla a
cada lado) con el HUD en dos cajones de la misma greca, uno a cada lado:
próxima pieza y contadores a la izquierda; a la derecha, según el HUD que
elijas, el banner de TETRIS, las estadísticas o el panel del otro jugador. Nada se escala ni se recorta; las 2
columnas que sobran son el marco de ese segundo campo, que ya no enmarca
nada. En vertical el NES tiene 30 filas y el GBA 20, y el campo se lleva las
20 exactas: por eso la franja de etiquetas que el NES pone *arriba* del campo
se reparte a los costados. Mismos tiles, misma tipografía.

Eso no es teoría: `make gba-check` arranca la ROM en un emulador y verifica
que el campo caiga en sus columnas, que las dos columnas de marco lleguen a
los 160 px de alto, y que el marco, el banner y el panel se dibujen. Ver
`reference/NOTES.md` para el detalle.

## Estado actual

Hay una ROM de GBA que arranca, se juega y corre las reglas reales de Tengen.

- `src/tengen_core.{h,c}` — núcleo de reglas, independiente de plataforma
  (C99 puro, sin dependencias de GBA). Todas sus mecánicas están trazadas al
  disassembly y citadas línea por línea: el generador de piezas real de
  Tengen (con su sesgo característico, sin anti-repetición), el rotado con su
  wallkick que solo patea a la izquierda, el DAS de 11/6 frames, el
  auto-rotate, la curva de gravedad con sus niveles fraccionarios, el soft
  drop que acelera, el puntaje por pieza apoyada, y la subida de nivel.
- `tests/test_tengen.c` — tests nativos de esas reglas (`make test`).
- `gba/` — capa de GBA: registros de hardware, `crt0.s`, linker script y el
  renderer. No depende de devkitARM ni de libgba: compila con un
  `arm-none-eabi-gcc` estándar. Tiene la pantalla de título original (la
  catedral de San Basilio), el selector de nivel dentro del marco de menú de
  la ROM, HUD completo, game over, la animación de línea completa (la
  bocanada de humo que cruza la fila y deja escrito SINGLE / DOUBLE /
  TRIPLE / TETRIS donde estaban los bloques), la pausa con su placa original,
  los tres códigos de trucos de Tengen (subir de nivel, barra larga y deshacer
  la última pieza), los bailarines cosacos entre niveles, y **la música y los
  efectos del cartucho** (ver abajo).
- `tools/extract_assets.py` — saca del cartucho original los tiles, las
  paletas, el layout de pantalla y las poses de los bailarines. **Todo el
  arte del port sale de ahí; no hay nada dibujado a mano.**
- `src/tengen_link.c` + `gba/link.c` — **dos jugadores por cable link**. El
  2P del cartucho es una carrera: dos campos independientes, sin basura
  cruzada entre ellos, así que el port lo hace en *lockstep* — las dos
  consolas simulan a los dos jugadores desde la misma semilla y solo se
  mandan sus botones. Las reglas y el handshake son independientes de
  plataforma y se testean en el host (`make test` corre dos partidas
  enlazadas y las compara byte a byte); el cable vive en `gba/link.c` y va
  por interrupción, para que ninguna consola pueda perderse una
  transferencia ni mandar botones viejos.
- `tools/render_nes.py` + `tools/zoom.py` — dibujan las pantallas del
  cartucho original tal como las vería una NES (ejecutando su propia rutina
  de subida de nametable, con su CHR y sus paletas) y permiten ampliarlas
  pixel a pixel. Es la imagen de referencia: cada decisión gráfica del port
  se contrasta contra ella en vez de contra un recuerdo.
- `tools/run_rom.py` — arranca la ROM en mGBA headless y verifica que
  realmente dibuje y se juegue (`make gba-check`), no solo que linkee:
  incluye ver la animación de línea completa sprite por sprite, comprobar
  que deja escrita la palabra correcta, y pausar y teclear los códigos de
  trucos. Las 61 comprobaciones viven en `tools/romcheck/`, un módulo por
  familia (partida, audio, menú de pausa, pantallas previas, HUD, la
  máquina, récords) más `harness.py` con lo que comparten.
- `tools/run_link.py` — arranca **dos** mGBA y les pone un cable link
  simulado en medio: modela el modo multiplayer del GBA (quién es maestro,
  qué lee cada consola, el `$FFFF` del hueco vacío, la interrupción) y
  comprueba que, dándole botones distintos a cada una, el estado de la
  partida queda idéntico en las dos byte a byte. También tira del cable a
  mitad de partida para ver que las dos se enteran y ninguna se cuelga.
- `gba/nes6502.c` + `gba/nes_audio.c` — el sonido. **El port no reimplementa
  el motor de audio de Tengen: lo ejecuta.** Es un intérprete de 6502 chico
  corriendo el propio código del cartucho, y una capa que traduce lo que ese
  motor le escribe al APU de la NES a los registros de sonido del GBA. Así
  suenan las cuatro músicas originales (Loginska, Bradinsky, Karinka,
  Troika), la del título, la de game over, la de los bailarines y todos los
  efectos — con la misma mezcla y las mismas prioridades que en la NES,
  porque las decide el mismo código.
- `reference/disasm/` — el disassembly completo que sirve de fuente de
  verdad, y `reference/NOTES.md` con el resumen curado de qué está
  verificado y contra qué línea de la ROM.

### Qué tiene

Los cinco modos del GAME SELECT del cartucho: **1 PLAYER**, **2 PLAYER** y
**COOPERATIVE** por cable link, y **VERSUS COMPUTER** y **WITH COMPUTER**
contra el jugador de la máquina de la ROM (`computerMove`, transcrito). El
handicap inicial, la tabla de HIGH SCORES guardada en el cartucho — en su
SRAM o, donde no hay pila, en su propia flash (más abajo) —
(y cruzada por cable, con los nombres de los dos), la demo de atracción, la
cuenta de BONUS entre niveles con los cosacos y su coreografía real, y los
títulos de tres prototipos como skins con sus reglas.

Lo que se aparta del cartucho está listado en la sección "Decisions" de
`CLAUDE.md`, y el porqué de cada cosa en `reference/HISTORY.md`. Nada de eso
es un olvido.

## Controles

Los del NES, uno a uno: la cruceta mueve y hace soft drop (solo Abajo a
secas: Abajo+lateral no acelera, igual que en el original), **A** y **B**
rotan — y si los dejás apretados 15 frames la pieza empieza a girar sola,
que es una rareza real de Tengen, no un bug del port —, y **START** pausa.

Lo que el GBA tiene de más:

| Dónde | Botón | Qué hace |
|---|---|---|
| Partida | SELECT | cambia de HUD (dos por modo); con HOLD, guarda la pieza |
| Partida, HUD Stats, con el acorde | L+R | cambia el color del cosaco; con HOLD, cambia de HUD (en cualquier HUD) |
| GAME SELECT, LEVEL SETTINGS o el título | L+R | el acorde, el mismo en las tres: destapa las canciones ocultas (Korobeiniki, Katiuska, MUSIC MIX), los niveles 18-19, el menú de pausa, los títulos de los prototipos y lo demás que añade el port (abajo). El título no cambia: los prototipos se ponen desde OPTIONS |
| GAME SELECT, con el acorde | Arriba desde 1 PLAYER, y A | OPTIONS, la sexta línea: los ajustes, los prototipos, las cifras de toda la vida y el borrado (abajo) |
| GAME SELECT | A o START en un modo marcado ▣ | vuelve a su partida guardada con SAVE, en pausa, y con ella el acorde (abajo) |
| Título, con el acorde | SELECT | las tablas de HIGH SCORES, una por modo (Izquierda/Derecha las recorren); sin el acorde, SELECT es otro START, como en el cartucho |
| Pantalla LINK CABLE | SELECT | Single-Pak: envía el juego a una GBA sin cartucho |
| Carrera, tablero muerto | A+B | se levanta y sigue, como en el cartucho (con el acorde lo dice bajo el GAME OVER) |
| Carrera por cable, tablero muerto | START | deja la carrera y va a su tabla; la otra consola sigue jugando sola |
| En cualquier sitio salvo una partida por cable | L+R+SELECT | reposo: pantalla y sonido apagados, casi sin gastar pila (una partida en solitario se pausa antes); A, B, START o SELECT despiertan |
| En cualquier momento | A+B+START+SELECT | reinicia el juego (en la EZ-Flash IV lo intercepta el propio cartucho) |

**El menú de pausa** (con el acorde) es PAUSE, MUSIC, OPTIONS y EXIT GAME;
START o B lo cierran y siguen la partida. MUSIC abre la lista de canciones,
alineada a la izquierda, y suena la que tiene la flecha. EXIT GAME pregunta
SURE? con tres respuestas, YES, SAVE y NO, la flecha en NO y sin dar la
vuelta: YES está a dos pulsaciones, pasando por SAVE. SAVE sale como YES y
deja la partida en el cartucho: hay una por modo de los que juega una sola
consola (1 PLAYER, VERSUS COMPUTER, WITH COMPUTER; por cable no hay SAVE,
media partida está en la otra GBA). GAME SELECT marca con una casilla los
modos que tienen una, y elegir el modo vuelve directo a ella, en pausa, y la
saca del cartucho (para conservarla, SAVE otra vez). Sobreviven al apagado
y se recuperan desde su modo aunque el acorde no se haya vuelto a hacer: solo
guarda quien lo encontró, así que la partida lo trae de vuelta consigo.
OPTIONS tiene seis interruptores, todos en OFF hasta que los toqués, que se
guardan en el cartucho y vuelven cada vez que hacés el acorde: **HARD DROP**
(Arriba, recto y mantenido un instante —tres fotogramas, sin lateral—, tira
la pieza de golpe; un roce de la cruceta al pasar de un lado al otro no la tira), **FAST DOWN** (Abajo baja una fila por
frame), **FAST SHIFT** (el lateral repite antes y más rápido), **GHOST** (la
silueta de dónde va a caer la pieza, y en cooperativo también la del
compañero: la tuya de línea entera y la suya a trazos, y si las dos piezas
van al mismo hueco las siluetas encajan una sobre otra en vez de pisarse),
**SWAP A-B** (A y B giran al revés) y **HOLD** (SELECT guarda la
pieza que cae, y la cambia por la guardada; una vez por pieza; suena SWIPE, un
barrido que el motor del cartucho trae y nunca pide). Con HOLD, el
HUD se cambia con L+R y en HUD STATS la pieza guardada sale donde estaba el
cosaco. Cada uno tiene su casilla, en blanco: vacía apagado, con punto encendido. A,
Izquierda o Derecha cambian el de la flecha, con una campanilla (un efecto
que el motor del cartucho trae y nunca usa); B vuelve. Por cable, HARD DROP,
FAST DOWN, FAST SHIFT y HOLD son reglas de la partida: las del maestro, para
los dos, y cualquiera las cambia desde el menú compartido solo para esa
partida; GHOST y SWAP A-B son de cada jugador.

**OPTIONS** (con el acorde, la sexta línea de GAME SELECT, debajo de WITH
COMPUTER; la lista sube una fila para que quepa) es una página, como LEVEL
SETTINGS, centrada en la pantalla: primero las líneas con casillas, que se
cambian ahí mismo (A da la vuelta, Izquierda/Derecha mueven), y después las
que abren una caja, centrada también:

- **BRIGHTNESS** ▣□□□: la curva de la pantalla para una GBA sin luz (aclara
  los oscuros sin tocar el negro), cuatro, de la normal a la más brillante.
- **MUSIC** ▣▣▣▣ y **EFFECTS** ▣▣▣▣: el volumen de las canciones y el de los
  efectos por separado (ninguna casilla es silencio). El motor toca las dos
  cosas por los mismos canales, y el port distingue cuál suena por la voz
  que escribe en cada uno.
- **RUMBLE** ▣: debajo de EFFECTS, solo en un Game Boy Player; la casilla
  apaga la vibración.
- **BACKGROUND**: BLACK, DARK GREY, LIGHT GREY o IVORY, el fondo de todas las
  pantallas menos el título, que es de noche (los fuegos, y las gemas y
  lingotes de sus columnas toman el negro del fondo). Los grises son del port
  y no del NES: los del NES y su blanco son los tonos de las piezas, y una
  pieza perdía un tono en ellos; por lo mismo no hay blanco (los brillos de
  las piezas desaparecían en él) y el marfil está por debajo de él. En los
  claros las letras blancas pasan a negras, y lo que el arte pinta con el
  negro del fondo se queda negro: la sombra de las letras del banner, la
  línea fuera de la greca, la junta de los ladrillos del muro de la pausa y
  las grecas y banners de los prototipos (cada píxel en color 0 que el
  espacio vacío no alcanza es tinta negra; lo calcula `make assets` sobre
  las pantallas del cartucho). La nube que borra la línea es del color del
  fondo y los puntos de la caída se oscurecen.
- **PROTOTYPES**: RELEASE y los prototipos que tenga la ROM, por el editor
  para el que se hicieron, en el orden en que se hicieron (MIRRORSOFT, ATARI,
  NINTENDO); A pone el elegido, la página se lo pone en el acto y su skin
  llega al título y al juego.
- **STATS**: las cifras de toda la vida de esta consola — tiempo jugado,
  partidas, piezas, líneas, SINGLES, DOUBLES, TRIPLES, TETRISES, el % de
  líneas hechas de cuatro en cuatro y el nivel más alto —. Cuentan las
  partidas del jugador de esta consola: no la demo, ni el ordenador, ni el
  rival por cable.
- **ERASE DATA**, una fila más abajo y en el gris oscuro de los créditos:
  Arriba y Abajo lo saltan; solo SELECT, que baja de línea en línea, sigue
  de STATS hasta él. A pregunta ARE YOU SURE? y luego ARE YOU REALLY SURE?,
  las dos con NO elegido, y solo SÍ dos veces lo borra todo — las tablas,
  las cifras de toda la vida, los interruptores de la pausa, estos ajustes y
  las partidas guardadas —: la consola queda como recién comprada. (El
  borrado al encender con L+R+B se quitó: un botón mantenido de más bastaba
  para vaciar la consola.)

B vuelve atrás; START cierra las cajas, y con ninguna abierta la página.
Los ajustes se guardan en el cartucho y valen desde que se enciende, con
acorde o sin él (son de la consola, no del juego); lo que está detrás del
acorde es la página. Los cinco sonidos que el motor trae y el cartucho nunca
pide suenan ya cada uno en su sitio: LEVEL UP con un récord nuevo, SWIPE al
guardar con HOLD, CHIRP al poner un prototipo, DING en los interruptores y
ALARM al negar algo y al borrarlo todo. Fuera de ellos no queda nada sin
usar con contenido: lo demás que el desensamblado marca como unused es
código muerto (un borrado de pantalla que ninguna entrada pide, un salto y
un vector de reset), la lectura del mando de expansión de Famicom que nadie
mira y el canal DMC, que el motor no toca.

**Sin el acorde, el cartucho tal cual** en lo que se ve y se oye: los
créditos son los suyos (con LICENSED BY MIRRORSOFT LTD., y al final las dos
líneas de copyright de su título, ©1987 ACADEMYSOFT-ELORG. y ©1988 TENGEN,
con su propio ©), SELECT en el título
es otro START, el mando no vibra y el título y el cosaco son los del
cartucho (L y R no hacen nada). Con el acorde, el crédito del port ocupa
el sitio del de Mirrorsoft y la versión sobre el año el de los
copyrights, SELECT en el título abre los récords, el Game Boy
Player vibra, y además de lo de arriba la música acelera con la pila alta
(solo la música: los efectos siguen a su ritmo), la
pausa tapa el tablero con un muro de bloques que sube desde el suelo (y al
seguir baja desde arriba) y esconde NEXT y la pieza de HOLD, un récord nuevo suena con el
jingle que el cartucho no usa (por cable, todo esto con el acorde del
maestro), la demo la juega el ordenador del port, en HUD STATS, con las
opciones de partida del jugador (la sombra, FAST DOWN...), sube hasta el
nivel 19 como cualquier partida con el acorde y al perder enseña, como una
partida, la hoja y la tabla de récords — con la fila de CPU metida en su
sitio si llega, y sin guardarse nunca —, GAME SELECT trae OPTIONS, en HUD
STATS L+R cambia el color del cosaco, y al acabar la partida
la cortinilla del Tetris de Game Boy — un muro de ladrillos sube tapando la
pila muerta y luego baja vaciando el pozo — deja en el tablero la hoja de la
partida — TIME, PIECES, SINGLE, DOUBLE, TRIPLE,
TETRIS, RATE (el % de líneas hechas de cuatro en cuatro) y DROUGHT (la racha
más larga sin barra) —, con el GAME OVER cinco segundos más para leerla, y
la tabla de récords se queda cinco segundos y no diecisiete si la partida
no metió a nadie en ella; y se arreglan los fallos propios del cartucho:
el reparto en cooperativo que nace dentro de la pieza del compañero, la
pieza que cae metida en las filas del compañero que se derrumban, los
estantes del panel que no tocan la cuerda, el bisel de las fichas $01 y $03,
que dibujan su esquina un píxel más abajo que las demás (es el arte del
cartucho, byte a byte), y la puntuación que al pasar de 999999 vuelve a
100000 (con el acorde se para en 999999, y las líneas en 9999).

**Apagar en pausa no pierde la partida**, como en Tetris DX: una partida en
solitario en pausa se guarda en la memoria del cartucho, y al encender vuelve
en pausa donde estaba. Si se reanuda, se acaba o se sale, se borra; el
reinicio A+B+START+SELECT también la borra. Solo esa vuelve sola al
encender; las guardadas con SAVE esperan en su modo. (Una ROM nueva no
recupera las partidas de otra: la estructura puede haber cambiado.)

**Game Boy Player**: al encender sale su logo, como en los juegos que lo
soportaban, y si el juego corre en un Game Boy Player lo reconoce y, con el
acorde, el mando de GameCube vibra al hacer líneas (más con un TETRIS), al
subir de nivel y al perder (RUMBLE, en OPTIONS, lo apaga). Y en la tele los ajustes vuelven a ser las tres pantallas del
cartucho, LEVEL, HANDICAP y MUSIC, cada una con su columna de números (START
pasa de pantalla, B vuelve). El logo es de Nintendo y no va en el repo: se genera con
`python3 tools/make_gbp_logo.py captura.png` a partir de una captura sin
pérdidas (240x160) de un juego que lo muestre; sin él no sale ni se busca el
Player.

**Al encender salen los logos de Tengen, Mirrorsoft y Tetris** sobre blanco,
como en los juegos de GBA: el editor, la licencia que el propio cartucho
nombra (LICENSED BY MIRRORSOFT LTD.) y la de Tetris, como abre todo Tetris con
licencia. Solo al encender (no tras el reinicio por botones ni en la copia
Single-Pak), y cada uno se salta con un botón. Los logos no van en el repo: se
generan una vez con
`python3 tools/make_splash.py logo.png`,
`python3 tools/make_splash.py mirrorsoft.jpg --mirrorsoft` y
`python3 tools/make_splash.py tetris_logo.png --tetris` (estos dos con 256
colores, por sus degradados); sin ellos la ROM arranca directa.

Con el juego en pausa entran los tres códigos originales, un botón por
frame:

| Código | Secuencia | Límite |
|---|---|---|
| Subir de nivel | Arriba Abajo Arriba Abajo Izq Der B B A | ilimitado (tope 17) |
| Barra larga | Abajo Abajo Izq Der Izq Der B A | una por nivel |
| Deshacer | Izq Abajo Der Arriba Izq Abajo Der B A | una por partida |

Están reproducidos con sus manías: un botón que rompe la secuencia se come
esa pulsación, y al completar un código el cursor no se rebobina, así que
volver a pulsar **A** repite el último — que es como se sube de nivel a
pulsos de A.

## Dos jugadores

**GAME SELECT → 2 PLAYER** (o COOPERATIVE), con un cable link entre las dos
consolas. Quién es jugador 1 lo decide el cable: el maestro elige nivel,
handicap, música y skin, y el otro espera con un cosaco. Las dos consolas
simulan la misma partida desde la misma semilla y solo se mandan botones, y
`tools/run_link.py` comprueba que acaben idénticas byte a byte. Si el cable
se va, las dos lo detectan. Las dos necesitan esta misma versión del ROM
(con otra más vieja se conectan igual, pero sin skin).

En una **carrera** cada tablero es de su jugador: el que pierde el suyo lo
levanta con **A+B** y sigue, o se va con **START** — y entonces la otra
consola dice RIVAL LEFT un momento y sigue jugando sola hasta el final, sin
pausa ni corte. Si una consola se reinicia o se apaga a media partida, la
otra no se queda esperando: en carrera sigue sola, y en cooperativo (un solo
tablero para los dos) lo da por perdido con CABLE LOST.

**Dos títulos, una demo** (con el acorde en las dos). Dos cartuchos
enlazados y dejados en su pantalla de título —lo que pasa solo al acabar
una partida entre dos— arrancan sus demos a la vez, y con el acorde hecho
en ambas esas dos demos son una sola partida cooperativa: los dos
ordenadores del port en el mismo tablero, igual en las dos pantallas.
Cualquier botón saca a esa consola a GAME SELECT, como de cualquier demo, y
la otra sigue con la partida ella sola. Sin el acorde en las dos no hay
nada de esto: cada consola hace la demo del cartucho, a su hora, y por el
cable no dice nada que no dijera ya.

**Los récords entre dos cartuchos se preguntan.** Cada cartucho tiene sus
tablas, y las del amigo con puntuaciones monstruosas echarían de ellas las
tuyas: al enlazar, si las tablas no coinciden, cada consola pregunta a su
jugador si quiere añadir los mejores récords de la otra (THE OTHER GBA HAS
HIGH SCORES OF ITS OWN / ADD THEM HERE?, con NO bajo la flecha). Solo quien
dice que sí los recibe; no se vuelve a preguntar esa tarde, y si las dos
tablas ya son iguales no se pregunta nunca. Lo que sí pasa sin preguntar es
lo de siempre: las partidas que jugáis juntos quedan en las dos tablas, con
el nombre de cada uno, y los récords de una copia Single-Pak (que no tiene
dónde guardarlos) pasan al cartucho con el que enlace.

**Single-Pak**: la otra consola no necesita cartucho. La pantalla LINK
CABLE dice qué hay al otro lado: una GBA sin cartucho (y se lo envía sola; B lo
para, y parado se queda hasta **SELECT**), otra consola con el juego (esperar a su
jugador) o nada (encenderla, o conectar el cable). Para encontrar a la consola
sin cartucho el lobby calla un momento de cada segundo y pregunta como pregunta
el propio envío: en dos consolas de verdad la primera sonda, que preguntaba
entre palabra y palabra del lobby, no la encontró nunca y había que pulsar
SELECT. La consola que envía
tiene que estar en el extremo maestro; si no, lo dice. La otra se enciende
sin cartucho y su BIOS lo recibe, con una barra de bloques y los KB en la
que envía;
arranca directamente en el lobby, juega el modo que eligió la del cartucho y
no guarda récords. Y la copia puede hacer copias: en su propia pantalla LINK
CABLE, SELECT la envía a su vez a otra consola sin cartucho, desde su
memoria. Ver `CLAUDE.md` para lo que se aprendió por el camino.

## En qué cartucho guarda

Récords, opciones, cifras y partidas en pausa van a la memoria de guardado
del cartucho. Qué hay detrás de esa memoria lo averigua el juego solo, al
encender, y no hay nada que elegir ni que parchear:

- **Emulador, EZ-Flash, EverDrive, SuperCard, cartucho con pila**: la RAM
  de guardado (SRAM), como siempre. La ROM lleva la firma `SRAM_V113`, que
  es por la que emuladores y flash carts saben que tiene una.
- **Placa de reproducción sin pila** — las AGB-E05-xx y parientes: un chip
  flash de 16 MB con el juego, una SRAM y nada que la mantenga (o ni
  siquiera la SRAM) —: ahí la
  SRAM se olvida de todo al apagar, y el juego guarda **además en la flash
  del propio cartucho**, un megabyte adentro, lejos del programa. Al
  encender lee de donde esté lo bueno: de la SRAM si conserva su marca (una
  placa con pila), de la flash si no.

Cómo lo sabe: le pregunta al cartucho quién es (la consulta CFI que
contesta cualquier flash NOR, la misma por la que el grabador sabe grabarla).
Una ROM de verdad, o un flash cart, no contesta y ahí queda la cosa. Un chip
contesta con su tabla — tamaño, sectores, juego de comandos — y de paso con
cómo está cableado: en estas placas las líneas de datos D0 y D1 suelen venir
cruzadas (y a veces D6 y D7), y la tabla llega cruzada también. Se usan los
chips de **comandos AMD** con cualquiera de esos cableados (MSP55LV128M,
S29GL128/256, M29W128, W29GL128… la familia que FlashGBX graba con sus
perfiles «AAA/A9» y «AAA/AA»); los de comandos Intel se reconocen y se dejan
en paz, con la SRAM como hasta ahora. Tampoco se toca un chip que tenga
**otro juego** detrás de este (un flash cart de los que guardan varios en la
misma memoria): se le reconoce por la cabecera, y ese megabyte es suyo.

Lo guardado es un diario: cada cambio se añade detrás del anterior con su
suma de control, en uno de dos sectores, y cuando se llena se pasa en limpio
al otro. Si se va la luz a mitad, queda el guardado anterior o el nuevo,
nunca medio. Guardar son milisegundos (una pausa, unos 50 la primera vez y 6
las siguientes); el único segundo largo, borrar un sector, se hace nada más
encender. Por cable se espera a que acabe la partida.

**Mantener L+R+A al encender** saca, en vez de los logos, la página
CARTRIDGE SAVE: qué contestó el chip, cómo está cableado, su identificador
(el mismo que enseña el grabador), si la SRAM conservó lo suyo, de dónde se
ha leído la partida y qué se le va a pedir al chip a continuación. Si un
cartucho no guarda, una foto de esa página dice por qué. La página sale
**antes de escribir nada en el chip**: START sigue con normalidad, y SELECT
sigue *sin tocar la flash* en esa sesión (la SRAM sola), que es la salida si
un chip se atragantase al escribirle.

Tres cosas a saber:

- **Probado en una placa de estas** (AGB-E05-01, v2.22): el chip contestó
  la consulta con D0/D1 cruzadas (`0052 0051 005A`), comandos AMD, 64 MB en
  512 sectores de 128 KB e identificador `008A 227D` (fabricante 0x89,
  0x227E al deshacer el cruce: no el MSP55LV128M de 16 MB que se le
  suponía), y el diario llevaba 85 guardados cuando se leyó la página.
  **Lo que aún no ha corrido en hardware es el borrado de un sector**, que
  toca al encender cuando al diario le queda menos de un cuarto (unos
  seiscientos guardados): hasta entonces solo está comprobado contra el
  modelo del chip (`tools/romcheck/flashchip.py`, `--cartsave`). Los chips
  de comandos Intel siguen sin soporte.
- Mientras se le habla, el chip no es una ROM: el trozo que le habla corre
  desde RAM. Si el chip no volviera nunca a ser ROM, la pantalla se queda
  **roja** en vez de colgarse a ciegas: apagar, y encender con L+R+A para
  ver la página y seguir con SELECT.
- Al regrabar una versión nueva, lo guardado sigue ahí si el grabador borra
  solo los sectores que escribe (FlashGBX, tal cual viene); un borrado
  completo del chip se lo lleva.

## Compilar

El arte del juego **no está en este repo**: sale de un dump del cartucho
original que aportes vos. Una vez, para generarlo:

```sh
make assets ROM=/ruta/a/tetris.nes
```

(Con eso basta. Los prototipos son un extra: si tenés sus dumps,
`make assets ROM=tetris.nes PROTO="proto_a.nes proto_b.nes proto_c.nes"`
añade sus títulos y sus tableros como pieles; sin ellos el port es el
cartucho y lo que añade el acorde, sin la línea PROTOTYPES.)

Después:

```sh
make test        # tests del núcleo en el host, solo necesita gcc
make gba         # compila la ROM -> build/tengen.gba
make gba-check   # arranca la ROM en mGBA headless y la verifica
```

`build/tengen.gba` **arranca en hardware real**. El header lleva el logo de
arranque que el BIOS exige, puesto por `gbafix` (la herramienta oficial de
devkitPro, vendorizada en `tools/gbafix/`), y `tools/check_header.py`
comprueba después las tres validaciones que hace el BIOS.

Para `make gba` hace falta un toolchain ARM:

```sh
apt-get install gcc-arm-none-eabi      # Debian/Ubuntu
brew install --cask gcc-arm-embedded   # macOS
```

Para `make gba-check`, además: `pip install pygba` y la librería de mGBA
(`apt-get install libmgba0.10`).

En GitHub, cada push y cada PR corre lo que no necesita el dump
(`.github/workflows/tengen-gba-port.yml`): `make test`
con `-Werror` y otra vez con los sanitizers, `make assets-check`, el núcleo
compilado para el GBA y el arnés de `make trace`. `make gba-check` y las
comparaciones contra el cartucho necesitan el dump y se corren en local.

## Estructura

```
Tetris-Tengen-to-GBA/
├── src/                  núcleo del juego (C99, sin dependencias de GBA),
│                         y el diario del guardado en flash
├── tests/                tests nativos del núcleo y del diario
├── gba/                  capa de GBA: hardware, crt0, linker script, renderer,
│                         el cable link (link.c) y dónde se guarda (cartsave.c)
├── tools/                extracción de assets, verificación de ROM, gbafix
├── reference/
│   ├── disasm/           disassembly completo de Tetris (NES, Tengen)
│   ├── NOTES.md          mecánicas verificadas, con citas a la ROM
│   └── HISTORY.md        cómo llegó cada parte del port a su forma
├── CLAUDE.md             cómo trabajar en este proyecto
└── PROMPT_ARRANQUE.md    resumen para retomar el trabajo en una sesión nueva
```

## La versión 2 (o «Tengen Tetris 2», entre nosotros)

Esto es un port, y en pantalla nunca dice otra cosa: sin el acorde es el
cartucho de 1989, regla a regla y nota a nota — las trazas lo comparan con
el cartucho real frame a frame — con lo justo que pide una GBA (una pantalla
más estrecha, logos al encender, reposo, partida guardada, cable y adaptador
inalámbrico). Pero el acorde destapa tanto que, puertas adentro, a la
versión con truco la llamamos la versión 2.

Lo que hay detrás del acorde no es un modo: es el juego terminado.

- **Los fallos del propio cartucho, arreglados** (sin el acorde se quedan,
  porque son suyos): en cooperativo, la pieza que se reparte dentro de la del
  compañero; los bloques que, al hundirse las filas del compañero, caen
  dentro de la pieza que está bajando; los estantes del panel izquierdo que
  no llegan a la cuerda (el tile `$6A` tiene la primera columna vacía); y el
  bisel de las fichas `$01` y `$03`, que dibujan su esquina un píxel más
  abajo que las otras doce; y la puntuación, que al pasar de 999999 vuelve
  a 100000 y deja el HIGH en lo que llevaba (se para en 999999, y las
  líneas en 9999). Cinco del cartucho, y dos más del parche XE (dos errores
  de uno).
- **Lo que el cartucho dejó a medias, acabado**: el jingle de subir de nivel
  que el motor de sonido trae y nadie toca suena con un récord nuevo; la
  música corre más con la pila alta, la pausa tapa el tablero y la partida
  acabada se lee en el propio tablero, con la cortinilla del Tetris de Game
  Boy. (Dos de estas cosas ni siquiera esperan al acorde, porque completan
  el original sin cambiarlo: cada modo tiene su tabla de récords, y el
  nombre que escribiste vuelve solo.)
- **Un juego moderno sobre el viejo**: menú de pausa con música y opciones
  (HARD DROP, FAST DOWN, FAST SHIFT, GHOST, SWAP A-B, HOLD), dos canciones
  que el cartucho no tenía, los niveles 18 y 19 de Tetris Tengen XE, y un
  ordenador nuevo — El-Tetris sobre un tablero de bits, con pesos propios
  hallados por evolución para el cooperativo — que juega VERSUS, WITH
  COMPUTER y la demo.
- **Los prototipos**: los títulos y las reglas de tres versiones previas del
  cartucho, como pieles que llegan hasta el tablero.

En cifras: unas 6.500 líneas de C sin nada de GBA (`src/`: las reglas, y el
diario con que se guarda en la flash; probadas en el PC con 183 tests y un
fuzzer), 18.000 de capa GBA y 32.000 de herramientas y
comprobaciones — 61 baterías en `make gba-check`, más de ochenta
comprobaciones que arrancan la ROM en un emulador, la juegan y miran la
pantalla, el sonido y la memoria —, todo contrastado contra el disassembly
del cartucho, línea citada a línea citada. Nada de eso se ve desde fuera, y
así tiene que ser: lo que se ve es Tetris de Tengen.

## Créditos del disassembly

El material en `reference/disasm/` es el trabajo de disassembly de Tetris
(NES, Tengen) — ver `reference/disasm/README.md.txt` para los créditos
completos (threecreepio, CelestialAmber, ejona86, qalle2, kirjavascript).
