#!/usr/bin/env python3
"""gen_vu0.py - translate the decomp's VU0 / COP2 functions from their matched assembly to C.

The game's VU0 maths (Sony libvu0, RhGeom*, lRhModelCpu*) is written in EE assembly that issues COP2 macro
instructions. The decomp keeps the exact matched assembly (asm/matchings/**/<func>.s, with every delay slot as
the hardware runs it), so this script turns each listed function into C that runs the same instructions on an
emulated EE register file (vu0.h: ee) and VU0 (vu0_cop2). Nothing is hand-edited; regenerate from a pinned
decomp commit:

    python gen_vu0.py <decomp-root> <out-dir> [--protos vu0_protos.h]

Output: <out-dir>/vu0_funcs.c, plus FUNCTIONS.txt (what the port generator must cut from the main program).
"""
import os, re, sys, glob
from typing import NoReturn

FUNCS = """
sceVu0ApplyMatrix sceVu0MulMatrix sceVu0OuterProduct sceVu0InnerProduct sceVu0Normalize sceVu0TransposeMatrix
sceVu0InversMatrix sceVu0DivVector sceVu0DivVectorXYZ sceVu0InterVector sceVu0AddVector sceVu0SubVector
sceVu0MulVector sceVu0ScaleVector sceVu0TransMatrix sceVu0CopyVector sceVu0CopyMatrix sceVu0FTOI4Vector
sceVu0FTOI0Vector sceVu0ITOF4Vector sceVu0ITOF0Vector sceVu0UnitMatrix _sceVu0ecossin sceVu0RotMatrixZ
sceVu0RotMatrixX sceVu0RotMatrixY sceVu0ClampVector sceVu0RotTransPersN sceVu0RotTransPers sceVu0InterVectorXYZ
sceVu0ScaleVectorXYZ sceVu0ClipScreen sceVu0ClipScreen3 sceVu0ClipAll sceVpu0Reset
RhGeom_SetMatrix RhGeom_GetMatrix RhGeom_PushMatrix RhGeom_PopMatrix RhGeom_UnitMatrix RhGeom_MulMatrix
RhGeom_MulMatrixRev RhGeom_ScaleMatrix RhGeom_ScaleRotXYZTransMatrix RhGeom_MulMatrixVector
RhGeom_MulMatrixVectorArray RhGeomEx_SetMatrixForStraightLight RhGeomEx_SetMatrixForPointLight RhGeomEx_GetMatrix
RhGeomEx_PushMatrix RhGeomEx_PopMatrix RhGeomEx_ScaleRotXYZTransMatrix
lRhModelCpu_SetupGlobal lRhModelCpu_TraceDynamicBlock lRhModelCpu_CalcIndepStrip lRhModelCpu_CalcIndepPoint
lRhModelCpu_CalcSharedPoint lRhModel_SetupNoMotion lRhModel_SetupMultiMotion lRhModel_SetupFloatMotion
lRhModel_SetupPulseMotion
""".split()

# Signatures the C definitions do not show: arguments the assembly reads beyond the declared ones.
EXTRA_ARGS = {
    "sceVu0RotTransPersN": ["s32 mode"],
    "sceVu0ClipAll": ["s32 count"],
    "RhGeom_MulMatrixVectorArray": ["void *dst", "s32 mode", "void *src", "s32 count"],   # declared "()" in the decomp
}

GPR = {n: i for i, n in enumerate(
    "zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra".split())}
GPR["s8"] = 30
INT_ARG_REGS = [4, 5, 6, 7, 8, 9, 10, 11]
FLT_ARG_REGS = list(range(12, 20))

FLOAT_TYPES = {"f32", "float"}


def die(msg) -> NoReturn:
    sys.stderr.write("gen_vu0: " + msg + "\n")
    sys.exit(1)


# ---- prototypes from the decomp's C ----------------------------------------------------------------------------

def load_protos(root):
    protos = {}
    for f in sorted(glob.glob(os.path.join(root, "src", "*.c"))):
        s = open(f, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r"^(?:asm\s+)?((?:static\s+)?[A-Za-z_][\w\s\*]*?)\b(\w+)\s*\(([^;{)]*)\)\s*\{", s, re.M):
            ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
            if ret.startswith(("static", "return", "else")) or name in ("if", "while", "for", "switch"):
                continue
            protos.setdefault(name, (ret, params))
    return protos


