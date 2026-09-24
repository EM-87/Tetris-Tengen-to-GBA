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
2. `tengen_gba_port/reference/NOTES.md` — qué está verificado contra la ROM,
   con número de línea de `main.asm.txt`. Evita re-derivar de cero cosas ya
   trazadas: el RNG, el wallkick, el timing de DAS, la curva de gravedad, la
   geometría del campo, el puntaje y las paletas.
3. `tengen_gba_port/README.md` — estado general y cómo compilar.

Una cosa que sorprende y conviene saber de entrada: **el sonido no está
reimplementado, está emulado**. `gba/nes6502.c` es un intérprete de 6502 chico
que ejecuta el propio motor de audio del cartucho (`gba/audio_prg.h`), y
`gba/nes_audio.c` traduce lo que ese motor le escribe al APU de la NES a los
registros de sonido del GBA. Es la única forma que había de tener la música,
los efectos y su mezcla por prioridades sin adivinar un formato que nadie
documentó.

El núcleo vive en `src/tengen_core.{h,c}` (C99 puro, sin dependencias de GBA)
con tests nativos en `tests/test_tengen.c` (`make test`). La capa de GBA está
en `gba/` y compila con un `arm-none-eabi-gcc` común, sin devkitARM. El
disassembly completo está en `reference/disasm/` y es la fuente primaria para
cualquier duda sobre comportamiento exacto.

Estado: el port está completo. Los cinco modos del cartucho (dos por cable
link, dos contra la máquina), la música y los efectos del propio motor del
cartucho, los cosacos con su coreografía, la tabla de récords en SRAM, y los
prototipos como skins. `make test` (host) y `make gba-check` (48
comprobaciones sobre la ROM en mGBA, dos de ellas con dos consolas y cable)
tienen que pasar antes de dar por terminado cualquier cambio; tras tocar
`tengen_step`, además `make trace ROM=...` contra el cartucho.

No hay nada pendiente: ver la sección "Open" de `CLAUDE.md`. Cualquier
cambio nuevo empieza por mirar el cartucho (`tools/render_nes.py`,
`tools/nes_console.py`) antes que la memoria de cómo es "el Tetris".
