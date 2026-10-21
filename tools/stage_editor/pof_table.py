"""Sega 'POF' data tables inside stage _gdt.one archives (ARTISTGADGET.BIN, ARTISTWORK.BIN, STONEPOLE.BIN ...).

Layout (big-endian): 32-byte header [total size, data size, POF size, 1, 0, 0x3142, 0, 0], the data block (offsets in it
are relative to its start), the POF pointer list (one entry per pointer word in the data block, encoded as distances in
words: 0b01xxxxxx = 6 bits, 0b10 + 1 byte = 14 bits, 0b11 + 3 bytes = 30 bits, 0 ends) and a short trailer.
The data block starts with (offset, count) pairs, one per list. Written by for the phase 3 dependency compiler.
"""
import struct


def pof_decode(b):
    out, at, i = [], 0, 0
    while i < len(b):
        c = b[i]; kind = c >> 6
        if kind == 0: break
        if kind == 1: n = c & 0x3F; i += 1
        elif kind == 2: n = ((c & 0x3F) << 8) | b[i + 1]; i += 2
        else: n = ((c & 0x3F) << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]; i += 4
        at += n * 4; out.append(at)
    return out


def pof_encode(offsets):
    out, prev = bytearray(), 0
    for at in sorted(offsets):
        n = (at - prev) // 4; prev = at
        if n < 0x40: out.append(0x40 | n)
        elif n < 0x4000: out += bytes([0x80 | n >> 8, n & 0xFF])
        else: out += bytes([0xC0 | n >> 24, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF])
    out += b'\0' * (-len(out) % 4)   # zero padding to a word; it also ends the list (none when already aligned)
    return bytes(out)


class Table:
    """header, data words (as a bytearray), pointer offsets (set), trailer; lists = [(offset, count)]"""
    def __init__(self, raw):
        total, size, pof_size = struct.unpack_from('>3I', raw, 0)
        if total != len(raw) or 32 + size + pof_size > len(raw): raise ValueError('Not a POF table')
        self.header = bytearray(raw[:32]); self.data = bytearray(raw[32:32 + size])
        pof = raw[32 + size:32 + size + pof_size]
        self.pointers = set(pof_decode(pof))
        if pof_encode(self.pointers) != pof: raise ValueError('POF list is not canonical')
        self.trailer = raw[32 + size + pof_size:]
        if any(p + 4 > size or p % 4 for p in self.pointers): raise ValueError('POF pointer outside the data')

    def word(self, at): return struct.unpack_from('>I', self.data, at)[0]
    def string(self, at):
        e = self.data.find(b'\0', at); return self.data[at:e].decode('ascii', 'replace')

    def serialize(self):
        pof = pof_encode(self.pointers)
        h =bytearray(self.header); struct.pack_into('>3I', h, 0, 32 + len(self.data) + len(pof) + len(self.trailer), len(self.data), len(pof))
        return bytes(h) + bytes(self.data) + pof + self.trailer