def split_params(params):
    if params in ("", "void"):
        return []
    out = []
    depth = 0
    cur = ""
    for ch in params:
        if ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""
            continue
        if ch == "(": depth += 1
        if ch == ")": depth -= 1
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def param_kind(p):
    """'i' integer/pointer, 'f' float."""
    t = re.sub(r"\b\w+\s*$", "", p).strip() if "(" not in p else "void *"
    if "*" in p or "[" in p or "(" in p:
        return "i"
    t = t or p
    return "f" if t.split()[-1] in FLOAT_TYPES else "i"


def ret_kind(ret):
    r = ret.replace("static", "").strip()
    if r == "void":
        return "v"
    if "*" in r:
        return "p"
    return "f" if r in FLOAT_TYPES else "i"


def c_param_list(params):
    """Parameter list for our own definition: every parameter re-typed to u32 / float with its own name."""
    out = []
    for k, p in enumerate(params):
        out.append(("float" if param_kind(p) == "f" else "u32") + " p%d" % k)
    return ", ".join(out) if out else "void"


# ---- assembly parsing ------------------------------------------------------------------------------------------

LINE = re.compile(r"^\s*/\*\s*([0-9A-F]+)\s+([0-9A-F]{8})\s+([0-9A-F]{8})\s*\*/\s*(.*)$")


def parse_s(path):
    insns = []
    for ln in open(path, encoding="utf-8", errors="replace"):
        if ln.startswith("endlabel"):
            break
        m = LINE.match(ln)
        if not m:
            continue
        addr = int(m.group(2), 16)
        raw = m.group(3)
        word = int(raw[6:8] + raw[4:6] + raw[2:4] + raw[0:2], 16)   # the dump shows bytes little-endian
        orig = m.group(4).strip()
        text = re.sub(r"\s*#.*$", "", orig)
        text = re.sub(r"/\*.*?\*/", "", text).strip()
        if text.startswith(".word"):
            cm = re.search(r"/\*\s*(.*?)\s*\*/", orig) or re.search(r"#\s*(\S.*?)\s*(#|$)", orig)
            if cm:
                COMMENTS[addr] = cm.group(1).split(" #")[0].strip()
        insns.append((addr, word, text))
    return insns


def reg(tok):
    tok = tok.strip()
    if not tok.startswith("$"):
        die("not a register: " + tok)
    n = tok[1:]
    if n in GPR:
        return GPR[n]
    if re.fullmatch(r"\d+", n):
        return int(n)
    die("unknown register " + tok)


def freg(tok):
    m = re.fullmatch(r"\$f(\d+)", tok.strip())
    if not m:
        die("not an FPU register: " + tok)
    return int(m.group(1))


def vreg(tok, pre="vf"):
    m = re.fullmatch(r"\$" + pre + r"(\d+)", tok.strip())
    if not m:
        die("not a %s register: %s" % (pre, tok))
    return int(m.group(1))


SYMS = {}
COMMENTS = {}


def sym_addr(name):
    m = re.search(r"([0-9A-Fa-f]{8})$", name)
    if m and re.match(r"^(D_|func_|jtbl_|B_)", name):
        return int(m.group(1), 16)
    if name in SYMS:
        return SYMS[name]
    die("unknown symbol " + name)


def imm(tok):
    tok = tok.strip()
    m = re.fullmatch(r"%hi\((\w+)\)", tok)
    if m:
        return ((sym_addr(m.group(1)) + 0x8000) >> 16) & 0xFFFF
    m = re.fullmatch(r"%lo\((\w+)\)", tok)
    if m:
        v = sym_addr(m.group(1)) & 0xFFFF
        return v - 0x10000 if v >= 0x8000 else v
    if re.fullmatch(r"[0-9A-Fa-fx()+\-<>&| ]+", tok):
        return int(eval(tok, {"__builtins__": {}}))
    die("bad immediate " + tok)


def memop(tok):
    m = re.fullmatch(r"(.*)\((\$\w+)\)", tok.strip())
    if not m:
        die("bad memory operand " + tok)
    off = m.group(1).strip() or "0"
    return imm(off), reg(m.group(2))


