"""
The sound: the cartridge's engine against its golden recording,
pause, the hand-entered tunes and the menu effects.
"""
import os
from .harness import (
    APU_REGS, AUDIO_ALIGN_SEARCH, AUDIO_GOLDEN_SKIP, EFFECT_WATCH_FRAMES,
    GOLDEN_PATH, KEYS, MUSIC_ROW, REG_SOUND1CNT_H,
    REG_SOUND1CNT_X, REG_SOUND2CNT_L, REG_SOUND3CNT_H, REG_SOUNDCNT_X,
    game_offsets, game_state_address, load, press_start,
    run, sound_state, start_game, tilemap_text,
    to_music_page,
)
# Where the APU register file and the fault flag sit inside Nes6502. These
# move whenever that struct changes, so the ROM exports them rather than
# letting a constant here drift out of date: kNes6502Probe is three halfwords,
# {offsetof(bus.apu), offsetof(faulted), how many registers}. Reading a stale
# offset does not fail loudly — it reads a neighbouring byte and quietly
# reports the wrong thing, which is how a real check turns into a green light
# that means nothing.
def nes6502_probe(rom_path, core):
    addr, why = game_state_address(rom_path, "kNes6502Probe")
    if addr is None:
        return None, why
    return tuple(core.memory.u16[addr + i * 2] for i in range(3)), None


def pause_audio_check(rom_path):
    core, screen = load(rom_path)    # `screen` must stay alive; see load()
    start_game(core, tune=1)         # LOGINSKA: a silent pause proves nothing

    # The music has to have got going, or a silent pause proves nothing.
    heard = set()
    for _ in range(180):
        core.run_frame()
        state = sound_state(core)
        if state["activos"]:
            heard.add(state["activos"])

    core.set_keys(KEYS["START"])
    run(core, 4)
    core.set_keys()
    run(core, 8)

    worst = {k: 0 for k in sound_state(core)}
    for _ in range(120):
        core.run_frame()
        for k, v in sound_state(core).items():
            worst[k] = max(worst[k], v)

    print(f"  sonando: se oyeron los canales {sorted(heard)}")
    print("  en pausa: " + ", ".join(f"{k}={v}" for k, v in worst.items()))

    failures = []
    if not heard:
        failures.append("no habia musica que pausar; la medida no prueba nada")
    for name in ("pulso 1", "pulso 2", "triangulo", "ruido", "activos"):
        if worst[name]:
            failures.append(f"en pausa {name} sigue sonando ({worst[name]})")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: al pausar todos los canales quedan a cero.")
    return 0


