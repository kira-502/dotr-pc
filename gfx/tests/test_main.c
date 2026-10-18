/* test_main.c - windowless unit tests of the graphics core. Run: gfx_tests  (exit code 0 = all passed). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../gfx_internal.h"
#include "../gfx.h"

extern int hw_stub_intc[16];
extern int hw_stub_dmac[16];
extern int hw_stub_quiet;

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECKEQ(a, b) do { unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b); checks++; \
    if (_a != _b) { fails++; printf("  FAIL %s:%d: %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static void reset_all(void)
{
    gfx_config c = { 1, 0, NULL };
    gfx_init(&c);
    memset(gfx_test_ram, 0, 32u << 20);
    memset(gfx_test_spr, 0, 16384);
    memset(hw_stub_intc, 0, sizeof hw_stub_intc);
    memset(hw_stub_dmac, 0, sizeof hw_stub_dmac);
}

/* ---- GIF packet builder ---------------------------------------------------------------------------- */

static qword pk[4096];
static int npk;

static void q_put(u64 lo, u64 hi)
{
    pk[npk].w[0] = (u32)lo; pk[npk].w[1] = (u32)(lo >> 32); pk[npk].w[2] = (u32)hi; pk[npk].w[3] = (u32)(hi >> 32);
    npk++;
}
static u64 giftag(u32 nloop, int eop, int pre, u32 prim, u32 flg, u32 nreg) {
    return (u64)nloop | ((u64)eop << 15) | ((u64)pre << 46) | ((u64)prim << 47) | ((u64)flg << 58) | ((u64)nreg << 60);
}
static void ad(u32 reg, u64 v) { q_put(v, reg); }
static void ad_begin(u32 n) { q_put(giftag(n, 1, 0, 0, 0, 1), 0xE); }
static void flush3(void) { gif_write(GIF_PATH3, pk, (u32)npk); npk = 0; }

static u64 frame_reg(u32 fbp, u32 fbw, u32 psm, u32 msk) { return fbp | ((u64)fbw << 16) | ((u64)psm << 24) | ((u64)msk << 32); }
static u64 xyz(int x, int y, u32 z) { return (u32)(x * 16) | ((u64)(u32)(y * 16) << 16) | ((u64)z << 32); }
static u64 rgba(u32 r, u32 g, u32 b, u32 a) { return r | (g << 8) | (b << 16) | ((u64)a << 24) | (0x3F800000ull << 32); }

/* Basic context: 640-wide CT32 frame at 0, scissor 0..639, no tests (ZTE=1 ALWAYS), z buffer off-screen. */
static void setup_ctx(u32 psm)
{
    ad_begin(6);
    ad(0x4C, frame_reg(0, 10, psm, 0));
    ad(0x4E, (0x100) | ((u64)0 << 24) | (1ull << 32));      /* ZBUF at 0x100*2048 words, PSMZ32, ZMSK */
    ad(0x40, (u64)639 << 16 | ((u64)447 << 48));
    ad(0x47, (1ull << 16) | (1ull << 17));                   /* ZTE=1, ZTST=ALWAYS */
    ad(0x18, 0);
    ad(0x46, 1);
    flush3();
}

/* ---- tests ----------------------------------------------------------------------------------------- */

static void test_gs_mem(void)
{
    static const u32 fmts[] = { PSMCT32, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMZ32, PSMZ16, PSMZ16S };
    static const int pw[] = { 64, 64, 64, 128, 128, 64, 64, 64 }, ph[] = { 32, 64, 64, 64, 128, 32, 64, 64 };
    static u8 seen[16384 * 2];
    int f;
    printf("gs memory layout\n");
    reset_all();
    /* known answers */
    CHECKEQ(gs_addr32(0, 1, 1, 0), 1); CHECKEQ(gs_addr32(0, 1, 0, 1), 2); CHECKEQ(gs_addr32(0, 1, 2, 0), 4);
    CHECKEQ(gs_addr32(0, 1, 8, 0), 64); CHECKEQ(gs_addr32(0, 1, 0, 8), 128); CHECKEQ(gs_addr32(0, 1, 16, 0), 256);
    CHECKEQ(gs_addr32(0, 1, 0, 2), 16); CHECKEQ(gs_addr32(0, 2, 64, 0), 2048); CHECKEQ(gs_addr32(32, 1, 0, 0), 2048);
    CHECKEQ(gs_addr32z(0, 1, 0, 0), 24 * 64);
    CHECKEQ(gs_addr16(0, 1, 1, 0), 2); CHECKEQ(gs_addr16(0, 1, 8, 0), 1); CHECKEQ(gs_addr16(0, 1, 16, 0), 2 * 128);
    CHECKEQ(gs_addr16(0, 1, 0, 8), 128); CHECKEQ(gs_addr16s(0, 1, 32, 0), 16 * 128);
    CHECKEQ(gs_addr8(0, 2, 1, 0), 4); CHECKEQ(gs_addr8(0, 2, 8, 0), 2); CHECKEQ(gs_addr8(0, 2, 4, 2), 1);
    CHECKEQ(gs_addr8(0, 2, 16, 0), 256);
    CHECKEQ(gs_addr4(0, 2, 1, 0), 8); CHECKEQ(gs_addr4(0, 2, 32, 0), 2 * 512);
    /* every pixel of a page maps to a distinct address inside that page */
    for (f = 0; f < 8; f++) {
        int x, y, dup = 0, out = 0;
        u32 bpp = (u32)gs_psm_bpp(fmts[f]), units = 8192u * 8 / bpp;
        memset(seen, 0, sizeof seen);
        for (y = 0; y < ph[f]; y++)
            for (x = 0; x < pw[f]; x++) {
                u32 a;
                u32 bw = (bpp < 16) ? 2 : 1;
                switch (fmts[f]) {
                case PSMCT32: a = gs_addr32(0, bw, x, y); break;
                case PSMZ32: a = gs_addr32z(0, bw, x, y); break;
                case PSMCT16: a = gs_addr16(0, bw, x, y); break;
                case PSMCT16S: a = gs_addr16s(0, bw, x, y); break;
                case PSMZ16: a = gs_addr16z(0, bw, x, y); break;
                case PSMZ16S: a = gs_addr16sz(0, bw, x, y); break;
                case PSMT8: a = gs_addr8(0, bw, x, y); break;
                default: a = gs_addr4(0, bw, x, y); break;
                }
                if (a >= units) { out++; continue; }
                if (seen[a]) dup++;
                seen[a] = 1;
            }
        CHECKEQ(dup, 0); CHECKEQ(out, 0);
    }
    /* PSMT8H/4HL/4HH share a CT32 word */
    gs_write_pixel(PSMCT24, 0, 1, 3, 3, 0x123456);
    gs_write_pixel(PSMT8H, 0, 1, 3, 3, 0xAB);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 1, 3, 3), 0xAB123456);
    gs_write_pixel(PSMT4HL, 0, 1, 3, 3, 0x5);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 1, 3, 3), 0xA5123456);
    CHECKEQ(gs_read_pixel(PSMT4HH, 0, 1, 3, 3), 0xA);
}

