# Prompt de arranque

Pegar esto al empezar una sesión nueva sobre este proyecto, para no repetir
el trabajo de contexto ya hecho.

---

Estoy trabajando en un port de **Tetris (NES, Tengen, 1989)** a Game Boy
Advance, en `tengen_gba_port/` dentro de este repo. El objetivo es jugabilidad
y gráficos del playfield 1:1 con el original — mismos controles, mismas
rarezas de rotación y de RNG — adaptando solo el HUD alrededor, porque la
pantalla de GBA (240×160) no tiene el mismo ancho que la de NES (256×240)
aunque el playfield (80×160 px) entra exacto en el alto del GBA sin escalar.

Antes de tocar código, leé en este orden:

1. `tengen_gba_port/CLAUDE.md` — reglas de trabajo del proyecto (la ROM es la
   fuente de verdad, no la memoria de "cómo es el Tetris normal"; núcleo
   independiente de plataforma; nunca subir un placeholder a "verificado" sin
   citar la línea del disassembly que lo confirma).
2. `tengen_gba_port/reference/NOTES.md` — qué está verificado contra la ROM
   (con número de línea de `main.asm.txt`) y qué es todavía un placeholder
   marcado `TODO(verify)`. Esto evita re-derivar de cero cosas como el
   algoritmo de RNG, el wallkick, o el timing de DAS, que ya están trazados.
3. `tengen_gba_port/README.md` — estado general y cómo compilar (`make test`,
   solo necesita gcc; todavía no hay capa de GBA).

El núcleo del juego vive en `src/tengen_core.{h,c}` (C99 puro, sin
dependencias de GBA) y tiene tests nativos en `tests/test_tengen.c`
(`make test`). El disassembly completo de la ROM está en
`reference/disasm/` — es la fuente primaria para cualquier duda sobre
comportamiento exacto.

Lo próximo según `CLAUDE.md`'s roadmap: cerrar los placeholders listados en
`reference/NOTES.md` (curva de gravedad por nivel, puntaje exacto por línea,
rampa del soft-drop, indexado de niveles con start-level distinto de 0), cada
uno respaldado por una relectura puntual de `main.asm.txt` en las líneas que
`NOTES.md` ya señala como próximo objetivo, y un test nativo nuevo antes de
tocar nada de GBA.