def audio_check(rom_path):
    base, why = game_state_address(rom_path, "g_cpu")
    if base is None:
        print(f"SALTADO: {why}")
        return 0
    if not os.path.exists(GOLDEN_PATH):
        print(f"SALTADO: falta {GOLDEN_PATH} (lo genera `make assets ROM=...`)")
        return 0

    raw = open(GOLDEN_PATH, "rb").read()
    golden = [raw[i:i + APU_REGS] for i in range(0, len(raw), APU_REGS)]

    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    probe, why = nes6502_probe(rom_path, core)
    if probe is None:
        print(f"SALTADO: {why}")
        return 0
    apu_offset, fault_offset, apu_regs = probe
    if apu_regs != APU_REGS:
        print(f"FALLA: la ROM dice {apu_regs} registros de APU, el golden trae {APU_REGS}")
        return 1
    iwram = core.memory.iwram
    apu_off = base + apu_offset - 0x03000000
    fault_off = base + fault_offset - 0x03000000

    # THE FIREWORKS HAVE TO BE HELD OFF FOR THIS. Every burst calls
    # setMusicOrSoundEffect of its own (LACA0, main.asm.txt:6104-6109), so the
    # title's APU carries bangs the reference recording — which is a tune and
    # nothing else — knows nothing about. That is the cartridge working
    # correctly; it just cannot be inside the measurement.
    #
    # The cartridge's own lever for it is player2FallTimer ($6B): LA9DE counts
    # it down once a frame and starts a burst when it reaches zero (:5740). The
    # harness holds it away from zero, which is a fixture, never anything the
    # ROM knows about — the same shape as planting completed rows for the
    # line-clear check.
    ram, why = game_state_address(rom_path, "g_nes_ram")
    if ram is None:
        print(f"SALTADO: {why}")
        return 0
    fireworks_timer = ram + 0x6B

    seen = []
    for _ in range(len(golden) + AUDIO_ALIGN_SEARCH):
        core.memory.u8[fireworks_timer] = 200
        core.run_frame()
        seen.append(bytes(iwram[apu_off:apu_off + APU_REGS]))

    failures = []
    if iwram[fault_off]:
        failures.append("el interprete 6502 se detuvo por un opcode que no conoce")

    # WHERE THE TWO LINE UP, in both directions. The ROM reaches this screen
    # through a SOUND_SCREEN_SWITCH effect that is still ringing when the tune
    # starts, and the reference recording has no effect in it, so the first
    # frames of the golden have no counterpart in the ROM at all — searching
    # only for a shift in one of the two could never find the match. Skipping
    # a few frames of each finds it, and everything after has to be identical.
    start, gstart, matched = 0, 0, 0
    for gskip in range(AUDIO_GOLDEN_SKIP):
        for offset in range(AUDIO_ALIGN_SEARCH):
            n = 0
            while (gskip + n < len(golden) and offset + n < len(seen)
                    and seen[offset + n] == golden[gskip + n]):
                n += 1
            if n > matched:
                matched, start, gstart = n, offset, gskip

    want = len(golden) - gstart
    print(f"  motor de sonido alineado en el frame {start} de la ROM y el "
          f"{gstart} del golden; {matched} de {want} frames identicos")
    if matched < want:
        failures.append(f"el APU emulado se desvia en el frame {matched}: "
                        f"ROM {seen[start + matched].hex(' ')} "
                        f"vs referencia {golden[gstart + matched].hex(' ')}")

    if not (core.memory.u16[REG_SOUNDCNT_X] & 0x0080):
        failures.append("el sonido del GBA nunca se encendio")
    if core.memory.u16[REG_SOUND1CNT_H] == 0:
        failures.append("el canal de pulso 1 quedo sin configurar")

    # And it has to fit in a frame. Emulating a few thousand 6502 instructions
    # every frame is the one thing in this port that could plausibly overrun
    # its budget, and the symptom would be a missed vsync — which shows up as
    # gravity running slow. Level 0 drops the piece exactly every 33 frames
    # (possibleFallTimerTable entry 0), so any other interval means a frame was
    # lost to the sound engine.
    game_base, why = game_state_address(rom_path)
    if game_base is None:
        print(f"  (sin comprobar el presupuesto de CPU: {why})")
    else:
        core.reset()
        start_game(core)
        piece_y = game_base + game_offsets(rom_path)["y"]
        seen_y = []
        for _ in range(400):
            core.run_frame()
            seen_y.append(core.memory.u8[piece_y])
        drops = [i for i in range(1, len(seen_y)) if seen_y[i] != seen_y[i - 1]]
        gaps = sorted({drops[i] - drops[i - 1] for i in range(1, len(drops))})
        if gaps != [33]:
            failures.append(f"la gravedad cayo cada {gaps} frames en vez de 33: "
                            "el bucle esta perdiendo vsyncs")
        else:
            print(f"  presupuesto de CPU: {len(drops)} caidas, todas a 33 frames exactos")

    # The choppiness this check used to measure — how much of the envelope
    # moves at exactly the frame rate — came out of the emulator's audio
    # buffer, and that measurement is not reproducible here: the same build
    # over the same frames gives 2% on one run and 4% on the next. A number
    # that changes when nothing changed is not evidence, so it is gone rather
    # than quietly reported. What replaced it is `--pause-audio`, which reads
    # the sound registers instead and gives the same answer every time.

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: la ROM ejecuta el motor de sonido del cartucho, nota por nota.")
    return 0


