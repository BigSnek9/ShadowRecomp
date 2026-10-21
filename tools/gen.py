"""Driver: DOL + dtk symbols -> generated C++ (one function per guest function)."""
import re, struct, sys, os, collections
sys.path.insert(0, os.path.dirname(__file__))
from dol import Dol
import lift

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# the extracted disc (tools/game_import): SHADOW_DISC=<folder>, else ..\Shadow next to the project
DOL = os.path.join(os.environ.get('SHADOW_DISC') or os.path.join(os.path.dirname(ROOT), 'Shadow'), 'sys', 'main.dol')
SYMS = os.path.join(ROOT, 'work', 'dtk', 'symbols.txt')
OUT = os.path.join(ROOT, 'generated')
FUNCS_PER_FILE = 220

sym_re = re.compile(r'^(\S+) = (\.\w+|extab\w*):0x([0-9A-Fa-f]+); // (.*)$')


def load_symbols():
    funcs, jt, allsyms = [], {}, {}
    for line in open(SYMS):
        m = sym_re.match(line.strip())
        if not m:
            continue
        name, sec, addr, rest = m.group(1), m.group(2), int(m.group(3), 16), m.group(4)
        sz = re.search(r'size:0x([0-9A-Fa-f]+)', rest)
        sz = int(sz.group(1), 16) if sz else 0
        allsyms[addr] = name
        if 'type:function' in rest:
            funcs.append((addr, sz, name))
        elif name.startswith('jumptable_'):
            jt[addr] = sz
    funcs.sort()
    return funcs, jt, allsyms


def write_if_changed(fn, text):
    try:
        if open(fn).read() == text:
            return
    except OSError:
        pass
    open(fn, 'w').write(text)


class ChangedWriter:
    # 'with ChangedWriter(path) as f:' -- only touches the file if the content changed (keeps ninja incremental)
    def __init__(self, fn):
        self.fn, self.parts = fn, []

    def write(self, t):
        self.parts.append(t)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        write_if_changed(self.fn, ''.join(self.parts))


def load_trace_fns():
    # tracefns.txt: guest function addresses (hex) whose entry is logged by rt_trace_enter (debugging)
    p = os.path.join(ROOT, 'tracefns.txt')
    out = set()
    if os.path.exists(p):
        for ln in open(p):
            ln = ln.split('#')[0].strip()
            if ln:
                out.add(int(ln.replace('fn_', ''), 16))
    return out


TRACE_FNS = load_trace_fns()


def load_call_hooks():
    # callhooks.txt: "<address of a bl/bctrl/blrl> <hook>" - that one call goes through the runtime function
    # `void hook(Cpu& c, uint32_t target)` instead of straight to the target; the hook makes the call itself
    # (call_addr(c, target)) and can do work before/after (e.g. tag what a draw belongs to).
    p = os.path.join(ROOT, 'callhooks.txt')
    out = {}
    if os.path.exists(p):
        for ln in open(p):
            ln = ln.split('#')[0].split()
            if len(ln) >= 2:
                out[int(ln[0].replace('fn_', ''), 16)] = ln[1]
    return out


CALL_HOOKS = load_call_hooks()


def load_dtscale():
    # dtscale.txt: "<addr> a|b  # why" - a per-frame fadds/fsubs whose operand a or b is the per-frame increment.
    # The increment is multiplied by rt_dt60() (= dt * 60 with a custom frame rate, exactly 1 at the original 60).
    p = os.path.join(ROOT, 'dtscale.txt')
    out = {}
    if os.path.exists(p):
        for ln in open(p):
            ln = ln.split('#')[0].strip()
            if ln:
                a, which = ln.split()[:2]
                out[int(a, 16)] = which
    return out


DTSCALE = load_dtscale()


def dtscale_insn(addr, w, which):
    op, xo5 = w >> 26, (w >> 1) & 31
    d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    if op != 59 or xo5 not in (20, 21) or (w & 1):
        raise SystemExit(f'dtscale.txt: {addr:08X} is not fadds/fsubs')
    fa, fb = f'c.f[{a}]', f'c.f[{b}]'
    if which == 'a': fa = f'({fa} * rt_dt60())'
    else: fb = f'({fb} * rt_dt60())'
    sym = '+' if xo5 == 21 else '-'
    return [f'c.f[{d}] = c.ps1[{d}] = rs({fa} {sym} {fb});   // dtscale.txt']


