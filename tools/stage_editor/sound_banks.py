"""Boss sound banks in any stage (2026-10-20). The game loads ONE stage sound-effect bank (ARAM bank 7) picked
by the stage number (DOL tables 8053E1B8 .mlt / 8053CF20 AudioData*.bin); a boss placed in another stage asks for its
own ids (Egg Breaker 0x7Cxx, Heavy Dog / Blue Falcon 0x7Dxx ...) and finds nothing. ARAM banks 8 / 9 / 10 exist but are
reserved with 500 bytes and never loaded (files/sound/GCAX.conf); 11-15 are not made at all. So for the extra boss banks
(as many as fit the free ARAM, 2.9 MB - in practice all six boss families placeable outside their stage) the build writes:
  * sound/<boss bank>.mlt  - the boss stage's bank, its bank number (byte 0x24 of the MLT header, 7) set to 8..15;
  * sound/<slot table>.bin - the slot's own AudioData table (every row kept) + the boss tables' rows whose ids the slot
    lacks, pointing at bank 8..15 (same sample index);
  * mods/sound_banks.txt   - '<bank> <file> <ARAM bytes>' lines: runtime/sound_banks.cpp sizes ARAM banks 8-10 and makes
    11-15 at boot (gcaxNewAramBank; the sizes are compiled in - files/sound/GCAX.conf is not read by the game) and loads
    the files right after the stage bank.
AudioData*.bin (big-endian, offsets relative to 0x20): u32 size, u32 relocation-list offset, u32 its length (padded to
4), 1, 0, 0x3142, 0, 0 | u32 8 (rows pointer), u32 row count | rows of 6 u32: name offset, sound id, bank << 24 |
index << 16 | .., params, 0, 0 | names (NUL-terminated, padded to 4) | relocation list (Sega POF0: '@' 'B' then 'F' per
further row = pointer at +0, +8, then every +24) | 8 zero bytes | 'SCENE' NUL."""
import struct

MLT_BANK_BYTE = 0x24
EXTRA_BANKS = tuple(range(8, 16))   # 8-10 exist (500 bytes compiled in, resized at boot); 11-15 are made after bank 10
ARAM_REST = 2968192                 # free ARAM after the game's own banks ('AX SDLIB: aramRest', vanilla boot)
ARAM_MARGIN = 64 * 1024             # left free on purpose
COMPILED_IN = {8: 512, 9: 512, 10: 512}   # what banks 8-10 already take (500 bytes, aligned)


def aram_bytes(mlt_size):
    """ARAM a bank file needs: its size rounded up to 4 KiB"""
    return (mlt_size + 4095) // 4096 * 4096


def fit_banks(need, sizes):
    """#125 sound cap by SIZE, not count: need = boss bank names in placement order, sizes = name ->
    ARAM bytes. Keeps them in order while they fit ARAM_REST - ARAM_MARGIN (and there is a bank number left);
    returns (kept [(bank, name)], left out [names], bytes used)"""
    kept, left, used = [], [], 0
    for name in need:
        bank = EXTRA_BANKS[len(kept)] if len(kept) < len(EXTRA_BANKS) else None
        extra = sizes[name] - COMPILED_IN.get(bank, 0) if bank else None
        if bank is None or used + extra > ARAM_REST - ARAM_MARGIN: left.append(name); continue
        kept.append((bank, name)); used += extra
    return kept, left, used


def read_table(raw):
    n = struct.unpack_from('>I', raw, 0x24)[0]
    rows = []
    for i in range(n):
        noff, sid, w2, w3, w4, w5 = struct.unpack_from('>6I', raw, 0x28 + 24 * i)
        a = 0x20 + noff
        rows.append(dict(name=raw[a:raw.index(b'\0', a)], id=sid, w=[w2, w3, w4, w5]))
    return rows


def write_table(rows):
    n = len(rows); rows_end = 0x28 + 24 * n
    names = b''; offs = []
    for r in rows:
        offs.append(rows_end + len(names) - 0x20); names += r['name'] + b'\0'
    names += b'\0' * (-len(names) % 4)
    pof = b'@B' + b'F' * (n - 1); pof_len = len(pof) + (-len(pof) % 4); pof = pof.ljust(pof_len, b'\0')
    pof_off = rows_end + len(names) - 0x20
    body = b''.join(struct.pack('>6I', o, r['id'], *r['w']) for o, r in zip(offs, rows))
    tail = pof + b'\0' * 8 + b'SCENE\0'
    size = rows_end + len(names) + len(tail)
    head = struct.pack('>8I', size, pof_off, pof_len, 1, 0, 0x3142, 0, 0) + struct.pack('>2I', 8, n)
    out = head + body + names + tail
    assert len(out) == size
    return out


def merged_table(slot_raw, extra):
    """extra = [(bank, boss table raw)]: slot rows + each boss's rows for ids nobody has yet (bank byte set)"""
    rows = read_table(slot_raw); have = {r['id'] for r in rows}; added = []
    for bank, raw in extra:
        for r in read_table(raw):
            if r['id'] in have: continue
            r = dict(r, w=[(r['w'][0] & 0x00FFFFFF) | (bank << 24)] + r['w'][1:]); rows.append(r); have.add(r['id']); added.append((bank, r['id']))
    return write_table(rows), added


def renumber_mlt(raw, bank):
    assert raw[:8] == b'gcaxMLT ' and raw[0x10:0x18] == b'gcaxMLTM', 'not a GCAX MLT bank'
    b = bytearray(raw); b[MLT_BANK_BYTE] = bank; return bytes(b)


def gcax_conf(text, sizes):
    """GCAX.conf with 'AramBank <n> <bytes>' raised for the banks in sizes {bank: bytes}"""
    out = []
    for line in text.splitlines(keepends=True):
        p = line.split()
        if len(p) >= 3 and p[0] == 'AramBank' and p[1].isdigit() and int(p[1]) in sizes:
            line = f'AramBank\t{int(p[1]):2d}\t{sizes[int(p[1])]}\r\n'
        out.append(line)
    return ''.join(out)


if __name__ == '__main__':   # self-test: the game's own tables survive a read / write round trip byte for byte
    import sys
    from pathlib import Path
    d = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[3] / 'Shadow' / 'files' / 'sound')   # ..\Shadow next to the project
    for f in sorted(d.glob('AudioData*.bin')):
        raw = f.read_bytes(); ok = write_table(read_table(raw)) == raw
        print(f.name, 'round trip', 'OK' if ok else 'DIFFERS')
