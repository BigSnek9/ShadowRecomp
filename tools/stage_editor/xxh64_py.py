"""Pure-Python XXH64 (the xxhash spec, seed support), API-compatible with xxhash.xxh64(data, seed).intdigest() /
hexdigest(). Used by server.texture_decoder when the C 'xxhash' package cannot be imported (#80: it was installed in a
user site the editor's Explorer-launched process does not see). Slower than the C module (about 1 s per 10 MB), only
the #44 texture replacement keys (texture_dump.key_name) use it. Checked against xxhash 4.0.1 (test at the bottom)."""
import struct

M = 0xFFFFFFFFFFFFFFFF
P1, P2, P3, P4, P5 = 11400714785074694791, 14029467366897019727, 1609587929392839161, 9650029242287828579, 2870177450012600261

def _rotl(x, r): return ((x << r) | (x >> (64 - r))) & M
def _round(acc, lane): return (_rotl((acc + lane * P2) & M, 31) * P1) & M
def _merge(acc, val): return ((acc ^ _round(0, val)) * P1 + P4) & M

def xxh64_int(data, seed=0):
    data = bytes(data); n = len(data); i = 0
    if n >= 32:
        v1 = (seed + P1 + P2) & M; v2 = (seed + P2) & M; v3 = seed & M; v4 = (seed - P1) & M
        stripes = n // 32; words = struct.unpack_from(f'<{stripes * 4}Q', data, 0)
        for s in range(0, stripes * 4, 4):
            v1 = _round(v1, words[s]); v2 = _round(v2, words[s + 1]); v3 = _round(v3, words[s + 2]); v4 = _round(v4, words[s + 3])
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & M
        h = _merge(h, v1); h = _merge(h, v2); h = _merge(h, v3); h = _merge(h, v4); i = stripes * 32
    else:
        h = (seed + P5) & M
    h = (h + n) & M
    while i + 8 <= n:
        h = (_rotl(h ^ _round(0, struct.unpack_from('<Q', data, i)[0]), 27) * P1 + P4) & M; i += 8
    if i + 4 <= n:
        h = (_rotl(h ^ ((struct.unpack_from('<I', data, i)[0] * P1) & M), 23) * P2 + P3) & M; i += 4
    while i < n:
        h = (_rotl(h ^ ((data[i] * P5) & M), 11) * P1) & M; i += 1
    h ^= h >> 33; h = (h * P2) & M; h ^= h >> 29; h = (h * P3) & M; h ^= h >> 32
    return h

class xxh64:
    def __init__(self, data=b'', seed=0): self._d = bytearray(data); self._s = seed
    def update(self, data): self._d += data
    def intdigest(self): return xxh64_int(self._d, self._s)
    def hexdigest(self): return f'{self.intdigest():016x}'

if __name__ == '__main__':   # python tools/stage_editor/xxh64_py.py  (needs the real xxhash to compare)
    import os, time, xxhash
    for n in list(range(0, 80)) + [1000, 4096, 65537, 1 << 20]:
        for seed in (0, 1, 0xDEADBEEF):
            b = os.urandom(n); assert xxh64(b, seed).intdigest() == xxhash.xxh64(b, seed=seed).intdigest(), (n, seed)
    b = os.urandom(10 << 20); t = time.time(); xxh64_int(b); print('ok; 10 MB in', round(time.time() - t, 2), 's')