def load_overrides(syms=None):
    p = os.path.join(ROOT, 'overrides.txt')
    ov = {}
    byname = {n: a for a, n in (syms or {}).items()}
    if os.path.exists(p):
        for line in open(p):
            line = line.split('#')[0].strip()
            if line:
                parts = line.split()
                a, n = parts[0], parts[1]
                addr = byname[a] if a in byname else int(a, 16)
                ov[addr] = n
                if 'direct' in parts[2:]:   # "direct": only direct calls are redirected, not vtable/pointer calls
                    DIRECT_ONLY.add(addr)
    return ov


DIRECT_ONLY = set()


class FnCtx:
    def __init__(self, gen, start, end):
        self.gen, self.func_start, self.func_end = gen, start, end
        self.labels = set()
        self.cur = 0

    def label_for(self, a):
        return 'L_%08X' % a

    def call_stmt(self, tgt):
        g = self.gen
        if tgt in g.func_starts:
            name = g.overrides.get(tgt, 'f_%08X' % tgt)
            return f'{name}(c);'
        g.stats['call_nonstart'] += 1
        g.nonstart.add(tgt)
        return f'call_addr(c, {lift.hx(tgt)});'

    def jump_stmts(self, addr, tgt):
        g = self.gen
        if self.func_start <= tgt < self.func_end:
            self.labels.add(tgt)
            pre = f'if (--c.budget < 0) rt_poll(c, {lift.hx(addr)}); ' if tgt <= addr else ''
            return [pre + f'goto {self.label_for(tgt)};']
        # tail call
        if tgt in g.func_starts:
            return [self.call_stmt(tgt), 'return;']
        g.stats['jump_nonstart'] += 1
        g.nonstart.add(tgt)
        return [f'call_addr(c, {lift.hx(tgt)});', 'return;']

    def switch_targets(self, addr):
        return self.gen.find_switch(self, addr)

    def switch_stmt(self, addr, sw):
        cases = []
        for t in sorted(set(sw)):
            self.labels.add(t)
            cases.append(f'case {lift.hx(t)}: goto {self.label_for(t)};')
        return 'switch (c.ctr) { ' + ' '.join(cases) + ' default: call_addr(c, c.ctr); return; }'