def label_addr(tok):
    m = re.search(r"([0-9A-Fa-f]{8})$", tok.strip())
    if not m:
        die("bad label " + tok)
    return int(m.group(1), 16)


# ---- translation -----------------------------------------------------------------------------------------------

BRANCH = {"b", "beq", "bne", "beqz", "bnez", "blez", "bgtz", "bltz", "bgez", "bc1t", "bc1f",
          "beql", "bnel", "beqzl", "bnezl", "bc1tl", "bc1fl", "j"}


def R(n):
    return "0" if n == 0 else "R(%d)" % n


def translate_one(ins, name):
    """C for one non-branch instruction."""
    addr, word, text = ins
    if text.startswith(".word"):
        w = int(text.split()[1], 0)
        if (w >> 26) == 0x12:
            return "vu0_cop2(0x%08Xu);%s" % (w, ("  /* %s */" % COMMENTS[addr]) if addr in COMMENTS else "")
        die("%s: unexpected .word %08x at %08x" % (name, w, addr))
    parts = text.split(None, 1)
    op = parts[0]
    args = [a.strip() for a in parts[1].split(",")] if len(parts) > 1 else []

    def wr(rd, expr):
        return "" if rd == 0 else "R(%d) = %s;" % (rd, expr)

    if op in ("nop", "sync.p", "sync", "sync.l"):
        return ""
    if op in ("addiu", "addi"):
        return wr(reg(args[0]), "SX32((u32)%s + (u32)(%d))" % (R(reg(args[1])), imm(args[2])))
    if op == "daddiu":
        return wr(reg(args[0]), "%s + (u64)(s64)(%d)" % (R(reg(args[1])), imm(args[2])))
    if op in ("addu", "add"):
        return wr(reg(args[0]), "SX32((u32)%s + (u32)%s)" % (R(reg(args[1])), R(reg(args[2]))))
    if op in ("subu", "sub"):
        return wr(reg(args[0]), "SX32((u32)%s - (u32)%s)" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "daddu":
        return wr(reg(args[0]), "%s + %s" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "dsubu":
        return wr(reg(args[0]), "%s - %s" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "lui":
        return wr(reg(args[0]), "SX32((u32)%du << 16)" % imm(args[1]))
    if op == "ori":
        return wr(reg(args[0]), "%s | (u64)%du" % (R(reg(args[1])), imm(args[2]) & 0xFFFF))
    if op == "andi":
        return wr(reg(args[0]), "%s & (u64)%du" % (R(reg(args[1])), imm(args[2]) & 0xFFFF))
    if op == "xori":
        return wr(reg(args[0]), "%s ^ (u64)%du" % (R(reg(args[1])), imm(args[2]) & 0xFFFF))
    if op in ("and", "or", "xor", "nor"):
        e = {"and": "&", "or": "|", "xor": "^", "nor": "|"}[op]
        x = "%s %s %s" % (R(reg(args[1])), e, R(reg(args[2])))
        return wr(reg(args[0]), "~(%s)" % x if op == "nor" else x)
    if op == "sll":
        return wr(reg(args[0]), "SX32((u32)%s << %d)" % (R(reg(args[1])), imm(args[2])))
    if op == "srl":
        return wr(reg(args[0]), "SX32((u32)%s >> %d)" % (R(reg(args[1])), imm(args[2])))
    if op == "sra":
        return wr(reg(args[0]), "SX32((u32)((s32)%s >> %d))" % (R(reg(args[1])), imm(args[2])))
    if op == "sllv":
        return wr(reg(args[0]), "SX32((u32)%s << (%s & 31))" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "srlv":
        return wr(reg(args[0]), "SX32((u32)%s >> (%s & 31))" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "srav":
        return wr(reg(args[0]), "SX32((u32)((s32)%s >> (%s & 31)))" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "dsll":
        return wr(reg(args[0]), "%s << %d" % (R(reg(args[1])), imm(args[2])))
    if op == "dsrl":
        return wr(reg(args[0]), "%s >> %d" % (R(reg(args[1])), imm(args[2])))
    if op == "dsra":
        return wr(reg(args[0]), "(u64)((s64)%s >> %d)" % (R(reg(args[1])), imm(args[2])))
    if op == "dsll32":
        return wr(reg(args[0]), "%s << %d" % (R(reg(args[1])), imm(args[2]) + 32))
    if op == "dsrl32":
        return wr(reg(args[0]), "%s >> %d" % (R(reg(args[1])), imm(args[2]) + 32))
    if op == "dsra32":
        return wr(reg(args[0]), "(u64)((s64)%s >> %d)" % (R(reg(args[1])), imm(args[2]) + 32))
    if op == "slt":
        return wr(reg(args[0]), "(s64)%s < (s64)%s" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "sltu":
        return wr(reg(args[0]), "%s < %s" % (R(reg(args[1])), R(reg(args[2]))))
    if op == "slti":
        return wr(reg(args[0]), "(s64)%s < (s64)(%d)" % (R(reg(args[1])), imm(args[2])))
    if op == "sltiu":
        return wr(reg(args[0]), "%s < (u64)(s64)(%d)" % (R(reg(args[1])), imm(args[2])))
    if op == "movz":
        return "if (%s == 0) %s" % (R(reg(args[2])), wr(reg(args[0]), R(reg(args[1]))) or ";")
    if op == "movn":
        return "if (%s != 0) %s" % (R(reg(args[2])), wr(reg(args[0]), R(reg(args[1]))) or ";")
    if op == "div":
        a, b = R(reg(args[-2])), R(reg(args[-1]))
        return "ee_div((s32)%s, (s32)%s);" % (a, b)
    if op == "divu":
        a, b = R(reg(args[-2])), R(reg(args[-1]))
        return "ee_divu((u32)%s, (u32)%s);" % (a, b)
    if op in ("mult", "multu"):
        a, b = R(reg(args[-2])), R(reg(args[-1]))
        t = "s64)(s32" if op == "mult" else "u64)(u32"
        s = "{ u64 m_ = (u64)((%s)%s * (%s)%s); ee.lo = SX32((u32)m_); ee.hi = SX32((u32)(m_ >> 32));" % (t, a, t, b)
        if len(args) == 3 and reg(args[0]):
            s += " R(%d) = ee.lo;" % reg(args[0])
        return s + " }"
    if op == "mfhi":
        return wr(reg(args[0]), "ee.hi")
    if op == "mflo":
        return wr(reg(args[0]), "ee.lo")
    # memory
    if op in ("lw", "lh", "lhu", "lb", "lbu", "ld", "lwu"):
        off, base = memop(args[1])
        a = "(u32)(%s + (%d))" % (R(base), off)
        e = {"lw": "SX32(ld32(%s))", "lwu": "(u64)ld32(%s)", "lh": "(u64)(s64)(s16)ld16(%s)", "lhu": "(u64)ld16(%s)",
             "lb": "(u64)(s64)(s8)ld8(%s)", "lbu": "(u64)ld8(%s)", "ld": "ld64(%s)"}[op] % a
        return wr(reg(args[0]), e)
    if op in ("sw", "sh", "sb", "sd"):
        off, base = memop(args[1])
        a = "(u32)(%s + (%d))" % (R(base), off)
        f = {"sw": "st32(%s, (u32)%s);", "sh": "st16(%s, (u16)%s);", "sb": "st8(%s, (u8)%s);", "sd": "st64(%s, %s);"}[op]
        return f % (a, R(reg(args[0])))
    if op == "lq":
        off, base = memop(args[1])
        rt = reg(args[0])
        return "ldq(%d, (u32)(%s + (%d)));" % (rt, R(base), off) if rt else ""
    if op == "sq":
        off, base = memop(args[1])
        return "stq((u32)(%s + (%d)), %d);" % (R(base), off, reg(args[0]))
    if op == "lwc1":
        off, base = memop(args[1])
        return "F(%d) = ldf((u32)(%s + (%d)));" % (freg(args[0]), R(base), off)
    if op == "swc1":
        off, base = memop(args[1])
        return "stf((u32)(%s + (%d)), F(%d));" % (R(base), off, freg(args[0]))
    if op == "lqc2":
        off, base = memop(args[1])
        return "lqc2(%d, (u32)(%s + (%d)));" % (vreg(args[0]), R(base), off)
    if op == "sqc2":
        off, base = memop(args[1])
        return "sqc2((u32)(%s + (%d)), %d);" % (R(base), off, vreg(args[0]))
    # MMI (the few that appear)
    if op == "paddub":
        return "mmi_paddub(%d, %d, %d);" % (reg(args[0]), reg(args[1]), reg(args[2]))
    if op in ("pextlw", "pextuw", "pcpyld", "pcpyud"):
        return "mmi_%s(%d, %d, %d);" % (op, reg(args[0]), reg(args[1]), reg(args[2]))
    if op == "por":
        return "mmi_por(%d, %d, %d);" % (reg(args[0]), reg(args[1]), reg(args[2]))
    # COP2 moves
    if op in ("qmtc2", "qmtc2.ni", "qmtc2.i"):
        return "qmtc2(%d, %d);" % (reg(args[0]), vreg(args[1]))
    if op in ("qmfc2", "qmfc2.ni", "qmfc2.i"):
        return "qmfc2(%d, %d);" % (reg(args[0]), vreg(args[1]))
    if op in ("ctc2", "ctc2.ni", "ctc2.i"):
        return "vu0_ctc2(%d, (u32)%s);" % (vreg(args[1], "vi"), R(reg(args[0])))
    if op in ("cfc2", "cfc2.ni", "cfc2.i"):
        return wr(reg(args[0]), "SX32(vu0_cfc2(%d))" % vreg(args[1], "vi"))
    # FPU
    if op == "mtc1":
        return "F(%d) = u2f((u32)%s);" % (freg(args[1]), R(reg(args[0])))
    if op == "mfc1":
        return wr(reg(args[0]), "SX32(f2u(F(%d)))" % freg(args[1]))
    if op == "mov.s":
        return "F(%d) = F(%d);" % (freg(args[0]), freg(args[1]))
    if op == "neg.s":
        return "F(%d) = u2f(f2u(F(%d)) ^ 0x80000000u);" % (freg(args[0]), freg(args[1]))
    if op == "abs.s":
        return "F(%d) = u2f(f2u(F(%d)) & 0x7FFFFFFFu);" % (freg(args[0]), freg(args[1]))
    if op in ("add.s", "sub.s", "mul.s", "div.s"):
        fn = {"add.s": "fpu_add", "sub.s": "fpu_sub", "mul.s": "fpu_mul", "div.s": "fpu_div"}[op]
        return "F(%d) = %s(F(%d), F(%d));" % (freg(args[0]), fn, freg(args[1]), freg(args[2]))
    if op == "sqrt.s":
        return "F(%d) = fpu_sqrt(F(%d));" % (freg(args[0]), freg(args[1]))
    if op == "mula.s":
        return "ee.facc = fpu_mul(F(%d), F(%d));" % (freg(args[0]), freg(args[1]))
    if op == "adda.s":
        return "ee.facc = fpu_add(F(%d), F(%d));" % (freg(args[0]), freg(args[1]))
    if op == "suba.s":
        return "ee.facc = fpu_sub(F(%d), F(%d));" % (freg(args[0]), freg(args[1]))
    if op == "madd.s":
        return "F(%d) = fpu_add(ee.facc, fpu_mul(F(%d), F(%d)));" % (freg(args[0]), freg(args[1]), freg(args[2]))
    if op == "msub.s":
        return "F(%d) = fpu_sub(ee.facc, fpu_mul(F(%d), F(%d)));" % (freg(args[0]), freg(args[1]), freg(args[2]))
    if op == "madda.s":
        return "ee.facc = fpu_add(ee.facc, fpu_mul(F(%d), F(%d)));" % (freg(args[0]), freg(args[1]))
    if op == "cvt.s.w":
        return "F(%d) = (float)(s32)f2u(F(%d));" % (freg(args[0]), freg(args[1]))
    if op == "cvt.w.s":
        return "F(%d) = u2f((u32)fpu_cvtw(F(%d)));" % (freg(args[0]), freg(args[1]))
    if op in ("c.lt.s", "c.le.s", "c.eq.s", "c.f.s"):
        e = {"c.lt.s": "<", "c.le.s": "<=", "c.eq.s": "=="}.get(op)
        if not e:
            return "ee.fcc = 0;"
        return "ee.fcc = fpu_in(F(%d)) %s fpu_in(F(%d));" % (freg(args[0]), e, freg(args[1]))
    die("%s: unsupported instruction '%s' at %08x" % (name, text, addr))


def branch_cond(op, args):
    if op in ("b", "j"):
        return "1"
    if op in ("beq", "beql"):
        return "%s == %s" % (R(reg(args[0])), R(reg(args[1])))
    if op in ("bne", "bnel"):
        return "%s != %s" % (R(reg(args[0])), R(reg(args[1])))
    if op in ("beqz", "beqzl"):
        return "%s == 0" % R(reg(args[0]))
    if op in ("bnez", "bnezl"):
        return "%s != 0" % R(reg(args[0]))
    if op == "blez":
        return "(s64)%s <= 0" % R(reg(args[0]))
    if op == "bgtz":
        return "(s64)%s > 0" % R(reg(args[0]))
    if op == "bltz":
        return "(s64)%s < 0" % R(reg(args[0]))
    if op == "bgez":
        return "(s64)%s >= 0" % R(reg(args[0]))
    if op in ("bc1t", "bc1tl"):
        return "ee.fcc"
    if op in ("bc1f", "bc1fl"):
        return "!ee.fcc"
    die("branch " + op)


def body_of(name, insns, local_funcs, callees):
    out = []
    targets = set()
    for addr, word, text in insns:
        p = text.split(None, 1)
        op = p[0]
        if op in BRANCH and op != "j":
            targets.add(label_addr(p[1].split(",")[-1]))
        if op == "j":
            targets.add(label_addr(p[1]))
        if text.startswith(".word") and (int(text.split()[1], 0) >> 26) == 2:
            w = int(text.split()[1], 0)
            targets.add(((addr & 0xF0000000) | ((w & 0x3FFFFFF) << 2)))
    i = 0
    n = len(insns)
    while i < n:
        addr, word, text = insns[i]
        if addr in targets:
            out.append("L_%08X:;" % addr)
        p = text.split(None, 1)
        op = p[0]
        args = [a.strip() for a in p[1].split(",")] if len(p) > 1 else []
        is_jword = text.startswith(".word") and (int(text.split()[1], 0) >> 26) == 2
        if op in BRANCH or is_jword or op in ("jal", "jr", "jalr"):
            delay = translate_one(insns[i + 1], name) if i + 1 < n else ""
            if (i + 1) < n and insns[i + 1][0] in targets:
                die("%s: a branch target sits in a delay slot at %08x" % (name, insns[i + 1][0]))
            if op == "jr":
                if reg(args[0]) != 31:
                    die("%s: jr through a register other than ra" % name)
                out.append("{ %s return; }" % delay)
            elif op == "jal":
                callee = args[0]
                callees.add(callee)
                call = ("F_%s();" % callee) if callee in local_funcs else ("T_%s();" % callee)
                out.append("{ %s R(31) = 0x%08Xu; %s }" % (delay, addr + 8, call))
            elif op == "jalr":
                die("%s: jalr not supported" % name)
            elif is_jword:
                w = int(text.split()[1], 0)
                tgt = (addr & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                out.append("{ %s goto L_%08X; }" % (delay, tgt))
            else:
                tgt = label_addr(args[-1]) if op != "j" else label_addr(args[0])
                cond = branch_cond(op, args)
                if op.endswith("l") and op not in ("b",):
                    out.append("if (%s) { %s goto L_%08X; }" % (cond, delay, tgt))
                else:
                    out.append("{ int c_ = %s; %s if (c_) goto L_%08X; }" % (cond, delay, tgt))
            i += 2
            continue
        c = translate_one(insns[i], name)
        if c:
            out.append(c)
        i += 1
    # labels that point past the last instruction (none expected)
    return out, targets


def read_commit(root):
    """HEAD commit of the decomp checkout, read from .git directly (git refuses UNC paths it does not own)."""
    g = os.path.join(root, ".git")
    try:
        head = open(os.path.join(g, "HEAD")).read().strip()
        if not head.startswith("ref:"):
            return head[:10]
        ref = head[4:].strip()
        p = os.path.join(g, *ref.split("/"))
        if os.path.exists(p):
            return open(p).read().strip()[:10]
        for ln in open(os.path.join(g, "packed-refs")):
            if ln.strip().endswith(ref):
                return ln.split()[0][:10]
    except OSError:
        pass
    return "unknown"


def arg_check(name, insns, nint, nflt):
    """Warn when the assembly reads an argument register the C signature does not pass."""
    written = set()
    warn = set()
    for addr, word, text in insns:
        p = text.split(None, 1)
        if len(p) < 2:
            continue
        ops = [a.strip() for a in p[1].split(",")]
        dst = ops[0] if p[0] not in ("sw", "sh", "sb", "sd", "sq", "swc1", "sqc2", "beq", "bne", "beqz", "bnez",
                                      "bltz", "bgez", "blez", "bgtz", "jr", "qmtc2.ni", "ctc2.ni", "mtc1") else None
        srcs = ops[1:] if dst else ops
        if p[0] == "mtc1":
            srcs = ops[:1]
        for s_ in srcs:
            for r in re.findall(r"\$(a[0-3]|t[0-3]|f1[2-9])\b", s_):
                if r not in written:
                    idx = ["a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3"]
                    if r in idx and idx.index(r) >= nint:
                        warn.add(r)
                    if r.startswith("f") and int(r[1:]) - 12 >= nflt:
                        warn.add(r)
        if dst:
            m = re.match(r"\$(\w+)", dst)
            if m:
                written.add(m.group(1))
        if p[0] in ("jal",):
            break
    return sorted(warn)


PROTOS_OUT = None


def main():
    global PROTOS_OUT
    args = sys.argv[1:]
    if "--protos" in args:
        i = args.index("--protos")
        PROTOS_OUT = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        die("usage: gen_vu0.py <decomp-root> <out-dir> [--protos <vu0_protos.h>]")
    root, outdir = args
    os.makedirs(outdir, exist_ok=True)
    protos = load_protos(root)
    for ln in open(os.path.join(root, "config", "symbol_addrs.txt"), encoding="utf-8", errors="replace"):
        m = re.match(r"\s*(\w+)\s*=\s*0x([0-9A-Fa-f]+)", ln)
        if m:
            SYMS[m.group(1)] = int(m.group(2), 16)
    commit = read_commit(root)
    paths = {}
    for p in glob.glob(os.path.join(root, "asm", "matchings", "**", "*.s"), recursive=True):
        paths.setdefault(os.path.basename(p)[:-2], p)
    local = set(FUNCS)
    callees = set()
    bodies = {}
    for f in FUNCS:
        if f not in paths:
            die("no matched assembly for " + f)
        if f not in protos:
            die("no C prototype for " + f)
        insns = parse_s(paths[f])
        pl = split_params(protos[f][1]) + EXTRA_ARGS.get(f, [])
        extra = arg_check(f, insns, sum(1 for x in pl if param_kind(x) == "i"), sum(1 for x in pl if param_kind(x) == "f"))
        if extra:
            print("gen_vu0: warning: %s reads %s beyond its C signature" % (f, " ".join(extra)))
        bodies[f], _ = body_of(f, insns, local, callees)
        bodies[f] = (bodies[f], insns[0][0])
    native = sorted(callees - local)
    o = []
    o.append("/* vu0_funcs.c - GENERATED by gfx/vu0/gen_vu0.py from decomp commit %s. Do not edit. */" % commit)
    o.append('#include "vu0_rt.h"\n')
    # native callee prototypes + thunks
    for c in native:
        if c not in protos:
            die("no prototype for callee " + c)
        ret, params = protos[c]
        pl = split_params(params)
        rk = ret_kind(ret)
        cret = {"v": "void", "f": "float", "i": "u32", "p": "u32"}[rk]
        o.append("extern %s %s(%s);" % (cret, c, ", ".join(("float" if param_kind(x) == "f" else "u32") for x in pl) or "void"))
    o.append("")
    for f in FUNCS:
        o.append("static void F_%s(void);" % f)
    o.append("")
    for c in native:
        ret, params = protos[c]
        pl = split_params(params)
        ia = iter(INT_ARG_REGS); fa = iter(FLT_ARG_REGS)
        argv = []
        for x in pl:
            argv.append("F(%d)" % next(fa) if param_kind(x) == "f" else "(u32)R(%d)" % next(ia))
        call = "%s(%s)" % (c, ", ".join(argv))
        rk = ret_kind(ret)
        if rk == "v":
            stmt = call + ";"
        elif rk == "f":
            stmt = "F(0) = %s;" % call
        else:
            stmt = "R(2) = SX32(%s);" % call
        o.append("static void T_%s(void) { VU0_NATIVE_BEGIN(); %s VU0_NATIVE_END(); }" % (c, stmt))
    o.append("")
    manifest = []
    for f in FUNCS:
        ret, params = protos[f]
        pl = split_params(params) + EXTRA_ARGS.get(f, [])
        rk = ret_kind(ret)
        cret = {"v": "void", "f": "float", "i": "u32", "p": "u32"}[rk]
        body, start = bodies[f]
        o.append("/* %s at %08X */" % (f, start))
        o.append("static void F_%s(void)\n{" % f)
        for ln in body:
            o.append("    " + ln)
        o.append("}\n")
        ia = iter(INT_ARG_REGS); fa = iter(FLT_ARG_REGS)
        sets = []
        for k, x in enumerate(pl):
            if param_kind(x) == "f":
                sets.append("F(%d) = p%d;" % (next(fa), k))
            else:
                sets.append("R(%d) = SX32(p%d);" % (next(ia), k))
        o.append("%s %s(%s)\n{" % (cret, f, c_param_list(pl)))
        o.append("    VU0_ENTER();")
        if sets:
            o.append("    " + " ".join(sets))
        o.append("    F_%s();" % f)
        if rk == "v":
            o.append("    VU0_LEAVE();")
        elif rk == "f":
            o.append("    { float r_ = F(0); VU0_LEAVE(); return r_; }")
        else:
            o.append("    { u32 r_ = (u32)R(2); VU0_LEAVE(); return r_; }")
        o.append("}\n")
        manifest.append("%08X %s" % (start, f))
    open(os.path.join(outdir, "vu0_funcs.c"), "w", newline="\n").write("\n".join(o) + "\n")
    # The exact C prototypes, for the port generator's declarations and call remap (names and kinds only).
    ph = ["/* vu0_protos.h - GENERATED by gfx/vu0/gen_vu0.py (decomp %s). The prototypes of the functions lane G" % commit,
          " * provides in C. Every pointer/integer is 32-bit and every float is a float (cdecl); arguments the",
          " * decomp's C declarations omit are included (see EXTRA_ARGS in gen_vu0.py). */",
          "#ifndef DOTR_VU0_PROTOS_H", "#define DOTR_VU0_PROTOS_H", "", "#include <stdint.h>", ""]
    for f in FUNCS:
        ret, params = protos[f]
        pl = split_params(params) + EXTRA_ARGS.get(f, [])
        cret = {"v": "void", "f": "float", "i": "int32_t", "p": "void *"}[ret_kind(ret)]
        args = []
        for k, x in enumerate(pl):
            nm = re.findall(r"(\w+)\s*(?:\[[^\]]*\])*\s*$", x.replace(")", " ").replace("(", " "))
            nm = nm[0] if nm and nm[0] not in ("void", "s32", "u32", "f32", "float", "int") else "p%d" % k
            if param_kind(x) == "f":
                args.append("float " + nm)
            elif "*" in x or "[" in x or "(" in x:
                args.append("void *" + nm)
            else:
                args.append("int32_t " + nm)
        ph.append("%s %s(%s);" % (cret, f, ", ".join(args) or "void"))
    ph += ["", "#endif", ""]
    if PROTOS_OUT:
        open(PROTOS_OUT, "w", newline="\n").write("\n".join(ph))
    open(os.path.join(outdir, "FUNCTIONS.txt"), "w", newline="\n").write(
        "# Functions lane G provides in C (gfx/vu0); the port generator cuts them from the main program.\n"
        "# address name   (decomp %s)\n" % commit + "\n".join(manifest) + "\n")
    print("gen_vu0: %d functions, %d native callees (%s) -> %s" % (len(FUNCS), len(native), " ".join(native), outdir))


if __name__ == "__main__":
    main()
