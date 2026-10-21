"""Game import: check the player's own Shadow the Hedgehog disc and unpack it for the runtime.

The repo ships no game files. The player picks their GameCube copy (ISO / GCM image, a Dolphin-compressed RVZ / WIA /
GCZ image, or an extracted folder); this checks it is the USA disc (GUPE8P rev 0) file by file against gupe8p.json
(SHA-1 hashes only) and writes it as <dest>\\sys\\* + <dest>\\files\\* - the layout runtime/main.cpp find_disc() reads.
Standard library only, so the launcher exe (PyInstaller) can bundle it. Plan: docs/release/GAME-IMPORT.md.

Library:  identify(path) -> Info      import_disc(src, dest, progress=None) -> Result      verify_folder(path) -> Result
Command:  gameimport.py identify <disc or folder>
          gameimport.py import <disc or folder> <dest folder>      (dest is normally <game folder>\\Shadow)
          gameimport.py verify <extracted folder>
          add --json for machine-readable output (the launcher)
"""
import hashlib, json, os, shutil, struct, subprocess, sys, tempfile, time
from dataclasses import dataclass, field, asdict

HERE = os.path.dirname(os.path.abspath(__file__))
MANIFEST = os.path.join(getattr(sys, '_MEIPASS', HERE), 'gupe8p.json')   # PyInstaller: bundle it next to this file
GAME_ID = 'GUPE8P'
REVISION = 0
DOL_SHA1 = '118ef49ebd45b371b6f514e0e1b6d1ba7cf12904'
REGIONS = {'E': 'USA', 'P': 'European (PAL)', 'J': 'Japanese'}
CHUNK = 1 << 20
SYS = ('sys/boot.bin', 'sys/bi2.bin', 'sys/apploader.img', 'sys/main.dol', 'sys/fst.bin')


@dataclass
class Info:
    ok: bool                 # the right game and a format we can read
    kind: str                # iso | folder | gcrebuilder | rvz | wia | gcz | ciso | wbfs | nkit | unknown | missing
    game_id: str = ''
    revision: int = -1
    title: str = ''
    message: str = ''        # one plain sentence for the player


@dataclass
class Result:
    ok: bool
    message: str
    checked: int = 0
    bad: list = field(default_factory=list)       # files whose size / hash differ
    missing: list = field(default_factory=list)   # files the disc should have
    dest: str = ''


def load_manifest():
    with open(MANIFEST, encoding='utf-8') as f:
        return json.load(f)['files']


# --- reading the header ----------------------------------------------------------------------------------------------

def _magic_kind(head):
    if head[:4] == b'RVZ\x01': return 'rvz'
    if head[:4] == b'WIA\x01': return 'wia'
    if head[:4] == b'\x01\xc0\x0b\xb1': return 'gcz'
    if head[:4] == b'CISO': return 'ciso'
    if head[:4] == b'WBFS': return 'wbfs'
    return ''


def _from_boot(kind, boot):
    if len(boot) < 0x440 or boot[0x1C:0x20] != b'\xc2\x33\x9f\x3d':
        return Info(False, 'unknown', message="This isn't a GameCube disc image.")
    gid = boot[:6].decode('ascii', 'replace')
    rev = boot[7]
    title = boot[0x20:0x60].split(b'\0')[0].decode('ascii', 'replace').strip()
    info = Info(False, kind, gid, rev, title)
    if boot[0x200:0x204] == b'NKIT':
        info.kind = 'nkit'
        info.message = 'This is a trimmed NKit image (files are missing). Restore it to a full ISO with NKit first.'
    elif gid[:3] != 'GUP' or gid[4:] != '8P':
        info.message = f"This isn't Shadow the Hedgehog (it's {title or 'another game'}, {gid})."
    elif gid != GAME_ID:
        info.message = (f'This is the {REGIONS.get(gid[3], gid[3])} version ({gid}). '
                        'Only the USA version (GUPE8P) is supported for now.')
    elif rev != REVISION:
        info.message = f'This is USA revision {rev}. Only the first release (revision {REVISION}) is supported for now.'
    else:
        info.ok = True
        info.message = f'Shadow the Hedgehog, USA ({gid}) - OK.'
    return info


