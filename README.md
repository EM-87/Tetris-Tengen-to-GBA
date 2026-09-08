# Tengen Tetris (NES) → GBA port

Port de **Tetris (NES, Tengen, 1989)** a Game Boy Advance, con el objetivo de
mantener su jugabilidad y sus gráficos lo más fiel posible al original —
controles 1:1, mismas rarezas de rotación, mismo playfield pixel a pixel —
adaptando solo lo que la propia pantalla del GBA obliga a adaptar.

## Por qué es viable un port "1:1"

La pantalla de GBA (240×160 px) no es del mismo tamaño que la de NES
(256×240 px), pero el **campo de juego** de Tengen Tetris — 10 columnas × 20
filas de tiles de 8×8 — mide 80×160 px, y esos 160 px de alto son *exactamente*
el alto completo de la pantalla del GBA. El playfield entra sin recortar ni
escalar, tile por tile. Lo que no entra 1:1 es el HUD alrededor (NES tiene
176px de ancho sobrantes para repartir a los costados; GBA tiene 160px), así
que el plan es: **playfield y jugabilidad idénticos, marco/HUD rediseñado**
para el ancho disponible. Ver `reference/NOTES.md` para el detalle.

## Estado actual

Arrancando. Lo que existe hoy:

- `src/tengen_core.{h,c}` — núcleo de reglas del juego, independiente de
  plataforma (compila con cualquier gcc, sin GBA de por medio todavía).
  Implementa: playfield, las 7 piezas con sus 4 orientaciones, el
  generador de piezas real de Tengen (con su sesgo característico, sin
  anti-repetición), el rotado con su wallkick-solo-a-la-izquierda, DAS,
  auto-rotate, líneas y subida de nivel.
- `tests/test_tengen.c` — tests nativos (host) de esas reglas.
- `reference/disasm/` — el disassembly completo de Tetris (NES, Tengen) que
  sirve de fuente de verdad; ver `reference/disasm/README.md.txt` para el
  proyecto de disassembly en sí, y `reference/NOTES.md` para el resumen
  curado de qué está verificado contra la ROM y qué es un placeholder.
- Todavía no hay capa GBA (video/input/audio). Ese es el próximo paso una
  vez el núcleo esté suficientemente probado — ver `CLAUDE.md`.

## Compilar y correr los tests

```sh
make test
```

Solo necesita gcc — no hace falta devkitARM todavía. `make gba` existe como
target documentado para cuando exista la capa GBA (ver `Makefile`).

## Estructura

```
tengen_gba_port/
├── src/                  núcleo del juego (C99, sin dependencias de GBA)
├── tests/                tests nativos del núcleo
├── reference/
│   ├── disasm/           disassembly completo de Tetris (NES, Tengen)
│   └── NOTES.md          verificado vs. placeholder, con citas a la ROM
├── gba/                  (no existe todavía) capa específica de GBA
├── CLAUDE.md             cómo trabajar en este proyecto
└── PROMPT_ARRANQUE.md    resumen para retomar el trabajo en una sesión nueva
```

## Créditos del disassembly

El material en `reference/disasm/` es el trabajo de disassembly de Tetris
(NES, Tengen) — ver `reference/disasm/README.md.txt` para los créditos
completos (threecreepio, CelestialAmber, ejona86, qalle2, kirjavascript).
