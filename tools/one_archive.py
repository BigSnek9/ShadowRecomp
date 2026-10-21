# Shadow the Hedgehog (GameCube) .ONE archives: "One Ver 0.50 / 0.60", a table of 0x38-byte entries from 0xb0
# (v0.50: name[0x20] size offset flag; v0.60: name[0x2c] size offset flag) and the files behind it, PRS-compressed
# (Sega's LZ77 variant) when flag = 1. Offsets are relative to 0xc.
#   python tools/one_archive.py list    <file.one>
#   python tools/one_archive.py extract <file.one> <out dir>
#   python tools/one_archive.py pack    <original.one> <new.one> <replacement file> [<replacement file> ...]
#       (every other file is copied from the original; a replacement takes the entry with the same name, a file
#        whose name the original lacks is added as a new entry)
import os, struct, sys


def prs_decompress(src):
    """Sega PRS (control bits LSB first)"""
    out = bytearray()
    p, ctrl, bits = 0, 0, 0

    def bit():
        nonlocal p, ctrl, bits
        if bits == 0:
            ctrl, bits = src[p], 8
            p += 1
        b = ctrl & 1
        ctrl >>= 1
        bits -= 1
        return b

    while p < len(src):
        if bit():                      # literal
            out.append(src[p]); p += 1
            continue
        if bit():                      # long copy
            if p + 1 >= len(src):
                break
            a, b = src[p], src[p + 1]
            p += 2
            off = ((b << 8) | a) >> 3
            n = a & 7
            if off == 0:
                break
            off -= 0x2000
            if n == 0:
                n = src[p] + 1
                p += 1
            else:
                n += 2
        else:                          # short copy
            n = (bit() << 1 | bit()) + 2
            off = src[p] - 0x100
            p += 1
        for _ in range(n):
            out.append(out[len(out) + off])
    return bytes(out)