def _folder_layout(path):
    """(kind, boot.bin bytes or None) for an extracted folder."""
    p = os.path.join(path, 'sys', 'boot.bin')
    if os.path.isfile(p):
        return 'folder', open(p, 'rb').read(0x440)
    if os.path.isfile(os.path.join(path, 'DATA', 'sys', 'boot.bin')):   # some tools nest it one level down
        return 'folder', open(os.path.join(path, 'DATA', 'sys', 'boot.bin'), 'rb').read(0x440)
    for name in os.listdir(path):   # GCRebuilder: &&SystemData\ISO.hdr (= boot.bin + bi2.bin) next to root\
        if name.lower() == '&&systemdata' and os.path.isfile(os.path.join(path, name, 'ISO.hdr')):
            return 'gcrebuilder', open(os.path.join(path, name, 'ISO.hdr'), 'rb').read(0x440)
    return 'folder', None


def identify(path):
    """What the player picked, without reading the whole disc."""
    if not os.path.exists(path):
        return Info(False, 'missing', message='That file or folder does not exist.')
    if os.path.isdir(path):
        kind, boot = _folder_layout(path)
        if boot is None:
            return Info(False, 'folder', message="This folder isn't an extracted GameCube disc (no sys\\boot.bin).")
        return _from_boot(kind, boot)
    with open(path, 'rb') as f:
        head = f.read(0x440)
    kind = _magic_kind(head)
    if kind:
        if kind in ('rvz', 'wia', 'gcz') and not find_dolphin_tool():
            return Info(False, kind, message=f'This is a compressed {kind.upper()} image. Install Dolphin, or in '
                        'Dolphin right-click the game > Convert File... > Format: ISO, then choose the ISO.')
        if kind in ('ciso', 'wbfs'):
            return Info(False, kind, message=f'{kind.upper()} images are not supported. Convert it to ISO in Dolphin '
                        '(right-click the game > Convert File... > Format: ISO).')
        boot = _dolphin_header(path)
        if not boot:   # header unreadable: the full check runs on the converted ISO
            return Info(True, kind, message=f'Compressed {kind.upper()} image: it is checked after Dolphin converts it.')
        info = _from_boot(kind, boot)
        info.kind = kind
        return info
    return _from_boot('iso', head)


# --- Dolphin's compressed formats: DolphinTool makes a temporary ISO -------------------------------------------------

