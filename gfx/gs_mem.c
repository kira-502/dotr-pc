/* gs_mem.c - the GS's 4 MB local memory with its exact page/block/column layout per pixel format. */
#include <stdlib.h>
#include "gfx_internal.h"
#include "gs_fast.h"

u32 *gs_vram;

/* Block order inside a page (rows of blocks top to bottom, columns left to right). */
static const u8 blk32[4][8] = {
    { 0,  1,  4,  5, 16, 17, 20, 21},
    { 2,  3,  6,  7, 18, 19, 22, 23},
    { 8,  9, 12, 13, 24, 25, 28, 29},
    {10, 11, 14, 15, 26, 27, 30, 31},
};
static const u8 blk16[8][4] = {
    { 0,  2,  8, 10}, { 1,  3,  9, 11}, { 4,  6, 12, 14}, { 5,  7, 13, 15},
    {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31},
};
static const u8 blk16s[8][4] = {
    { 0,  2, 16, 18}, { 1,  3, 17, 19}, { 8, 10, 24, 26}, { 9, 11, 25, 27},
    { 4,  6, 20, 22}, { 5,  7, 21, 23}, {12, 14, 28, 30}, {13, 15, 29, 31},
};

/* Pixel order inside a block. */
static const u8 col32[8][8] = {
    { 0,  1,  4,  5,  8,  9, 12, 13}, { 2,  3,  6,  7, 10, 11, 14, 15},
    {16, 17, 20, 21, 24, 25, 28, 29}, {18, 19, 22, 23, 26, 27, 30, 31},
    {32, 33, 36, 37, 40, 41, 44, 45}, {34, 35, 38, 39, 42, 43, 46, 47},
    {48, 49, 52, 53, 56, 57, 60, 61}, {50, 51, 54, 55, 58, 59, 62, 63},
};
static const u8 col16[8][16] = {
    {  0,   2,   8,  10,  16,  18,  24,  26,   1,   3,   9,  11,  17,  19,  25,  27},
    {  4,   6,  12,  14,  20,  22,  28,  30,   5,   7,  13,  15,  21,  23,  29,  31},
    { 32,  34,  40,  42,  48,  50,  56,  58,  33,  35,  41,  43,  49,  51,  57,  59},
    { 36,  38,  44,  46,  52,  54,  60,  62,  37,  39,  45,  47,  53,  55,  61,  63},
    { 64,  66,  72,  74,  80,  82,  88,  90,  65,  67,  73,  75,  81,  83,  89,  91},
    { 68,  70,  76,  78,  84,  86,  92,  94,  69,  71,  77,  79,  85,  87,  93,  95},
    { 96,  98, 104, 106, 112, 114, 120, 122,  97,  99, 105, 107, 113, 115, 121, 123},
    {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127},
};
/* 8-bit: one column (16x4) of bytes; the four columns of a block alternate between two patterns. */
static const u8 col8_even[4][16] = {
    { 0,  4, 16, 20, 32, 36, 48, 52,  2,  6, 18, 22, 34, 38, 50, 54},
    { 8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
    {33, 37, 49, 53,  1,  5, 17, 21, 35, 39, 51, 55,  3,  7, 19, 23},
    {41, 45, 57, 61,  9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
};
static const u8 col8_odd[4][16] = {
    {32, 36, 48, 52,  0,  4, 16, 20, 34, 38, 50, 54,  2,  6, 18, 22},
    {40, 44, 56, 60,  8, 12, 24, 28, 42, 46, 58, 62, 10, 14, 26, 30},
    { 1,  5, 17, 21, 33, 37, 49, 53,  3,  7, 19, 23, 35, 39, 51, 55},
    { 9, 13, 25, 29, 41, 45, 57, 61, 11, 15, 27, 31, 43, 47, 59, 63},
};
/* 4-bit: one column (32x4) of nibbles. */
static const u8 col4_even[4][32] = {
    { 0,  8, 32, 40, 64, 72, 96, 104,  2, 10, 34, 42, 66, 74, 98, 106,  4, 12, 36, 44, 68, 76, 100, 108,  6, 14, 38, 46, 70, 78, 102, 110},
    {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
    {65, 73, 97, 105,  1,  9, 33, 41, 67, 75, 99, 107,  3, 11, 35, 43, 69, 77, 101, 109,  5, 13, 37, 45, 71, 79, 103, 111,  7, 15, 39, 47},
    {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
};
static const u8 col4_odd[4][32] = {
    {64, 72, 96, 104,  0,  8, 32, 40, 66, 74, 98, 106,  2, 10, 34, 42, 68, 76, 100, 108,  4, 12, 36, 44, 70, 78, 102, 110,  6, 14, 38, 46},
    {80, 88, 112, 120, 16, 24, 48, 56, 82, 90, 114, 122, 18, 26, 50, 58, 84, 92, 116, 124, 20, 28, 52, 60, 86, 94, 118, 126, 22, 30, 54, 62},
    { 1,  9, 33, 41, 65, 73, 97, 105,  3, 11, 35, 43, 67, 75, 99, 107,  5, 13, 37, 45, 69, 77, 101, 109,  7, 15, 39, 47, 71, 79, 103, 111},
    {17, 25, 49, 57, 81, 89, 113, 121, 19, 27, 51, 59, 83, 91, 115, 123, 21, 29, 53, 61, 85, 93, 117, 125, 23, 31, 55, 63, 87, 95, 119, 127},
};

void gs_mem_init(void)
{
    if (!gs_vram)
        gs_vram = (u32 *)calloc(GS_MEM_WORDS, 4);
    else
        memset(gs_vram, 0, GS_MEM_BYTES);
    gs_invalidate_all();
}

/* Block number (0..16383) of the block holding pixel (x,y). pw/ph = page size, bw_pages = pages per row. */
#define BLOCK(bp, page, b) (((bp) + (page) * 32u + (b)) & 0x3FFFu)

u32 gs_addr32(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 page = (y >> 5) * bw + (x >> 6);
    u32 b = blk32[(y >> 3) & 3][(x >> 3) & 7];
    return BLOCK(bp, page, b) * 64u + col32[y & 7][x & 7];
}

u32 gs_addr32z(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 page = (y >> 5) * bw + (x >> 6);
    u32 b = blk32[(y >> 3) & 3][(x >> 3) & 7] ^ 24u;
    return BLOCK(bp, page, b) * 64u + col32[y & 7][x & 7];
}

static u32 addr16_common(const u8 blk[8][4], u32 bp, u32 bw, u32 x, u32 y, u32 xr)
{
    u32 page = (y >> 6) * bw + (x >> 6);
    u32 b = blk[(y >> 3) & 7][(x >> 4) & 3] ^ xr;
    return BLOCK(bp, page, b) * 128u + col16[y & 7][x & 15];
}

u32 gs_addr16(u32 bp, u32 bw, u32 x, u32 y)   { return addr16_common(blk16, bp, bw, x, y, 0); }
u32 gs_addr16s(u32 bp, u32 bw, u32 x, u32 y)  { return addr16_common(blk16s, bp, bw, x, y, 0); }
u32 gs_addr16z(u32 bp, u32 bw, u32 x, u32 y)  { return addr16_common(blk16, bp, bw, x, y, 24); }
u32 gs_addr16sz(u32 bp, u32 bw, u32 x, u32 y) { return addr16_common(blk16s, bp, bw, x, y, 24); }

u32 gs_addr8(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 page = (y >> 6) * (bw >> 1) + (x >> 7);
    u32 b = blk32[(y >> 4) & 3][(x >> 4) & 7];
    u32 c = (y >> 2) & 3;   /* column inside the block */
    u32 o = (c & 1) ? col8_odd[y & 3][x & 15] : col8_even[y & 3][x & 15];
    return BLOCK(bp, page, b) * 256u + c * 64u + o;
}

u32 gs_addr4(u32 bp, u32 bw, u32 x, u32 y)
{
    u32 page = (y >> 7) * (bw >> 1) + (x >> 7);
    u32 b = blk16[(y >> 4) & 7][(x >> 5) & 3];
    u32 c = (y >> 2) & 3;
    u32 o = (c & 1) ? col4_odd[y & 3][x & 31] : col4_even[y & 3][x & 31];
    return BLOCK(bp, page, b) * 512u + c * 128u + o;
}

int gs_psm_bpp(u32 psm)
{
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMZ32: case PSMZ24: case PSMT8H: case PSMT4HL: case PSMT4HH: return 32;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    default: return 32;
    }
}

u32 gs_read_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y)
{
    u8 *b8 = (u8 *)gs_vram;
    u16 *b16 = (u16 *)gs_vram;
    u32 a;
    switch (psm) {
    case PSMCT32:  return gs_vram[gs_addr32(bp, bw, x, y)];
    case PSMCT24:  return gs_vram[gs_addr32(bp, bw, x, y)] & 0xFFFFFFu;
    case PSMZ32:   return gs_vram[gs_addr32z(bp, bw, x, y)];
    case PSMZ24:   return gs_vram[gs_addr32z(bp, bw, x, y)] & 0xFFFFFFu;
    case PSMCT16:  return b16[gs_addr16(bp, bw, x, y)];
    case PSMCT16S: return b16[gs_addr16s(bp, bw, x, y)];
    case PSMZ16:   return b16[gs_addr16z(bp, bw, x, y)];
    case PSMZ16S:  return b16[gs_addr16sz(bp, bw, x, y)];
    case PSMT8:    return b8[gs_addr8(bp, bw, x, y)];
    case PSMT4:    a = gs_addr4(bp, bw, x, y); return (b8[a >> 1] >> ((a & 1) * 4)) & 0xF;
    case PSMT8H:   return gs_vram[gs_addr32(bp, bw, x, y)] >> 24;
    case PSMT4HL:  return (gs_vram[gs_addr32(bp, bw, x, y)] >> 24) & 0xF;
    case PSMT4HH:  return gs_vram[gs_addr32(bp, bw, x, y)] >> 28;
    default:       return gs_vram[gs_addr32(bp, bw, x, y)];
    }
}

int gs_store_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v);

void gs_write_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v)
{
    {
        /* keep the texture cache honest: the block this pixel lives in has changed */
        u32 blk;
        switch (gs_psm_bpp(psm)) {
        case 32: blk = ((psm & 0x30) == 0x30 ? gs_addr32z(bp, bw, x, y) : gs_addr32(bp, bw, x, y)) >> 6; break;
        case 16: blk = (psm == PSMCT16 ? gs_addr16(bp, bw, x, y) : psm == PSMCT16S ? gs_addr16s(bp, bw, x, y) :
                        psm == PSMZ16 ? gs_addr16z(bp, bw, x, y) : gs_addr16sz(bp, bw, x, y)) >> 7; break;
        case 8:  blk = gs_addr8(bp, bw, x, y) >> 8; break;
        default: blk = gs_addr4(bp, bw, x, y) >> 9; break;
        }
        gs_mark_written(blk, blk);
    }
    gs_store_pixel(psm, bp, bw, x, y, v);
}

