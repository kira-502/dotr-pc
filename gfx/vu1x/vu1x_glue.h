/* vu1x_glue.h - included by vu1.c after pair_step: choose between the translated VU1 programs (built from the
 * owner's disc by gen_vu1x.py into the build folder; never committed) and the interpreter.
 *
 * The translation is used only while VU1 micro memory hashes to the image it was built from. DOTR_VU1X=0 turns
 * it off; DOTR_VU1X_VERIFY=1 runs every program both ways from the same state (the interpreter's GIF output is
 * hashed, not drawn) and logs any difference in registers, memory, timing or GIF output. */
#ifndef DOTR_VU1X_GLUE_H
#define DOTR_VU1X_GLUE_H

#ifdef VU1X_INC
#include "vu1x_prog.inc"     /* generated: VU1X_GENERATED_OK, VU1X_HASH and run_x(), or a stub */
#endif

static int vu1x_state = -1;  /* -1 unknown (micro memory changed), 0 interpreter, 1 translated */

void vu1_micro_changed(void) { vu1x_state = -1; }

#ifdef VU1X_GENERATED_OK
static u32 vu1x_hash_micro(void)
{
    u32 h = 2166136261u, i;
    for (i = 0; i < VU1_MICRO_BYTES; i++) h = (h ^ vu1.micro[i]) * 16777619u;
    return h;
}

/* ---- lockstep check ---------------------------------------------------------------------------------- */

typedef struct vu1_snap {
    vu1_state v;
    cyc_t vf_ready[33];
    flag_ev fq[FQ_N];
    int fq_head, fq_n, q_busy, p_busy;
    u32 clip_latest, q_pending, p_pending, qflag_pending;
    cyc_t q_ready, p_ready;
    u8 gif[512];
} vu1_snap;

static void vu1_snap_take(vu1_snap *s)
{
    memcpy(&s->v, &vu1, sizeof vu1);
    memcpy(s->vf_ready, vf_ready, sizeof vf_ready); memcpy(s->fq, fq, sizeof fq);
    s->fq_head = fq_head; s->fq_n = fq_n; s->q_busy = q_busy; s->p_busy = p_busy;
    s->clip_latest = clip_latest; s->q_pending = q_pending; s->p_pending = p_pending; s->qflag_pending = qflag_pending;
    s->q_ready = q_ready; s->p_ready = p_ready;
    memcpy(s->gif, gif_state_ptr(), gif_state_size());
}

static void vu1_snap_put(const vu1_snap *s)
{
    memcpy(&vu1, &s->v, sizeof vu1);
    memcpy(vf_ready, s->vf_ready, sizeof vf_ready); memcpy(fq, s->fq, sizeof fq);
    fq_head = s->fq_head; fq_n = s->fq_n; q_busy = s->q_busy; p_busy = s->p_busy;
    clip_latest = s->clip_latest; q_pending = s->q_pending; p_pending = s->p_pending; qflag_pending = s->qflag_pending;
    q_ready = s->q_ready; p_ready = s->p_ready;
    memcpy(gif_state_ptr(), s->gif, gif_state_size());
}

static int vu1_snap_same(const vu1_snap *a, const vu1_snap *b)
{
    /* the sticky status bits (O U S Z, 6-9) collect every MAC result, also the ones the translation drops because no
     * instruction can see them (and none reads the sticky bits: gen_vu1x.py dead_mac); everything else must match */
    vu1_state va = a->v, vb = b->v;
    va.status &= ~0x3C0u; vb.status &= ~0x3C0u;
    return !memcmp(&va, &vb, sizeof va) && !memcmp(a->vf_ready, b->vf_ready, sizeof a->vf_ready) &&
           a->fq_n == b->fq_n && a->q_busy == b->q_busy && a->p_busy == b->p_busy && a->clip_latest == b->clip_latest &&
           a->q_pending == b->q_pending && a->p_pending == b->p_pending && a->q_ready == b->q_ready &&
           a->p_ready == b->p_ready;
}

static void vu1x_verify(void)
{
    static vu1_snap *before, *after_i, *after_x;
    static int checked, bad;
    u32 hi_, hx;
    if (!before) {
        before = (vu1_snap *)malloc(sizeof *before); after_i = (vu1_snap *)malloc(sizeof *after_i);
        after_x = (vu1_snap *)malloc(sizeof *after_x);
        if (!before || !after_i || !after_x) { run_interp(); return; }
    }
    vu1_snap_take(before);
    xk_mode = 2; xk_hash = 0;          /* interpreter: GIF output hashed only */
    run_interp();
    hi_ = xk_hash;
    vu1_snap_take(after_i);
    vu1_snap_put(before);
    xk_mode = 1; xk_hash = 0;          /* translated: drawn and hashed */
    run_x();
    hx = xk_hash;
    xk_mode = 0;
    vu1_snap_take(after_x);
    checked++;
    if (hi_ != hx || !vu1_snap_same(after_i, after_x)) {
        if (bad++ < 20)
            hw_log(0, "vu1x: run %d from %03x differs from the interpreter (gif %08x/%08x, end pc %03x/%03x, cycles %u/%u)",
                   checked, before->v.tpc, hi_, hx, after_i->v.tpc, after_x->v.tpc,
                   (unsigned)(after_i->v.cycles - before->v.cycles), (unsigned)(after_x->v.cycles - before->v.cycles));
    }
    if ((checked & 4095) == 0) hw_log(2, "vu1x: %d runs checked, %d differ", checked, bad);
}
#endif

static void vu1x_run(void)
{
    if (vu1x_state < 0) {
        const char *e = getenv("DOTR_VU1X");
        vu1x_state = 0;
#ifdef VU1X_GENERATED_OK
        if (!(e && e[0] == '0') && vu1x_hash_micro() == VU1X_HASH) vu1x_state = 1;
        {
            static int logged;
            if (!logged++) hw_log(2, "vu1x: %s", vu1x_state ? "translated VU1 programs in use" : "VU1 programs differ from the disc build: interpreter");
        }
#else
        (void)e;
#endif
    }
#ifdef VU1X_GENERATED_OK
    if (vu1x_state == 1) {
        static int verify = -1;
        if (verify < 0) verify = getenv("DOTR_VU1X_VERIFY") && getenv("DOTR_VU1X_VERIFY")[0] == '1';
        if (verify) vu1x_verify(); else run_x();
        return;
    }
#endif
    run_interp();
}

#endif