class Gen:
    def __init__(self):
        self.dol = Dol(DOL)
        self.funcs, self.jt, self.syms = load_symbols()
        self.func_starts = {a for a, _, _ in self.funcs}
        self.overrides = load_overrides(self.syms)
        self.multi = set()
        mp = os.path.join(ROOT, 'multientry.txt')
        if os.path.exists(mp):
            for line in open(mp):
                line = line.split('#')[0].strip()
                if line:
                    self.multi.add(int(line, 16))
        self.stats = collections.Counter()
        self.nonstart = set()
        self.unknown = collections.Counter()
        self.insn_cache = {}
        self.unres_sw = []

    def word(self, addr):
        return self.dol.u32(addr)

    def find_switch(self, ctx, addr):
        """Resolve a bctr jump table by looking for lis/addi of a known jumptable address."""
        # scan backwards up to 16 instructions
        regs = {}
        a = addr - 4
        cand = None
        for _ in range(24):
            if a < ctx.func_start:
                break
            w = self.word(a)
            op = w >> 26
            rd, ra = (w >> 21) & 31, (w >> 16) & 31
            imm = w & 0xFFFF
            if op == 15 and ra == 0:  # lis
                regs.setdefault(('lis', rd), imm << 16)
            a -= 4
        # second pass forward to apply addi/ori on lis results
        a = ctx.func_start
        val = {}
        found = []
        scan_from = max(ctx.func_start, addr - 4 * 24)
        a = scan_from
        while a < addr:
            w = self.word(a)
            op = w >> 26
            rd, ra = (w >> 21) & 31, (w >> 16) & 31
            simm = lift.sext(w & 0xFFFF, 16)
            if op == 15 and ra == 0:
                val[rd] = (w & 0xFFFF) << 16
            elif op == 14 and ra in val:
                v = (val[ra] + simm) & 0xFFFFFFFF
                val[rd] = v
                found.append(v)
            elif op == 24 and rd in val:
                v = val[rd] | (w & 0xFFFF)
                val[ra] = v
                found.append(v)
            a += 4
        # bound from a preceding "cmplwi rX, N" (+ bgt)
        bound = None
        a2 = addr - 4
        for _ in range(24):
            if a2 < ctx.func_start:
                break
            w = self.word(a2)
            if (w >> 26) == 10:        # cmpli
                bound = (w & 0xFFFF) + 1
                break
            a2 -= 4
        for v in found[::-1]:
            n = None
            if v in self.jt:
                n = self.jt[v] // 4
            elif bound and self.dol.find(v) is not None:
                n = bound
            if not n:
                continue
            tg = []
            ok = True
            for i in range(n):
                t = self.dol.u32(v + 4 * i)
                if t is None or not (ctx.func_start <= t < ctx.func_end):
                    ok = False
                    break
                tg.append(t)
            if ok and tg:
                self.last_table = (v, n)
                return tg
        # fallback: the table base is an argument/global. Any code address stored in data that points into
        # this function is a candidate case label (superset of whatever table is used).
        if bound is not None or True:
            if not hasattr(self, '_data_ptrs'):
                import bisect
                ptrs = set()
                lo = min(s.addr for s in self.dol.text)
                hi = max(s.end for s in self.dol.text)
                for sec in self.dol.data:
                    d = sec.data
                    for off in range(0, len(d) - 3, 4):
                        v = struct.unpack_from('>I', d, off)[0]
                        if lo <= v < hi and (v & 3) == 0:
                            ptrs.add(v)
                self._data_ptrs = sorted(ptrs)
            import bisect
            i0 = bisect.bisect_left(self._data_ptrs, ctx.func_start)
            i1 = bisect.bisect_left(self._data_ptrs, ctx.func_end)
            found_targets = self._data_ptrs[i0:i1]
            if len(found_targets) >= 2 or bound:
                self.last_table = (0, 0)
                self.stats['switch_scanned'] += 1
                # Tables can also be built at run time, so any instruction may be a case label: allow all.
                return list(range(ctx.func_start, ctx.func_end, 4))
        self.stats['switch_unresolved'] += 1
        self.unres_sw.append(addr)
        return None

    def emit_function(self, start, size, name):
        end = start + size
        ctx = FnCtx(self, start, end)
        body = []
        a = start
        lines = []
        while a < end:
            w = self.word(a)
            if w is None:
                break
            try:
                st = dtscale_insn(a, w, DTSCALE[a]) if a in DTSCALE else lift.lift_insn(ctx, a, w)
                if a in CALL_HOOKS:
                    h = CALL_HOOKS[a]
                    joined = ' '.join(st)
                    assert 'call_addr(c, t);' in joined, f'callhooks.txt: {a:08X} is not an indirect call'
                    st = [x.replace('call_addr(c, t);', f'void {h}(Cpu&, uint32_t); {h}(c, t);') for x in st]
            except lift.Unknown as e:
                self.unknown[str(e)] += 1
                st = [f'rt_unimpl(c, {hex(a)}, "{e}");']
            lines.append((a, st, w))
            a += 4
        multi = start in self.multi
        if multi:
            for a, st, w in lines:
                ctx.labels.add(a)
            out = [f'// {name} @ {start:08X} size {size:X}  (multi-entry)',
                   f'void fe_{start:08X}(Cpu& c, uint32_t pc);',
                   f'void f_{start:08X}(Cpu& c) {{ fe_{start:08X}(c, 0x{start:08X}u); }}',
                   f'void fe_{start:08X}(Cpu& __restrict c, uint32_t pc) {{', '  RT_COV(pc);', '  switch (pc) {']
            for a, st, w in lines:
                out.append(f'    case 0x{a:08X}u: goto L_{a:08X};')
            out.append('    default: rt_unimpl(c, pc, "multi-entry: bad entry");')
            out.append('  }')
        else:
            out = [f'// {name} @ {start:08X} size {size:X}', f'void f_{start:08X}(Cpu& __restrict c) {{',
                   f'  RT_COV(0x{start:08X}u);']
            if start in TRACE_FNS:
                out.append(f'  rt_trace_enter(c, 0x{start:08X}u);')
        for a, st, w in lines:
            if a in ctx.labels:
                out.append(f'{ctx.label_for(a)}:;')
            for s in st:
                out.append('  ' + s)
        # fallthrough past the end
        out.append(f'  rt_fallthrough(c, {lift.hx(end)});')
        out.append('}')
        return out

    def split_by_roots(self, funcs):
        """Functions that are only reached through pointers are not found by dtk's call-graph analysis and end
        up merged into their neighbour. Find code addresses referenced from data / lis+addi pairs and split."""
        import bisect
        text = self.dol.text
        lo = min(s.addr for s in text)
        hi = max(s.end for s in text)
        labels = set()
        jt_ranges = []
        for taddr, tsz in self.jt.items():
            jt_ranges.append((taddr, taddr + tsz))
            for i in range(tsz // 4):
                t = self.dol.u32(taddr + 4 * i)
                if t:
                    labels.add(t)
        # also every switch table that find_switch can resolve (tables dtk did not name)
        saved_stats = collections.Counter(self.stats)
        saved_unres = list(self.unres_sw)
        for s, sz, nm in funcs:
            ctx = FnCtx(self, s, s + sz)
            for a in range(s, s + sz, 4):
                if self.word(a) == 0x4E800420:           # bctr
                    tg = self.find_switch(ctx, a)
                    if tg is not None:
                        v, n = self.last_table
                        jt_ranges.append((v, v + 4 * n))
                        labels.update(tg)
        self.stats = saved_stats
        self.unres_sw = saved_unres
        jt_ranges.sort()
        jt_lo = [r[0] for r in jt_ranges]

        def in_jt(a):
            i = bisect.bisect_right(jt_lo, a) - 1
            return i >= 0 and jt_ranges[i][0] <= a < jt_ranges[i][1]

        cands = {}
        # (a) pointers stored in data
        for sec in self.dol.data:
            d = sec.data
            for off in range(0, len(d) - 3, 4):
                v = struct.unpack_from('>I', d, off)[0]
                if lo <= v < hi and (v & 3) == 0 and not in_jt(sec.addr + off):
                    cands[v] = 'data'
        # (b) lis/addi pairs in code whose target follows a block terminator
        for sec in text:
            d = sec.data
            n = len(d) // 4
            words = struct.unpack('>%dI' % n, d[:n * 4])
            val = {}
            for i, w in enumerate(words):
                op = w >> 26
                rd, ra = (w >> 21) & 31, (w >> 16) & 31
                if op == 15 and ra == 0:
                    val[rd] = (w & 0xFFFF) << 16
                elif op == 14 and ra in val:
                    v = (val[ra] + lift.sext(w & 0xFFFF, 16)) & 0xFFFFFFFF
                    val[rd] = v
                    if lo <= v < hi and (v & 3) == 0 and v not in cands:
                        j = (v - sec.addr) // 4 - 1 if sec.addr <= v < sec.end else -1
                        if j >= 0:
                            pw = words[j]
                            if pw == 0x4E800020 or ((pw >> 26) == 18 and not (pw & 1)):
                                cands[v] = 'lis'
                elif op == 24 and rd in val:
                    val[ra] = val[rd] | (w & 0xFFFF)
                elif (op in (14, 15)) or op in (31,):
                    pass
        starts = [f[0] for f in funcs]
        newf = {}
        added = collections.Counter()
        for c in sorted(cands):
            if c in labels:
                continue
            i = bisect.bisect_right(starts, c) - 1
            if i < 0:
                continue
            s, sz, nm = funcs[i]
            if s < c < s + sz:
                newf.setdefault(i, []).append(c)
                added[cands[c]] += 1
        out = []
        for i, f in enumerate(funcs):
            if i not in newf:
                out.append(f)
                continue
            s, sz, nm = f
            pts = [s] + newf[i] + [s + sz]
            for k in range(len(pts) - 1):
                out.append((pts[k], pts[k + 1] - pts[k], nm if k == 0 else 'fn_%08X' % pts[k]))
        print('root discovery: split', sum(added.values()), dict(added))
        return sorted(out)

    def run(self, only=None):
        os.makedirs(OUT, exist_ok=True)
        funcs = [f for f in self.funcs if f[1] > 0]
        funcs = self.split_by_roots(funcs)
        self.func_starts = {f[0] for f in funcs}
        # unsized (size 0) functions are skipped
        # Pass 0: discover mid-function entry points by a dry run, add tail stubs.
        for _ in range(4):
            self.nonstart.clear()
            for start, size, name in funcs:
                self.emit_function(start, size, name)
            new = []
            import bisect
            starts = [f[0] for f in funcs]
            for t in sorted(self.nonstart):
                i = bisect.bisect_right(starts, t) - 1
                if i >= 0 and funcs[i][0] < t < funcs[i][0] + funcs[i][1]:
                    f = funcs[i]
                    new.append((t, f[0] + f[1] - t, f'{f[2]}+{t - f[0]:X}'))
            if not new:
                break
            for n in new:
                self.func_starts.add(n[0])
            funcs = sorted(funcs + new)
            self.stats.clear()
        files = []
        for i in range(0, len(funcs), FUNCS_PER_FILE):
            chunk = funcs[i:i + FUNCS_PER_FILE]
            fn = os.path.join(OUT, 'gen_%04d.cpp' % (i // FUNCS_PER_FILE))
            lines = ['#include "ppc_rt.h"', '#include "funcs.h"', '']
            for start, size, name in chunk:
                lines += self.emit_function(start, size, name)
                lines.append('')
            open(fn, 'w').write('\n'.join(lines))
            files.append(fn)
        # header + table
        with ChangedWriter(os.path.join(OUT, 'funcs.h')) as f:
            f.write('#pragma once\n#include "ppc_rt.h"\n')
            for start, size, name in funcs:
                f.write(f'void f_{start:08X}(Cpu&);\n')
            for n in sorted(set(self.overrides.values())):
                f.write(f'void {n}(Cpu&);\n')
        with ChangedWriter(os.path.join(OUT, 'functab.cpp')) as f:
            f.write('#include "funcs.h"\n#include "rt_internal.h"\nextern const GuestFunc g_functab[] = {\n')
            for start, size, name in funcs:
                # indirect calls (vtables, function pointers) go through this table: overrides apply here too
                fn = self.overrides.get(start, f'f_{start:08X}') if start not in DIRECT_ONLY else f'f_{start:08X}'
                f.write(f'  {{{lift.hx(start)}, {lift.hx(size)}, {fn}, "{name}"}},\n')
            f.write('};\nextern const unsigned g_functab_count = %d;\n' % len(funcs))
        with ChangedWriter(os.path.join(OUT, 'multi.cpp')) as f:
            f.write('#include "funcs.h"\n#include "rt_internal.h"\n')
            for s, sz, n in funcs:
                if s in self.multi:
                    f.write(f'void fe_{s:08X}(Cpu&, uint32_t);\n')
            f.write('extern const MultiFunc g_multi[] = {\n')
            for s, sz, n in sorted(funcs):
                if s in self.multi:
                    f.write(f'  {{{lift.hx(s)}, {lift.hx(sz)}, fe_{s:08X}}},\n')
            f.write('  {0, 0, nullptr}\n};\n')
        with ChangedWriter(os.path.join(OUT, 'files.cmake')) as f:
            f.write('set(GEN_SOURCES\n')
            for fn in files:
                f.write('  ${CMAKE_CURRENT_LIST_DIR}/' + os.path.basename(fn) + '\n')
            f.write('  ${CMAKE_CURRENT_LIST_DIR}/functab.cpp\n  ${CMAKE_CURRENT_LIST_DIR}/multi.cpp\n)\n')
        print('functions', len(funcs), 'files', len(files))
        print('stats', dict(self.stats))
        print('unknown', dict(self.unknown))
        print('nonstart targets', len(self.nonstart), sorted('%08X' % x for x in self.nonstart)[:40])


if __name__ == '__main__':
    Gen().run()