def find_dolphin_tool():
    cands = [os.environ.get('SHADOW_DOLPHINTOOL', ''), shutil.which('DolphinTool') or '']
    for base in (os.environ.get('ProgramFiles', r'C:\Program Files'), os.environ.get('ProgramFiles(x86)', ''),
                 os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Programs')):
        if base:
            cands += [os.path.join(base, 'Dolphin', 'DolphinTool.exe'), os.path.join(base, 'Dolphin-x64', 'DolphinTool.exe')]
    return next((c for c in cands if c and os.path.isfile(c)), '')


def _dolphin_header(path):
    """boot.bin of a compressed image via 'DolphinTool header' (game id + revision); empty on failure."""
    tool = find_dolphin_tool()
    try:
        out = subprocess.run([tool, 'header', '-i', path, '-j'], capture_output=True, text=True, timeout=60,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout
        j = json.loads(out)
        gid = (j.get('game_id') or j.get('gameid') or '').encode('ascii')[:6].ljust(6, b'\0')
        rev = int(j.get('revision', 0))
        title = (j.get('internal_name') or '').encode('ascii', 'replace')[:0x40]
        boot = bytearray(0x440)
        boot[:6] = gid
        boot[7] = rev
        boot[0x1C:0x20] = b'\xc2\x33\x9f\x3d'
        boot[0x20:0x20 + len(title)] = title
        return bytes(boot)
    except Exception:
        return b''


def _to_iso(path, tmpdir, progress):
    out = os.path.join(tmpdir, 'disc.iso')
    if progress: progress(0, 1, 'Converting the compressed image with Dolphin...')
    r = subprocess.run([find_dolphin_tool(), 'convert', '-i', path, '-o', out, '-f', 'iso'], capture_output=True,
                       text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if r.returncode != 0 or not os.path.isfile(out):
        raise RuntimeError('Dolphin could not convert the image: ' + (r.stderr or r.stdout).strip()[-300:])
    return out


# --- disc image layout ----------------------------------------------------------------------------------------------

def _be32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def _dol_size(dol_head):
    end = 0x100
    for i in range(18):
        off, size = _be32(dol_head, i * 4), _be32(dol_head, 0x90 + i * 4)
        if size:
            end = max(end, off + size)
    return end


def _iso_entries(f):
    """[(path, disc offset, size)] for every file of an ISO / GCM, sys/ first - the same files Dolphin extracts."""
    f.seek(0)
    boot = f.read(0x440)
    dol_off, fst_off, fst_size = _be32(boot, 0x420), _be32(boot, 0x424), _be32(boot, 0x428)
    f.seek(0x2440)
    al = f.read(0x20)
    f.seek(dol_off)
    dol_head = f.read(0x100)
    f.seek(fst_off)
    fst = f.read(fst_size)
    out = [('sys/boot.bin', 0, 0x440), ('sys/bi2.bin', 0x440, 0x2000),
           ('sys/apploader.img', 0x2440, 0x20 + _be32(al, 0x14) + _be32(al, 0x18)),
           ('sys/main.dol', dol_off, _dol_size(dol_head)), ('sys/fst.bin', fst_off, fst_size)]
    count = _be32(fst, 8)
    strings = 12 * count

    def name(i):
        o = strings + (_be32(fst, i * 12) & 0xFFFFFF)
        return fst[o:fst.index(b'\0', o)].decode('shift_jis', 'replace')

    stack = [(count, '')]   # (end index, path prefix) of the open directories
    for i in range(1, count):
        while stack and i >= stack[-1][0]:
            stack.pop()
        prefix = stack[-1][1] if stack else ''
        flags = fst[i * 12]
        if flags:   # directory: next index = end of its subtree
            stack.append((_be32(fst, i * 12 + 8), prefix + name(i) + '/'))
        else:
            out.append(('files/' + prefix + name(i), _be32(fst, i * 12 + 4), _be32(fst, i * 12 + 8)))
    return out


def _folder_entries(path, kind):
    """[(relative path in the standard layout, source path)] of an extracted folder."""
    if kind == 'gcrebuilder':
        sysdir = next(os.path.join(path, n) for n in os.listdir(path) if n.lower() == '&&systemdata')
        root = os.path.join(path, 'root')
        sysmap = {'sys/main.dol': 'Start.dol', 'sys/apploader.img': 'AppLoader.ldr', 'sys/fst.bin': 'Game.toc'}
        out = [(k, os.path.join(sysdir, v)) for k, v in sysmap.items()]
        out += [('sys/boot.bin', (os.path.join(sysdir, 'ISO.hdr'), 0, 0x440)),
                ('sys/bi2.bin', (os.path.join(sysdir, 'ISO.hdr'), 0x440, 0x2000))]
    else:
        if not os.path.isfile(os.path.join(path, 'sys', 'boot.bin')):
            path = os.path.join(path, 'DATA')
        root = os.path.join(path, 'files')
        out = [(s, os.path.join(path, *s.split('/'))) for s in SYS]
    for d, _, fs in os.walk(root):
        for n in fs:
            p = os.path.join(d, n)
            out.append(('files/' + os.path.relpath(p, root).replace(os.sep, '/'), p))
    return out


# --- copy + check ---------------------------------------------------------------------------------------------------

def _copy_hash(read_chunks, out_path):
    h = hashlib.sha1()
    n = 0
    with (open(out_path, 'wb') if out_path else _Null()) as w:
        for b in read_chunks:
            h.update(b)
            w.write(b)
            n += len(b)
    return n, h.hexdigest()


class _Null:
    def __enter__(self): return self
    def __exit__(self, *a): return False
    def write(self, b): pass


def _file_chunks(src):
    if isinstance(src, tuple):   # (file, offset, size): a slice of a file (GCRebuilder ISO.hdr)
        p, off, size = src
        with open(p, 'rb') as f:
            f.seek(off)
            yield f.read(size)
        return
    with open(src, 'rb') as f:
        for b in iter(lambda: f.read(CHUNK), b''):
            yield b


def _iso_chunks(f, off, size):
    f.seek(off)
    while size > 0:
        b = f.read(min(CHUNK, size))
        if not b:
            raise RuntimeError('The disc image ends early (it is cut short or damaged).')
        size -= len(b)
        yield b


def _run(src, dest, progress, write):
    """Check (and with write, unpack) src. dest: final folder (written via dest + '.importing', renamed at the end)."""
    info = identify(src)
    if not info.ok:
        return Result(False, info.message)
    want = load_manifest()
    total = sum(v[0] for v in want.values())
    done = 0
    res = Result(False, '', dest=dest if write else '')
    tmp_iso_dir = None
    work = dest + '.importing' if write else ''
    try:
        if info.kind in ('rvz', 'wia', 'gcz'):
            tmp_iso_dir = tempfile.mkdtemp(prefix='shadow-import-', dir=os.path.dirname(os.path.abspath(dest)) if write else None)
            src = _to_iso(src, tmp_iso_dir, progress)
            info = identify(src)
            if not info.ok:
                return Result(False, info.message)
        if write:
            if os.path.exists(work):
                shutil.rmtree(work)
            free = shutil.disk_usage(os.path.dirname(os.path.abspath(dest))).free
            if free < total + (64 << 20):
                return Result(False, f'Not enough free space: the game needs {total / 2**30:.1f} GB, '
                              f'{free / 2**30:.1f} GB is free on that drive.')
        f = open(src, 'rb') if info.kind == 'iso' else None
        try:
            if f:
                entries = [(rel, ('iso', off, size)) for rel, off, size in _iso_entries(f)]
            else:
                entries = _folder_entries(src, info.kind)
            have = {rel: s for rel, s in entries}
            for rel in sorted(want):
                size, sha = want[rel]
                s = have.get(rel)
                if s is None:
                    res.missing.append(rel)
                    continue
                out = os.path.join(work, *rel.split('/')) if write else ''
                if out:
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                if progress:
                    progress(done, total, rel)
                chunks = _iso_chunks(f, s[1], s[2]) if isinstance(s, tuple) and s[0] == 'iso' else _file_chunks(s)
                n, got = _copy_hash(chunks, out)
                res.checked += 1
                done += size
                if n != size or got != sha:
                    res.bad.append(rel)
        finally:
            if f:
                f.close()
        if progress:
            progress(total, total, '')
        if res.bad or res.missing:
            bits = []
            if res.bad: bits.append(f'{len(res.bad)} file(s) do not match a clean disc (first: {res.bad[0]})')
            if res.missing: bits.append(f'{len(res.missing)} file(s) are missing (first: {res.missing[0]})')
            res.message = ('This copy is damaged or modified: ' + '; '.join(bits) +
                           '. Make a fresh dump of your disc and try again.')
            if write:
                shutil.rmtree(work, ignore_errors=True)
            return res
        if write:
            with open(os.path.join(work, 'import.json'), 'w', encoding='utf-8') as j:
                json.dump({'game_id': GAME_ID, 'revision': REVISION, 'source': os.path.abspath(src) if not tmp_iso_dir else 'converted image',
                           'format': info.kind, 'files': res.checked, 'bytes': total,
                           'imported': time.strftime('%Y-%m-%d %H:%M:%S'), 'tool': 'tools/game_import/gameimport.py'}, j, indent=1)
            if os.path.exists(dest):
                old = dest + '.old'
                shutil.rmtree(old, ignore_errors=True)
                os.replace(dest, old)
                os.replace(work, dest)
                shutil.rmtree(old, ignore_errors=True)
            else:
                os.replace(work, dest)
        res.ok = True
        res.message = (f'Shadow the Hedgehog (USA) checked: all {res.checked} files match' +
                       (f'. Unpacked to {dest}.' if write else '.'))
        return res
    except Exception as e:
        if write and work:
            shutil.rmtree(work, ignore_errors=True)
        return Result(False, f'Import failed: {e}')
    finally:
        if tmp_iso_dir:
            shutil.rmtree(tmp_iso_dir, ignore_errors=True)


def import_disc(src, dest, progress=None):
    """Check src and unpack it to dest (sys\\ + files\\ + import.json). progress(done_bytes, total_bytes, file)."""
    return _run(src, os.path.abspath(dest), progress, True)


def verify_folder(path, progress=None):
    """Check an extracted folder (or image) in place, writing nothing."""
    return _run(path, '', progress, False)


def is_imported(dest):
    """True when dest holds a finished import (the launcher's first-run test)."""
    return os.path.isfile(os.path.join(dest, 'import.json')) and os.path.isfile(os.path.join(dest, 'sys', 'main.dol'))


def main(argv):
    as_json = '--json' in argv
    args = [a for a in argv if a != '--json']
    if len(args) < 2 or args[0] not in ('identify', 'import', 'verify') or (args[0] == 'import' and len(args) < 3):
        print(__doc__)
        return 2
    last = [0.0]

    def progress(done, total, name):
        if as_json or time.time() - last[0] < 0.5:
            return
        last[0] = time.time()
        print(f'\r{100 * done / max(total, 1):5.1f}%  {name[:60]:<60}', end='', file=sys.stderr, flush=True)

    if args[0] == 'identify':
        r = identify(args[1])
    elif args[0] == 'verify':
        r = verify_folder(args[1], progress)
    else:
        r = import_disc(args[1], args[2], progress)
    if not as_json and args[0] != 'identify':
        print(file=sys.stderr)
    print(json.dumps(asdict(r), indent=1) if as_json else r.message)
    return 0 if r.ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
