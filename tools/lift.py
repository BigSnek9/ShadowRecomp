"""PowerPC (Gekko) -> C++ lifter.

lift_insn(ctx, addr, word) -> list[str]   (C++ statements, one instruction)

ctx supplies:
  ctx.func_start, ctx.func_end   : bounds of the function being emitted
  ctx.label_for(addr)            : label name if addr is a branch target in this function
  ctx.call_stmt(target_addr)     : C++ statement calling a guest function (direct)
  ctx.switch_targets(addr)       : list of targets for a bctr jump table at addr, or None
"""
import struct

def sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m

def mask32(mb, me):
    if mb <= me:
        return ((0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    return (~mask32(me + 1, mb - 1)) & 0xFFFFFFFF if not (mb == me + 1) else 0xFFFFFFFF

def hx(v):
    return '0x%08XU' % (v & 0xFFFFFFFF)

SPR_NAMES = {1: 'XER', 8: 'LR', 9: 'CTR'}


class Unknown(Exception):
    pass


def lift_insn(ctx, addr, w):
    op = w >> 26
    D = (w >> 21) & 31
    A = (w >> 16) & 31
    B = (w >> 11) & 31
    Cc = (w >> 6) & 31
    Rc = w & 1
    simm = sext(w & 0xFFFF, 16)
    uimm = w & 0xFFFF
    out = []
    nxt = (addr + 4) & 0xFFFFFFFF

    def ea_d(disp=None):
        d = simm if disp is None else disp
        if A == 0:
            return hx(d)
        if d == 0:
            return f'c.r[{A}]'
        return f'(c.r[{A}] + {hx(d)})'

    def ea_x():
        if A == 0:
            return f'c.r[{B}]'
        return f'(c.r[{A}] + c.r[{B}])'

    def rc(reg_expr):
        if Rc:
            out.append(f'cr0(c, {reg_expr});')

    # ------------------------------------------------------------------ integer D-forms
    if op == 14:  # addi
        out.append(f'c.r[{D}] = {hx(simm)};' if A == 0 else f'c.r[{D}] = c.r[{A}] + {hx(simm)};')
    elif op == 15:  # addis
        v = (uimm << 16) & 0xFFFFFFFF
        out.append(f'c.r[{D}] = {hx(v)};' if A == 0 else f'c.r[{D}] = c.r[{A}] + {hx(v)};')
    elif op == 12:  # addic
        out.append(f'c.r[{D}] = addc_({{c}}, c.r[{A}], {hx(simm)});'.replace('{c}', 'c'))
    elif op == 13:  # addic.
        out.append(f'c.r[{D}] = addc_(c, c.r[{A}], {hx(simm)}); cr0(c, c.r[{D}]);')
    elif op == 8:  # subfic
        out.append(f'c.r[{D}] = subfc_(c, c.r[{A}], {hx(simm)});')
    elif op == 7:  # mulli
        out.append(f'c.r[{D}] = (uint32_t)((int32_t)c.r[{A}] * (int32_t){simm});')
    elif op == 24:  # ori
        if w == 0x60000000:
            return []
        out.append(f'c.r[{A}] = c.r[{D}] | {hx(uimm)};')
    elif op == 25:  # oris
        out.append(f'c.r[{A}] = c.r[{D}] | {hx(uimm << 16)};')
    elif op == 26:  # xori
        out.append(f'c.r[{A}] = c.r[{D}] ^ {hx(uimm)};')
    elif op == 27:  # xoris
        out.append(f'c.r[{A}] = c.r[{D}] ^ {hx(uimm << 16)};')
    elif op == 28:  # andi.
        out.append(f'c.r[{A}] = c.r[{D}] & {hx(uimm)}; cr0(c, c.r[{A}]);')
    elif op == 29:  # andis.
        out.append(f'c.r[{A}] = c.r[{D}] & {hx(uimm << 16)}; cr0(c, c.r[{A}]);')
    elif op == 11:  # cmpi
        crf = (w >> 23) & 7
        out.append(f'cmp_s(c, {crf}, (int32_t)c.r[{A}], (int32_t){simm});')
    elif op == 10:  # cmpli
        crf = (w >> 23) & 7
        out.append(f'cmp_u(c, {crf}, c.r[{A}], {hx(uimm)});')
    elif op == 3:  # twi
        out.append(f'tw_(c, {D}, c.r[{A}], {hx(simm)}, {hex(addr)});')

    # ------------------------------------------------------------------ rotates
    elif op == 21:  # rlwinm
        S, SH, MB, ME = D, B, Cc, (w >> 1) & 31
        m = mask32(MB, ME)
        if SH == 0:
            out.append(f'c.r[{A}] = c.r[{S}] & {hx(m)};')
        elif m == (0xFFFFFFFF << SH) & 0xFFFFFFFF:
            out.append(f'c.r[{A}] = c.r[{S}] << {SH};')
        elif m == (0xFFFFFFFF >> (32 - SH)) and SH:
            out.append(f'c.r[{A}] = c.r[{S}] >> {32 - SH};')
        else:
            out.append(f'c.r[{A}] = rotl32(c.r[{S}], {SH}) & {hx(m)};')
        rc(f'c.r[{A}]')
    elif op == 20:  # rlwimi
        S, SH, MB, ME = D, B, Cc, (w >> 1) & 31
        m = mask32(MB, ME)
        out.append(f'c.r[{A}] = (rotl32(c.r[{S}], {SH}) & {hx(m)}) | (c.r[{A}] & {hx(~m)});')
        rc(f'c.r[{A}]')
    elif op == 23:  # rlwnm
        S, MB, ME = D, Cc, (w >> 1) & 31
        m = mask32(MB, ME)
        out.append(f'c.r[{A}] = rotl32(c.r[{S}], c.r[{B}] & 31) & {hx(m)};')
        rc(f'c.r[{A}]')

    # ------------------------------------------------------------------ loads / stores
    elif op in (32, 34, 40, 42):  # lwz lbz lhz lha
        fn = {32: 'ld32', 34: 'ld8', 40: 'ld16', 42: 'ld16s'}[op]
        out.append(f'c.r[{D}] = {fn}({ea_d()});')
    elif op in (33, 35, 41, 43):
        fn = {33: 'ld32', 35: 'ld8', 41: 'ld16', 43: 'ld16s'}[op]
        out.append(f'{{ uint32_t ea = c.r[{A}] + {hx(simm)}; c.r[{D}] = {fn}(ea); c.r[{A}] = ea; }}')
    elif op in (36, 38, 44):  # stw stb sth
        fn = {36: 'st32', 38: 'st8', 44: 'st16'}[op]
        out.append(f'{fn}({ea_d()}, c.r[{D}]);')
    elif op in (37, 39, 45):
        fn = {37: 'st32', 39: 'st8', 45: 'st16'}[op]
        out.append(f'{{ uint32_t ea = c.r[{A}] + {hx(simm)}; {fn}(ea, c.r[{D}]); c.r[{A}] = ea; }}')
    elif op == 46:  # lmw
        out.append(f'{{ uint32_t ea = {ea_d()}; for (int i = {D}; i < 32; ++i, ea += 4) c.r[i] = ld32(ea); }}')
    elif op == 47:  # stmw
        out.append(f'{{ uint32_t ea = {ea_d()}; for (int i = {D}; i < 32; ++i, ea += 4) st32(ea, c.r[i]); }}')

    # ------------------------------------------------------------------ FP loads / stores
    elif op == 48:  # lfs
        out.append(f'c.f[{D}] = c.ps1[{D}] = ldf32({ea_d()});')
    elif op == 49:
        out.append(f'{{ uint32_t ea = c.r[{A}] + {hx(simm)}; c.f[{D}] = c.ps1[{D}] = ldf32(ea); c.r[{A}] = ea; }}')
    elif op == 50:  # lfd
        out.append(f'c.f[{D}] = ldf64({ea_d()});')
    elif op == 51:
        out.append(f'{{ uint32_t ea = c.r[{A}] + {hx(simm)}; c.f[{D}] = ldf64(ea); c.r[{A}] = ea; }}')
    elif op == 52:  # stfs
        out.append(f'stf32({ea_d()}, c.f[{D}]);')
    elif op == 53:
        out.append(f'{{ uint32_t ea = c.r[{A}] + {hx(simm)}; stf32(ea, c.f[{D}]); c.r[{A}] = ea; }}')
    elif op == 54:  # stfd
        out.append(f'stf64({ea_d()}, c.f[{D}]);')
    elif op == 55:
        out.append(f'{{ uint32_t ea = c.r[{A}] + {hx(simm)}; stf64(ea, c.f[{D}]); c.r[{A}] = ea; }}')

    # ------------------------------------------------------------------ paired-single quantised load/store
    elif op in (56, 57, 60, 61):
        Wb = (w >> 15) & 1
        Ib = (w >> 12) & 7
        d12 = sext(w & 0xFFF, 12)
        upd = op in (57, 61)
        base = f'c.r[{A}]' if A else '0u'
        ea = f'(uint32_t)({base} + {hx(d12)})'
        if op in (56, 57):
            out.append(f'{{ uint32_t ea = {ea}; psq_l(c, {D}, ea, {Wb}, {Ib}); ' + (f'c.r[{A}] = ea; ' if upd else '') + '}')
        else:
            out.append(f'{{ uint32_t ea = {ea}; psq_st(c, {D}, ea, {Wb}, {Ib}); ' + (f'c.r[{A}] = ea; ' if upd else '') + '}')

    # ------------------------------------------------------------------ branches
    elif op == 18:  # b / bl / ba / bla
        li = sext(w & 0x03FFFFFC, 26)
        AA, LK = (w >> 1) & 1, w & 1
        tgt = (li if AA else addr + li) & 0xFFFFFFFF
        if LK:
            out.append(f'c.lr = {hx(nxt)};')
            out.append(ctx.call_stmt(tgt))
        else:
            out.extend(ctx.jump_stmts(addr, tgt))
    elif op == 16:  # bc
        BO, BI = D, A
        bd = sext(w & 0xFFFC, 16)
        AA, LK = (w >> 1) & 1, w & 1
        tgt = (bd if AA else addr + bd) & 0xFFFFFFFF
        cond = branch_cond(BO, BI)
        if LK:
            body = [f'c.lr = {hx(nxt)};', ctx.call_stmt(tgt)]
            if cond:
                out.append('if (' + cond[0] + ') {' + ' '.join(body) + '}')
            else:
                out.extend(body)
            if cond and cond[1]:
                out.insert(0, cond[1])
        else:
            jmp = ctx.jump_stmts(addr, tgt)
            if cond:
                if cond[1]:
                    out.append(cond[1])
                out.append('if (' + cond[0] + ') { ' + ' '.join(jmp) + ' }')
            else:
                out.extend(jmp)
    elif op == 19:
        xo = (w >> 1) & 0x3FF
        if xo == 16:  # bclr
            BO, BI, LK = D, A, w & 1
            cond = branch_cond(BO, BI)
            if LK:
                body = f'{{ uint32_t t = c.lr; c.lr = {hx(nxt)}; call_addr(c, t); }}'
                if cond:
                    if cond[1]: out.append(cond[1])
                    out.append('if (' + cond[0] + ') ' + body)
                else:
                    out.append(body)
            else:
                if cond:
                    if cond[1]: out.append(cond[1])
                    out.append('if (' + cond[0] + ') return;')
                else:
                    out.append('return;')
        elif xo == 528:  # bcctr
            BO, BI, LK = D, A, w & 1
            cond = branch_cond(BO, BI)
            if LK:
                body = f'{{ uint32_t t = c.ctr; c.lr = {hx(nxt)}; call_addr(c, t); }}'
                if cond:
                    if cond[1]: out.append(cond[1])
                    out.append('if (' + cond[0] + ') ' + body)
                else:
                    out.append(body)
            else:
                sw = ctx.switch_targets(addr)
                if sw is not None and not cond:
                    out.append(ctx.switch_stmt(addr, sw))
                else:
                    body = '{ uint32_t t = c.ctr; call_addr(c, t); return; }'
                    if cond:
                        if cond[1]: out.append(cond[1])
                        out.append('if (' + cond[0] + ') ' + body)
                    else:
                        out.append(body)
        elif xo == 0:  # mcrf
            out.append(f'c.cr[{(w >> 23) & 7}] = c.cr[{(w >> 18) & 7}];')
        elif xo in (33, 129, 193, 225, 257, 289, 417, 449):  # cr logic
            bd, ba, bb = D, A, B
            expr = {33: '!(a | b)', 129: 'a & !b', 193: 'a ^ b', 225: '!(a & b)',
                    257: 'a & b', 289: '!(a ^ b)', 417: 'a | !b', 449: 'a | b'}[xo]
            out.append(f'{{ uint32_t a = CRB(c, {ba}), b = CRB(c, {bb}); uint32_t r = ({expr}) & 1; CRS(c, {bd}, r); }}')
        elif xo == 150:  # isync
            return []
        elif xo == 50:  # rfi
            out.append(f'rt_rfi(c, {hex(addr)}); return;')
        else:
            raise Unknown(f'op19 xo={xo}')
    elif op == 17:  # sc
        out.append(f'rt_sc(c, {hex(addr)});')

    # ------------------------------------------------------------------ opcode 31
    elif op == 31:
        xo = (w >> 1) & 0x3FF
        OE = (w >> 10) & 1
        xo9 = xo & 0x1FF
        S = D
        # arithmetic with OE bit
        if xo9 in (266, 10, 138, 40, 8, 136, 235, 491, 459, 104, 200, 202, 232, 234):
            # 266 add,10 addc,138 adde, 40 subf, 8 subfc, 136 subfe, 235 mullw, 491 divw, 459 divwu,
            # 104 neg, 200 subfze, 202 addze, 232 subfme, 234 addme
            a, b = f'c.r[{A}]', f'c.r[{B}]'
            r = f'c.r[{D}]'
            ov = None
            if xo9 == 266:
                out.append(f'{{ uint32_t a = {a}, b = {b}; uint32_t r = a + b; {r} = r;' + (f' set_ov(c, ((~(a ^ b)) & (a ^ r)) >> 31);' if OE else '') + ' }')
            elif xo9 == 10:
                out.append(f'{{ uint32_t a = {a}, b = {b}; uint32_t r = addc_(c, a, b); {r} = r;' + (f' set_ov(c, ((~(a ^ b)) & (a ^ r)) >> 31);' if OE else '') + ' }')
            elif xo9 == 138:
                out.append(f'{{ uint32_t a = {a}, b = {b}; uint32_t r = adde_(c, a, b); {r} = r;' + (f' set_ov(c, ((~(a ^ b)) & (a ^ r)) >> 31);' if OE else '') + ' }')
            elif xo9 == 40:
                out.append(f'{{ uint32_t a = {a}, b = {b}; uint32_t r = b - a; {r} = r;' + (f' set_ov(c, ((a ^ b) & (b ^ r)) >> 31);' if OE else '') + ' }')
            elif xo9 == 8:
                out.append(f'{{ uint32_t a = {a}, b = {b}; uint32_t r = subfc_(c, a, b); {r} = r;' + (f' set_ov(c, ((a ^ b) & (b ^ r)) >> 31);' if OE else '') + ' }')
            elif xo9 == 136:
                out.append(f'{{ uint32_t a = {a}, b = {b}; uint32_t r = subfe_(c, a, b); {r} = r;' + (f' set_ov(c, ((a ^ b) & (b ^ r)) >> 31);' if OE else '') + ' }')
            elif xo9 == 235:
                out.append(f'{{ int64_t p = (int64_t)(int32_t){a} * (int32_t){b}; {r} = (uint32_t)p;' + (' set_ov(c, p != (int64_t)(int32_t)p);' if OE else '') + ' }')
            elif xo9 == 491:
                out.append(f'{{ int32_t a = (int32_t){a}, b = (int32_t){b}; bool o = (b == 0) || (a == INT32_MIN && b == -1); {r} = o ? (a < 0 ? 0xFFFFFFFFu : 0u) : (uint32_t)(a / b);' + (' set_ov(c, o);' if OE else '') + ' }')
            elif xo9 == 459:
                out.append(f'{{ uint32_t a = {a}, b = {b}; {r} = b ? a / b : 0u;' + (' set_ov(c, b == 0);' if OE else '') + ' }')
            elif xo9 == 104:
                out.append(f'{{ uint32_t a = {a}; {r} = 0u - a;' + (' set_ov(c, a == 0x80000000u);' if OE else '') + ' }')
            elif xo9 == 200:
                out.append(f'{{ uint32_t a = {a}; {r} = subfe_(c, a, 0u); }}')
            elif xo9 == 202:
                out.append(f'{{ uint32_t a = {a}; {r} = adde_(c, a, 0u); }}')
            elif xo9 == 232:
                out.append(f'{{ uint32_t a = {a}; {r} = subfe_(c, a, 0xFFFFFFFFu); }}')
            elif xo9 == 234:
                out.append(f'{{ uint32_t a = {a}; {r} = adde_(c, a, 0xFFFFFFFFu); }}')
            rc(r)
        elif xo == 75:  # mulhw
            out.append(f'c.r[{D}] = (uint32_t)(((int64_t)(int32_t)c.r[{A}] * (int32_t)c.r[{B}]) >> 32);')
            rc(f'c.r[{D}]')
        elif xo == 11:  # mulhwu
            out.append(f'c.r[{D}] = (uint32_t)(((uint64_t)c.r[{A}] * c.r[{B}]) >> 32);')
            rc(f'c.r[{D}]')
        elif xo == 0:  # cmp
            out.append(f'cmp_s(c, {(w >> 23) & 7}, (int32_t)c.r[{A}], (int32_t)c.r[{B}]);')
        elif xo == 32:  # cmpl
            out.append(f'cmp_u(c, {(w >> 23) & 7}, c.r[{A}], c.r[{B}]);')
        elif xo in (28, 60, 124, 284, 316, 412, 444, 476, 26, 922, 954, 24, 536, 792, 824):
            dst = f'c.r[{A}]'
            s_, b_ = f'c.r[{S}]', f'c.r[{B}]'
            if xo == 28:
                out.append(f'{dst} = {s_} & {b_};')
            elif xo == 60:
                out.append(f'{dst} = {s_} & ~{b_};')
            elif xo == 124:
                out.append(f'{dst} = ~({s_} | {b_});')
            elif xo == 284:
                out.append(f'{dst} = ~({s_} ^ {b_});')
            elif xo == 316:
                out.append(f'{dst} = {s_} ^ {b_};')
            elif xo == 412:
                out.append(f'{dst} = {s_} | ~{b_};')
            elif xo == 444:
                out.append(f'{dst} = {s_} | {b_};')
            elif xo == 476:
                out.append(f'{dst} = ~({s_} & {b_});')
            elif xo == 26:
                out.append(f'{dst} = {s_} ? (uint32_t)__builtin_clz({s_}) : 32u;')
            elif xo == 922:
                out.append(f'{dst} = (uint32_t)(int32_t)(int16_t){s_};')
            elif xo == 954:
                out.append(f'{dst} = (uint32_t)(int32_t)(int8_t){s_};')
            elif xo == 24:  # slw
                out.append(f'{{ uint32_t n = {b_} & 63; {dst} = n < 32 ? {s_} << n : 0u; }}')
            elif xo == 536:  # srw
                out.append(f'{{ uint32_t n = {b_} & 63; {dst} = n < 32 ? {s_} >> n : 0u; }}')
            elif xo == 792:  # sraw
                out.append(f'{dst} = sraw_(c, {s_}, {b_} & 63);')
            elif xo == 824:  # srawi
                out.append(f'{dst} = sraw_(c, {s_}, {B});')
            rc(dst)
        # (first group done)
        # remaining op31: loads/stores/system
        if xo in (23, 87, 279, 343, 55, 119, 311, 375, 534):
            fn = {23: 'ld32', 87: 'ld8', 279: 'ld16', 343: 'ld16s', 55: 'ld32', 119: 'ld8', 311: 'ld16', 375: 'ld16s', 534: 'ld32r'}[xo]
            if xo in (55, 119, 311, 375):
                out.append(f'{{ uint32_t ea = c.r[{A}] + c.r[{B}]; c.r[{D}] = {fn}(ea); c.r[{A}] = ea; }}')
            else:
                out.append(f'c.r[{D}] = {fn}({ea_x()});')
        elif xo in (151, 215, 407, 183, 247, 439, 662, 918):
            fn = {151: 'st32', 215: 'st8', 407: 'st16', 183: 'st32', 247: 'st8', 439: 'st16', 662: 'st32r', 918: 'st16r'}[xo]
            if xo in (183, 247, 439):
                out.append(f'{{ uint32_t ea = c.r[{A}] + c.r[{B}]; {fn}(ea, c.r[{S}]); c.r[{A}] = ea; }}')
            else:
                out.append(f'{fn}({ea_x()}, c.r[{S}]);')
        elif xo == 790:  # lhbrx
            out.append(f'c.r[{D}] = ld16r({ea_x()});')
        elif xo == 20:  # lwarx
            out.append(f'c.r[{D}] = ld32({ea_x()}); c.reserve = 1;')
        elif xo == 150:  # stwcx.
            out.append(f'st32({ea_x()}, c.r[{S}]); c.cr[0] = 2 | (uint8_t)c.xer_so;')
        elif xo in (535, 567, 599, 631):  # lfsx lfsux lfdx lfdux
            if xo == 535:
                out.append(f'c.f[{D}] = c.ps1[{D}] = ldf32({ea_x()});')
            elif xo == 567:
                out.append(f'{{ uint32_t ea = c.r[{A}] + c.r[{B}]; c.f[{D}] = c.ps1[{D}] = ldf32(ea); c.r[{A}] = ea; }}')
            elif xo == 599:
                out.append(f'c.f[{D}] = ldf64({ea_x()});')
            else:
                out.append(f'{{ uint32_t ea = c.r[{A}] + c.r[{B}]; c.f[{D}] = ldf64(ea); c.r[{A}] = ea; }}')
        elif xo in (663, 695, 727, 759, 983):  # stfsx stfsux stfdx stfdux stfiwx
            if xo == 663:
                out.append(f'stf32({ea_x()}, c.f[{S}]);')
            elif xo == 695:
                out.append(f'{{ uint32_t ea = c.r[{A}] + c.r[{B}]; stf32(ea, c.f[{S}]); c.r[{A}] = ea; }}')
            elif xo == 727:
                out.append(f'stf64({ea_x()}, c.f[{S}]);')
            elif xo == 759:
                out.append(f'{{ uint32_t ea = c.r[{A}] + c.r[{B}]; stf64(ea, c.f[{S}]); c.r[{A}] = ea; }}')
            else:
                out.append(f'st32({ea_x()}, (uint32_t)bits64(c.f[{S}]));')
        elif xo in (597, 533, 725, 661):  # lswi lswx stswi stswx
            out.append(f'rt_unimpl(c, {hex(addr)}, "string load/store");')
        elif xo == 19:  # mfcr
            out.append(f'c.r[{D}] = get_cr(c);')
        elif xo == 144:  # mtcrf
            out.append(f'set_crf(c, c.r[{S}], {(w >> 12) & 0xFF});')
        elif xo == 512:  # mcrxr
            out.append(f'{{ c.cr[{(w >> 23) & 7}] = (uint8_t)((c.xer_so << 3) | (c.xer_ov << 2) | (c.xer_ca << 1)); c.xer_so = c.xer_ov = c.xer_ca = 0; }}')
        elif xo == 83:  # mfmsr
            out.append(f'c.r[{D}] = c.msr;')
        elif xo == 146:  # mtmsr
            out.append(f'c.msr = c.r[{S}];')
        elif xo == 339:  # mfspr
            spr = A | (B << 5)
            out.append(mfspr(D, spr))
        elif xo == 467:  # mtspr
            spr = A | (B << 5)
            out.append(mtspr(D, spr))
        elif xo == 371:  # mftb
            tbr = A | (B << 5)
            out.append(f'c.r[{D}] = rt_mftb({tbr});')
        elif xo in (595,):  # mfsr
            out.append(f'c.r[{D}] = c.sr[{A & 15}];')
        elif xo == 210:  # mtsr
            out.append(f'c.sr[{A & 15}] = c.r[{S}];')
        elif xo == 242:  # mtsrin
            out.append(f'c.sr[c.r[{B}] >> 28] = c.r[{S}];')
        elif xo == 659:  # mfsrin
            out.append(f'c.r[{D}] = c.sr[c.r[{B}] >> 28];')
        elif xo == 1014:  # dcbz
            out.append(f'dcbz({ea_x()});')
        elif xo in (86, 54, 470, 278, 246, 982, 598, 854, 566, 306, 22):
            # dcbf dcbst dcbi dcbt dcbtst icbi sync eieio tlbsync tlbie icbt
            if xo == 470:
                out.append(f'rt_dcbi({ea_x()});')
            return out
        elif xo == 4:  # tw
            out.append(f'tw_(c, {D}, c.r[{A}], c.r[{B}], {hex(addr)});')
        elif xo in (28, 60, 124, 284, 316, 412, 444, 476, 26, 922, 954, 24, 536, 792, 824) or xo9 in (266, 10, 138, 40, 8, 136, 235, 491, 459, 104, 200, 202, 232, 234) or xo in (75, 11, 0, 32):
            pass
        else:
            raise Unknown(f'op31 xo={xo}')

    # ------------------------------------------------------------------ single float (59)
    elif op == 59:
        xo5 = (w >> 1) & 31
        fd, fa, fb, fc = D, A, B, Cc
        e = {
            18: f'rs(c.f[{fa}] / c.f[{fb}])',
            20: f'rs(c.f[{fa}] - c.f[{fb}])',
            21: f'rs(c.f[{fa}] + c.f[{fb}])',
            22: f'rs(std::sqrt(c.f[{fb}]))',
            24: f'rs(fres_(c.f[{fb}]))',
            25: f'rs(c.f[{fa}] * c.f[{fc}])',
            28: f'rs(std::fma(c.f[{fa}], c.f[{fc}], -c.f[{fb}]))',
            29: f'rs(std::fma(c.f[{fa}], c.f[{fc}], c.f[{fb}]))',
            30: f'rs(-std::fma(c.f[{fa}], c.f[{fc}], -c.f[{fb}]))',
            31: f'rs(-std::fma(c.f[{fa}], c.f[{fc}], c.f[{fb}]))',
        }.get(xo5)
        if e is None:
            raise Unknown(f'op59 xo={xo5}')
        out.append(f'c.f[{fd}] = c.ps1[{fd}] = {e};')
        if Rc:
            out.append('cr1(c);')

    # ------------------------------------------------------------------ double float (63)
    elif op == 63:
        xo5 = (w >> 1) & 31
        xo = (w >> 1) & 0x3FF
        fd, fa, fb, fc = D, A, B, Cc
        e = {
            18: f'c.f[{fa}] / c.f[{fb}]',
            20: f'c.f[{fa}] - c.f[{fb}]',
            21: f'c.f[{fa}] + c.f[{fb}]',
            22: f'std::sqrt(c.f[{fb}])',
            23: f'(c.f[{fa}] >= 0.0 ? c.f[{fc}] : c.f[{fb}])',
            25: f'c.f[{fa}] * c.f[{fc}]',
            26: f'frsqrte_(c.f[{fb}])',
            28: f'std::fma(c.f[{fa}], c.f[{fc}], -c.f[{fb}])',
            29: f'std::fma(c.f[{fa}], c.f[{fc}], c.f[{fb}])',
            30: f'-std::fma(c.f[{fa}], c.f[{fc}], -c.f[{fb}])',
            31: f'-std::fma(c.f[{fa}], c.f[{fc}], c.f[{fb}])',
        }.get(xo5)
        if xo5 in (18, 20, 21, 22, 23, 25, 26, 28, 29, 30, 31):
            out.append(f'c.f[{fd}] = {e};')
        elif xo == 0 or xo == 32:  # fcmpu / fcmpo
            out.append(f'fcmp_(c, {(w >> 23) & 7}, c.f[{fa}], c.f[{fb}]);')
        elif xo == 12:  # frsp
            out.append(f'c.f[{fd}] = rs(c.f[{fb}]);')
        elif xo == 14:  # fctiw
            out.append(f'c.f[{fd}] = fctiw_(c.f[{fb}], false);')
        elif xo == 15:  # fctiwz
            out.append(f'c.f[{fd}] = fctiw_(c.f[{fb}], true);')
        elif xo == 40:
            out.append(f'c.f[{fd}] = -c.f[{fb}];')
        elif xo == 72:
            out.append(f'c.f[{fd}] = c.f[{fb}];')
        elif xo == 136:
            out.append(f'c.f[{fd}] = -std::fabs(c.f[{fb}]);')
        elif xo == 264:
            out.append(f'c.f[{fd}] = std::fabs(c.f[{fb}]);')
        elif xo == 583:  # mffs
            out.append(f'c.f[{fd}] = from_bits64(0xFFF8000000000000ull | c.fpscr);')
        elif xo == 711:  # mtfsf
            fm = (w >> 17) & 0xFF
            out.append(f'mtfsf_(c, {fm}, (uint32_t)bits64(c.f[{fb}]));')
        elif xo == 134:  # mtfsfi
            crf = (w >> 23) & 7
            imm = (w >> 12) & 15
            out.append(f'mtfsf_(c, {1 << (7 - crf)}, {imm} << {28 - 4 * crf});')
        elif xo == 38:  # mtfsb1
            out.append(f'c.fpscr |= {hx(1 << (31 - D))};')
        elif xo == 70:  # mtfsb0
            out.append(f'c.fpscr &= ~{hx(1 << (31 - D))};')
        elif xo == 64:  # mcrfs
            out.append(f'{{ int s = {(w >> 18) & 7}; c.cr[{(w >> 23) & 7}] = (c.fpscr >> (28 - 4 * s)) & 15; }}')
        else:
            raise Unknown(f'op63 xo={xo}')
        if Rc:
            out.append('cr1(c);')

    # ------------------------------------------------------------------ paired singles (4)
    elif op == 4:
        xo5 = (w >> 1) & 31
        xo6 = (w >> 1) & 63
        xo = (w >> 1) & 0x3FF
        fd, fa, fb, fc = D, A, B, Cc
        names5 = {10: 'ps_sum0', 11: 'ps_sum1', 12: 'ps_muls0', 13: 'ps_muls1', 14: 'ps_madds0',
                  15: 'ps_madds1', 18: 'ps_div', 20: 'ps_sub', 21: 'ps_add', 23: 'ps_sel',
                  24: 'ps_res', 25: 'ps_mul', 26: 'ps_rsqrte', 28: 'ps_msub', 29: 'ps_madd',
                  30: 'ps_nmsub', 31: 'ps_nmadd'}
        if xo5 in names5:
            out.append(f'{names5[xo5]}(c, {fd}, {fa}, {fb}, {fc});')
        elif xo6 in (6, 7, 38, 39):
            Wb = (w >> 10) & 1
            Ib = (w >> 7) & 7
            base = f'c.r[{A}]' if A else '0u'
            ea = f'(uint32_t)({base} + c.r[{B}])'
            upd = xo6 in (38, 39)
            fn = 'psq_l' if xo6 in (6, 38) else 'psq_st'
            out.append(f'{{ uint32_t ea = {ea}; {fn}(c, {D}, ea, {Wb}, {Ib}); ' + (f'c.r[{A}] = ea; ' if upd else '') + '}')
        elif xo in (0, 32, 64, 96):
            out.append(f'ps_cmp(c, {(w >> 23) & 7}, {fa}, {fb}, {xo // 32});')
        elif xo == 40:
            out.append(f'c.f[{fd}] = -c.f[{fb}]; c.ps1[{fd}] = -c.ps1[{fb}];')
        elif xo == 72:
            out.append(f'c.f[{fd}] = c.f[{fb}]; c.ps1[{fd}] = c.ps1[{fb}];')
        elif xo == 136:
            out.append(f'c.f[{fd}] = -std::fabs(c.f[{fb}]); c.ps1[{fd}] = -std::fabs(c.ps1[{fb}]);')
        elif xo == 264:
            out.append(f'c.f[{fd}] = std::fabs(c.f[{fb}]); c.ps1[{fd}] = std::fabs(c.ps1[{fb}]);')
        elif xo in (528, 560, 592, 624):
            a_ = 'c.f' if xo in (528, 560) else 'c.ps1'
            b_ = 'c.f' if xo in (528, 592) else 'c.ps1'
            out.append(f'{{ double x = {a_}[{fa}], y = {b_}[{fb}]; c.f[{fd}] = x; c.ps1[{fd}] = y; }}')
        elif xo == 1014:  # dcbz_l
            out.append(f'dcbz({ea_x()});')
        else:
            raise Unknown(f'op4 xo={xo}')
        if Rc and (xo5 in names5 or xo in (40, 72, 136, 264, 528, 560, 592, 624)):
            out.append('cr1(c);')
    else:
        raise Unknown(f'op={op}')

    return out


def branch_cond(BO, BI):
    """Return None for unconditional, else (cond_expr, pre_stmt)."""
    pre = ''
    parts = []
    if not (BO & 4):
        pre = '--c.ctr;'
        parts.append('(c.ctr == 0)' if (BO & 2) else '(c.ctr != 0)')
    if not (BO & 16):
        parts.append(f'(CRB(c, {BI}) == {1 if (BO & 8) else 0})')
    if not parts:
        return None
    return (' && '.join(parts), pre)


def mfspr(D, spr):
    if spr == 1:
        return f'c.r[{D}] = get_xer(c);'
    if spr == 8:
        return f'c.r[{D}] = c.lr;'
    if spr == 9:
        return f'c.r[{D}] = c.ctr;'
    if spr == 22:
        return f'c.r[{D}] = rt_get_dec(c);'
    if spr == 268 or spr == 269:
        return f'c.r[{D}] = rt_mftb({spr});'
    if 912 <= spr <= 919:
        return f'c.r[{D}] = c.gqr[{spr - 912}];'
    if spr == 26:
        return f'c.r[{D}] = c.srr0;'
    if spr == 27:
        return f'c.r[{D}] = c.srr1;'
    return f'c.r[{D}] = rt_mfspr(c, {spr});'


def mtspr(S, spr):
    if spr == 1:
        return f'set_xer(c, c.r[{S}]);'
    if spr == 8:
        return f'c.lr = c.r[{S}];'
    if spr == 9:
        return f'c.ctr = c.r[{S}];'
    if spr == 22:
        return f'rt_set_dec(c, c.r[{S}]);'
    if 912 <= spr <= 919:
        return f'c.gqr[{spr - 912}] = c.r[{S}];'
    if spr == 26:
        return f'c.srr0 = c.r[{S}];'
    if spr == 27:
        return f'c.srr1 = c.r[{S}];'
    return f'rt_mtspr(c, {spr}, c.r[{S}]);'