static void test_transfers(void)
{
    u32 data[64], i, ok = 1;
    printf("gs transfers (IMAGE, HWREG, local->local, local->host)\n");
    reset_all();
    for (i = 0; i < 64; i++) data[i] = 0x01000000u * i + i * 3;
    /* host->local 8x8 CT32 at block 0x200 */
    ad_begin(4);
    ad(0x50, ((u64)0x200 << 32) | ((u64)1 << 48) | ((u64)PSMCT32 << 56));
    ad(0x51, 0);
    ad(0x52, 8 | ((u64)8 << 32));
    ad(0x53, 0);
    q_put(giftag(16, 1, 0, 0, 2, 0), 0);
    for (i = 0; i < 16; i++) { pk[npk].w[0] = data[i * 4]; pk[npk].w[1] = data[i * 4 + 1]; pk[npk].w[2] = data[i * 4 + 2]; pk[npk].w[3] = data[i * 4 + 3]; npk++; }
    flush3();
    for (i = 0; i < 64; i++) if (gs_read_pixel(PSMCT32, 0x200, 1, i % 8, i / 8) != data[i]) ok = 0;
    CHECK(ok);
    /* host->local PSMT4 through HWREG: 16x2 pixels, nibbles in order */
    ad_begin(5);
    ad(0x50, ((u64)0x300 << 32) | ((u64)2 << 48) | ((u64)PSMT4 << 56));
    ad(0x51, ((u64)4 << 32) | ((u64)1 << 48));
    ad(0x52, 16 | ((u64)2 << 32));
    ad(0x53, 0);
    ad(0x54, 0x76543210FEDCBA98ull);
    flush3();
    ad_begin(1); ad(0x54, 0x0123456789ABCDEFull); flush3();
    CHECKEQ(gs_read_pixel(PSMT4, 0x300, 2, 4, 1), 8);
    CHECKEQ(gs_read_pixel(PSMT4, 0x300, 2, 5, 1), 9);
    CHECKEQ(gs_read_pixel(PSMT4, 0x300, 2, 19, 1), 7);
    CHECKEQ(gs_read_pixel(PSMT4, 0x300, 2, 4, 2), 0xF);
    CHECKEQ(gs_read_pixel(PSMT4, 0x300, 2, 19, 2), 0);
    /* 24-bit packing: 4 pixels in 3 words */
    ad_begin(5);
    ad(0x50, ((u64)0x400 << 32) | ((u64)1 << 48) | ((u64)PSMCT24 << 56));
    ad(0x51, 0); ad(0x52, 4 | ((u64)1 << 32)); ad(0x53, 0);
    ad(0x54, 0x33221100CCBBAAull);
    flush3();
    ad_begin(1); ad(0x54, 0x0000000077665544ull); flush3();
    CHECKEQ(gs_read_pixel(PSMCT24, 0x400, 1, 0, 0), 0xCCBBAA);
    CHECKEQ(gs_read_pixel(PSMCT24, 0x400, 1, 1, 0), 0x221100);
    CHECKEQ(gs_read_pixel(PSMCT24, 0x400, 1, 2, 0), 0x440033);
    CHECKEQ(gs_read_pixel(PSMCT24, 0x400, 1, 3, 0), 0x776655);
    /* local->local: copy the 8x8 block to (16,16) of a 640-wide buffer */
    ad_begin(4);
    ad(0x50, 0x200 | ((u64)1 << 16) | ((u64)PSMCT32 << 24) | ((u64)0 << 32) | ((u64)10 << 48) | ((u64)PSMCT32 << 56));
    ad(0x51, ((u64)16 << 32) | ((u64)16 << 48));
    ad(0x52, 8 | ((u64)8 << 32));
    ad(0x53, 2);
    flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 16 + 5, 16 + 7), data[7 * 8 + 5]);
    /* local->host back into EE RAM */
    gfx_gs_store_image(0x200 | ((u64)1 << 16) | ((u64)PSMCT32 << 24), 0, 8 | ((u64)8 << 32), 0x100000, 16);
    CHECK(memcmp(gfx_test_ram + 0x100000, data, 256) == 0);
}

static int count_color(u32 c, int w, int h)
{
    int x, y, n = 0;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) if (gs_read_pixel(PSMCT32, 0, 10, x, y) == c) n++;
    return n;
}

static void test_prims(void)
{
    int x, y, ok;
    printf("gs primitives (sprite, triangles, fill rule, gouraud, z, alpha)\n");
    reset_all();
    setup_ctx(PSMCT32);
    /* sprite (2,3)-(10,7) -> 8x4 pixels */
    ad_begin(4);
    ad(0x00, 6); ad(0x01, rgba(10, 20, 30, 0x80)); ad(0x05, xyz(2, 3, 0)); ad(0x05, xyz(10, 7, 0));
    flush3();
    CHECKEQ(count_color(0x801E140A, 64, 32), 32);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 2, 3), 0x801E140A);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 9, 6), 0x801E140A);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 10, 6), 0);
    /* two triangles of a 16x16 square, additive blending: every pixel must be hit exactly once */
    reset_all();
    setup_ctx(PSMCT32);
    ad_begin(1); ad(0x42, 0 | (2 << 2) | (2 << 4) | (1 << 6) | (0x80ull << 32)); flush3();  /* Cs*FIX + Cd */
    ad_begin(8);
    ad(0x00, 3 | (1 << 6)); ad(0x01, rgba(1, 1, 1, 0x80));
    ad(0x05, xyz(20, 20, 0)); ad(0x05, xyz(36, 20, 0)); ad(0x05, xyz(20, 36, 0));
    ad(0x05, xyz(36, 20, 0)); ad(0x05, xyz(36, 36, 0)); ad(0x05, xyz(20, 36, 0));
    flush3();
    ok = 1;
    for (y = 16; y < 40; y++) for (x = 16; x < 40; x++) {
        u32 p = gs_read_pixel(PSMCT32, 0, 10, x, y) & 0xFFFFFF;
        int inside = x >= 20 && x < 36 && y >= 20 && y < 36;
        if (p != (inside ? 0x010101u : 0)) { ok = 0; if (ok == 0) { } }
    }
    CHECK(ok);
    /* gouraud: red 0 -> 160 along x on a sprite-shaped strip */
    reset_all();
    setup_ctx(PSMCT32);
    ad_begin(9);
    ad(0x00, 4 | (1 << 3));
    ad(0x01, rgba(0, 0, 0, 0x80)); ad(0x05, xyz(0, 0, 0));
    ad(0x01, rgba(0, 0, 0, 0x80)); ad(0x05, xyz(0, 8, 0));
    ad(0x01, rgba(160, 0, 0, 0x80)); ad(0x05, xyz(160, 0, 0));
    ad(0x01, rgba(160, 0, 0, 0x80)); ad(0x05, xyz(160, 8, 0));
    flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 0, 4) & 0xFF, 0);
    CHECK(abs((int)(gs_read_pixel(PSMCT32, 0, 10, 80, 4) & 0xFF) - 80) <= 1);
    /* z test GEQUAL with z write: near sprite then far sprite over it */
    reset_all();
    setup_ctx(PSMCT32);
    ad_begin(3);
    ad(0x4E, 0x100);                                         /* ZBUF, PSMZ32, write on */
    ad(0x47, (1ull << 16) | (2ull << 17));                   /* ZTST GEQUAL */
    ad(0x00, 6);
    flush3();
    ad_begin(6);
    ad(0x01, rgba(255, 0, 0, 0x80)); ad(0x05, xyz(0, 0, 1000)); ad(0x05, xyz(8, 8, 1000));
    ad(0x01, rgba(0, 255, 0, 0x80)); ad(0x05, xyz(4, 4, 500)); ad(0x05, xyz(12, 12, 500));
    flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 5, 5), 0x800000FF);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 10, 10), 0x8000FF00);
    CHECKEQ(gs_read_pixel(PSMZ32, 0x100 * 32, 10, 5, 5), 1000);
    /* alpha blend (Cs-Cd)*As+Cd with As = 0x40 (half): 200 over 100 -> 150 */
    reset_all();
    setup_ctx(PSMCT32);
    ad_begin(6);
    ad(0x00, 6); ad(0x01, rgba(100, 100, 100, 0x80)); ad(0x05, xyz(0, 0, 0)); ad(0x05, xyz(4, 4, 0));
    ad(0x42, 0 | (1 << 2) | (0 << 4) | (1 << 6));
    ad(0x00, 6 | (1 << 6));
    flush3();
    ad_begin(3); ad(0x01, rgba(200, 200, 200, 0x40)); ad(0x05, xyz(0, 0, 0)); ad(0x05, xyz(4, 4, 0)); flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 1, 1) & 0xFFFFFF, 0x969696);
    /* alpha test GREATER 0x20 with AFAIL KEEP */
    reset_all();
    setup_ctx(PSMCT32);
    ad_begin(4);
    ad(0x47, (1ull << 16) | (1ull << 17) | 1 | (6 << 1) | (0x20 << 4));
    ad(0x00, 6); ad(0x01, rgba(9, 9, 9, 0x10)); ad(0x05, xyz(0, 0, 0));
    flush3();
    ad_begin(1); ad(0x05, xyz(4, 4, 0)); flush3();
    CHECKEQ(count_color(0x10090909, 8, 8), 0);
    /* 16-bit frame with FBA and mask */
    reset_all();
    setup_ctx(PSMCT16);
    ad_begin(5);
    ad(0x4A, 1);
    ad(0x00, 6); ad(0x01, rgba(0xF8, 0, 0x08, 0)); ad(0x05, xyz(0, 0, 0)); ad(0x05, xyz(2, 2, 0));
    flush3();
    CHECKEQ(gs_read_pixel(PSMCT16, 0, 10, 1, 1), 0x8000 | 0x1F | (1 << 10));
}

