"""gen_vu1x.py - translate the game's VU1 programs into C at build time (output goes to the build folder only).

The micro memory image is rebuilt from the owner's disc (vu1_image.py). Every instruction pair becomes a call of
vu1.c's pair_step with its constant instruction words, so the compiler specializes the interpreter's own code per
instruction. On top of that, straight-line blocks (from each branch target or entry to the next branch or end)
are chained into one function each, MAC results no instruction can see skip the flag queue (dead_mac), and stall
checks that cannot fire are removed at translate time:

  - a result is ready 4 cycles after issue and every pair takes at least one cycle, so a read 4 or more pairs
    after the register's latest write in the block cannot stall;
  - entering a block at cycle c0, nothing written before it is ready later than c0 + 3, so from the block's
    fourth pair on, registers not written inside the block cannot stall either.

The write sets come from a conservative decoder: anything it does not know is treated as writing every register,
which only keeps checks. Results stay identical to the interpreter (checked in lockstep, DOTR_VU1X_VERIFY=1).
The image's hash is recorded; at run time the translation is used only while VU1 micro memory has that hash.

    python gen_vu1x.py <game.iso> <out/vu1x_prog.inc>
With no disc, an empty file is written and the game uses the interpreter.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vu1_image  # noqa: E402

PAIRS = 2048
ACC = 32
ALL = (1 << 33) - 2          # every VF register but VF0, and ACC
MAX_BLOCK = 64


def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def imm11(w):
    v = w & 0x7FF
    return v - 0x800 if v >= 0x400 else v


def upper_writes(hi):
    """VF registers (bit 32 = ACC) the upper instruction writes; ALL when unsure."""
    op, ft, fd = hi & 0x3F, (hi >> 16) & 0x1F, (hi >> 6) & 0x1F
    if op < 0x30:
        return (1 << fd) if fd else 0               # FMAC to fd (incl. OPMSUB, MAX, MINI)
    if op < 0x3C:
        return ALL
    ext = ((hi >> 4) & 0x7C) | (hi & 3)
    if ext in (0x2F, 0x1F):                         # NOP, CLIP
        return 0
    if 0x10 <= ext <= 0x17 or ext == 0x1D:          # ITOF, FTOI, ABS
        return (1 << ft) if ft else 0
    if ext < 0x10 or 0x18 <= ext <= 0x1C or ext == 0x1E or 0x20 <= ext <= 0x2A or ext in (0x2C, 0x2D, 0x2E):
        return 1 << ACC                             # the ...A forms and OPMULA write ACC
    return ALL


def lower_writes(lo, hi):
    if hi & 0x80000000:                             # I bit: the lower word is a constant
        return 0
    op, ft = lo >> 25, (lo >> 16) & 0x1F
    if op == 0x40:
        sop = lo & 0x3F
        if sop < 0x3C:
            return 0                                # integer ALU
        ext = ((lo >> 4) & 0x7C) | (lo & 3)
        if ext in (0x30, 0x31, 0x34, 0x36, 0x3D, 0x40, 0x41, 0x64):
            return (1 << ft) if ft else 0           # MOVE MR32 LQI LQD MFIR RNEXT RGET MFP
        if ext in (0x35, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F, 0x42, 0x43, 0x68, 0x69, 0x6C, 0x7B) or \
                0x70 <= ext <= 0x7E:
            return 0
        return ALL
    if op == 0x00:
        return (1 << ft) if ft else 0               # LQ
    if op in (0x01, 0x04, 0x05, 0x08, 0x09) or 0x10 <= op <= 0x1C or 0x20 <= op <= 0x2F:
        return 0
    return ALL


def lower_branch(lo, hi):
    if hi & 0x80000000:
        return False
    op = lo >> 25
    return op in (0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F)


def upper_sets_mac(hi):
    """True when the upper instruction certainly sets the MAC flags (vu1.c exec_upper -> fmac, not MAX/MINI)."""
    op = hi & 0x3F
    if op < 0x1C:
        return (op >> 2) in (0, 1, 2, 3, 6)
    if op < 0x3C:
        return op in (0x1C, 0x1E) or 0x20 <= op <= 0x2A or op in (0x2C, 0x2D, 0x2E)
    ext = ((hi >> 4) & 0x7C) | (hi & 3)
    if ext < 0x10 or 0x18 <= ext < 0x1C:
        return (ext >> 2) in (0, 1, 2, 3, 6)
    return ext in (0x1C, 0x1E) or 0x20 <= ext <= 0x2A or ext in (0x2C, 0x2D, 0x2E)


def lower_reads_mac(lo, hi):
    """FSEQ FSSET FSAND FSOR (status) and FMEQ FMAND FMOR (MAC) see what the MAC results set."""
    return not (hi & 0x80000000) and (lo >> 25) in (0x14, 0x15, 0x16, 0x17, 0x18, 0x1A, 0x1B)


def dead_mac(lo, hi, used):
    """Pairs whose MAC result no instruction can ever see, so the run skips queueing it.

    A flag read at pair r sees writer q's result unless a later writer w lies between them at least 4 pairs before r
    (a pair takes at least one cycle, so w has landed by then). When a program ends every pending result lands, so
    any later writer hides q there. Every path from q is searched over (pc, pending branch, pairs since the first
    later writer); an unknown target (JR/JALR, a branch or E-bit in a delay slot, leaving the image) counts as a
    read unless a later writer has landed by then, and so does the end of a program with no later writer (the next
    program may read the flags)."""
    END = -1

    def step(pc, pend):
        l, h = lo[pc], hi[pc]
        br, ebit = lower_branch(l, h), bool(h & 0x40000000)
        if pend is not None:                       # pc is a delay slot
            if br or ebit:
                return None
            return [(END, None)] if pend == "end" else [(t, None) for t in pend]
        if ebit:
            return [((pc + 1) & 0x7FF, "end")]
        if br:
            op = l >> 25
            if op in (0x24, 0x25):
                return None
            tgt = (pc + 1 + imm11(l)) & 0x7FF
            return [((pc + 1) & 0x7FF, (tgt,) if op in (0x20, 0x21) else (tgt, (pc + 2) & 0x7FF))]
        return [((pc + 1) & 0x7FF, None)]

    def after(pc, since):           # pairs since the first later writer (landed results stay hidden), next pair
        if since >= 0:
            return min(since + 1, 4)
        return 1 if upper_sets_mac(hi[pc]) else -1

    dead = set()
    for q in range(used):
        if not upper_sets_mac(hi[q]):
            continue
        first = step(q, None)
        live = first is None
        todo = [] if live else [(pc, pend, -1) for pc, pend in first]
        seen = set()
        while todo and not live:
            pc, pend, since = todo.pop()
            if pc == END:
                live = since < 0
                continue
            if pc >= used:
                live = since < 4
                continue
            if (pc, pend, since) in seen:
                continue
            seen.add((pc, pend, since))
            if lower_reads_mac(lo[pc], hi[pc]) and since < 4:
                live = True
                break
            nxt = step(pc, pend)
            s = after(pc, since)
            if nxt is None:           # unknown next pc: harmless once a later writer has landed for whatever comes
                live = s < 4
                continue
            todo += [(npc, npend, s) for npc, npend in nxt]
        if not live:
            dead.add(q)
    return dead


def generate(img):
    lo = [struct.unpack_from("<I", img, i * 8)[0] for i in range(PAIRS)]
    hi = [struct.unpack_from("<I", img, i * 8 + 4)[0] for i in range(PAIRS)]
    used = min(PAIRS, len(img.rstrip(b"\0")) // 8 + 1)
    nomac = dead_mac(lo, hi, used)
    leaders = {0}
    for i in range(used):
        if lower_branch(lo[i], hi[i]):
            op = lo[i] >> 25
            if op not in (0x24, 0x25):
                leaders.add((i + 1 + imm11(lo[i])) & 0x7FF)
            leaders.add((i + 2) & 0x7FF)
        if hi[i] & 0x40000000:
            leaders.add((i + 2) & 0x7FF)
    leaders = sorted(l for l in leaders if l < used)

    out = ["/* generated by gen_vu1x.py from the owner's disc: do not commit, do not edit */",
           "#define VU1X_HASH 0x%08Xu" % fnv1a(img), ""]
    # one function per pair, no skipping: entry at any address
    for i in range(PAIRS):
        out.append("static int px%d(runst *st) { return pair_step(st, %d, 0x%08Xu, 0x%08Xu, 0, 0, %d); }" %
                   (i, i, lo[i], hi[i], i in nomac))
    out.append("")
    # one function per block, with the stall checks that cannot fire removed. The block works on a local copy of
    # the run state (kept in registers; written back on the way out). After its first pair, a pair that follows a
    # pair without branch or E-bit starts with nothing pending: it advances to pc+1 without the generic logic, and
    # the pc check after it is known to pass.
    for L in leaders:
        body = ["static int bx%d(runst *stp)" % L, "{", "    runst s = *stp, *st = &s;", "    int r = 0;"]
        last_write = {}          # register -> pair index (within the block) of its latest write
        j, pc, end = 0, L, False
        while not end and j < MAX_BLOCK and pc < used:
            skip = 0
            for r in range(1, 33):
                w = last_write.get(r)
                if (w is not None and j - w >= 4) or (w is None and j >= 3):
                    skip |= 1 << r
            plain = not lower_branch(lo[pc], hi[pc]) and not (hi[pc] & 0x40000000)
            lin = 1 if (j > 0 and plain) else 0
            body.append("    if (pair_step(st, %d, 0x%08Xu, 0x%08Xu, 0x%Xull, %d, %d)) { r = 1; goto out; }" %
                        (pc, lo[pc], hi[pc], skip, lin, pc in nomac))
            writes = upper_writes(hi[pc]) | lower_writes(lo[pc], hi[pc])
            for r in range(1, 33):
                if (writes >> r) & 1:
                    last_write[r] = j
            if lower_branch(lo[pc], hi[pc]) or (hi[pc] & 0x40000000):
                # the delay slot runs too, then the generic logic has set the next pc
                nxt = pc + 1
                if nxt < used and not lower_branch(lo[nxt], hi[nxt]) and not (hi[nxt] & 0x40000000):
                    body.append("    if (st->pc != %d) goto out;" % nxt)
                    body.append("    if (pair_step(st, %d, 0x%08Xu, 0x%08Xu, 0, 0, %d)) { r = 1; goto out; }" %
                                (nxt, lo[nxt], hi[nxt], nxt in nomac))
                end = True
            else:
                if not lin:
                    body.append("    if (st->pc != %d) goto out;" % (pc + 1))
                pc += 1
                j += 1
        body += ["out:", "    *stp = s;", "    return r;", "}"]
        out += body
    out += ["", "static int (*const px_table[%d])(runst *) = {" % PAIRS]
    out += ["    " + ", ".join("px%d" % j for j in range(i, min(i + 16, PAIRS))) + "," for i in range(0, PAIRS, 16)]
    out += ["};", "static int (*const bx_table[%d])(runst *) = {" % PAIRS]
    lset = set(leaders)
    names = ["bx%d" % j if j in lset else "0" for j in range(PAIRS)]
    out += ["    " + ", ".join(names[i:i + 16]) + "," for i in range(0, PAIRS, 16)]
    out += ["};", "",
            "static void run_x(void)", "{", "    runst st;", "    unsigned int csr = run_begin(&st);",
            "    for (;;) {",
            "        int (*f)(runst *) = bx_table[st.pc] ? bx_table[st.pc] : px_table[st.pc];",
            "        if (f(&st)) break;",
            "    }",
            "    run_end(&st, csr);", "}", ""]
    return "\n".join(out)


def main():
    iso, dst = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    if not os.path.exists(iso):
        text = "/* no game disc at build time: VU1 runs in the interpreter */\n"
    else:
        text = "#define VU1X_GENERATED_OK 1\n" + generate(vu1_image.build(iso))
    old = open(dst).read() if os.path.exists(dst) else None
    if old != text:
        open(dst, "w", newline="\n").write(text)


if __name__ == "__main__":
    main()
