# Texture extractor: every texture on the disc -> PNG, in tidy folders, named so the game can load edited copies.
#
#   python tools/texture_dump.py <disc files dir> <output dir>      (e.g. ..\Shadow\files play\textures\Original)
#
# Sources: RenderWare texture dictionaries (.txd, GameCube native, platform 6) - loose (UI layouts, fonts, menus) and
# inside "One" archives (.one, PRS-compressed: stages, characters, enemies, effects, cutscenes).
# File names: "<texture name> [tex1_<w>x<h>_<texhash>[_<tluthash>]_<fmt>].png". The bracket part is the replacement
# key Aurora computes at run time (Dolphin convention: XXH64 of the base level; for palette textures XXH64 of the
# palette entries the texture uses) - an edited copy placed anywhere under textures\Custom keeps working as long as
# that part of the name stays.
# Needs: numpy, Pillow, xxhash (pip install --user xxhash). Run without python -I (xxhash is in the user site).
import os, re, sys, struct, collections
import numpy as np
from PIL import Image
import xxhash

# ------------------------------------------------------------------------------------------------- PRS (Sega LZ)
def prs_decompress(src, start=0, expect=0):
    out = bytearray()
    i = start
    ctrl = 0
    bits = 0
    n = len(src)
    def bit():
        nonlocal ctrl, bits, i
        if bits == 0:
            ctrl = src[i]; i += 1; bits = 8
        b = ctrl & 1
        ctrl >>= 1
        bits -= 1
        return b
    while i < n:
        if bit():
            out.append(src[i]); i += 1
            continue
        if bit():
            v = src[i] | (src[i + 1] << 8); i += 2
            if v == 0:
                break
            off = (v >> 3) - 0x2000
            size = v & 7
            if size == 0:
                size = src[i] + 1; i += 1
            else:
                size += 2
        else:
            size = ((bit() << 1) | bit()) + 2
            off = src[i] - 0x100; i += 1
        p = len(out) + off
        if size <= -off:
            out += out[p:p + size]
        else:
            for k in range(size):
                out.append(out[p + k])
    return bytes(out)

# ------------------------------------------------------------------------------------------------- One archives
def one_entries(path):
    d = open(path, 'rb').read()
    if d[0xC:0x17] not in (b'One Ver 0.6', b'One Ver 0.5'):
        return
    count = struct.unpack_from('<I', d, 0x1C)[0]
    for k in range(count):
        e = 0xB0 + k * 0x38
        name = d[e:e + 0x2C].split(b'\0')[0].decode('ascii', 'replace')
        size, off, _ = struct.unpack_from('<III', d, e + 0x2C)
        yield name, d, off + 0xC, size

# ------------------------------------------------------------------------------------------------- RW chunks / TXD
def chunks(d, off, end):
    while off + 12 <= end:
        t, sz, ver = struct.unpack_from('<III', d, off)
        if sz > end - off - 12:
            return
        yield t, off + 12, sz
        off += 12 + sz

BLOCK = {0: (8, 8, 4), 1: (8, 4, 8), 2: (8, 4, 8), 3: (4, 4, 16), 4: (4, 4, 16), 5: (4, 4, 16), 6: (4, 4, 32),
         8: (8, 8, 4), 9: (8, 4, 8), 10: (4, 4, 16), 14: (8, 8, 4)}