static void test_texture(void)
{
    u32 i, x, y, ok;
    printf("gs textures (CT32 nearest, PSMT8+CLUT CSM1, PSMT4+CT16 CLUT, bilinear, decal/modulate)\n");
    reset_all();
    setup_ctx(PSMCT32);
    /* 4x4 CT32 texture at block 0x800 */
    ad_begin(4);
    ad(0x50, ((u64)0x800 << 32) | ((u64)1 << 48) | ((u64)PSMCT32 << 56));
    ad(0x51, 0); ad(0x52, 4 | ((u64)4 << 32)); ad(0x53, 0);
    q_put(giftag(4, 1, 0, 0, 2, 0), 0);
    for (i = 0; i < 4; i++) { u32 k; for (k = 0; k < 4; k++) pk[npk].w[k] = 0x80000000u | (i * 4 + k) * 0x010203u; npk++; }
    flush3();
    /* decal sprite with UV covering the texture exactly */
    ad_begin(6);
    ad(0x06, 0x800 | (1ull << 14) | ((u64)PSMCT32 << 20) | (2ull << 26) | (2ull << 30) | (1ull << 34) | (1ull << 35));
    ad(0x14, 0);
    ad(0x00, 6 | (1 << 4) | (1 << 8));
    ad(0x03, 0); ad(0x05, xyz(8, 8, 0));
    ad(0x03, 64 | (64 << 16));
    flush3();
    ad_begin(1); ad(0x05, xyz(12, 12, 0)); flush3();
    ok = 1;
    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++)
        if (gs_read_pixel(PSMCT32, 0, 10, 8 + x, 8 + y) != (0x80000000u | (y * 4 + x) * 0x010203u)) ok = 0;
    CHECK(ok);
    /* PSMT8 texture 16x16 with a CT32 CLUT (CSM1 layout) */
    reset_all();
    setup_ctx(PSMCT32);
    for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) gs_write_pixel(PSMT8, 0x1000, 2, x, y, y * 16 + x);
    for (i = 0; i < 256; i++) {
        u32 j = (i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1);   /* CSM1 storage position of entry i */
        gs_write_pixel(PSMCT32, 0x1200, 1, j & 15, j >> 4, 0x80000000u | i);
    }
    ad_begin(5);
    ad(0x06, 0x1000 | (2ull << 14) | ((u64)PSMT8 << 20) | (4ull << 26) | (4ull << 30) | (1ull << 34) | (1ull << 35) |
             ((u64)0x1200 << 37) | (1ull << 61));
    ad(0x00, 6 | (1 << 4) | (1 << 8));
    ad(0x03, 0); ad(0x05, xyz(0, 0, 0));
    ad(0x03, 256 | (256 << 16));
    flush3();
    ad_begin(1); ad(0x05, xyz(16, 16, 0)); flush3();
    ok = 1;
    for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) if (gs_read_pixel(PSMCT32, 0, 10, x, y) != (0x80000000u | (y * 16 + x))) ok = 0;
    CHECK(ok);
    /* PSMT4 with CT16 CLUT at CSA 1, TEXA alpha */
    reset_all();
    setup_ctx(PSMCT32);
    for (x = 0; x < 16; x++) gs_write_pixel(PSMT4, 0x1000, 2, x, 0, x);
    for (i = 0; i < 16; i++) gs_write_pixel(PSMCT16, 0x1200, 1, i & 7, i >> 3, 0x8000 | (i << 10));  /* loaded to CLUT entries 16..31 */
    ad_begin(6);
    ad(0x3B, 0x10 | (0x7Full << 32));
    ad(0x06, 0x1000 | (2ull << 14) | ((u64)PSMT4 << 20) | (4ull << 26) | (0ull << 30) | (1ull << 34) | (1ull << 35) |
             ((u64)0x1200 << 37) | ((u64)PSMCT16 << 51) | (1ull << 56) | (1ull << 61));
    ad(0x00, 6 | (1 << 4) | (1 << 8));
    ad(0x03, 0); ad(0x05, xyz(0, 0, 0));
    ad(0x03, 256 | (16 << 16));
    flush3();
    ad_begin(1); ad(0x05, xyz(16, 1, 0)); flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 5, 0), 0x7F000000u | ((5u << 3) << 16));
    /* modulate: vertex 0x40 (half) times texel */
    reset_all();
    setup_ctx(PSMCT32);
    gs_write_pixel(PSMCT32, 0x800, 1, 0, 0, 0x80C86432);
    ad_begin(6);
    ad(0x06, 0x800 | (1ull << 14) | ((u64)PSMCT32 << 20) | (1ull << 34));
    ad(0x00, 6 | (1 << 4) | (1 << 8));
    ad(0x01, rgba(0x40, 0x40, 0x40, 0x80));
    ad(0x03, 0); ad(0x05, xyz(0, 0, 0));
    ad(0x03, 16 | (16 << 16));
    flush3();
    ad_begin(1); ad(0x05, xyz(1, 1, 0)); flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 0, 0), 0x80643219);
    /* bilinear halfway between two texels */
    reset_all();
    setup_ctx(PSMCT32);
    gs_write_pixel(PSMCT32, 0x800, 1, 0, 0, 0x80000000);
    gs_write_pixel(PSMCT32, 0x800, 1, 1, 0, 0x800000C8);
    gs_write_pixel(PSMCT32, 0x800, 1, 0, 1, 0x80000000);
    gs_write_pixel(PSMCT32, 0x800, 1, 1, 1, 0x800000C8);
    ad_begin(7);
    ad(0x06, 0x800 | (1ull << 14) | ((u64)PSMCT32 << 20) | (1ull << 26) | (1ull << 30) | (1ull << 34) | (1ull << 35));
    ad(0x08, 1 | (1 << 2));
    ad(0x14, 1 << 5);
    ad(0x00, 6 | (1 << 4) | (1 << 8));
    ad(0x03, 16 | (8 << 16)); ad(0x05, xyz(0, 0, 0));
    ad(0x03, 32 | (24 << 16));
    flush3();
    ad_begin(1); ad(0x05, xyz(1, 1, 0)); flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 0, 0) & 0xFF, 100);
}

static void put_vif(u32 *w, int *n, u32 v) { w[(*n)++] = v; }
#define VIFCODE(cmd, num, imm) (((u32)(cmd) << 24) | ((u32)(num) << 16) | (u32)(imm))

