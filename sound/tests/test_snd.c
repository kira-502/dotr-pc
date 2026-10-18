/* test_snd.c - windowless checks of the IOP emulator and the SPU2. Run: snd_tests (exit code 0 = all passed). */
#include <stdio.h>
#include <stdlib.h>
#include "../iop.h"
#include "../snd.h"

static int fails, checks;
#define CHECKEQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); checks++; \
    if (_a != _b) { fails++; printf("  FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 pc_at;
static void emit(u32 w) { iop_write32(pc_at, w); pc_at += 4; }
#define R_(op, rs, rt, rd, sa, fn) (((u32)(op) << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) | ((u32)(rd) << 11) | ((u32)(sa) << 6) | (fn))
#define I_(op, rs, rt, imm) (((u32)(op) << 26) | ((u32)(rs) << 21) | ((u32)(rt) << 16) | ((u32)(imm) & 0xFFFF))
enum { ZERO = 0, V0 = 2, V1 = 3, A0 = 4, A1 = 5, T0 = 8, T1 = 9, RA = 31 };
#define JR_RA R_(0, RA, 0, 0, 0, 8)
#define NOP 0

static void reset(void)
{
    snd_config c = { 1, NULL };
    snd_init(&c);
}

static void test_cpu(void)
{
    u32 v;
    printf("iop cpu (ALU, mult/div, branch delay, load delay, unaligned loads)\n");
    reset();
    /* v0 = ((0x1234 << 16) | 0x5678) * 3 / 7 ; v1 = remainder */
    pc_at = 0x1000;
    emit(I_(0x0F, 0, T0, 0x1234));        /* lui t0, 0x1234 */
    emit(I_(0x0D, T0, T0, 0x5678));       /* ori t0, t0, 0x5678 */
    emit(I_(0x09, 0, T1, 3));             /* addiu t1, zero, 3 */
    emit(R_(0, T0, T1, 0, 0, 0x19));      /* multu t0, t1 */
    emit(R_(0, 0, 0, T0, 0, 0x12));       /* mflo t0 */
    emit(I_(0x09, 0, T1, 7));
    emit(R_(0, T0, T1, 0, 0, 0x1B));      /* divu t0, t1 */
    emit(R_(0, 0, 0, V0, 0, 0x12));       /* mflo v0 */
    emit(R_(0, 0, 0, V1, 0, 0x10));       /* mfhi v1 */
    emit(JR_RA);
    emit(NOP);
    v = iop_call(0x1000, 0, 0, 0, 0, 0x1F0000);
    CHECKEQ(v, (0x12345678u * 3u) / 7u);
    CHECKEQ(iop.r[V1], 0);   /* registers are restored after the call */
    /* branch delay slot runs; load delay: the instruction after a load still sees the old value */
    iop_write32(0x2000, 0xAABBCCDD);
    pc_at = 0x1100;
    emit(I_(0x09, 0, V0, 5));             /* v0 = 5 */
    emit(I_(0x09, 0, A1, 0x2000));        /* a1 = 0x2000 */
    emit(I_(0x23, A1, V0, 0));            /* lw v0, 0(a1) */
    emit(R_(0, V0, 0, V1, 0, 0x21));      /* addu v1, v0, zero   -> old v0 (5) */
    emit(I_(0x04, 0, 0, 2));              /* beq zero, zero, +2 */
    emit(R_(0, V1, V1, V1, 0, 0x21));     /* delay slot: v1 = v1 + v1 = 10 */
    emit(I_(0x09, 0, V1, 99));            /* skipped */
    emit(JR_RA);
    emit(NOP);
    pc_at = 0x1200;                       /* v0 = old v1 test via a separate call */
    v = iop_call(0x1100, 0, 0, 0, 0, 0x1F0000);
    CHECKEQ(v, 0xAABBCCDD);
    /* the load-delay slot instruction overwrites the loaded register with the same value it already had */
    pc_at = 0x1300;
    emit(I_(0x0F, 0, V0, 0x0000));        /* lui v0, 0 (v0 = 0) */
    emit(I_(0x23, V0, V0, 0x2000));       /* lw v0, 0x2000(v0) */
    emit(I_(0x0F, 0, V0, 0x0000));        /* lui v0, 0 in the delay slot: the load is lost */
    emit(JR_RA);
    emit(NOP);
    v = iop_call(0x1300, 0, 0, 0, 0, 0x1F0000);
    CHECKEQ(v, 0);
    /* lwl/lwr pair: unaligned word at 0x2001 */
    iop_write32(0x2004, 0x11223344);
    pc_at = 0x1400;
    emit(I_(0x09, 0, A1, 0x2001));
    emit(I_(0x22, A1, V0, 3));            /* lwl v0, 3(a1) */
    emit(I_(0x26, A1, V0, 0));            /* lwr v0, 0(a1) */
    emit(NOP);
    emit(JR_RA);
    emit(NOP);
    v = iop_call(0x1400, 0, 0, 0, 0, 0x1F0000);
    CHECKEQ(v, 0x44AABBCC);
}

static void spu_w(u32 off, u16 v) { iop_write16(0x1F900000u + off, v); }

static void test_spu_dma_and_stream(void)
{
    s16 out[512 * 2];
    u32 i;
    int bad = 0;
    printf("spu2 (DMA into sound RAM, AutoDMA stream through both cores)\n");
    reset();
    /* channel 7 (core 1) normal transfer of 64 bytes to sound RAM word 0x1000 */
    for (i = 0; i < 64; i++) iop_ram[0x3000 + i] = (u8)i;
    spu_w(0x400 + 0x1A8, 0); spu_w(0x400 + 0x1AA, 0x1000);
    spu_w(0x400 + 0x19A, 0xC020);                 /* ATTR: core on, unmuted, DMA write mode */
    iop_write32(0x1F801500, 0x3000); iop_write32(0x1F801504, (1u << 16) | 16); iop_write32(0x1F801508, 0x01000201);
    CHECKEQ(spu2_mem[0x1000], 0x0100);
    CHECKEQ(spu2_mem[0x101F], 0x3F3E);
    CHECKEQ(spu2_read(0x1F900744) & 0x80, 0x80);
    /* AutoDMA: core 0 streams 256 L + 256 R samples from IOP RAM; core 1 passes core 0 through */
    for (i = 0; i < 256; i++) {
        s16 l = (s16)(i * 100), r = (s16)(-(s16)i * 50);
        memcpy(iop_ram + 0x10000 + i * 2, &l, 2);
        memcpy(iop_ram + 0x10200 + i * 2, &r, 2);
    }
    spu_w(0x19A, 0xC000);                       /* ATTR core 0: on, unmuted */
    spu_w(0x1B0, 1);                            /* ADMAS core 0 */
    spu_w(0x198, 0xC0);                         /* MMIX core 0: sound data input to dry L/R */
    spu_w(0x76C, 0x7FFF); spu_w(0x76E, 0x7FFF); /* BVOL */
    spu_w(0x760, 0x3FFF); spu_w(0x762, 0x3FFF); /* MVOL core 0 */
    spu_w(0x400 + 0x198, 0x0C);                 /* MMIX core 1: external input to dry */
    spu_w(0x790, 0x7FFF); spu_w(0x792, 0x7FFF); /* AVOL core 1 */
    spu_w(0x788, 0x3FFF); spu_w(0x78A, 0x3FFF); /* MVOL core 1 */
    iop_write32(0x1F8010C0, 0x10000); iop_write32(0x1F8010C4, (16u << 16) | 16); iop_write32(0x1F8010C8, 0x01000201);
    spu2_render(out, 300);
    for (i = 0; i < 256; i++) {
        s32 wl = (s16)(i * 100), wr = (s16)(-(s16)i * 50), gl = out[i * 2], gr = out[i * 2 + 1];
        /* four 0x7FFF-ish volume stages each lose a little, as on the console */
        if (abs(gl - wl) > 4 + abs(wl) / 2048 || abs(gr - wr) > 4 + abs(wr) / 2048) {
            if (!bad) printf("  first difference at %u: got %d/%d, want %d/%d\n", i, gl, gr, wl, wr);
            bad++;
        }
    }
    CHECKEQ(bad, 0);
    CHECKEQ(out[280 * 2], 0);   /* stream ended: silence, not stale data */
}

static void test_voice(void)
{
    s16 out[200 * 2];
    int i, nonzero = 0;
    printf("spu2 voice (ADPCM block, key on, loop end flag, ENDX)\n");
    reset();
    /* one ADPCM block at word 0x2000: filter 0, shift 0, flags loop end (no repeat), nibbles 1..7 repeating */
    spu2_mem[0x2000] = (u16)(0x01 << 8);
    for (i = 1; i < 8; i++) spu2_mem[0x2000 + i] = 0x7531;
    spu_w(0x400 + 0x1C0, 0); spu_w(0x400 + 0x1C2, 0x2000);       /* SSA voice 0, core 1 */
    spu_w(0x400 + 0x00, 0x3FFF); spu_w(0x400 + 0x02, 0x3FFF);   /* VOL */
    spu_w(0x400 + 0x04, 0x1000);                                /* pitch 1.0 */
    spu_w(0x400 + 0x06, 0x000F);                                /* fastest linear attack, sustain level max */
    spu_w(0x400 + 0x08, 0x0000);
    spu_w(0x400 + 0x188, 1); spu_w(0x400 + 0x190, 1);           /* VMIXL/R voice 0 */
    spu_w(0x400 + 0x198, 0x0C00);                               /* MMIX core 1: voices to dry */
    spu_w(0x788, 0x3FFF); spu_w(0x78A, 0x3FFF);
    spu_w(0x400 + 0x19A, 0x8000);                               /* ATTR: on but muted */
    spu_w(0x400 + 0x1A0, 1);                                    /* KON voice 0 */
    spu2_render(out, 20);
    for (i = 0; i < 20; i++) if (out[i * 2]) nonzero++;
    CHECKEQ(nonzero, 0);                                         /* bit 14 clear mutes the voices */
    spu_w(0x400 + 0x19A, 0xC000);
    spu_w(0x400 + 0x1A0, 1);
    spu2_render(out, 200);
    for (i = 0; i < 200; i++) if (out[i * 2]) nonzero++;
    CHECK(nonzero > 10);
    CHECKEQ(spu2_read(0x1F900740) & 1, 1);                       /* ENDX voice 0 */
    CHECK(out[190 * 2] == 0);                                    /* ended without repeat: silent */
}

static void test_reverb(void)
{
    static s16 out[4000 * 2];
    int i, tail = 0;
    printf("spu2 reverb (work area written, echo continues after the dry sound ends)\n");
    reset();
    spu2_mem[0x2000] = (u16)(0x01 << 8);
    for (i = 1; i < 8; i++) spu2_mem[0x2000 + i] = 0x7531;
    spu_w(0x400 + 0x1C0, 0); spu_w(0x400 + 0x1C2, 0x2000);
    spu_w(0x400 + 0x00, 0x3FFF); spu_w(0x400 + 0x02, 0x3FFF);
    spu_w(0x400 + 0x04, 0x1000); spu_w(0x400 + 0x06, 0x000F);
    spu_w(0x400 + 0x18C, 1); spu_w(0x400 + 0x194, 1);            /* VMIXEL/ER: voice 0 to the effect input */
    spu_w(0x400 + 0x198, 0x0300);                                /* MMIX: voices wet only */
    spu_w(0x400 + 0x2E0, 0x000E); spu_w(0x400 + 0x2E2, 0x0000);  /* ESA 0xE0000 */
    spu_w(0x400 + 0x33C, 0x000F);                                /* EEA 0xFFFFF */
    /* a simple room: comb taps at 1000/2000 words, same-side reflection at 500 */
    spu_w(0x400 + 0x2EC, 0); spu_w(0x400 + 0x2EE, 3000);         /* IIR_DEST_A0 */
    spu_w(0x400 + 0x2F0, 0); spu_w(0x400 + 0x2F2, 3100);         /* IIR_DEST_A1 */
    spu_w(0x400 + 0x304, 0); spu_w(0x400 + 0x306, 2500);         /* IIR_SRC_A0 */
    spu_w(0x400 + 0x308, 0); spu_w(0x400 + 0x30A, 2600);
    spu_w(0x400 + 0x2F4, 0); spu_w(0x400 + 0x2F6, 2000);         /* ACC_SRC_A0 */
    spu_w(0x400 + 0x2F8, 0); spu_w(0x400 + 0x2FA, 2100);
    spu_w(0x788 + 0x14, 0x6000);                                 /* IIR_ALPHA */
    spu_w(0x788 + 0x16, 0x7000);                                 /* ACC_COEF_A */
    spu_w(0x788 + 0x1E, 0x4000);                                 /* IIR_COEF (wall) */
    spu_w(0x788 + 0x24, 0x7FFF); spu_w(0x788 + 0x26, 0x7FFF);    /* IN_COEF */
    spu_w(0x788 + 0x4, 0x7FFF); spu_w(0x788 + 0x6, 0x7FFF);      /* EVOL */
    spu_w(0x788, 0x3FFF); spu_w(0x78A, 0x3FFF);
    spu_w(0x400 + 0x19A, 0xC080);                                /* ATTR: on, unmuted, effects on */
    spu_w(0x400 + 0x1A0, 1);
    spu2_render(out, 4000);
    for (i = 1000; i < 4000; i++) if (out[i * 2]) tail++;
    CHECK(tail > 100);
    {
        int written = 0;
        for (i = 0xE0000; i < 0xE2000; i++) if (spu2_mem[i]) written++;
        CHECK(written > 10);
    }
}

float snd_out_ratio(int queued);
int snd_out_engine_check(void);
static int sim_queue_hitch(int use_ratio, double wobble, double hitch, int *underruns, int *drops);

/* The output queue over 10 minutes of the game's 59.94 Hz fields (800 frames each) against a 48 kHz card that
 * plays at snd_out_ratio: it must never run dry (a gap) nor reach the drop limit (a skipped field), also when the
 * game's clock wobbles by +-0.2%. Without the ratio (1.0) the queue drains: the check that the model sees the bug. */
static int sim_queue(int use_ratio, double wobble, int *underruns, int *drops)
{
    return sim_queue_hitch(use_ratio, wobble, 0, underruns, drops);
}

static int sim_queue_hitch(int use_ratio, double wobble, double hitch, int *underruns, int *drops)
{
    double t = 0, played = 0, next_field = 0;   /* seconds; frames of the current buffer already played */
    int queued = 0, k;
    *underruns = *drops = 0;
    for (k = 0; k < 600 * 600; k++) {          /* 10 minutes in steps of 1/600 s */
        double dt = 1.0 / 600.0, rate = 48000.0 * (use_ratio ? snd_out_ratio(queued) : 1.0);
        t += dt;
        while (t >= next_field) {               /* the game hands over a field */
            next_field += (1.0 / 59.94) * (1.0 + wobble * ((k / 6000) % 2 ? 1 : -1));
            if (hitch > 0 && k % 36000 == 18000) next_field += hitch;   /* one slow field a minute */
            if (queued >= 10 - 2) (*drops)++; else queued++;
        }
        if (queued > 0) {
            played += rate * dt;
            while (played >= 800 && queued > 0) { played -= 800; queued--; }
        } else if (t > 1.0) (*underruns)++;
    }
    return queued;
}

static void test_output_pacing(void)
{
    int under, drops;
    printf("sound out: queue stays fed at the 59.94 Hz field clock (10 min simulated) + XAudio2 engine\n");
    sim_queue(1, 0.0, &under, &drops);
    CHECK(under == 0 && drops == 0);
    sim_queue(1, 0.002, &under, &drops);
    CHECK(under == 0 && drops == 0);
    sim_queue(0, 0.0, &under, &drops);
    CHECK(under > 0);                            /* the old fixed rate ran dry */
    sim_queue_hitch(1, 0.0, 0.075 - 1.0 / 59.94, &under, &drops);
    CHECK(under == 0);                           /* a 75 ms field (the M1g log's slowest) leaves no gap */
    CHECK(snd_out_ratio(5) > 0.9989f && snd_out_ratio(5) < 0.9991f);
    CHECK(snd_out_ratio(0) >= 0.996f && snd_out_ratio(0) < snd_out_ratio(5));
    CHECK(snd_out_ratio(9) <= 1.002f && snd_out_ratio(9) > snd_out_ratio(5));
    CHECK(snd_out_engine_check());
}

int main(void)
{
    test_cpu();
    test_spu_dma_and_stream();
    test_voice();
    test_reverb();
    test_output_pacing();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