/* The store alone: the caller tells the texture cache which blocks changed (an image transfer marks its whole
 * rectangle once, and only when a pixel really changed). Returns 1 when the memory changed. */
int gs_store_pixel(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 v)
{
    u8 *b8 = (u8 *)gs_vram;
    u16 *b16 = (u16 *)gs_vram;
    u32 a, *p, nv;
    u16 *h;
    u8 *c, nb;
    switch (psm) {
    case PSMCT16:  h = &b16[gs_addr16(bp, bw, x, y)]; goto half;
    case PSMCT16S: h = &b16[gs_addr16s(bp, bw, x, y)]; goto half;
    case PSMZ16:   h = &b16[gs_addr16z(bp, bw, x, y)]; goto half;
    case PSMZ16S:  h = &b16[gs_addr16sz(bp, bw, x, y)]; goto half;
    case PSMT8:    c = &b8[gs_addr8(bp, bw, x, y)]; nb = (u8)v; goto byte;
    case PSMT4:
        a = gs_addr4(bp, bw, x, y);
        c = &b8[a >> 1];
        nb = (u8)((a & 1) ? ((*c & 0x0F) | ((v & 0xF) << 4)) : ((*c & 0xF0) | (v & 0xF)));
        goto byte;
    case PSMZ32: case PSMZ24: p = &gs_vram[gs_addr32z(bp, bw, x, y)]; break;
    default:     p = &gs_vram[gs_addr32(bp, bw, x, y)]; break;
    }
    switch (psm) {
    case PSMCT24: case PSMZ24: nv = (*p & 0xFF000000u) | (v & 0xFFFFFFu); break;
    case PSMT8H:  nv = (*p & 0x00FFFFFFu) | (v << 24); break;
    case PSMT4HL: nv = (*p & 0xF0FFFFFFu) | ((v & 0xF) << 24); break;
    case PSMT4HH: nv = (*p & 0x0FFFFFFFu) | ((v & 0xF) << 28); break;
    default:      nv = v; break;
    }
    if (*p == nv) return 0;
    *p = nv;
    return 1;
half:
    if (*h == (u16)v) return 0;
    *h = (u16)v;
    return 1;
byte:
    if (*c == nb) return 0;
    *c = nb;
    return 1;
}