def quit_audio_check(rom_path):
    """SALIR POR EL MENU DE PAUSA NO PUEDE DEJAR LA MAQUINA MUDA.

    Pausar manda el MUSIC_SUSPEND del cartucho, que no es un silencio de la
    musica sino una MORDAZA sobre el motor entero: tambien calla los efectos, y
    lo unico que la levanta es MUSIC_RESUME. Toda salida normal de la pausa
    pasa por pauseOrUnpause y lo manda; la del menu secreto desmonta la partida
    por debajo del cartel y no pasaba por ahi, asi que el motor se quedaba
    amordazado para el resto de la sesion. Sonaba exactamente asi: ni las
    piezas al caer, ni la musiquita de game over, ni el blip de los menus, ni
    el tema al volver al titulo, hasta apagar la consola.

    Asi que esto no mira una pantalla: recorre el camino del jugador -- una
    partida en WITH COMPUTER, pausa, L+R, EXIT, SI -- y cuenta frames con algun
    canal sonando en cada sitio al que lleva.
    """
    core, screen = load(rom_path)   # `screen` must stay alive; see load()

    def tap(*names, hold=4, settle=12):
        core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
        core.set_keys(); run(core, settle)

    def heard(frames):
        n = 0
        for _ in range(frames):
            core.run_frame()
            if sound_state(core)["activos"]:
                n += 1
        return n

    run(core, 8)
    tap("START"); run(core, 10)            # titulo -> GAME SELECT
    tap("L", "R")                           # el acorde vive aqui ahora
    for _ in range(4):                      # -> WITH COMPUTER
        tap("DOWN")
    tap("START"); run(core, 12)             # -> LEVEL SETTINGS
    for _ in range(2):                      # el cursor hasta MUSIC
        tap("DOWN")
    tap("RIGHT")                            # NO MUSIC -> LOGINSKA
    tap("START"); run(core, 30)             # -> a jugar

    playing = heard(120)
    tap("START"); run(core, 10)             # pausa, que ya es el menu
    tap("DOWN")                             # MUSIC -> EXIT
    tap("A")                                # -> la pregunta
    tap("RIGHT")                            # NO -> YES
    tap("A"); run(core, 40)                 # fuera

    title = heard(300)
    tap("START"); run(core, 30)             # -> GAME SELECT
    core.set_keys(KEYS["DOWN"])
    blip = heard(4)
    core.set_keys()
    blip += heard(20)
    tap("START"); run(core, 20)             # -> LEVEL SETTINGS
    for _ in range(2):
        tap("DOWN")
    tap("RIGHT")
    tap("START"); run(core, 30)             # -> a jugar otra vez
    again = heard(180)

    print(f"  jugando antes de salir:   {playing:3d}/120 frames con sonido")
    print(f"  el titulo al volver:      {title:3d}/300")
    print(f"  el blip del cursor:       {blip:3d}/24")
    print(f"  la siguiente partida:     {again:3d}/180")

    failures = []
    if playing < 20:
        failures.append("no habia sonido antes de salir; la medida no prueba nada")
    if title < 60:
        failures.append("el titulo vuelve mudo despues de salir por el menu de pausa")
    if blip < 2:
        failures.append("los menus pierden su blip despues de salir por el menu de pausa")
    if again < 40:
        failures.append("la siguiente partida es muda despues de salir por el menu de pausa")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: salir por el menu de pausa deja el motor de sonido como estaba.")
    return 0


