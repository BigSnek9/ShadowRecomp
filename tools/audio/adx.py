"""CRI ADX (type 3, 4-bit, version 4 header with a loop) encoder / decoder for the Stage Workshop (#100 custom music,
2026-10-19). Matches the game's stage songs (files/sng_stgNNNN.adx: 48000 Hz stereo, high-pass 500, header v4,
loop block at 0x20..0x37, '(c)CRI' before the data, file padded to 2048 bytes).

    python tools/audio/adx.py encode in.wav out.adx [--rate 48000] [--no-loop] [--loop-start SECONDS]
    python tools/audio/adx.py decode in.adx out.wav
    python tools/audio/adx.py tone out.wav [--seconds 20]       (a test melody for game runs)

Input: WAV (PCM 8 / 16 / 24 / 32 bit, any rate, mono or stereo); it is resampled to 48000 Hz stereo. Other songs (MP3,
M4A / AAC, FLAC, WMA) are decoded by Windows' own Media Foundation (mf_decode.ps1 through Windows PowerShell, nothing to
install); anything Windows cannot read (e.g. OGG on a PC without the Web Media Extensions) by the portable ffmpeg in
tools/audio/ffmpeg/ when that folder is there. Neither needs PATH."""
import math, os, struct, subprocess, sys, tempfile, wave
from pathlib import Path
import numpy as np

RATE = 48000
FRAME = 32          # samples per 18-byte frame


def coefs(rate, cutoff=500):
    a = math.sqrt(2) - math.cos(2 * math.pi * cutoff / rate); b = math.sqrt(2) - 1
    c = (a - math.sqrt((a + b) * (a - b))) / b
    return int(c * 8192), int(c * c * -4096)


def read_wav(path):
    with wave.open(str(path), 'rb') as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 1: x = (np.frombuffer(raw, np.uint8).astype(np.float64) - 128) / 128
    elif width == 2: x = np.frombuffer(raw, '<i2').astype(np.float64) / 32768
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32); v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16); v[v >= 1 << 23] -= 1 << 24; x = v / float(1 << 23)
    elif width == 4: x = np.frombuffer(raw, '<i4').astype(np.float64) / 2 ** 31
    else: raise ValueError(f'{path}: {width * 8}-bit WAV not supported')
    return x.reshape(-1, ch), rate


def resample(x, rate, to=RATE):
    if rate == to: return x
    n = int(round(len(x) * to / rate)); t = np.arange(n) * (rate / to)
    return np.stack([np.interp(t, np.arange(len(x)), x[:, c]) for c in range(x.shape[1])], 1)


