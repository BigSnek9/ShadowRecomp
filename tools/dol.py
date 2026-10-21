"""GameCube DOL loader."""
import struct

class Section:
    def __init__(self, idx, kind, off, addr, size, data):
        self.idx, self.kind, self.off, self.addr, self.size, self.data = idx, kind, off, addr, size, data
    @property
    def end(self): return self.addr + self.size

class Dol:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        self.raw = raw
        offs = struct.unpack('>18I', raw[0x00:0x48])
        addrs = struct.unpack('>18I', raw[0x48:0x90])
        sizes = struct.unpack('>18I', raw[0x90:0xD8])
        self.bss_addr, self.bss_size, self.entry = struct.unpack('>3I', raw[0xD8:0xE4])
        self.text, self.data = [], []
        for i in range(18):
            if sizes[i] == 0: continue
            s = Section(i, 'text' if i < 7 else 'data', offs[i], addrs[i], sizes[i],
                        raw[offs[i]:offs[i] + sizes[i]])
            (self.text if i < 7 else self.data).append(s)
        self.sections = self.text + self.data

    def find(self, addr):
        for s in self.sections:
            if s.addr <= addr < s.end: return s
        return None

    def u32(self, addr):
        s = self.find(addr)
        if not s or addr + 4 > s.end: return None
        o = addr - s.addr
        return struct.unpack('>I', s.data[o:o + 4])[0]

    def in_text(self, addr):
        return any(s.addr <= addr < s.end for s in self.text)

if __name__ == '__main__':
    import sys
    d = Dol(sys.argv[1])
    for s in d.sections:
        print(f'{s.kind:4} #{s.idx:2} {s.addr:08X}-{s.end:08X} size={s.size:X}')
    print(f'bss {d.bss_addr:08X} +{d.bss_size:X} entry {d.entry:08X}')