def effects_check(rom_path):
    """LOS DOS RUIDOS DE UN MENU, QUE NO SON LOS MISMOS EN LAS CUATRO ROMS.

    Medido en los volcados de verdad, apuntando cada escritura a $4000-$4015
    mientras se movia el cursor y mientras cambiaba la pantalla:

      cursor    release      pulso 2, periodo $4B7 (unos 93Hz) con el barrido
                             del chip doblandolo hacia abajo once frames
                A, B y C     pulso 2, periodo $021 (unos 3.3kHz), sin barrido,
                             y se acaba en ocho
      pantalla  release      pulso 1, una nota barrida
                proto_a      nada en absoluto
                B y C        pulso 1, un trino que sube y que barren a mano,
                             reescribiendo el periodo cada frame

    El del release lo toca el motor del cartucho, que va dentro de esta ROM.
    Los de los prototipos no: se capturan de sus volcados como los registros
    que escriben y se reproducen por la misma conversion (nes_audio_effect).

    Lo que se comprueba aqui es lo que llega al chip: que el tic del cursor
    con skin esta CINCO OCTAVAS por encima del que suena sin ella, que la
    pantalla suena distinta, que un prototipo que contesta con silencio
    contesta con silencio, y que ninguno de los dos se queda sonando.
    """
    failures = []
    R1X, R1H = 0x04000064, 0x04000062
    R2H, R2L = 0x0400006C, 0x04000068

    def listen(skin, button):
        """(frecuencias oidas, volumenes frame a frame) de los dos pulsos."""
        core, screen = load(rom_path)   # `screen` must stay alive; see load()
        _ = screen

        def tap(*names, hold=4, settle=12):
            core.set_keys(*[KEYS[n] for n in names]); run(core, hold)
            core.set_keys(); run(core, settle)

        run(core, 40)
        if skin:
            tap("L", "R"); run(core, 20)
            for _ in range(skin - 1):
                tap("R"); run(core, 20)
        press_start(core); run(core, 30)      # -> GAME SELECT
        core.set_keys(KEYS[button]); run(core, 4); core.set_keys()
        f1, f2, v1, v2 = set(), set(), [], []
        for _ in range(EFFECT_WATCH_FRAMES):
            core.run_frame()
            v1.append(core.memory.u16[R1H] >> 12)
            v2.append(core.memory.u16[R2L] >> 12)
            if v1[-1]:
                f1.add(core.memory.u16[R1X] & 0x7FF)
            if v2[-1]:
                f2.add(core.memory.u16[R2H] & 0x7FF)
        del core, screen
        return f1, f2, v1, v2

    # EL TIC DEL CURSOR. El del release es grave y el de los prototipos agudo,
    # y en la GBA una frecuencia mas alta es un registro MAS GRANDE (R = 2048 -
    # 131072/f), asi que el numero del prototipo tiene que ser mayor.
    _f1, rel_f2, _v1, rel_v2 = listen(0, "DOWN")
    if not rel_f2:
        failures.append("sin skin el cursor no hace ruido")
    _f1, pro_f2, _v1, pro_v2 = listen(1, "DOWN")
    if not pro_f2:
        failures.append("con skin el cursor no hace ruido")
    if rel_f2 and pro_f2:
        rel, pro = max(rel_f2), max(pro_f2)
        if pro <= rel:
            failures.append(f"el tic del cursor con skin ({pro}) no es mas agudo "
                             f"que el del cartucho ({rel})")
        else:
            print(f"  el tic del cursor sube de {rel} a {pro}: el del prototipo "
                   f"esta cinco octavas por encima")
    # ...y NINGUNO se queda sonando: los dos acaban en silencio.
    for name, vols in (("release", rel_v2), ("prototipo", pro_v2)):
        if vols and vols[-1]:
            failures.append(f"el tic del cursor del {name} se queda sonando "
                             f"(volumen {vols[-1]:X} al final de "
                             f"{EFFECT_WATCH_FRAMES} frames)")
        else:
            print(f"  ...y el del {name} se apaga solo")

    # EL CAMBIO DE PANTALLA. proto_a contesta con silencio y los otros dos con
    # un trino que sube; el release, con una nota barrida.
    rel_f1, _f2, rel_v1, _v2 = listen(0, "START")
    a_f1, _f2, a_v1, _v2 = listen(1, "START")
    b_f1, _f2, b_v1, _v2 = listen(2, "START")
    if not rel_f1:
        failures.append("sin skin cambiar de pantalla no hace ruido")
    else:
        print(f"  el cambio de pantalla del cartucho suena en {sorted(rel_f1)}")
    if any(a_v1):
        failures.append(f"proto_a deberia cambiar de pantalla EN SILENCIO y "
                         f"suena (volumenes {a_v1})")
    else:
        print("  proto_a cambia de pantalla sin decir nada, como su volcado")
    if len(b_f1) < 3:
        failures.append(f"el trino de proto_b no barre: solo {sorted(b_f1)}")
    elif b_f1 == rel_f1:
        failures.append("el cambio de pantalla de proto_b suena igual que el "
                         "del cartucho")
    else:
        print(f"  ...y proto_b con un trino de {min(b_f1)} a {max(b_f1)}, "
               f"{len(b_f1)} pasos barridos a mano")
    # ...Y NINGUNO SE QUEDA SONANDO. El de proto_c gastaba justo el ultimo
    # frame de la ventana de captura, asi que su apagado se quedaba fuera y el
    # trino sonaba para siempre; la ventana es mas ancha y el extractor se
    # niega ahora a capturar un efecto que no ha terminado dentro de ella.
    c_f1, _f2, c_v1, _v2 = listen(3, "START")
    for name, vols in (("proto_b", b_v1), ("proto_c", c_v1)):
        if vols and vols[-1]:
            failures.append(f"el trino de {name} se queda sonando (volumen "
                             f"{vols[-1]:X} tras {EFFECT_WATCH_FRAMES} frames)")
        else:
            print(f"  ...y el trino de {name} se apaga solo")

    for f in failures:
        print("FALLA:", f)
    if failures:
        return 1
    print("OK: cada construccion hace en sus menus el ruido que hace la suya.")
    return 0