def level_size(fmt, w, h):
    bw, bh, bpp = BLOCK[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bpp // 8

def native_texture(s):
    """RW texture-native struct data (GameCube, platform 6) -> (name, fmt, w, h, base level, palette, palette fmt)"""
    if len(s) < 0x6C or struct.unpack_from('>I', s, 0)[0] != 6:
        return None
    name = s[0x18:0x38].split(b'\0')[0].decode('ascii', 'replace')
    w, h = struct.unpack_from('>HH', s, 0x5C)
    bpp, mips, fmt, tlfmt = s[0x60:0x64]
    if fmt not in BLOCK or w == 0 or h == 0 or w > 1024 or h > 1024:
        return None
    p = 0x68
    pal = None
    if fmt in (8, 9):
        n = 16 if fmt == 8 else 256
        pal = s[p:p + n * 2]
        p += n * 2
    p += 4                                   # data size
    base = level_size(fmt, w, h)
    data = s[p:p + base]
    if len(data) < base:
        return None
    return name, fmt, w, h, data, pal, tlfmt

def natives_in(d):
    """every texture-native chunk (0x15) anywhere in a buffer: .txd dictionaries and textures embedded in other
    files (the UI layouts .gncp carry their own)"""
    i = d.find(b'\x15\x00\x00\x00')
    while i >= 0:
        if i + 24 <= len(d) and d[i + 8:i + 12] == d[i + 20:i + 24] and struct.unpack_from('<I', d, i + 12)[0] == 1:
            sz = struct.unpack_from('<I', d, i + 4)[0]
            ssz = struct.unpack_from('<I', d, i + 16)[0]
            if ssz <= sz and i + 24 + ssz <= len(d):
                t = native_texture(d[i + 24:i + 24 + ssz])
                if t:
                    yield t
                    i = d.find(b'\x15\x00\x00\x00', i + 24 + ssz)
                    continue
        i = d.find(b'\x15\x00\x00\x00', i + 1)

# ------------------------------------------------------------------------------------------------- GX decoding
def deblock(a, w, h, bw, bh):
    """a: per-texel array in GX block order (shape [n] or [n, c]) -> image [H, W, ...] cropped to w x h"""
    bx, by = (w + bw - 1) // bw, (h + bh - 1) // bh
    a = a.reshape((by, bx, bh, bw) + a.shape[1:])
    a = a.swapaxes(1, 2).reshape((by * bh, bx * bw) + a.shape[4:])
    return a[:h, :w]

def c565(v):
    r = ((v >> 11) & 31) * 255 // 31; g = ((v >> 5) & 63) * 255 // 63; b = (v & 31) * 255 // 31
    return np.stack([r, g, b, np.full_like(r, 255)], -1).astype(np.uint8)

def c5a3(v):
    opaque = (v & 0x8000) != 0
    r = np.where(opaque, ((v >> 10) & 31) * 255 // 31, ((v >> 8) & 15) * 17)
    g = np.where(opaque, ((v >> 5) & 31) * 255 // 31, ((v >> 4) & 15) * 17)
    b = np.where(opaque, (v & 31) * 255 // 31, (v & 15) * 17)
    a = np.where(opaque, 255, ((v >> 12) & 7) * 255 // 7)
    return np.stack([r, g, b, a], -1).astype(np.uint8)

def cia8(v):
    a = (v >> 8) & 0xFF; i = v & 0xFF
    return np.stack([i, i, i, a], -1).astype(np.uint8)

def palette_rgba(pal, tlfmt):
    v = np.frombuffer(pal, '>u2').astype(np.int32)
    return {0: cia8, 1: c565, 2: c5a3}.get(tlfmt, c5a3)(v)

def decode(fmt, w, h, data, pal=None, tlfmt=2):
    b = np.frombuffer(data, np.uint8)
    bw, bh, _ = BLOCK[fmt]
    if fmt in (0, 8):                                  # 4 bpp: I4 / C4
        idx = np.stack([b >> 4, b & 15], -1).reshape(-1)
        idx = deblock(idx, w, h, bw, bh)
        if fmt == 8:
            return palette_rgba(pal, tlfmt)[idx]
        i = (idx * 17).astype(np.uint8)
        return np.stack([i, i, i, i], -1)
    if fmt in (1, 9):                                  # 8 bpp: I8 / C8
        idx = deblock(b, w, h, bw, bh)
        if fmt == 9:
            return palette_rgba(pal, tlfmt)[idx]
        return np.stack([idx, idx, idx, idx], -1)
    if fmt == 2:                                       # IA4
        p = deblock(b, w, h, bw, bh)
        i = (p & 15) * 17; a = (p >> 4) * 17
        return np.stack([i, i, i, a], -1).astype(np.uint8)
    if fmt in (3, 4, 5):                               # IA8 / RGB565 / RGB5A3
        v = deblock(np.frombuffer(data, '>u2').astype(np.int32), w, h, bw, bh)
        return {3: cia8, 4: c565, 5: c5a3}[fmt](v)
    if fmt == 6:                                       # RGBA8: per 4x4 block 16 AR pairs then 16 GB pairs
        blk = b.reshape(-1, 2, 16, 2)
        ar, gb = blk[:, 0], blk[:, 1]
        px = np.stack([ar[..., 1], gb[..., 0], gb[..., 1], ar[..., 0]], -1).reshape(-1, 4)
        return deblock(px, w, h, 4, 4)
    if fmt == 14:                                      # CMPR: 8x8 blocks of four DXT1 4x4 sub-blocks
        sub = b.reshape(-1, 8)
        c0 = sub[:, 0].astype(np.int32) << 8 | sub[:, 1]; c1 = sub[:, 2].astype(np.int32) << 8 | sub[:, 3]
        p0, p1 = c565(c0).astype(np.int32), c565(c1).astype(np.int32)
        gt = (c0 > c1)[:, None]
        p2 = np.where(gt, (2 * p0 + p1) // 3, (p0 + p1) // 2)
        p3 = np.where(gt, (p0 + 2 * p1) // 3, 0)
        p2[:, 3] = 255
        p3[:, 3] = np.where(gt[:, 0], 255, 0)
        pals = np.stack([p0, p1, p2, p3], 1).astype(np.uint8)          # [nsub, 4, 4]
        bits = sub[:, 4:8]
        idx = np.stack([(bits >> s) & 3 for s in (6, 4, 2, 0)], -1).reshape(-1, 16)
        px = pals[np.arange(len(sub))[:, None], idx]                   # [nsub, 16, 4]
        # sub-blocks: per 8x8 block order TL, TR, BL, BR
        bx, by = (w + 7) // 8, (h + 7) // 8
        px = px.reshape(by, bx, 2, 2, 4, 4, 4)                         # by bx sy sx y x c
        px = px.transpose(0, 2, 4, 1, 3, 5, 6).reshape(by * 8, bx * 8, 4)
        return px[:h, :w]
    if fmt == 10:
        raise ValueError('C14X2 not supported')
    raise ValueError(f'format {fmt}')

def key_name(fmt, w, h, data, pal):
    th = xxhash.xxh64(data, seed=0).intdigest()
    if fmt in (8, 9):
        b = np.frombuffer(data, np.uint8)
        idx = np.concatenate([b >> 4, b & 15]) if fmt == 8 else b
        lo, hi = int(idx.min()), int(idx.max())
        tl = xxhash.xxh64(pal[2 * lo:2 * (hi + 1)], seed=0).intdigest()
        return f'tex1_{w}x{h}_{th:016x}_{tl:016x}_{fmt}'
    return f'tex1_{w}x{h}_{th:016x}_{fmt}'

# ------------------------------------------------------------------------------------------------- organisation
STAGES = {
    '0100': '01 Westopolis', '0200': '02 Digital Circuit', '0201': '02 Glyphic Canyon', '0202': '02 Lethal Highway',
    '0300': '03 Cryptic Castle', '0301': '03 Prison Island', '0302': '03 Circus Park',
    '0400': '04 Central City', '0401': '04 The Doom', '0402': '04 Sky Troops', '0403': '04 Mad Matrix',
    '0404': '04 Death Ruins',
    '0500': '05 The ARK', '0501': '05 Air Fleet', '0502': '05 Iron Jungle', '0503': '05 Space Gadget',
    '0504': '05 Lost Impact',
    '0600': '06 GUN Fortress', '0601': '06 Black Comet', '0602': '06 Lava Shelter', '0603': '06 Cosmic Fall',
    '0604': '06 Final Haunt',
    '0700': '07 The Last Way',
}
LANGS = {'EN': 'English', 'FR': 'French', 'GE': 'German', 'IT': 'Italian', 'JP': 'Japanese', 'SP': 'Spanish'}

def stage_folder(sid):
    if sid in STAGES:
        return f'Stages/{STAGES[sid]} (stg{sid})'
    g = sid[:2]
    if g in ('02', '03', '04', '05', '06', '07') and sid[2:] >= '10':
        return f'Stages/Boss and special stages/stg{sid}'
    if g == '09':
        return f'Stages/2P battle stages/stg{sid}'
    return f'Stages/Other (menus, tests, extra)/stg{sid}'

def lang_split(base):
    """'Options_EN' -> ('Options', 'English'); no language suffix -> (base, None)"""
    m = re.match(r'(.*?)_?(EN|FR|GE|IT|JP|SP)(\d\d)?$', base)
    if m and m.group(1):
        return m.group(1), LANGS[m.group(2)]
    return base, None

UI_NAMES = {'honban': 'Title screen and main menu', '2P_Result': '2P result screen', '2P_Start': '2P start screen',
            'BattleMode': 'Battle mode menu', 'GameOver': 'Game over', 'InfoWindow': 'Info windows and pop-ups',
            'Options': 'Options menu', 'RouteDisplay': 'Story route map', 'SelectMode': 'Select mode (stage select)',
            'Story_Library': 'Library (story, movies)', 'Synopsis': 'Synopsis screens', 'PalSel': 'PAL language select',
            'StafRoll': 'Staff roll', 'number': 'Numbers', 'FadeLogo': 'Logos', 'advertise': 'Advertisement',
            'Logo': 'Logos', 'E3RomTitle': 'Logos', 'Loading': 'Loading screens', 'Result': 'Stage results',
            'MissionFailed': 'Mission failed', 'MediaMenu': 'Library (story, movies)', 'Warning': 'Warnings'}
HUD = {'CommandWindow', '2P_CommandWindow', 'DarkGauge', 'HeroGauge', 'Hissatu_Dark', 'Hissatu_Hero', 'HPBar', 'Score',
       'TWindow', 'BbuttonWindow', 'VHArrow', 'BossGauge', 'EggBoss', '2P_Ring', '2P_Star'}
BOSS_PORTRAITS = {'BBull': 'Black Bull', 'BFalcon': 'Blue Falcon', 'Bdoom': 'Black Doom', 'DevDoom': 'Devil Doom',
                  'EBreaker': 'Egg Breaker', 'EDealer': 'Egg Dealer', 'HDog': 'Heavy Dog', 'SandD': 'Sonic and Diablon'}
SUBFOLDER = {'texcm': 'Level surfaces', 'gdt': 'Objects', 'fx': 'Effects'}

def stage_name(sid):
    return f'{STAGES[sid]} (stg{sid})' if sid in STAGES else f'stg{sid}'

def ui_folder(stem):
    base, lang = lang_split(stem)
    m = re.match(r'(StageTitle|pause)(\d{4}|Boss)$', base)
    if m:
        what = 'Stage title cards' if m.group(1) == 'StageTitle' else 'HUD and pause menu/Per stage'
        nice = f'{what}/{stage_name(m.group(2)) if m.group(2) != "Boss" else "Boss stages"}'
    elif base in HUD:
        nice = f'HUD (in stage)/{base}'
    elif base.startswith('BChara_'):
        nice = f'Character portraits (dialogue)/Bosses/{BOSS_PORTRAITS.get(base[7:], base[7:])}'
    elif base.lower().startswith('mchara'):
        who = base.split('_', 1)[1] if '_' in base else 'Shadow'
        nice = f'Character portraits (dialogue)/{who.capitalize()}'
    else:
        nice = UI_NAMES.get(base, base)
    if lang and lang != 'English':
        return f'UI/Menus and HUD/{nice}/Other languages/{lang}'
    return f'UI/Menus and HUD/{nice}'

def place(rel):
    """disc-relative path of a .txd / .gncp / .one -> output folder"""
    parts = rel.replace('\\', '/').split('/')
    top = parts[0]
    stem = os.path.splitext(parts[-1])[0]
    if top == 'csdFiles' or (len(parts) == 1 and stem in ('InfoWindow', 'number', 'FadeLogo', 'advertise')):
        return ui_folder(stem)
    if top == 'palsel':
        lang = LANGS.get(stem[-2:].upper(), stem[-2:])
        return 'UI/Menus and HUD/PAL language select' + ('' if lang == 'English' else f'/Other languages/{lang}')
    if top == 'stafroll':
        return 'UI/Menus and HUD/Staff roll'
    if top == 'fonts':
        sub = parts[1] if len(parts) > 2 else 'Common'
        base, lang = lang_split(stem)
        where = stage_name(sub[3:]) if sub.lower().startswith('stg') else sub
        lang = lang or 'Common'
        if lang != 'English':
            return f'UI/Fonts (subtitle and text glyphs)/Other languages/{lang}/{where}'
        return f'UI/Fonts (subtitle and text glyphs)/{where}'
    if top.startswith('stg') and len(top) == 7:
        return stage_folder(top[3:])
    if top == 'stg_cmn_gdt.one':
        return 'Objects (shared by all stages)'
    if top == 'character':
        return f'Characters/{stem}'
    if top == 'enemy':
        if len(parts) > 2 and parts[1] == 'boss':
            return f'Enemies/Bosses/{stem}'
        return f'Enemies/{stem}'
    if top == 'event':
        return f'Cutscenes (in-engine)/{stem}'
    if top == 'common':
        return f'Effects and common/{stem}'
    return f'Other/{stem}'

def sub_name(folder, entry):
    """sub-folder for a texture dictionary inside an archive (None = put the textures in the folder itself)"""
    e = re.sub(r'^stg\d{4}_', '', entry.lower())
    if e in SUBFOLDER:
        return SUBFOLDER[e]
    if e in ('texture', 'stg_gdt') or folder.split('/')[-1].lower().startswith(e) or folder.startswith('UI/') \
            or folder.startswith('Objects'):
        return None
    return e

# ------------------------------------------------------------------------------------------------- main
def main():
    src, out = sys.argv[1], sys.argv[2]
    stats = collections.Counter()
    recs = []                     # (folder, name, texture tuple, key)

    def collect(folder, texs):
        for t in texs:
            name, fmt, w, h, data, pal, tlfmt = t
            try:
                key = key_name(fmt, w, h, data, pal)
            except Exception:
                stats['key_fail'] += 1
                continue
            recs.append((folder, name, t, key))

    for root, dirs, files in os.walk(src):
        dirs.sort()
        for fn in sorted(files):
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, src)
            ext = fn.lower().rsplit('.', 1)[-1]
            if ext in ('txd', 'gncp'):
                collect(place(rel), natives_in(open(p, 'rb').read()))
                stats[ext] += 1
            elif ext == 'one':
                seen_entries = set()
                for name, d, off, size in one_entries(p):
                    if (off, size) in seen_entries:      # some archives list the same entry several times
                        continue
                    seen_entries.add((off, size))
                    try:
                        raw = prs_decompress(d, off, size)
                    except Exception:
                        raw = b''
                    if b'\x15\x00\x00\x00' not in raw:
                        raw2 = d[off:off + size]           # stored uncompressed?
                        if b'\x15\x00\x00\x00' not in raw2:
                            continue
                        raw = raw2
                    folder = place(rel)
                    sub = sub_name(folder, os.path.splitext(name)[0])
                    collect(f'{folder}/{sub}' if sub else folder, natives_in(raw))
                    stats['one_entries'] += 1
                print(f'{rel}: {len(recs)} textures so far', flush=True)

    # HUD / pause menu: textures several stages share go into one folder, per-stage copies only for what differs
    per_stage = collections.defaultdict(set)
    for folder, name, t, key in recs:
        if '/HUD and pause menu/Per stage/' in folder:
            per_stage[key].add(folder)
    final = []
    for folder, name, t, key in recs:
        if '/HUD and pause menu/Per stage/' in folder and len(per_stage[key]) > 1:
            lang = folder[folder.index('/Other languages/'):] if '/Other languages/' in folder else ''
            folder = folder.split('/Per stage/')[0] + lang
        final.append((folder, name, t, key))

    os.makedirs(out, exist_ok=True)
    index = open(os.path.join(out, 'index.txt'), 'w', encoding='utf-8')
    written = set()
    for folder, name, t, key in final:
        if (folder, key) in written:
            stats['dup'] += 1
            continue
        written.add((folder, key))
        n, fmt, w, h, data, pal, tlfmt = t
        try:
            rgba = decode(fmt, w, h, data, pal, tlfmt)
        except Exception:
            stats['decode_fail'] += 1
            continue
        safe = re.sub(r'[<>:"/\\|?*]', '_', name) or 'unnamed'
        path = os.path.join(out, folder, f'{safe} [{key}].png')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        Image.fromarray(np.ascontiguousarray(rgba), 'RGBA').save(path)
        index.write(f'{key}\t{folder}/{safe}\n')
        stats['png'] += 1
    index.close()
    print(dict(stats))

if __name__ == '__main__':
    main()
