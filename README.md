# Tengen Tetris (NES) → GBA port

Port de **Tetris (NES, Tengen, 1989)** a Game Boy Advance, con el objetivo de
mantener su jugabilidad y sus gráficos lo más fiel posible al original —
controles 1:1, mismas rarezas de rotación, mismo playfield pixel a pixel —
adaptando solo lo que la propia pantalla del GBA obliga a adaptar.

## Por qué es viable un port "1:1"

La pantalla de GBA (240×160 px) no es del mismo tamaño que la de NES
(256×240 px), pero el **campo de juego** de Tengen Tetris — 10 columnas × 20
filas de tiles de 8×8, más las dos columnas de marco: 96×160 px — tiene
*exactamente* los 160 px de alto de la pantalla del GBA. El playfield entra
sin recortar ni escalar, tile por tile. Lo que no entra 1:1 es el HUD
alrededor (al NES le sobran 160 px de ancho para repartir a los costados; al
GBA 144), así que el plan es: **playfield y jugabilidad idénticos, HUD
rediseñado** para los paneles más angostos.

Eso no es teoría: `make gba-check` arranca la ROM en un emulador y verifica
que el campo caiga exactamente en las columnas 72..167 y que las dos columnas
de marco ocupen los 160 px de alto. Ver `reference/NOTES.md` para el detalle.

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
- `gba/` — capa de GBA: registros de hardware, `crt0.s`, linker script, el
  renderer por tiles, la fuente del HUD y las paletas reales del juego. No
  depende de devkitARM ni de libgba: compila con un `arm-none-eabi-gcc`
  estándar. Tiene pantalla de título con selección de nivel (0-9), HUD
  completo (next, score, nivel, líneas, estadísticas por pieza) y game over.
- `tools/run_rom.py` — arranca la ROM en mGBA headless y verifica que
  realmente dibuje y se juegue (`make gba-check`), no solo que linkee.
- `reference/disasm/` — el disassembly completo que sirve de fuente de
  verdad, y `reference/NOTES.md` con el resumen curado de qué está
  verificado y contra qué línea de la ROM.

### Lo que falta

- **Las formas de los gráficos son placeholders**, pero **los colores no**:
  las paletas sí están en el disassembly y son las del juego original. El
  arte de 8×8 vive en la CHR de la ROM original y no está en este repo;
  `tools/chr_to_gba.py` lo convierte a tiles de GBA a partir de un dump que
  aportes vos (`make tiles ROM=/ruta/clean.nes`). El renderer ya está armado
  alrededor de tiles de 8×8, así que es un cambio de datos, no una
  reescritura.
- **Audio**: nada todavía.
- **Modos 2P y coop**: el core ya los modela (incluido el campo de 12
  columnas del coop), pero la capa GBA es solo de un jugador por ahora. Del
  menú original solo está la selección de nivel; falta tipo de partida,
  handicap y selección de música.
- **Animación de línea completa**: el core borra las filas al instante; la
  ROM primero reproduce una animación.
- La ROM corre en emuladores pero **no bootea en hardware real**: le falta el
  logo de Nintendo en el header (dato que no está en este repo). Pasarle
  `gbafix` de devkitPro lo resuelve. El checksum del header sí se calcula
  solo en el build.

## Compilar

```sh
make test        # tests del núcleo en el host, solo necesita gcc
make gba         # compila la ROM -> build/tengen.gba
make gba-check   # arranca la ROM en mGBA headless y la verifica
```

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
├── gba/                  capa de GBA: hardware, crt0, linker script, renderer
├── tools/                utilidades de build y verificación de la ROM
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