def handtunes_check(rom_path):
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    failures = []

    def tap(key, settle=8):
        core.set_keys(key)
        run(core, 4)
        core.set_keys()
        run(core, settle)

    run(core, 8)
    to_music_page(core, 8)
    run(core, 8)

    # Four tunes, and no fifth on offer.
    # musicSelectTable has FIVE entries and the first is silence
    # (main.asm.txt:4741, "silence, loginska, bradinsky, karinka, troika").
    seen = set()
    for _ in range(10):
        seen.add(tilemap_text(core, MUSIC_ROW))
        tap(KEYS["RIGHT"])
    for hidden in ("KOROBEINIKI", "KATIUSKA"):
        if any(hidden in row for row in seen):
            failures.append(f"{hidden} se ofrece sin haber metido el codigo")
    if len(seen) != 5:
        failures.append(f"el menu ofrece {len(seen)} canciones, no 5: {sorted(seen)}")
    elif not any("NO MUSIC" in row for row in seen):
        failures.append("falta la primera entrada de musicSelectTable, el silencio")
    else:
        print(f"  antes del codigo: {len(seen)} canciones, con el silencio de la ROM")

    # L+R together, the two buttons a NES pad never had.
    core.set_keys(KEYS["L"], KEYS["R"])
    run(core, 4)
    core.set_keys()
    run(core, 10)
    # The row carries the menu frame's border tiles either side of the name,
    # so this looks for the name IN it, the way the other menu checks do.
    if "KOROBEINIKI" not in tilemap_text(core, MUSIC_ROW):
        failures.append("L+R no descubre KOROBEINIKI ni la deja elegida")
        print(f"       la fila dice {tilemap_text(core, MUSIC_ROW)!r}")
    else:
        print("  L+R descubre KOROBEINIKI y la deja elegida")

    # The code uncovers THREE entries, not one: the two hand-entered tunes and
    # MUSIC MIX, which plays them all in turn and turns over at every level-up.
    unlocked = set()
    for _ in range(16):
        unlocked.add(tilemap_text(core, MUSIC_ROW))
        tap(KEYS["RIGHT"])
    if len(unlocked) != 8:
        failures.append(f"tras el codigo el menu ofrece {len(unlocked)}, no 8")
    else:
        missing = [n for n in ("KATIUSKA", "MUSIC MIX")
                   if not any(n in row for row in unlocked)]
        if missing:
            failures.append(f"el codigo no descubre {', '.join(missing)}")
        else:
            print(f"  tras el codigo: {len(unlocked)} entradas, "
                  "con KATIUSKA y MUSIC MIX")

    # Back onto it, then into a game.
    for _ in range(10):
        if "KOROBEINIKI" in tilemap_text(core, MUSIC_ROW):
            break
        tap(KEYS["RIGHT"])
    press_start(core)
    run(core, 10)

    # It has to make notes, and they have to move. The pulse channels are the
    # two it drives; the frequency register is what a tune changes.
    io = core._native.memory.io

    def reg(addr):
        return io[(addr - 0x04000000) >> 1]

    pitches, volumes = set(), set()
    bass_vol, bass_on, tri_on = set(), 0, 0
    for _ in range(400):
        core.run_frame()
        pitches.add(reg(REG_SOUND1CNT_X) & 0x7FF)
        volumes.add((reg(REG_SOUND1CNT_H) >> 12) & 0xF)
        v2 = (reg(REG_SOUND2CNT_L) >> 12) & 0xF
        if v2:
            bass_vol.add(v2)
            bass_on += 1
        if reg(REG_SOUND3CNT_H) & 0xE000:
            tri_on += 1
    if max(volumes) == 0:
        failures.append("KOROBEINIKI no suena: el pulso 1 queda a volumen cero")
    elif len(pitches) < 6:
        failures.append(f"KOROBEINIKI no cambia de nota: {len(pitches)} tono(s)")
    else:
        print(f"  suena y se mueve: {len(pitches)} tonos distintos en 400 frames")

    # AND NO LOUDER THAN THE FOUR THE CARTRIDGE PLAYS. Measured off these same
    # registers while the ROM's engine played its own: pulse 1 sits at 5 (7 for
    # BRADINSKY), pulse 2 at 3 (5 for TROIKA), and the triangle is going under
    # all four of them for half the frames or more. The hand-entered pair used
    # to run at 11 and 8 with the second sounding 97% of the time and nothing
    # on the triangle at all — twice the amplitude on the lead, nearly three
    # times on a second square that never stopped, which is what "saturated"
    # was.
    loud = max(volumes)
    if loud > 7:
        failures.append(f"KOROBEINIKI lleva el pulso 1 a {loud}; el cartucho "
                         "no pasa de 7")
    elif bass_vol and max(bass_vol) > 5:
        failures.append(f"...y el pulso 2 a {max(bass_vol)}; el cartucho no "
                         "pasa de 5")
    elif bass_on > 400 * 7 // 10:
        failures.append(f"el pulso 2 suena en el {100 * bass_on // 400}% de los "
                         "frames; en el cartucho no llega al 50")
    elif tri_on < 400 // 4:
        failures.append(f"el triangulo solo suena el {100 * tri_on // 400}%: el "
                         "cartucho lleva ahi el bajo de sus cuatro canciones")
    else:
        print(f"  y se mantiene en el nivel del cartucho: pulso 1 a {loud}, "
              f"pulso 2 a {max(bass_vol)} el {100 * bass_on // 400}% del "
              f"tiempo, triangulo el {100 * tri_on // 400}%")

    # PAUSE has to reach it too.
    tap(KEYS["START"], settle=10)
    worst = 0
    for _ in range(120):
        core.run_frame()
        for k, v in sound_state(core).items():
            worst = max(worst, v)
    if worst:
        failures.append(f"en pausa la cancion de mas sigue sonando ({worst})")
    else:
        print("  PAUSE la silencia igual que a las del cartucho")

    # ...and the cartridge's engine is still there underneath: unpause and drop
    # a piece, which plays SOUND_DROP through the ROM's own engine.
    # Watched over the whole drop, not sampled at the end: a sound effect is a
    # few frames long and asking once, afterwards, mostly asks too late.
    tap(KEYS["START"], settle=10)
    core.set_keys(KEYS["DOWN"])
    engine = 0
    for _ in range(300):
        core.run_frame()
        engine = max(engine, sound_state(core)["activos"])
    core.set_keys()
    if not engine:
        failures.append("el motor del cartucho no sigue vivo bajo la cancion de mas")
    else:
        print("  el motor del cartucho sigue sonando debajo (los efectos son suyos)")

    # THE SECOND HAND-ENTERED TUNE, on its own console. Katyusha is a
    # different score and a different tempo, so "it plays" is not enough: the
    # set of pitches it reaches has to be its own. Two tunes sharing one
    # sequencer is exactly how a bad table index looks like nothing at all.
    other, other_screen = load(rom_path)    # `other_screen` must stay alive
    _ = other_screen
    run(other, 8)
    to_music_page(other, 8)
    run(other, 8)
    other.set_keys(KEYS["L"], KEYS["R"]); run(other, 4)
    other.set_keys(); run(other, 10)
    for _ in range(12):
        if "KATIUSKA" in tilemap_text(other, MUSIC_ROW):
            break
        other.set_keys(KEYS["RIGHT"]); run(other, 4)
        other.set_keys(); run(other, 8)
    else:
        failures.append("no pude dejar KATIUSKA elegida en el menu")
    press_start(other)
    run(other, 10)
    other_io = other._native.memory.io
    kat_pitches, kat_volumes = set(), set()
    for _ in range(400):
        other.run_frame()
        kat_pitches.add(other_io[(REG_SOUND1CNT_X - 0x04000000) >> 1] & 0x7FF)
        kat_volumes.add((other_io[(REG_SOUND1CNT_H - 0x04000000) >> 1] >> 12) & 0xF)
    if max(kat_volumes) == 0:
        failures.append("KATIUSKA no suena: el pulso 1 queda a volumen cero")
    elif len(kat_pitches) < 6:
        failures.append(f"KATIUSKA no cambia de nota: {len(kat_pitches)} tono(s)")
    elif kat_pitches == pitches:
        failures.append("KATIUSKA toca exactamente los tonos de KOROBEINIKI")
    else:
        print(f"  KATIUSKA es otra cancion: {len(kat_pitches)} tonos, "
              f"{len(kat_pitches ^ pitches)} distintos de los de KOROBEINIKI")

    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: las dos canciones de mas estan escondidas, suenan, "
          "y no pisan al cartucho.")
    return 0


