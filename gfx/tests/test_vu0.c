/* test_vu0.c - checks of the translated VU0 maths against plain double-precision C. Windowless. */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "../vu0/vu0.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The 14 native game functions the translated code calls: never reached by these tests. */
void RhGeom_GetScaleRotXYZTransMatrix(u32 a, u32 b, u32 c, u32 d) { (void)a; (void)b; (void)c; (void)d; }
float RhMath_CosI(u32 a) { return cosf((float)(s32)a * 6.2831853f / 65536.0f); }
float RhMath_SinI(u32 a) { return sinf((float)(s32)a * 6.2831853f / 65536.0f); }
u32 RhMem_CopyMemory16(u32 d, u32 s, u32 n) { memcpy((void *)(uintptr_t)d, (void *)(uintptr_t)s, n * 16); return d; }
u32 RhView_GetClipZ(u32 a, u32 b) { (void)a; (void)b; return 0; }
u32 RhView_GetFogParameter(u32 a, u32 b, u32 c, u32 d, u32 e) { (void)a; (void)b; (void)c; (void)d; (void)e; return 0; }
void RhView_GetViewScreenMatrix(u32 a) { (void)a; }
u32 func_001A23B0(float f) { return (u32)(s32)f; }
u32 lRhModelCpu_TraceStaticBlock(u32 a, u32 b, u32 c) { (void)b; (void)c; return a; }
u32 lRhModel_SetupFloatMotionRecursive(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return 0; }
u32 lRhModel_SetupMultiMotionRecursive(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return 0; }
void lRhModel_SetupNoMotionRecursive(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; }
u32 lRhModel_SetupPulseMotionRecursive(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return 0; }
u32 sceDmaGetChan(u32 a) { (void)a; return 0; }

/* the translated functions (all take PS2 addresses, i.e. host pointers in this 32-bit process) */
void sceVu0ApplyMatrix(u32, u32, u32);
void sceVu0MulMatrix(u32, u32, u32);
void sceVu0OuterProduct(u32, u32, u32);
float sceVu0InnerProduct(u32, u32);
void sceVu0Normalize(u32, u32);
void sceVu0TransposeMatrix(u32, u32);
void sceVu0InversMatrix(u32, u32);
void sceVu0ScaleVector(u32, u32, float);
void sceVu0DivVector(u32, u32, float);
void sceVu0InterVector(u32, u32, u32, float);
void sceVu0UnitMatrix(u32);
void sceVu0FTOI4Vector(u32, u32);
void sceVu0ITOF4Vector(u32, u32);
void sceVu0ClampVector(u32, u32, float, float);
void sceVu0RotTransPers(u32, u32, u32, u32);
void sceVu0SubVector(u32, u32, u32);
void RhGeom_SetMatrix(u32);
void RhGeom_GetMatrix(u32);
void RhGeom_MulMatrix(u32, u32);
void RhGeom_MulMatrixVector(u32, u32, u32);
void RhGeom_MulMatrixVectorArray(u32, u32, u32, u32);
void RhGeom_PushMatrix(u32);
void RhGeom_PopMatrix(void);

#define A(p) ((u32)(uintptr_t)(p))
typedef __declspec(align(16)) float m44[4][4];
typedef __declspec(align(16)) float v4[4];

static int near_(float a, float b) { return fabsf(a - b) <= 1e-4f * (1.0f + fabsf(b)); }

static void ref_mul(float out[4][4], float a[4][4], float b[4][4])
{
    /* sceVu0MulMatrix(m0, m1, m2): m0 = m1 * m2, columns of m2 transformed by m1 (row-vector PS2 layout) */
    int i, j, k;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            double s = 0;
            for (k = 0; k < 4; k++) s += (double)a[k][j] * b[i][k];
            out[i][j] = (float)s;
        }
}