static void test_vif_unpack(void)
{
    u32 w[256]; int n = 0;
    u32 *m;
    printf("vif1 (UNPACK forms, masks, row modes, cycles, MPG, DIRECT, double buffer)\n");
    reset_all();
    m = (u32 *)vu1.mem;
    /* V4-32 at qword 2 */
    put_vif(w, &n, VIFCODE(0x01, 0, 0x0101));
    put_vif(w, &n, VIFCODE(0x6C, 2, 2));
    put_vif(w, &n, 1); put_vif(w, &n, 2); put_vif(w, &n, 3); put_vif(w, &n, 4);
    put_vif(w, &n, 5); put_vif(w, &n, 6); put_vif(w, &n, 7); put_vif(w, &n, 8);
    /* V4-16 signed at qword 10: -1, 2, -3, 4 */
    put_vif(w, &n, VIFCODE(0x6D, 1, 10));
    put_vif(w, &n, 0x0002FFFF); put_vif(w, &n, 0x0004FFFD);
    /* V4-8 unsigned (usn) at 11 */
    put_vif(w, &n, VIFCODE(0x6E, 1, 11 | 0x4000));
    put_vif(w, &n, 0x80FF0201);
    /* V3-8 signed, 2 vectors at 12: (1,-1,2),(3,4,-128) packed in 6 bytes */
    put_vif(w, &n, VIFCODE(0x6A, 2, 12));
    put_vif(w, &n, 0x0302FF01); put_vif(w, &n, 0x00008004);
    /* S-32 at 14 */
    put_vif(w, &n, VIFCODE(0x60, 1, 14));
    put_vif(w, &n, 0x3F800000);
    /* V4-5 at 15 */
    put_vif(w, &n, VIFCODE(0x6F, 2, 15));
    put_vif(w, &n, 0x7FFF | (0x801F << 16));
    /* V2-16 unsigned at 17 */
    put_vif(w, &n, VIFCODE(0x65, 1, 17 | 0x4000));
    put_vif(w, &n, 0xFFFF0005);
    vif1_write(w, (u32)n);
    CHECKEQ(m[8], 1); CHECKEQ(m[11], 4); CHECKEQ(m[12], 5); CHECKEQ(m[15], 8);
    CHECKEQ(m[40], 0xFFFFFFFF); CHECKEQ(m[41], 2); CHECKEQ(m[42], 0xFFFFFFFD); CHECKEQ(m[43], 4);
    CHECKEQ(m[44], 1); CHECKEQ(m[45], 2); CHECKEQ(m[46], 0xFF); CHECKEQ(m[47], 0x80);
    CHECKEQ(m[48], 1); CHECKEQ(m[49], 0xFFFFFFFF); CHECKEQ(m[50], 2);
    CHECKEQ(m[52], 3); CHECKEQ(m[53], 4); CHECKEQ(m[54], 0xFFFFFF80);
    CHECKEQ(m[56], 0x3F800000); CHECKEQ(m[59], 0x3F800000);
    CHECKEQ(m[60], 0xF8); CHECKEQ(m[61], 0xF8); CHECKEQ(m[62], 0xF8); CHECKEQ(m[63], 0);
    CHECKEQ(m[64], 0xF8); CHECKEQ(m[65], 0); CHECKEQ(m[67], 0x80);
    CHECKEQ(m[68], 5); CHECKEQ(m[69], 0xFFFF);
    /* masks + row/col + offset mode: x=data+row, y=row, z=col, w=protected */
    n = 0;
    m[80 * 4 + 3] = 0xDEAD;
    put_vif(w, &n, VIFCODE(0x30, 0, 0)); put_vif(w, &n, 100); put_vif(w, &n, 200); put_vif(w, &n, 300); put_vif(w, &n, 400);
    put_vif(w, &n, VIFCODE(0x31, 0, 0)); put_vif(w, &n, 7); put_vif(w, &n, 8); put_vif(w, &n, 9); put_vif(w, &n, 10);
    put_vif(w, &n, VIFCODE(0x20, 0, 0)); put_vif(w, &n, 0 | (1 << 2) | (2 << 4) | (3 << 6));
    put_vif(w, &n, VIFCODE(0x05, 0, 1));
    put_vif(w, &n, VIFCODE(0x7C, 1, 80));   /* V4-32 with mask (m bit) */
    put_vif(w, &n, 5); put_vif(w, &n, 6); put_vif(w, &n, 7); put_vif(w, &n, 8);
    /* difference mode: row += data */
    put_vif(w, &n, VIFCODE(0x05, 0, 2));
    put_vif(w, &n, VIFCODE(0x6C, 2, 81));
    put_vif(w, &n, 1); put_vif(w, &n, 1); put_vif(w, &n, 1); put_vif(w, &n, 1);
    put_vif(w, &n, 2); put_vif(w, &n, 2); put_vif(w, &n, 2); put_vif(w, &n, 2);
    put_vif(w, &n, VIFCODE(0x05, 0, 0));
    vif1_write(w, (u32)n);
    CHECKEQ(m[320], 105); CHECKEQ(m[321], 200); CHECKEQ(m[322], 7); CHECKEQ(m[323], 0xDEAD);
    CHECKEQ(m[324], 101); CHECKEQ(m[328], 103); CHECKEQ(m[331], 403);
    /* skipping write CL=2 WL=1: 3 vectors land at 90, 92, 94 */
    n = 0;
    put_vif(w, &n, VIFCODE(0x01, 0, 0x0102));
    put_vif(w, &n, VIFCODE(0x60, 3, 90));
    put_vif(w, &n, 11); put_vif(w, &n, 22); put_vif(w, &n, 33);
    /* filling write CL=1 WL=2: 4 writes from 2 data at 100..103, fill rows = row register (mask off) */
    put_vif(w, &n, VIFCODE(0x30, 0, 0)); put_vif(w, &n, 9); put_vif(w, &n, 9); put_vif(w, &n, 9); put_vif(w, &n, 9);
    put_vif(w, &n, VIFCODE(0x01, 0, 0x0201));
    put_vif(w, &n, VIFCODE(0x60, 4, 100));
    put_vif(w, &n, 44); put_vif(w, &n, 55);
    put_vif(w, &n, VIFCODE(0x01, 0, 0x0101));
    vif1_write(w, (u32)n);
    CHECKEQ(m[360], 11); CHECKEQ(m[368], 22); CHECKEQ(m[376], 33); CHECKEQ(m[364], 0);
    CHECKEQ(m[400], 44); CHECKEQ(m[404], 9); CHECKEQ(m[408], 55); CHECKEQ(m[412], 9);
    /* double buffer: BASE 100, OFFSET 50, MSCAL toggles TOPS; FLG unpack goes to TOPS */
    n = 0;
    memset(vu1.micro, 0, 16);
    { u32 nop[4] = { 0x8000033C, 0x400002FF, 0x8000033C, 0x000002FF }; memcpy(vu1.micro, nop, 16); }
    put_vif(w, &n, VIFCODE(0x03, 0, 100));
    put_vif(w, &n, VIFCODE(0x02, 0, 50));
    put_vif(w, &n, VIFCODE(0x60, 1, 0x8000 | 1));
    put_vif(w, &n, 0x1111);
    put_vif(w, &n, VIFCODE(0x14, 0, 0));
    put_vif(w, &n, VIFCODE(0x60, 1, 0x8000 | 1));
    put_vif(w, &n, 0x2222);
    vif1_write(w, (u32)n);
    CHECKEQ(m[101 * 4], 0x1111);
    CHECKEQ(m[151 * 4], 0x2222);
    CHECKEQ(vif1.top, 100);
    /* MPG upload */
    n = 0;
    put_vif(w, &n, VIFCODE(0x4A, 2, 10));
    put_vif(w, &n, 0xAAAA0001); put_vif(w, &n, 0xBBBB0002); put_vif(w, &n, 0xCCCC0003); put_vif(w, &n, 0xDDDD0004);
    vif1_write(w, (u32)n);
    CHECKEQ(((u32 *)vu1.micro)[20], 0xAAAA0001); CHECKEQ(((u32 *)vu1.micro)[23], 0xDDDD0004);
}

/* ---- VU1 assembler helpers ------------------------------------------------------------------------- */