def sweep_check(rom_path):
    """THE LINE CLEAR RISES, as the NES's pulse sweep makes it.

    The effect is pulse 2 with `$4005 = $8A` — sweep on, pitch rising, shift
    2 — and a period of $5B0. The NES then takes a quarter off the period on
    every half-frame, two a frame: $5B0 -> $444 -> $333 in the first frame,
    $333 -> $267 -> $1CE in the next, and so on up, until the engine writes
    the next note's low byte over what the sweep has left. Without the sweep
    the GBA held $5B0 flat, which is the zip's lowest note and the reason it
    sounded low ("mas grave").

    Read off `g_sweep` in gba/nes_audio.c — the period the channel is really
    playing — frame by frame across a clear.
    """
    base, why = game_state_address(rom_path)
    sweep, why2 = game_state_address(rom_path, "g_sweep")
    if base is None or sweep is None:
        print(f"FALLA: {why or why2}")
        return 1
    off = game_offsets(rom_path)
    core, screen = load(rom_path)   # `screen` must stay alive; see load()
    start_game(core)
    from .harness import fill_rows, KEY_DOWN, PF_H
    fill_rows(core, base + off["field"], [PF_H - 1])
    core.set_keys(KEY_DOWN)
    PULSE2 = 6              # sizeof(PulseSweep): the second channel's
    periods = []
    for _ in range(400):
        core.run_frame()
        periods.append(core.memory.u16[sweep + PULSE2])
    core.set_keys()
    want = [0x333, 0x1CE, 0x105]
    found = any(periods[i:i + 3] == want for i in range(len(periods) - 2))
    if not found:
        seen = [hex(p) for p in periods if p][:8]
        print(f"FALLA: el efecto de linea no sube como en el NES: se esperaba "
              f"{[hex(w) for w in want]} seguidos, y el pulso 2 hace {seen}")
        return 1
    print("  el pulso 2 barre de $5B0 hacia arriba: $333, $1CE, $105, dos "
          "pasos por frame como el barrido del NES")
    print("OK: el sonido de la linea sube de tono como en el cartucho.")
    return 0


