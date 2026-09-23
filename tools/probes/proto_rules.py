"""Las reglas de los prototipos, medidas en sus propios volcados.

Tres cosas se dan por ciertas en el port bajo `proto_rules`, y vinieron de una
lista escrita por terceros. Esto las comprueba contra las ROMs:

  1. una fila completa se va EL FRAME que se completa, sin escoba;
  2. no hay patada de pared: una pieza pegada al muro no gira;
  3. se sube de nivel cada DIEZ lineas, y el nivel elegido es un SUELO: el
     nivel es el mayor entre el de salida y lineas/10. Empezando en 0 sube en
     10 y 20; empezando en 3 (B, D) aguanta hasta 40, en 5 (C) hasta 60.

La tercera se dio por imposible de medir durante mucho tiempo: plantar una
fila completa y dejar caer piezas parecia subir el marcador sin tocar las
LINEAS. No era la ROM, era el experimento: la fila se plantaba debajo de un
monton que ya existia y la pieza nueva nunca llegaba a tocarla. Vaciando el
campo antes de cada fila, cada limpieza cuenta.

El campo de juego de estas construcciones esta donde el del release: $0600,
ocho bytes por fila (dieciseis nibbles), filas 6 a 25. Eso ya se midio antes
(protocells.py) y aqui se vuelve a comprobar antes de usarlo.
"""
import os
import sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import extract_assets as E
from nes_console import NesConsole, BTN

PF = 0x600
ROW0, ROW1 = 6, 26

def boot(path):
    nes = NesConsole(path)
    nes.run(120)
    for _ in range(E.SKIN_PLAY_PRESSES):
        if nes.ram(E.GAMESTATE_ADDR) == E.GAMESTATE_PLAYING:
            break
        nes.run(6, BTN["START"]); nes.run(E.SKIN_PLAY_SETTLE)
    return nes if nes.ram(E.GAMESTATE_ADDR) == E.GAMESTATE_PLAYING else None

def row_cells(nes, r):
    out = []
    for nib in range(16):
        b = nes.bus.ram[(PF + r * 8 + (nib >> 1)) & 0x7FF]
        out.append((b >> 4) if (nib & 1) == 0 else (b & 0xF))
    return out

def fill_row(nes, r, hole=None):
    """Llena las diez columnas jugables de una fila, con un hueco opcional."""
    for nib in range(3, 13):
        if nib == hole:
            continue
        i = (PF + r * 8 + (nib >> 1)) & 0x7FF
        b = nes.bus.ram[i]
        nes.bus.ram[i] = (b | 0x10) if (nib & 1) == 0 else (b | 0x01)

def playable(cells):
    return cells[3:13]

def screen_text(nes, row, lo=0, hi=32):
    nt = nes.bus.vram
    out = []
    for c in range(lo, hi):
        t = nt[row * 32 + c]
        out.append(chr(t) if 32 <= t < 127 else " ")
    return "".join(out)


def occupied(nes):
    """Las celdas ocupadas del campo, como conjunto de (fila, nibble)."""
    out = set()
    for r in range(ROW0, ROW1):
        cells = row_cells(nes, r)
        for nib in range(3, 13):
            if cells[nib]:
                out.add((r, nib))
    return out


def measure_clear(path):
    """Cuantos frames tarda en irse una fila completa tras asentar una pieza."""
    nes = boot(path)
    if nes is None:
        return None, "no llego a jugar"
    fill_row(nes, ROW1 - 1)
    # Asentar: abajo mantenido hasta que el numero de celdas ocupadas SUBA de
    # golpe (la pieza se une al monton) -- y desde ahi, contar.
    before = len(occupied(nes))
    locked = None
    for f in range(400):
        nes.run(1, BTN["DOWN"])
        n = len(occupied(nes))
        if n > before + 2:          # la pieza se ha sumado al campo
            locked = f
            break
        before = min(before, n)
    if locked is None:
        return None, "ninguna pieza asento en 400 frames"
    full = lambda: all(row_cells(nes, ROW1 - 1)[3:13])
    if not full():
        return None, "la fila de abajo no estaba completa al asentar"
    for f in range(1, 200):
        nes.run(1)
        if not full():
            return f, None
    return None, "la fila no se fue en 200 frames"


def measure_kick(path):
    """Gira una pieza pegada al muro izquierdo, y luego una columna a la derecha."""
    nes = boot(path)
    if nes is None:
        return None, "no llego a jugar"
    out = {}
    for name, steps in (("pegada al muro", 24), ("una columna dentro", 22)):
        n2 = boot(path)
        for _ in range(steps):
            n2.run(1, BTN["LEFT"])
        n2.run(6)
        before = occupied(n2)
        n2.run(4, BTN["A"]); n2.run(6)
        after = occupied(n2)
        out[name] = (before != after)
    return out, None