def prs_compress(data):
    """a straightforward greedy PRS encoder (hash chains over 3-byte keys); any valid stream decodes in the game"""
    out = bytearray()
    state = {'pos': 0, 'ctrl': 0, 'n': 0}

    def put_bit(b):
        if state['n'] == 0:
            state['pos'] = len(out)
            out.append(0)
            state['ctrl'] = 0
        state['ctrl'] |= (b & 1) << state['n']
        state['n'] += 1
        out[state['pos']] = state['ctrl']
        if state['n'] == 8:
            state['n'] = 0

    heads, prev = {}, [-1] * len(data)
    i, n = 0, len(data)
    while i < n:
        best_len, best_off = 0, 0
        if i + 3 <= n:
            j, tries = heads.get(data[i:i + 3], -1), 0
            while j >= 0 and tries < 48 and i - j <= 0x1FFF:
                ln = 0
                while i + ln < n and ln < 256 and data[j + ln] == data[i + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_off = ln, i - j
                    if ln == 256:
                        break
                j = prev[j]
                tries += 1
        if 2 <= best_len <= 5 and best_off <= 0x100:       # short copy
            put_bit(0); put_bit(0)
            put_bit(((best_len - 2) >> 1) & 1); put_bit((best_len - 2) & 1)
            out.append((-best_off) & 0xFF)
            step = best_len
        elif best_len >= 3:                                  # long copy
            ln = min(best_len, 256)
            put_bit(0); put_bit(1)
            v = ((-best_off) & 0x1FFF) << 3
            if ln <= 9:
                out += struct.pack('<H', v | (ln - 2))
            else:
                out += struct.pack('<H', v)
                out.append(ln - 1)
            step = ln
        else:                                                # literal
            put_bit(1)
            out.append(data[i])
            step = 1
        for k in range(i, min(i + step, n - 2)):
            key = data[k:k + 3]
            prev[k] = heads.get(key, -1)
            heads[key] = k
        i += step
    put_bit(0); put_bit(1)                                   # end marker: a long copy with offset 0
    out += b'\x00\x00'
    return bytes(out)


def entries(data):
    v60 = data[0x10:0x18] == b'Ver 0.60'
    nl = 0x2C if v60 else 0x20
    out, p = [], 0xB0
    count = struct.unpack_from('<I', data, 0x1C)[0]
    while len(out) < count and p + 0x38 <= len(data):
        raw = data[p:p + nl]
        name = raw.split(b'\x00')[0].decode('ascii', 'replace')
        size, off, flag = struct.unpack('<III', data[p + nl:p + nl + 12])
        out.append((name, size, off + 0xC, flag))
        p += 0x38
    return out


def read_all(data):
    """{name: decompressed bytes}, in table order"""
    ents = entries(data)
    res = {}
    for name, size, off, flag in ents:
        nxt = min([o for _, _, o, _ in ents if o > off] + [len(data)])
        blob = data[off:nxt]
        res[name] = prs_decompress(blob) if flag else bytes(blob)
    return res


def pack(original, replacements, compress=True):
    """a new archive: the original's header and table, every file from it except the replacements (name -> bytes).
    A replacement whose name the original lacks is added: a new row after the original ones (count at 0x1C),
    PRS-compressed like the others. Names match case-insensitively; added names are stored as given."""
    data = open(original, 'rb').read()
    ents = entries(data)
    # every row keeps its OWN data (two archives repeat a name: common/CommonEffect.one EFFWINGCUTTER.DFF / .UVA,
    # common/vehicleResource.one empty names); replacing a repeated name is ambiguous and refused
    count = {}
    for n, _, _, _ in ents: count[n.upper()] = count.get(n.upper(), 0) + 1
    nl = 0x2C if data[0x10:0x18] == b'Ver 0.60' else 0x20
    rows = []
    for name, size, off, flag in ents:
        nxt = min([o for _, _, o, _ in ents if o > off] + [len(data)]); blob = data[off:nxt]
        rows.append([name, flag, prs_decompress(blob) if flag else bytes(blob)])
    known = {r[0].upper(): r for r in rows}
    for name, raw in replacements.items():
        if count.get(name.upper(), 0) > 1:
            raise ValueError(f'{os.path.basename(original)}: {name!r} appears {count[name.upper()]} times; which one to replace is ambiguous - refused')
        if name.upper() not in known:
            if not name.isascii() or not 0 < len(name) < nl:
                raise ValueError(f'{name!r}: archive names are ASCII, 1-{nl - 1} characters')
            known[name.upper()] = [name, 1, None]; rows.append(known[name.upper()])
        known[name.upper()][2] = raw
    table_end = 0xB0 + 0x38 * len(rows)
    gap = 0x1C   # the originals leave this much between the table and the first file
    body, table = bytearray(), bytearray()
    for name, flag, raw in rows:
        use = bool(flag and compress)
        blob = prs_compress(raw) if use else raw
        off = table_end + gap - 0xC + len(body)
        table += name.encode('ascii').ljust(nl, b'\x00') + struct.pack('<III', len(raw), off, 1 if use else 0)
        table += b'\x00' * (0x38 - nl - 12)
        body += blob
        body += b'\x00' * ((-len(body)) % 4)
    out = bytearray(data[:0xB0]) + table + b'\x00' * gap + body
    struct.pack_into('<I', out, 4, len(out) - 0xC)
    struct.pack_into('<I', out, 0x1C, len(rows))
    return bytes(out)


def new_archive(template, files, block_name=None, compress=True):
    """a NEW archive with exactly `files` ([(name, bytes)], in order) in the format of `template` (an archive path:
    its version, header and name length); block_name replaces the header's name at 0x20 (chunk archives: 'blkNN')"""
    data = open(template, 'rb').read()
    nl = 0x2C if data[0x10:0x18] == b'Ver 0.60' else 0x20
    head = bytearray(data[:0xB0])
    if block_name is not None:
        head[0x20:0x40] = block_name.encode('ascii')[:31].ljust(32, b'\x00')
    table_end = 0xB0 + 0x38 * len(files); gap = 0x1C
    body, table = bytearray(), bytearray()
    for name, raw in files:
        if not name.isascii() or not 0 < len(name) < nl: raise ValueError(f'{name!r}: archive names are ASCII, 1-{nl - 1} characters')
        blob = prs_compress(raw) if compress else raw
        table += name.encode('ascii').ljust(nl, b'\x00') + struct.pack('<III', len(raw), table_end + gap - 0xC + len(body), 1 if compress else 0)
        table += b'\x00' * (0x38 - nl - 12)
        body += blob; body += b'\x00' * ((-len(body)) % 4)
    out = head + table + b'\x00' * gap + body
    struct.pack_into('<I', out, 4, len(out) - 0xC); struct.pack_into('<I', out, 0x1C, len(files))
    return bytes(out)


if __name__ == '__main__':
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == 'pack':
        reps = {os.path.basename(p).upper(): open(p, 'rb').read() for p in sys.argv[4:]}
        new = pack(path, reps)
        open(sys.argv[3], 'wb').write(new)
        print('packed', len(new), 'bytes (original', os.path.getsize(path), ')')
        sys.exit(0)
    data = open(path, 'rb').read()
    files = read_all(data) if cmd == 'extract' else {}
    for name, size, off, flag in entries(data):
        print(f'{name:26s} size {size:8d} offset {off:#9x} flag {flag}')
        if cmd == 'extract':
            os.makedirs(sys.argv[3], exist_ok=True)
            open(os.path.join(sys.argv[3], name), 'wb').write(files[name])