#define X 8
#define Y 4
#define Z 2
#define W 1
static u32 U(u32 op, u32 dest, u32 ft, u32 fs, u32 fd) { return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op; }
static u32 US(u32 ext, u32 dest, u32 ft, u32 fs) { return (dest << 21) | (ft << 16) | (fs << 11) | ((ext >> 2) << 6) | (ext & 3) | 0x3C; }
static u32 LX(u32 ext, u32 dest, u32 it, u32 is) { return 0x80000000u | (dest << 21) | (it << 16) | (is << 11) | ((ext >> 2) << 6) | (ext & 3) | 0x3C; }
static u32 LI(u32 sop, u32 it, u32 is, u32 id) { return 0x80000000u | (it << 16) | (is << 11) | (id << 6) | sop; }
static u32 LO(u32 op, u32 dest, u32 it, u32 is, s32 imm) { return (op << 25) | (dest << 21) | (it << 16) | (is << 11) | ((u32)imm & 0x7FF); }
static u32 IADDIU(u32 it, u32 is, u32 imm) { return (0x08u << 25) | (((imm >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm & 0x7FF); }
#define UNOP 0x000002FFu
#define LNOP 0x8000033Cu
#define EBIT 0x40000000u
#define IBIT 0x80000000u

static u32 prog[512][2]; static int np;
static void emit(u32 up, u32 lo) { prog[np][0] = lo; prog[np][1] = up; np++; }
static void load_prog(void) { memcpy(vu1.micro, prog, (size_t)np * 8); np = 0; }
static u32 ubits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void test_vu1(void)
{
    vu_vec *vf = vu1.vf;
    printf("vu1 (fmac ops, acc, Q/DIV latency, flags delay, integer loop, load/store, clip, EFU, XGKICK)\n");
    reset_all();
    vf[1].f[0] = 1; vf[1].f[1] = 2; vf[1].f[2] = 3; vf[1].f[3] = 4;
    vf[2].f[0] = 10; vf[2].f[1] = 20; vf[2].f[2] = 30; vf[2].f[3] = 40;
    emit(U(0x28, X | Y | Z | W, 2, 1, 3), LNOP);                    /* ADD vf3, vf1, vf2 */
    emit(U(0x18 | 3, X | Y, 2, 1, 4), LNOP);                       /* MULw.xy vf4, vf1, vf2w */
    emit(US(0x2A, X | Y | Z | W, 2, 1), LNOP);                     /* MULA ACC, vf1, vf2 */
    emit(U(0x08 | 0, X | Y | Z | W, 1, 2, 5), LNOP);               /* MADDx vf5, vf2, vf1x: ACC + vf2*1 */
    emit(US(0x14, X | Y | Z | W, 6, 5), LNOP);                     /* FTOI0 vf6, vf5 */
    emit(US(0x1D, X, 7, 0), LNOP);                                 /* ABS */
    emit(UNOP | EBIT, LNOP);
    emit(UNOP, LNOP);
    load_prog();
    vu1_run(0);
    CHECK(vf[3].f[0] == 11 && vf[3].f[3] == 44);
    CHECK(vf[4].f[0] == 40 && vf[4].f[1] == 80 && vf[4].f[2] == 0);
    CHECK(vf[5].f[0] == 20 && vf[5].f[1] == 60 && vf[5].f[3] == 200);
    CHECKEQ(vf[6].s[2], 120);
    /* DIV: Q is not ready right away; WAITQ then MULq */
    reset_all();
    vf[1].f[0] = 8; vf[2].f[1] = 2; vf[3].f[0] = 1; vf[3].f[1] = 1; vf[3].f[2] = 1; vf[3].f[3] = 1;
    emit(UNOP, LX(0x38, (1 << 2) | 0, 2, 1));                      /* DIV Q, vf1x, vf2y : ftf=y, fsf=x */
    emit(U(0x1C, X, 0, 3, 4), LNOP);                               /* MULq vf4x = vf3 * Q (old Q = 1.0) */
    emit(UNOP, LX(0x3B, 0, 0, 0));                                 /* WAITQ */
    emit(U(0x1C, X, 0, 3, 5), LNOP);                               /* MULq vf5x = 4 */
    emit(UNOP | EBIT, LNOP); emit(UNOP, LNOP);
    load_prog();
    vu1.q = ubits(1.0f);
    vu1_run(0);
    CHECK(vf[4].f[0] == 1.0f);
    CHECK(vf[5].f[0] == 4.0f);
    /* MAC flag delay: SUB sets Z, FMAND sees it only 4 cycles later */
    reset_all();
    vf[1].f[0] = 5; vf[2].f[0] = 5;
    emit(U(0x2C, X, 2, 1, 3), IADDIU(1, 0, 0x80));                 /* SUB.x -> zero ; vi1 = 0x80 (Zx mask is 0x8) */
    emit(UNOP, LO(0x1A, 0, 2, 1, 0) | IADDIU(0, 0, 0));            /* placeholder overwritten below */
    np = 1;
    emit(UNOP, IADDIU(1, 0, 0x8));
    emit(UNOP, LO(0x1A, 0, 2, 1, 0));                              /* FMAND vi2, vi1 : cycle 2 -> old flags */
    emit(UNOP, LNOP);
    emit(UNOP, LNOP);
    emit(UNOP, LO(0x1A, 0, 3, 1, 0));                              /* FMAND vi3, vi1 : cycle 5 -> new flags */
    emit(UNOP | EBIT, LNOP); emit(UNOP, LNOP);
    load_prog();
    vu1_run(0);
    CHECKEQ(vu1.vi[2], 0);
    CHECKEQ(vu1.vi[3], 8);
    /* integer loop with branch delay slot, LQ/SQ, IADD */
    reset_all();
    ((float *)vu1.mem)[0] = 1.5f;
    emit(UNOP, IADDIU(1, 0, 5));                                   /* vi1 = 5 (counter) */
    emit(UNOP, LO(0x00, X | Y | Z | W, 1, 0, 0));                  /* LQ vf1, 0(vi0) */
    emit(UNOP, LI(0x32, 1, 1, 0x1F));                              /* loop: IADDI vi1, vi1, -1 */
    emit(U(0x28, X, 1, 2, 2), LO(0x29, 0, 1, 0, -2));              /* ADD vf2x += vf1x ; IBNE vi1, vi0, loop */
    emit(UNOP, IADDIU(3, 3, 1));                                   /* delay slot: vi3++ */
    emit(UNOP, LO(0x01, X | Y | Z | W, 0, 2, 4));                  /* SQ vf2, 4(vi0) */
    emit(UNOP | EBIT, LNOP); emit(UNOP, LNOP);
    load_prog();
    vu1_run(0);
    CHECK(fabsf(((float *)vu1.mem)[16] - 7.5f) < 1e-6f);
    CHECKEQ(vu1.vi[3], 5);
    /* CLIP + FCAND, ELENG + WAITP + MFP, I register */
    reset_all();
    vf[1].f[0] = 3; vf[1].f[1] = 4; vf[1].f[2] = 0; vf[1].f[3] = 1;
    emit(US(0x1F, X | Y | Z, 1, 1), LX(0x72, 0, 0, 1));            /* CLIP vf1, vf1w ; ELENG P, vf1 */
    emit(UNOP, LNOP); emit(UNOP, LNOP); emit(UNOP, LNOP);
    emit(UNOP, LO(0x12, 0, 0, 0, 0) | 0x5);                        /* FCAND vi1, 0x5 (+x|+y) */
    emit(UNOP, LX(0x7B, 0, 0, 0));                                 /* WAITP */
    emit(UNOP, LX(0x64, X, 2, 0));                                 /* MFP vf2x */
    emit(UNOP | IBIT, ubits(0.5f));
    emit(U(0x1E, X, 0, 2, 3), LNOP);                               /* MULi vf3x = 5 * 0.5 */
    emit(UNOP | EBIT, LNOP); emit(UNOP, LNOP);
    load_prog();
    vu1_run(0);
    CHECKEQ(vu1.vi[1], 1);
    CHECKEQ(vu1.clip & 0x3F, 0x5);
    CHECK(fabsf(vf[2].f[0] - 5.0f) < 1e-5f);
    CHECK(fabsf(vf[3].f[0] - 2.5f) < 1e-5f);
    /* overflow clamps to the VU max, FTOI saturates */
    reset_all();
    vf[1].u[0] = 0x7F000000; vf[2].u[0] = 0x7F000000;
    emit(U(0x2A, X, 2, 1, 3), LNOP);                               /* MUL: huge */
    emit(UNOP, LNOP); emit(UNOP, LNOP); emit(UNOP, LNOP);
    emit(US(0x14, X, 4, 3), LNOP);
    emit(UNOP | EBIT, LNOP); emit(UNOP, LNOP);
    load_prog();
    vu1_run(0);
    CHECKEQ(vf[3].u[0], 0x7FFFFFFF);
    CHECKEQ(vf[4].u[0], 0x7FFFFFFF);
    CHECK(vu1.status & 0x8);
    /* XGKICK: a sprite packet in VU memory reaches the GS */
    reset_all();
    npk = 0;
    setup_ctx(PSMCT32);
    q_put(giftag(1, 1, 1, 6, 0, 3), 0x551);                        /* PRE sprite; regs RGBAQ, XYZ2, XYZ2 */
    q_put(0x0000007700000011ull, 0x0000008000000033ull);
    q_put((u64)(16 * 1) | ((u64)(16 * 1) << 32), 0);
    q_put((u64)(16 * 3) | ((u64)(16 * 3) << 32), 0);
    memcpy(vu1.mem + 0x100, pk, (size_t)npk * 16);
    npk = 0;
    emit(UNOP, IADDIU(1, 0, 0x10));
    emit(UNOP, LX(0x6C, 0, 0, 1));                                 /* XGKICK vi1 */
    emit(UNOP | EBIT, LNOP); emit(UNOP, LNOP);
    load_prog();
    vu1_run(0);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 2, 2), 0x80337711);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 3, 3), 0);
    {
        char line[160];
        vu1_disasm(U(0x28, 0xF, 2, 1, 3), LNOP, line, sizeof line);
        CHECK(strncmp(line, "ADDx", 4) == 0 || strncmp(line, "ADD", 3) == 0);
    }
}