def proto_pause_check(rom_path):
    """UNDER PROTO_A'S SKIN, PAUSE LEAVES THE TUNE PLAYING; UNDER PROTO_B'S IT
    DOES NOT.

    The list said all the prototypes pause without muting. Measured on the
    dumps (read_skin_pause_music in tools/extract_assets.py) it is proto_a's
    alone: proto_b, proto_c and proto_d go silent like the release. On the
    title L+R puts on the first skin (A) and R walks on from there.
    """
    failures = []
    for skin, name, keeps in ((1, "proto_a", True), (2, "proto_b", False)):
        core, screen = load(rom_path)    # `screen` must stay alive; see load()
        run(core, 40)
        core.set_keys(KEYS["L"], KEYS["R"]); run(core, 4)
        core.set_keys(); run(core, 32)
        for _ in range(skin - 1):
            core.set_keys(KEYS["R"]); run(core, 4)
            core.set_keys(); run(core, 32)
        start_game(core, tune=1)         # LOGINSKA
        heard = 0
        for _ in range(180):
            core.run_frame()
            heard = max(heard, sound_state(core)["activos"])
        core.set_keys(KEYS["START"]); run(core, 4)
        core.set_keys(); run(core, 8)
        paused = 0
        for _ in range(120):
            core.run_frame()
            paused = max(paused, sound_state(core)["activos"])
        del core, screen
        if not heard:
            failures.append(f"con la skin de {name} no sonaba nada que pausar")
        elif keeps and not paused:
            failures.append(f"con la skin de {name} la pausa calla la musica, y "
                            f"en su volcado sigue sonando")
        elif not keeps and paused:
            failures.append(f"con la skin de {name} la pausa deja sonar la "
                            f"musica, y en su volcado se calla")
        else:
            print(f"  {name}: en pausa la musica "
                  f"{'sigue sonando' if keeps else 'se calla'}, como en su volcado")
    for f in failures:
        print(f"FALLA: {f}")
    if failures:
        return 1
    print("OK: solo la pausa de proto_a deja sonar la musica.")
    return 0