def encode(pcm, rate=RATE, loop=True, loop_start=0):
    """pcm: float array (n, ch) in -1..1 (ch 1 or 2) -> ADX bytes (stereo if ch 2)"""
    x = np.clip(np.round(np.asarray(pcm, np.float64) * 32767), -32768, 32767).astype(np.int64)
    n, ch = x.shape; c1, c2 = coefs(rate); frames = -(-n // FRAME)
    x = np.vstack([x, np.zeros((frames * FRAME - n, ch), np.int64)])
    out = bytearray(frames * 18 * ch)
    for c in range(ch):
        h1 = h2 = 0; s = x[:, c].tolist()
        for f in range(frames):
            blk = s[f * FRAME:(f + 1) * FRAME]
            # open-loop estimate of the residual range -> scale, then quantise closed-loop (decoder history)
            e1, e2, mx = h1, h2, 0
            for v in blk:
                p = (c1 * e1 + c2 * e2) >> 12; r = abs(v - p); mx = r if r > mx else mx; e2, e1 = e1, v
            scale = max(1, -(-mx // 7))
            for attempt in range(3):
                g1, g2, nib, worst = h1, h2, [], 0
                for v in blk:
                    p = (c1 * g1 + c2 * g2) >> 12; q = int(round((v - p) / scale)); q = 7 if q > 7 else -8 if q < -8 else q
                    d = q * scale + p; d = 32767 if d > 32767 else -32768 if d < -32768 else d
                    worst = max(worst, abs(v - d)); nib.append(q & 15); g2, g1 = g1, d
                if worst <= scale * 1.5 or attempt == 2: break
                scale = int(scale * 1.6) + 1
            h1, h2 = g1, g2
            o = (f * ch + c) * 18; struct.pack_into('>H', out, o, min(scale, 0x7FFF) - 0)
            for k in range(16): out[o + 2 + k] = (nib[2 * k] << 4) | nib[2 * k + 1]
    head = 0x800 - 4                          # data at 0x800 like the game's songs (copyright offset field + 4)
    hdr = bytearray(head + 4)
    struct.pack_into('>HH4BIIHBB', hdr, 0, 0x8000, head, 3, 18, 4, ch, rate, n, 500, 4, 0)
    if loop:
        ls = int(loop_start) // FRAME * FRAME
        struct.pack_into('>IIIIII', hdr, 0x20, 1, 1, ls, head + 4 + ls // FRAME * 18 * ch, n, head + 4 + frames * 18 * ch)
    hdr[head - 2:head + 4] = b'(c)CRI'
    data = bytes(hdr) + bytes(out) + struct.pack('>H', 0x8001) + bytes(18 * ch - 2)
    return data + bytes(-len(data) % 2048)


def decode(data, max_seconds=None):
    if data[:2] != b'\x80\x00': raise ValueError('not an ADX file')
    off = struct.unpack_from('>H', data, 2)[0] + 4; ch, rate, n = data[7], struct.unpack_from('>I', data, 8)[0], struct.unpack_from('>I', data, 12)[0]
    c1, c2 = coefs(rate, struct.unpack_from('>H', data, 16)[0])
    if max_seconds: n = min(n, int(max_seconds * rate))
    frames = -(-n // FRAME); out = np.zeros((frames * FRAME, ch), np.int64)
    for c in range(ch):
        h1 = h2 = 0; col = []
        for f in range(frames):
            o = off + (f * ch + c) * 18; scale = struct.unpack_from('>H', data, o)[0]
            if scale & 0x8000: break
            for k in range(16):
                b = data[o + 2 + k]
                for q in (b >> 4, b & 15):
                    q = q - 16 if q > 7 else q; d = q * scale + ((c1 * h1 + c2 * h2) >> 12); d = 32767 if d > 32767 else -32768 if d < -32768 else d
                    col.append(d); h2, h1 = h1, d
        out[:len(col), c] = col
    return out[:n].astype(np.float64) / 32768, rate


def loop_info(data):
    return dict(zip(('count', 'enabled', 'startSample', 'startByte', 'endSample', 'endByte'), struct.unpack_from('>IIIIII', data, 0x20))) if data[18] == 4 else None


def wav_to_adx(path, loop=True, loop_start_s=0.0):
    x, rate = read_wav(path)
    x = resample(x, rate)
    if x.shape[1] == 1: x = np.repeat(x, 2, 1)
    return encode(x[:, :2], RATE, loop, int(loop_start_s * RATE))


HERE = Path(__file__).resolve().parent
FFMPEG = HERE / 'ffmpeg' / 'ffmpeg.exe'   # optional portable LGPL build (see tools/audio/ffmpeg/README.md)


def _run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return r.returncode, (r.stderr or r.stdout).strip()


def song_to_wav(src, dst):
    """any song -> a 16-bit PCM WAV at dst; returns the decoder's name. Windows' Media Foundation first, then ffmpeg."""
    src, dst = Path(src), Path(dst); why = []
    if os.name == 'nt' and not os.environ.get('SONG_NO_MF'):   # SONG_NO_MF=1: test the ffmpeg path
        ps = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
        code, msg = _run([str(ps), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(HERE / 'mf_decode.ps1'), str(src), str(dst)])
        if code == 0 and dst.exists() and dst.stat().st_size > 44: return 'Windows Media Foundation'
        why.append(msg or f'exit {code}')
    if FFMPEG.exists():
        code, msg = _run([str(FFMPEG), '-nostdin', '-hide_banner', '-loglevel', 'error', '-y', '-i', str(src), '-vn', '-acodec', 'pcm_s16le', '-ar', str(RATE), str(dst)])
        if code == 0 and dst.exists(): return 'ffmpeg'
        why.append(msg.splitlines()[-1] if msg else f'ffmpeg exit {code}')
    else: why.append('no portable ffmpeg in tools/audio/ffmpeg')
    raise ValueError(f'{src.name}: could not decode this song ({"; ".join(why)})')


def is_wav(path):
    with open(path, 'rb') as f: h = f.read(12)
    return h[:4] == b'RIFF' and h[8:12] == b'WAVE'


def song_to_adx(path, loop=True, loop_start_s=0.0):
    """WAV / MP3 / M4A / FLAC / WMA / OGG ... -> ADX bytes (and the decoder used)"""
    if is_wav(path): return wav_to_adx(path, loop, loop_start_s), 'WAV'
    with tempfile.TemporaryDirectory() as d:
        w = Path(d) / 'song.wav'; how = song_to_wav(path, w)
        return wav_to_adx(w, loop, loop_start_s), how


def tone(seconds=20):
    """a test melody: 440 / 554 / 659 Hz notes (A major), 0.5 s each, stereo"""
    t = np.arange(int(seconds * RATE)) / RATE; f = np.array([440.0, 554.37, 659.25])[(t * 2).astype(int) % 3]
    y = 0.35 * np.sin(2 * np.pi * np.cumsum(f) / RATE)
    return np.stack([y, y], 1)


def write_wav(path, x, rate=RATE):
    with wave.open(path if hasattr(path, 'write') else str(path), 'wb') as w:
        w.setnchannels(x.shape[1]); w.setsampwidth(2); w.setframerate(rate); w.writeframes((np.clip(x, -1, 1) * 32767).astype('<i2').tobytes())


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'encode': data, how = song_to_adx(a[1], '--no-loop' not in a, float(a[a.index('--loop-start') + 1]) if '--loop-start' in a else 0.0); Path(a[2]).write_bytes(data); print(f'{a[2]}: {len(data)} bytes (decoded by {how})')
    elif a[0] == 'decode': x, r = decode(Path(a[1]).read_bytes()); write_wav(a[2], x, r)
    elif a[0] == 'tone': write_wav(a[1], tone(float(a[a.index('--seconds') + 1]) if '--seconds' in a else 20))