static void test_dma_chain(void)
{
    u32 *ram;
    hw_dma_kick k;
    printf("dma (VIF1 source chain with TTE: cnt, call/ret, ref, end; GIF normal; scratchpad)\n");
    reset_all();
    ram = (u32 *)gfx_test_ram;
    /* the frame context through GIF normal mode */
    npk = 0;
    ad_begin(6);
    ad(0x4C, frame_reg(0, 10, PSMCT32, 0));
    ad(0x4E, 0x100 | (1ull << 32));
    ad(0x40, (u64)639 << 16 | ((u64)447 << 48));
    ad(0x47, (1ull << 16) | (1ull << 17));
    ad(0x18, 0);
    ad(0x46, 1);
    memcpy(gfx_test_ram + 0x200000, pk, (size_t)npk * 16);
    memset(&k, 0, sizeof k);
    k.channel = HW_DMA_GIF; k.mode = 0; k.madr = 0x200000; k.qwc = (u32)npk;
    npk = 0;
    CHECK(gfx_dma_kick(&k));
    /* VIF1 chain at 0x300000:
       cnt(0)  tag words: NOP, DIRECT 4   -> giftag + 3 regs follow as the tag's data? no: data is the next qwords
       here: cnt qwc=4 with DIRECT 4 in the tag's upper word, then a call into a sub-chain that draws nothing,
       then end. */
    {
        u32 *t = ram + 0x300000 / 4;
        qword pkt[4];
        npk = 0;
        q_put(giftag(1, 1, 1, 6, 0, 3), 0x551);
        q_put(0x000000AA000000BBull, 0x00000080000000CCull);
        q_put(0, 0);
        q_put((u64)(16 * 4) | ((u64)(16 * 2) << 32), 0);
        memcpy(pkt, pk, sizeof pkt);
        npk = 0;
        /* tag 0: cnt qwc=4, VIF: NOP, DIRECT 4 */
        t[0] = 4 | (1u << 28); t[1] = 0; t[2] = 0; t[3] = VIFCODE(0x50, 0, 4);
        memcpy(t + 4, pkt, sizeof pkt);
        /* tag 1 at +0x50: call to 0x310000, qwc=0 */
        t[20] = 0 | (5u << 28); t[21] = 0x310000; t[22] = 0; t[23] = 0;
        /* tag 2 at +0x60: end */
        t[24] = 0 | (7u << 28); t[25] = 0; t[26] = 0; t[27] = VIFCODE(0x07, 0, 0x1234);   /* MARK */
        /* sub-chain: ref qwc=1 at 0x320000 carrying STROW? simple: ref to a VIF data qword (four NOP+MARK) then ret */
        t = ram + 0x310000 / 4;
        t[0] = 1 | (3u << 28); t[1] = 0x320000; t[2] = 0; t[3] = 0;
        t[4] = 0 | (6u << 28); t[5] = 0; t[6] = 0; t[7] = 0;
        t = ram + 0x320000 / 4;
        t[0] = 0; t[1] = 0; t[2] = 0; t[3] = VIFCODE(0x07, 0, 0x777);
        k.channel = HW_DMA_VIF1; k.mode = 1; k.tte = 1; k.tadr = 0x300000;
        CHECK(gfx_dma_kick(&k));
    }
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 2, 0), 0x80CCAABB);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 4, 0), 0);
    CHECKEQ(vif1.mark, 0x1234);
    CHECKEQ(hw_stub_dmac[HW_DMA_VIF1], 1);
    /* scratchpad copies */
    ram[0x400000 / 4] = 0xCAFEBABE;
    k.channel = HW_DMA_SPR_TO; k.mode = 0; k.madr = 0x400000; k.sadr = 0x100; k.qwc = 1;
    CHECK(gfx_dma_kick(&k));
    CHECKEQ(((u32 *)gfx_test_spr)[0x40], 0xCAFEBABE);
    k.channel = HW_DMA_SPR_FROM; k.madr = 0x400010; k.sadr = 0x100; k.qwc = 1;
    CHECK(gfx_dma_kick(&k));
    CHECKEQ(ram[0x400010 / 4], 0xCAFEBABE);
}

static void test_display_and_capture(void)
{
    const u32 *img; int w, h, n;
    char path[600];
    u32 before;
    printf("display read-out, capture + replay\n");
    reset_all();
    setup_ctx(PSMCT32);
    ad_begin(4); ad(0x00, 6); ad(0x01, rgba(255, 128, 0, 0x80)); ad(0x05, xyz(0, 0, 0)); ad(0x05, xyz(640, 448, 0)); flush3();
    gfx_gs_priv_write(0x12000000, 1 | (1 << 5) | (0xFF << 8));     /* PMODE: circuit 1, ALP=255 */
    gfx_gs_priv_write(0x12000070, 0 | (10 << 9) | (0 << 15));      /* DISPFB1 */
    gfx_gs_priv_write(0x12000080, 636 | (50 << 12) | (3ull << 23) | (2559ull << 32) | (447ull << 44));
    CHECK(gfx_display_frame(&img, &w, &h));
    CHECKEQ(w, 640); CHECKEQ(h, 448);
    CHECKEQ(img[100 * 640 + 100], 0xFF0080FF);
    /* capture: arm, start frame, draw, end frame; then wipe and replay */
    capture_request();
    capture_begin_frame();       /* recording starts */
    CHECK(capture_recording());
    npk = 0;
    ad_begin(4); ad(0x00, 6); ad(0x01, rgba(1, 2, 3, 0x80)); ad(0x05, xyz(10, 10, 0)); ad(0x05, xyz(20, 20, 0));
    {
        hw_dma_kick k; memset(&k, 0, sizeof k);
        memcpy(gfx_test_ram + 0x200000, pk, (size_t)npk * 16);
        k.channel = HW_DMA_GIF; k.madr = 0x200000; k.qwc = (u32)npk; npk = 0;
        gfx_dma_kick(&k);
    }
    before = gs_read_pixel(PSMCT32, 0, 10, 15, 15);
    CHECKEQ(before, 0x80030201);
    capture_begin_frame();       /* file written */
    CHECK(!capture_recording());
    /* find the newest capture in the current directory */
    {
        WIN32_FIND_DATAA fd; HANDLE hf = FindFirstFileA("dotr-*.gcap", &fd);
        FILETIME best = { 0, 0 };
        path[0] = 0;
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (CompareFileTime(&fd.ftLastWriteTime, &best) >= 0) { best = fd.ftLastWriteTime; snprintf(path, sizeof path, "%s", fd.cFileName); }
            } while (FindNextFileA(hf, &fd));
            FindClose(hf);
        }
    }
    CHECK(path[0] != 0);
    gs_write_pixel(PSMCT32, 0, 10, 15, 15, 0);
    gs_write_pixel(PSMCT32, 0, 10, 100, 100, 0);
    n = gfx_replay_capture(path);
    CHECK(n >= 1);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 15, 15), 0x80030201);
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 100, 100), 0x800080FF);
    remove(path);
    { char bmp[600]; snprintf(bmp, sizeof bmp, "%.*s.bmp", (int)(strlen(path) - 5), path); remove(bmp); }
    /* report hooks: the picture as PNG, the GS state now, a capture into a folder */
    {
        u8 hdr[8] = { 0 };
        FILE *f;
        CreateDirectoryA("gfxtest_report", NULL);
        CHECK(gfx_save_picture("gfxtest_report\\picture.png"));
        f = fopen("gfxtest_report\\picture.png", "rb");
        if (f) { fread(hdr, 1, 8, f); fclose(f); }
        CHECK(hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G');
        CHECK(gfx_capture_now("gfxtest_report"));
        gs_write_pixel(PSMCT32, 0, 10, 100, 100, 0);
        CHECK(gfx_replay_capture("gfxtest_report\\gs-state.gcap") >= 0);
        CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 100, 100), 0x800080FF);
        gfx_capture_to("gfxtest_report");
        capture_begin_frame();
        capture_begin_frame();
        f = fopen("gfxtest_report\\gs-capture.gcap", "rb");
        CHECK(f != NULL);
        if (f) fclose(f);
    }
}

#include "../gs_fast.h"

static void test_fast_paths(void)
{
    static const u32 fmts[] = { PSMCT32, PSMCT24, PSMZ32, PSMZ24, PSMCT16, PSMCT16S, PSMZ16, PSMZ16S };
    int f, bad = 0;
    printf("fast paths (address tables vs exact layout, texture cache invalidation)\n");
    reset_all();
    for (f = 0; f < 8; f++) {
        u32 bp, bw;
        for (bp = 0; bp < 16384; bp += 3203)
            for (bw = 1; bw <= 16; bw += 5) {
                const gs_fbtab *t = gs_fbtab_get(bp, bw, fmts[f]);
                u32 x, y;
                for (y = 0; y < 300; y += 7)
                    for (x = 0; x < 700; x += 3) {
                        u32 want;
                        switch (fmts[f]) {
                        case PSMCT32: case PSMCT24: want = gs_addr32(bp, bw, x, y); break;
                        case PSMZ32: case PSMZ24: want = gs_addr32z(bp, bw, x, y); break;
                        case PSMCT16: want = gs_addr16(bp, bw, x, y); break;
                        case PSMCT16S: want = gs_addr16s(bp, bw, x, y); break;
                        case PSMZ16: want = gs_addr16z(bp, bw, x, y); break;
                        default: want = gs_addr16sz(bp, bw, x, y); break;
                        }
                        if (!t || ((t->yo[y] + t->xo[x]) & t->mask) != want) bad++;
                        {   /* image transfers use the base-0 table plus the base pointer's offset */
                            const gs_fbtab *t0 = gs_fbtab_get(0, bw, fmts[f]);
                            u32 unit = (fmts[f] == PSMCT16 || fmts[f] == PSMCT16S || fmts[f] == PSMZ16 || fmts[f] == PSMZ16S) ? 128 : 64;
                            if (!t0 || ((t0->yo[y] + t0->xo[x] + bp * unit) & t0->mask) != want) bad++;
                        }
                    }
            }
    }
    CHECKEQ(bad, 0);
    /* draw a 1x1 texture, change it with an upload, draw again: the second draw must see the new texel */
    setup_ctx(PSMCT32);
    gs_write_pixel(PSMCT32, 0x800, 1, 0, 0, 0x80112233);
    ad_begin(5);
    ad(0x06, 0x800 | (1ull << 14) | ((u64)PSMCT32 << 20) | (1ull << 34) | (1ull << 35));
    ad(0x00, 6 | (1 << 4) | (1 << 8));
    ad(0x03, 0); ad(0x05, xyz(0, 0, 0)); ad(0x05, xyz(1, 1, 0));
    flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 0, 0), 0x80112233);
    ad_begin(5);
    ad(0x50, ((u64)0x800 << 32) | ((u64)1 << 48) | ((u64)PSMCT32 << 56));
    ad(0x51, 0); ad(0x52, 1 | ((u64)1 << 32)); ad(0x53, 0);
    ad(0x54, 0x80445566);
    flush3();
    ad_begin(3); ad(0x00, 6 | (1 << 4) | (1 << 8)); ad(0x05, xyz(2, 0, 0)); ad(0x05, xyz(3, 1, 0)); flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 2, 0), 0x80445566);
    /* render into the texture's memory with context 2, then sample it with context 1: the cache must notice */
    ad_begin(6);
    ad(0x4D, frame_reg(0x800 / 32, 1, PSMCT32, 0));
    ad(0x41, (u64)63 << 16 | ((u64)31 << 48));
    ad(0x00, 6 | (1 << 9)); ad(0x01, rgba(7, 8, 9, 0x80)); ad(0x05, xyz(0, 0, 0)); ad(0x05, xyz(1, 1, 0));
    flush3();
    ad_begin(3); ad(0x00, 6 | (1 << 4) | (1 << 8)); ad(0x05, xyz(4, 0, 0)); ad(0x05, xyz(5, 1, 0)); flush3();
    CHECKEQ(gs_read_pixel(PSMCT32, 0, 10, 4, 0), 0x80090807);
}