int main(void)
{
    m44 a, b, c, r, u;
    v4 v, w, o;
    int i, j, ok;
    setvbuf(stdout, NULL, _IONBF, 0);
    vu0_reset();
    for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) { a[i][j] = (float)(i * 4 + j + 1) * 0.25f; b[i][j] = (float)((i + 2) * (j + 1)) - 3.0f; }
    printf("sceVu0 matrix/vector functions\n");
    sceVu0MulMatrix(A(c), A(a), A(b));
    ref_mul(r, a, b);
    ok = 1; for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) if (!near_(c[i][j], r[i][j])) ok = 0;
    CHECK(ok);
    v[0] = 1; v[1] = 2; v[2] = 3; v[3] = 1;
    sceVu0ApplyMatrix(A(o), A(a), A(v));
    ok = 1;
    for (j = 0; j < 4; j++) { double s = 0; for (i = 0; i < 4; i++) s += (double)a[i][j] * v[i]; if (!near_(o[j], (float)s)) ok = 0; }
    CHECK(ok);
    w[0] = 4; w[1] = 5; w[2] = 6; w[3] = 7;
    CHECK(near_(sceVu0InnerProduct(A(v), A(w)), 32.0f));
    sceVu0OuterProduct(A(o), A(v), A(w));
    CHECK(near_(o[0], 2 * 6 - 3 * 5) && near_(o[1], 3 * 4 - 1 * 6) && near_(o[2], 1 * 5 - 2 * 4) && o[3] == 0);
    sceVu0Normalize(A(o), A(w));
    CHECK(near_(o[0] * o[0] + o[1] * o[1] + o[2] * o[2], 1.0f));
    sceVu0TransposeMatrix(A(c), A(a));
    CHECK(c[1][2] == a[2][1] && c[3][0] == a[0][3]);
    sceVu0UnitMatrix(A(u));
    CHECK(u[0][0] == 1 && u[1][1] == 1 && u[3][3] == 1 && u[0][1] == 0 && u[3][0] == 0);
    /* rigid inverse: rotation 90 degrees about z plus translation */
    memset(c, 0, sizeof c);
    c[0][1] = 1; c[1][0] = -1; c[2][2] = 1; c[3][0] = 5; c[3][1] = 6; c[3][2] = 7; c[3][3] = 1;
    sceVu0InversMatrix(A(r), A(c));
    sceVu0MulMatrix(A(b), A(c), A(r));
    ok = 1; for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) if (!near_(b[i][j], i == j ? 1.0f : 0.0f)) ok = 0;
    CHECK(ok);
    sceVu0ScaleVector(A(o), A(w), 0.5f);
    CHECK(o[0] == 2 && o[3] == 3.5f);
    sceVu0DivVector(A(o), A(w), 2.0f);
    CHECK(near_(o[1], 2.5f));
    sceVu0InterVector(A(o), A(v), A(w), 0.25f);
    CHECK(near_(o[0], 0.25f * 1 + 0.75f * 4));
    sceVu0SubVector(A(o), A(w), A(v));
    CHECK(o[0] == 3 && o[2] == 3 && o[3] == 6);
    o[0] = 1.5f; o[1] = -2.0f; o[2] = 100.0f; o[3] = 0;
    sceVu0FTOI4Vector(A(w), A(o));
    CHECK(((s32 *)w)[0] == 24 && ((s32 *)w)[1] == -32 && ((s32 *)w)[2] == 1600);
    sceVu0ITOF4Vector(A(o), A(w));
    CHECK(o[0] == 1.5f && o[2] == 100.0f);
    o[0] = -5; o[1] = 0.5f; o[2] = 9; o[3] = 3;
    sceVu0ClampVector(A(w), A(o), 0.0f, 4.0f);
    CHECK(w[0] == 0 && w[1] == 0.5f && w[2] == 4 && w[3] == 3);
    /* RotTransPers with the unit matrix: (x/w, y/w, z/w) in 12.4 fixed, z and w integer with mode 1 */
    v[0] = 100; v[1] = 50; v[2] = 1000; v[3] = 2;
    sceVu0RotTransPers(A(w), A(u), A(v), 1);
    CHECK(((s32 *)w)[0] == 50 * 16 && ((s32 *)w)[1] == 25 * 16 && ((s32 *)w)[2] == 500);

    printf("RhGeom (current matrix in vf28-vf31)\n");
    {
        /* the matrix stack lives at D_00326A20 with its depth at D_0032A220: map that page for this test */
        void *pg = VirtualAlloc((void *)0x00326000, 0x5000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        RhGeom_SetMatrix(A(a));
        RhGeom_GetMatrix(A(c));
        CHECK(memcmp(c, a, sizeof a) == 0);
        RhGeom_MulMatrix(0, A(b));
        sceVu0MulMatrix(A(r), A(a), A(b));
        RhGeom_GetMatrix(A(c));
        CHECK(memcmp(c, r, sizeof r) == 0);
        v[0] = 1; v[1] = -1; v[2] = 2; v[3] = 1;
        RhGeom_MulMatrixVector(A(o), 0, A(v));
        sceVu0ApplyMatrix(A(w), A(r), A(v));
        CHECK(memcmp(o, w, sizeof o) == 0);
        {
            /* matrices the game keeps on its stack are only 4-aligned in the port (the duel camera came out
             * scrambled when 128-bit loads dropped the low address bits) */
            static float buf[2 * 16 + 1];
            float *x = buf + 1, *y = buf + 17;
            memcpy(x, a, sizeof a); memcpy(y, b, sizeof b);
            RhGeom_MulMatrix(A(x), A(y));
            ref_mul(r, a, b);
            ok = 1; for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) if (!near_(x[i * 4 + j], r[i][j])) ok = 0;
            CHECK(ok);
        }
        {
            __declspec(align(16)) float in[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } }, out[3][4];
            RhGeom_MulMatrixVectorArray(A(out), 0, A(in), 3);
            sceVu0ApplyMatrix(A(w), A(r), A(in[2]));
            CHECK(memcmp(out[2], w, 16) == 0);
        }
        if (pg) {
            RhGeom_PushMatrix(A(u));
            RhGeom_GetMatrix(A(c));
            CHECK(memcmp(c, u, sizeof u) == 0);
            RhGeom_PopMatrix();
            RhGeom_GetMatrix(A(c));
            CHECK(memcmp(c, r, sizeof r) == 0);
        } else {
            printf("  (matrix stack page not mappable here; push/pop skipped)\n");
        }
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