def empty_field(nes):
    """Vacia las diez columnas jugables de todas las filas."""
    for r in range(ROW0, ROW1):
        for nib in range(3, 13):
            i = (PF + r * 8 + (nib >> 1)) & 0x7FF
            b = nes.bus.ram[i]
            nes.bus.ram[i] = (b & 0x0F) if (nib & 1) == 0 else (b & 0xF0)


def shown_number(nes, row):
    d = "".join(ch for ch in screen_text(nes, row, 8, 16) if ch.isdigit())
    return int(d) if d else None


def boot_at_level(path, level):
    """Como boot(), pero bajando `level` posiciones en su LEVEL SELECT (B, C y
    D tienen uno de 0 a 9; el de A no es texto y se queda en 0)."""
    nes = NesConsole(path)
    nes.run(120)
    chose = False
    for _ in range(E.SKIN_PLAY_PRESSES):
        if nes.ram(E.GAMESTATE_ADDR) == E.GAMESTATE_PLAYING:
            break
        if not chose and any("LEVEL SELECT" in screen_text(nes, r)
                             for r in range(30)):
            for _ in range(level):
                nes.run(6, BTN["DOWN"]); nes.run(12)
            chose = True
        nes.run(6, BTN["START"]); nes.run(E.SKIN_PLAY_SETTLE)
    if nes.ram(E.GAMESTATE_ADDR) != E.GAMESTATE_PLAYING:
        return None, False
    return nes, chose


def measure_levels(path, start=0, clears=24):
    """(LINEAS, NIVEL) cada vez que el nivel cambia, limpiando una fila tras
    otra: el campo vaciado, una fila completa abajo, y abajo pulsado hasta que
    la cuenta de LINEAS se mueve."""
    nes, chose = boot_at_level(path, start)
    if nes is None:
        return None, "no llego a jugar"
    if start and not chose:
        return None, "no tiene LEVEL SELECT de texto"
    changes, last = [], None
    for _ in range(clears):
        empty_field(nes)
        fill_row(nes, ROW1 - 1)
        before = shown_number(nes, 4)
        for _ in range(600):
            nes.run(1, BTN["DOWN"])
            if shown_number(nes, 4) != before:
                break
        else:
            return None, "una fila completa no conto"
        nes.run(30)
        level = shown_number(nes, 6)
        if level != last:
            changes.append((shown_number(nes, 4), level))
            last = level
    return changes, None


if __name__ == "__main__":
    import sys

    def shape(cells):
        if not cells:
            return None
        r0 = min(r for r, _c in cells); c0 = min(c for _r, c in cells)
        return frozenset((r - r0, c - c0) for r, c in cells)

    print("LA FILA COMPLETA: de tocar el monton a desaparecer")
    for path in sys.argv[1:]:
        nes = boot(path)
        if nes is None:
            print("  %-14s no llego a jugar" % path.split("/")[-1]); continue
        fill_row(nes, ROW1 - 1)
        touch = gone = None
        for f in range(220):
            nes.run(1, BTN["DOWN"])
            if touch is None and any(row_cells(nes, ROW1 - 2)[3:13]):
                touch = f
            if sum(1 for v in row_cells(nes, ROW1 - 1)[3:13] if v) < 10:
                gone = f; break
        print("  %-14s toca en f%s, se va en f%s -> %s frames"
              % (path.split("/")[-1], touch, gone,
                 gone - touch if None not in (touch, gone) else "?"))

    print("LA PATADA DE PARED: girar pegado al muro derecho")
    for path in sys.argv[1:]:
        nes = boot(path)
        if nes is None:
            continue
        wall = inside = tries = moved = 0
        for _piece in range(14):
            for _ in range(90):
                nes.run(1, BTN["RIGHT"])
                cells = occupied(nes)
                if cells and max(c for _r, c in cells) >= 12:
                    break
            nes.run(4)
            before = occupied(nes)
            if not before or max(c for _r, c in before) < 12:
                nes.run(60); continue
            tries += 1
            xb = min(c for _r, c in before)
            nes.run(4, BTN["A"]); nes.run(6)
            after = occupied(nes)
            if shape(before) != shape(after):
                wall += 1
                if after and min(c for _r, c in after) != xb:
                    moved += 1
            nes.run(4, BTN["LEFT"]); nes.run(6)
            b2 = occupied(nes)
            nes.run(4, BTN["A"]); nes.run(6)
            if shape(b2) != shape(occupied(nes)):
                inside += 1
            for _ in range(60):
                nes.run(1, BTN["DOWN"])
        print("  %-14s %d intentos: gira pegada %d, una columna dentro %d, "
              "y se desplaza al girar %d veces"
              % (path.split("/")[-1], tries, wall, inside, moved))

    print("EL NIVEL: (lineas, nivel) cada vez que cambia")
    for path in sys.argv[1:]:
        for start, clears in ((0, 24), (3, 45)):
            got, why = measure_levels(path, start, clears)
            print("  %-14s desde %d: %s" % (path.split("/")[-1], start,
                                             got if got else why))