#include "../gs_par.h"

/* Overlapping blended, textured, z-tested triangles and sprites in one batch, as a DMA transfer delivers them. */
static u32 draw_busy_scene(void)
{
    int i;
    u32 h = 2166136261u, k;
    reset_all();
    setup_ctx(PSMCT32);
    for (i = 0; i < 64; i++) gs_write_pixel(PSMCT32, 0x2000, 1, i & 7, i >> 3, 0x80000000u | (u32)(i * 0x030507));
    ad_begin(5);
    ad(0x4E, 0x100);
    ad(0x47, (1ull << 16) | (2ull << 17));
    ad(0x42, 0 | (1 << 2) | (0 << 4) | (1 << 6));
    ad(0x06, 0x2000 | (1ull << 14) | ((u64)PSMCT32 << 20) | (3ull << 26) | (3ull << 30) | (1ull << 34));
    ad(0x14, 1 << 5);
    flush3();
    gs_in_dma++;
    for (i = 0; i < 600; i++) {
        int bx = (i * 37) % 560, by = (i * 53) % 380, sz = 8 + (i * 7) % 70;
        ad_begin(10);
        ad(0x00, (i % 3 ? 3 : 6) | (1 << 3) | (1 << 4) | (1 << 6));
        ad(0x01, rgba(40 + i % 200, 90, 200 - i % 150, 0x40 + i % 64)); ad(0x02, 0); ad(0x05, xyz(bx, by, 1000 + i));
        ad(0x01, rgba(200, i % 255, 30, 0x60)); ad(0x02, 0x3F800000ull); ad(0x05, xyz(bx + sz, by + sz / 3, 1000 + i));
        ad(0x01, rgba(10, 250, i % 255, 0x7F)); ad(0x02, 0x3F8000003F000000ull); ad(0x05, xyz(bx + sz / 2, by + sz, 1000 + i));
        flush3();
    }
    gs_in_dma--;
    gs_flush();
    for (k = 0; k < GS_MEM_WORDS / 2; k++) h = (h ^ gs_vram[k]) * 16777619u;
    return h;
}

static void test_threads_identical(void)
{
    u32 a, b;
    printf("batched multi-threaded drawing equals single-threaded drawing\n");
    a = draw_busy_scene();
    gs_par_shutdown();
    gs_par_init(1);
    b = draw_busy_scene();
    gs_par_shutdown();
    gs_par_init(0);
    CHECKEQ(a, b);
}

/* Random but well-formed GS traffic: every register a game can set, extreme vertices and texture coordinates,
 * uploads, local copies, CLUT loads and display read-outs, batched and multi-threaded. Passing means nothing in
 * the drawing path reads or writes outside its memory (a crash in a drawn duel, 17 Oct, was in gs_draw_prim). */
static u32 fz_s = 0x9E3779B9u;   /* found a region-clamp read past the decoded texture (fixed) */
static u32 fz(void) { fz_s ^= fz_s << 13; fz_s ^= fz_s >> 17; fz_s ^= fz_s << 5; return fz_s; }
static u64 fz64(void) { return (u64)fz() | ((u64)fz() << 32); }
static float fzf(void)
{
    static const float edge[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1e-30f, 1e30f, -1e30f, 3.4e38f, 4096.0f, -4096.0f };
    u32 r = fz() % 16;
    if (r < 11) return edge[r];
    if (r == 11) { u32 u = 0x7FC00000u; float f; memcpy(&f, &u, 4); return f; }   /* NaN */
    return (float)(s32)fz() / 65536.0f;
}

static volatile int fz_it = -1;

