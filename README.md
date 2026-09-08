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
cada lado) con el HUD repartido: puntaje, líneas, nivel y estadísticas a la
izquierda, próxima pieza a la derecha. Nada se escala ni se recorta; las 2
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
- `tools/run_rom.py` — arranca la ROM en mGBA headless y verifica que
  realmente dibuje y se juegue (`make gba-check`), no solo que linkee:
  incluye ver la animación de línea completa sprite por sprite, comprobar
  que deja escrita la palabra correcta, y pausar y teclear los códigos de
  trucos.
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

### Lo que falta

- **Coop y el jugador de la máquina**: el core ya modela el coop (incluido
  su campo de 12 columnas), pero la capa GBA todavía no lo ofrece; el
  GAME SELECT lista solo los dos modos que sí están, con las palabras del
  cartucho. Falta también el handicap inicial del 2P
  (`initHandicapGarbage`) y el jugador de la máquina de la ROM
  (`computerMove`), que es el único modo de dos jugadores que no necesita
  ni segunda consola ni cable.
- **Coreografía exacta de los bailarines**: están los seis, con su arte, sus
  poses, sus posiciones y su escenario reales — el blit de subida de nivel no
  solo despeja el banner, además dibuja las repisas sobre las que se paran, y
  eso ya está. Lo que falta son los datos del script individual de cada uno.
  El intérprete de esos scripts sí está trazado (ver `reference/NOTES.md`:
  cada entrada es una pose, un salto o una bifurcación al azar según contra
  qué dirección se compare), pero los datos no están cargados todavía, así
  que recorren la tabla de poses desde puntos escalonados.

## Controles

Los del NES, uno a uno: la cruceta mueve y hace soft drop (solo Abajo a
secas: Abajo+lateral no acelera, igual que en el original), **A** y **B**
rotan — y si los dejás apretados 15 frames la pieza empieza a girar sola,
que es una rareza real de Tengen, no un bug del port —, y **START** pausa.

En la pantalla de selección, **arriba/abajo** elige entre las cuatro músicas
del juego (Loginska, Bradinsky, Karinka, Troika), que es la selección de
música que el menú original también ofrece.

## Dos jugadores

**GAME SELECT → 2 PLAYER**, con un cable link entre las dos consolas y el
mismo cartucho en las dos. Quién es jugador 1 lo decide el cable, no el
software: la consola enchufada en el extremo de maestro juega de jugador 1 y
es la que manda su nivel y su música: las dos pasan por la pantalla de
selección, pero la del maestro es la que cuenta, y en la pantalla de cable se
ve cuál quedó. Cada consola muestra su propio campo, centrado igual que en un
jugador, con la puntuación del rival en el panel de la izquierda.

Es una carrera, como en el cartucho: no se manda basura de un lado al otro.
Cualquiera de los dos puede pausar (la ROM original hace el OR de los dos
mandos, y eso vale también por cable). Si el cable se va, las dos consolas lo
detectan y terminan la partida en vez de quedarse esperando.

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

## Compilar

El arte del juego **no está en este repo**: sale de un dump del cartucho
original que aportes vos. Una vez, para generarlo:

```sh
make assets ROM=/ruta/a/tetris.nes
```

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

## Estructura

```
tengen_gba_port/
├── src/                  núcleo del juego (C99, sin dependencias de GBA)
├── tests/                tests nativos del núcleo
├── gba/                  capa de GBA: hardware, crt0, linker script, renderer,
│                         y el cable link (link.c)
├── tools/                extracción de assets, verificación de ROM, gbafix
├── reference/
│   ├── disasm/           disassembly completo de Tetris (NES, Tengen)
│   └── NOTES.md          mecánicas verificadas, con citas a la ROM
├── CLAUDE.md             cómo trabajar en este proyecto
└── PROMPT_ARRANQUE.md    resumen para retomar el trabajo en una sesión nueva
```

## Créditos del disassembly

El material en `reference/disasm/` es el trabajo de disassembly de Tetris
(NES, Tengen) — ver `reference/disasm/README.md.txt` para los créditos
completos (threecreepio, CelestialAmber, ejona86, qalle2, kirjavascript).