static LONG WINAPI fz_crash(EXCEPTION_POINTERS *e)
{
    printf("CRASH %08lx at %08x (image offset; address %p) in fuzz iteration %d\n", e->ExceptionRecord->ExceptionCode,
           (unsigned)((u8 *)e->ExceptionRecord->ExceptionAddress - (u8 *)GetModuleHandleA(NULL)) + 0x400000u,
           (void *)e->ExceptionRecord->ExceptionInformation[1], fz_it);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void test_fuzz(void)
{
    static const u32 psms[] = { PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4, PSMT8H, PSMT4HL, PSMT4HH,
                                PSMZ32, PSMZ24, PSMZ16, PSMZ16S };
    static const u32 regs[] = { 0x06, 0x07, 0x08, 0x09, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
                                0x22, 0x34, 0x35, 0x36, 0x37, 0x3B, 0x3D, 0x3E, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
                                0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F };
    int it, k, nfz = 40000;
    const hw_u32 *img; int w, h;
    printf("fuzz: random GS packets (all registers, extreme vertices, uploads, copies, read-outs)\n");
    gs_reset();
    SetUnhandledExceptionFilter(fz_crash);
    if (getenv("DOTR_FUZZ_SEED")) fz_s = (u32)strtoul(getenv("DOTR_FUZZ_SEED"), NULL, 0);
    if (getenv("DOTR_FUZZ_N")) nfz = atoi(getenv("DOTR_FUZZ_N"));
    for (it = 0; it < nfz; it++) {
        u32 kind = fz() % 100;
        fz_it = it;
        if (it % 8 == 0) gs_in_dma++;          /* let primitives queue up as inside a DMA */
        if (kind < 70) {
            u32 nset = fz() % 6, type = fz() % 8, nv = type == 0 ? 1 : (type <= 2 || type == 6) ? 2 : 3;
            ad_begin(nset + 1 + nv * 3);
            for (k = 0; k < (int)nset; k++) {
                u32 r = regs[fz() % (sizeof regs / sizeof regs[0])];
                u64 v = fz64();
                if (r == 0x06 || r == 0x07)   /* TEX0: a valid format most of the time */
                    v = (v & ~(0x3Full << 20)) | ((u64)psms[fz() % 13] << 20);
                if (r == 0x4C || r == 0x4D)   /* FRAME */
                    v = (v & ~(0x3Full << 24)) | ((u64)psms[fz() % 13] << 24);
                ad(r, v);
            }
            ad(0x00, type | ((u64)(fz() & 0x7F8) << 0));
            for (k = 0; k < (int)nv; k++) {
                u32 sb, tb, qb;
                float s = fzf(), t = fzf(), q = fzf();
                memcpy(&sb, &s, 4); memcpy(&tb, &t, 4); memcpy(&qb, &q, 4);
                ad(0x01, (fz() & 0xFFFFFFFFu) | ((u64)qb << 32));
                ad(0x02, sb | ((u64)tb << 32));
                ad(0x03, fz() & 0x3FFF3FFFu);
                ad(0x05, (fz() % 3 ? (u64)(((0x8000 + (s32)(fz() % 0x4000) - 0x2000) & 0xFFFF) |
                                          (((0x8000 + (s32)(fz() % 0x4000) - 0x2000) & 0xFFFF) << 16)) : (u64)fz())
                         | ((u64)fz() << 32));
            }
            flush3();
        } else if (kind < 85) {   /* host->local upload of a small random rectangle */
            u32 dpsm = psms[fz() % 13], rw = 1 + fz() % 64, rh = 1 + fz() % 64, nqw, q;
            ad_begin(4);
            ad(0x50, ((u64)(fz() % 16384) << 32) | ((u64)(fz() % 32) << 48) | ((u64)dpsm << 56));
            ad(0x51, ((u64)(fz() % 2048) << 32) | ((u64)(fz() % 2048) << 48));
            ad(0x52, rw | ((u64)rh << 32));
            ad(0x53, 0);
            flush3();
            nqw = (rw * rh * gs_psm_bpp(dpsm) / 8 + 15) / 16 + 1;
            if (nqw > 1000) nqw = 1000;
            q_put(giftag(nqw, 1, 0, 0, 2, 0), 0);
            for (q = 0; q < nqw; q++) q_put(fz64(), fz64());
            flush3();
        } else if (kind < 92) {   /* local->local copy */
            ad_begin(4);
            ad(0x50, (fz() % 16384) | ((u64)(fz() % 32) << 16) | ((u64)psms[fz() % 13] << 24) |
                     ((u64)(fz() % 16384) << 32) | ((u64)(fz() % 32) << 48) | ((u64)psms[fz() % 13] << 56));
            ad(0x51, fz64() & 0x1FFFFFFFFFFFFFFFull);
            ad(0x52, (1 + fz() % 64) | ((u64)(1 + fz() % 64) << 32));
            ad(0x53, 2);
            flush3();
        } else if (kind < 96) {   /* display circuits and a read-out */
            gfx_gs_priv_write(0x12000000u, fz() & 0xFFFF);
            gfx_gs_priv_write(0x12000070u + (fz() & 1) * 0x20, fz64());
            gfx_gs_priv_write(0x12000080u + (fz() & 1) * 0x20, fz64());
            gfx_display_frame(&img, &w, &h);
        } else {
            gs_flush();
        }
        if (it % 8 == 7) { gs_in_dma--; gs_flush_async(); }
    }
    while (gs_in_dma) gs_in_dma--;
    gs_flush();
    CHECK(1);   /* reaching here without a crash is the check */
    gs_reset();
}

/* Mip levels on both renderers: textured triangles whose Q (and LCM's K) select levels 0..2, nearest and
 * bilinear minification; the GPU's GS memory must equal the exact renderer's, and the levels must show. */
#include "../gpu/gs_gpu.h"
static void test_mips_gpu(void)
{
    static u32 gm[GS_MEM_WORDS];
    u32 x, y, i, diff = 0, lvl1 = 0, lvl2 = 0;
    int pass, k;
    static const float qs[4] = { 1.0f, 0.5f, 0.25f, 0.125f };
    printf("mip levels: exact and GPU renderers pick and sample the same levels\n");
    reset_all();
    if (!gs_gpu_init()) { printf("  (no GPU renderer here: skipped)\n"); return; }
    setup_ctx(PSMCT32);
    for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) gs_write_pixel(PSMCT32, 0x1000, 1, x, y, 0x80000000u | (x * 4) | (y * 4 << 8));
    for (y = 0; y < 32; y++) for (x = 0; x < 32; x++) gs_write_pixel(PSMCT32, 0x1100, 1, x, y, 0x80000000u | (0xC0u << 8) | (x * 8 << 16));
    for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) gs_write_pixel(PSMCT32, 0x1140, 1, x, y, 0x80000000u | (0xE0u << 16) | (y * 16));
    gs_gpu_load_mem();
    for (pass = 0; pass < 4; pass++) {   /* mmin nearest/linear x LOD from Q / LCM with K */
        u32 lcm = pass >= 2, mmin = (pass & 1) ? 1 : 0;
        u64 tex1 = lcm | (2ull << 2) | ((u64)mmin << 6) | ((u64)(lcm ? 24 : 0) << 32);
        ad_begin(5);
        ad(0x06, 0x1000 | (1ull << 14) | ((u64)PSMCT32 << 20) | (6ull << 26) | (6ull << 30) | (1ull << 34) | (1ull << 35));
        ad(0x14, tex1);
        ad(0x34, 0x1100 | (1ull << 14) | ((u64)0x1140 << 20) | (1ull << 34));
        ad(0x08, 0);
        ad(0x00, 3 | (1 << 4));
        flush3();
        for (k = 0; k < 4; k++) {
            float q = qs[k], s0 = 0.05f * q, s1 = 0.95f * q;
            int ox = 8 + k * 80, oy = 8 + pass * 90;
            u32 qb, sb0, sb1;
            memcpy(&qb, &q, 4); memcpy(&sb0, &s0, 4); memcpy(&sb1, &s1, 4);
            ad_begin(9);
            ad(0x01, 0x80808080u | ((u64)qb << 32)); ad(0x02, sb0 | ((u64)sb0 << 32)); ad(0x05, xyz(ox, oy, 0));
            ad(0x01, 0x80808080u | ((u64)qb << 32)); ad(0x02, sb1 | ((u64)sb0 << 32)); ad(0x05, xyz(ox + 70, oy, 0));
            ad(0x01, 0x80808080u | ((u64)qb << 32)); ad(0x02, sb0 | ((u64)sb1 << 32)); ad(0x05, xyz(ox, oy + 80, 0));
            flush3();
        }
    }
    gs_flush();
    gs_gpu_read_mem(gm);
    for (i = 0; i < GS_MEM_WORDS; i++) if (gm[i] != gs_vram[i]) diff++;
    for (y = 0; y < 448; y++) for (x = 0; x < 640; x++) {
        u32 v = gs_read_pixel(PSMCT32, 0, 10, x, y);
        if (((v >> 8) & 0xFF) >= 0xC0 && !(v & 0xFF)) lvl1++;
        if (((v >> 16) & 0xFF) >= 0xE0) lvl2++;
    }
    CHECKEQ(diff, 0);
    CHECK(lvl1 > 0 && lvl2 > 0);
    gs_gpu_shutdown();
}

#include "../display.h"
static void test_window_present(void)
{
    int nw, nh, x, y, bad, rc, i;
    hw_u32 *pic = (hw_u32 *)malloc(640 * 448 * 4);
    printf("window: 4:3 layout and the present path on an off-screen target (no window)\n");
    /* a 4:3 window shows 640x448 with no bars */
    display_layout(640, 448, 640, 480, &nw, &nh, &x, &y);
    CHECK(nw == 640 && nh == 448 && x == 0 && y == 0);
    display_layout(640, 448, 1280, 960, &nw, &nh, &x, &y);
    CHECK(nw == 640 && nh == 448 && x == 0 && y == 0);
    /* 16:9 full screen: bars left and right, the picture keeps 4:3 */
    display_layout(640, 448, 1920, 1080, &nw, &nh, &x, &y);
    CHECK(nh == 448 && nw == 853 && x == 106 && y == 0);
    /* a tall window: bars above and below */
    display_layout(640, 448, 800, 1000, &nw, &nh, &x, &y);
    CHECK(nw == 640 && nh == 747 && x == 0 && y == 149);
    /* a 512-wide picture is still shown 4:3 */
    display_layout(512, 448, 640, 480, &nw, &nh, &x, &y);
    CHECK(nw == 512 && nh == 448 && x == 0 && y == 0);
    display_layout(512, 448, 1920, 1080, &nw, &nh, &x, &y);
    CHECK(nh == 448 && nw == 683 && x == 85);
    /* the same code the window runs, read back pixel by pixel */
    for (i = 0; i < 640 * 448; i++) pic[i] = 0xFF000000u | ((hw_u32)i * 2654435761u >> 8);
    rc = display_selftest(pic, 640, 448, 1920, 1080, &bad);
    CHECK(rc == 0 && bad == 0);
    if (rc) printf("    display_selftest: code %d, %d pixels wrong\n", rc, bad);
    rc = display_selftest(pic, 640, 448, 800, 1000, &bad);
    CHECK(rc == 0 && bad == 0);
    rc = display_selftest(pic, 512, 448, 640, 480, &bad);
    CHECK(rc == 0 && bad == 0);
    free(pic);
}

int main(void)
{
    hw_stub_quiet = 1;
    gfx_test_ram = (u8 *)calloc(32u << 20, 1);
    gfx_test_spr = (u8 *)calloc(16384, 1);
    test_gs_mem();
    test_transfers();
    test_prims();
    test_texture();
    test_vif_unpack();
    test_vu1();
    test_dma_chain();
    test_display_and_capture();
    test_fast_paths();
    test_threads_identical();
    test_mips_gpu();
    test_fuzz();
    test_window_present();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
