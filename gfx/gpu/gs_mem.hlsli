// gs_mem.hlsli - GS local memory on the GPU: the same page/block/column layouts as gs_mem.c, over a 4 MB
// byte-address buffer of 32-bit words. Word, halfword, byte and nibble indices as gs_addr32/16/8/4 return them.

#define PSMCT32  0x00
#define PSMCT24  0x01
#define PSMCT16  0x02
#define PSMCT16S 0x0A
#define PSMT8    0x13
#define PSMT4    0x14
#define PSMT8H   0x1B
#define PSMT4HL  0x24
#define PSMT4HH  0x2C
#define PSMZ32   0x30
#define PSMZ24   0x31
#define PSMZ16   0x32
#define PSMZ16S  0x3A

static const uint blk32[32] = {
      0,   1,   4,   5,  16,  17,  20,  21,   2,   3,   6,   7,  18,  19,  22,  23,
      8,   9,  12,  13,  24,  25,  28,  29,  10,  11,  14,  15,  26,  27,  30,  31 };
static const uint blk16[32] = {
      0,   2,   8,  10,   1,   3,   9,  11,   4,   6,  12,  14,   5,   7,  13,  15,
     16,  18,  24,  26,  17,  19,  25,  27,  20,  22,  28,  30,  21,  23,  29,  31 };
static const uint blk16s[32] = {
      0,   2,  16,  18,   1,   3,  17,  19,   8,  10,  24,  26,   9,  11,  25,  27,
      4,   6,  20,  22,   5,   7,  21,  23,  12,  14,  28,  30,  13,  15,  29,  31 };
static const uint col32[64] = {
      0,   1,   4,   5,   8,   9,  12,  13,   2,   3,   6,   7,  10,  11,  14,  15,
     16,  17,  20,  21,  24,  25,  28,  29,  18,  19,  22,  23,  26,  27,  30,  31,
     32,  33,  36,  37,  40,  41,  44,  45,  34,  35,  38,  39,  42,  43,  46,  47,
     48,  49,  52,  53,  56,  57,  60,  61,  50,  51,  54,  55,  58,  59,  62,  63 };
static const uint col16[128] = {
      0,   2,   8,  10,  16,  18,  24,  26,   1,   3,   9,  11,  17,  19,  25,  27,
      4,   6,  12,  14,  20,  22,  28,  30,   5,   7,  13,  15,  21,  23,  29,  31,
     32,  34,  40,  42,  48,  50,  56,  58,  33,  35,  41,  43,  49,  51,  57,  59,
     36,  38,  44,  46,  52,  54,  60,  62,  37,  39,  45,  47,  53,  55,  61,  63,
     64,  66,  72,  74,  80,  82,  88,  90,  65,  67,  73,  75,  81,  83,  89,  91,
     68,  70,  76,  78,  84,  86,  92,  94,  69,  71,  77,  79,  85,  87,  93,  95,
     96,  98, 104, 106, 112, 114, 120, 122,  97,  99, 105, 107, 113, 115, 121, 123,
    100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127 };
static const uint col8_even[64] = {
      0,   4,  16,  20,  32,  36,  48,  52,   2,   6,  18,  22,  34,  38,  50,  54,
      8,  12,  24,  28,  40,  44,  56,  60,  10,  14,  26,  30,  42,  46,  58,  62,
     33,  37,  49,  53,   1,   5,  17,  21,  35,  39,  51,  55,   3,   7,  19,  23,
     41,  45,  57,  61,   9,  13,  25,  29,  43,  47,  59,  63,  11,  15,  27,  31 };
static const uint col8_odd[64] = {
     32,  36,  48,  52,   0,   4,  16,  20,  34,  38,  50,  54,   2,   6,  18,  22,
     40,  44,  56,  60,   8,  12,  24,  28,  42,  46,  58,  62,  10,  14,  26,  30,
      1,   5,  17,  21,  33,  37,  49,  53,   3,   7,  19,  23,  35,  39,  51,  55,
      9,  13,  25,  29,  41,  45,  57,  61,  11,  15,  27,  31,  43,  47,  59,  63 };
static const uint col4_even[128] = {
      0,   8,  32,  40,  64,  72,  96, 104,   2,  10,  34,  42,  66,  74,  98, 106,
      4,  12,  36,  44,  68,  76, 100, 108,   6,  14,  38,  46,  70,  78, 102, 110,
     16,  24,  48,  56,  80,  88, 112, 120,  18,  26,  50,  58,  82,  90, 114, 122,
     20,  28,  52,  60,  84,  92, 116, 124,  22,  30,  54,  62,  86,  94, 118, 126,
     65,  73,  97, 105,   1,   9,  33,  41,  67,  75,  99, 107,   3,  11,  35,  43,
     69,  77, 101, 109,   5,  13,  37,  45,  71,  79, 103, 111,   7,  15,  39,  47,
     81,  89, 113, 121,  17,  25,  49,  57,  83,  91, 115, 123,  19,  27,  51,  59,
     85,  93, 117, 125,  21,  29,  53,  61,  87,  95, 119, 127,  23,  31,  55,  63 };
static const uint col4_odd[128] = {
     64,  72,  96, 104,   0,   8,  32,  40,  66,  74,  98, 106,   2,  10,  34,  42,
     68,  76, 100, 108,   4,  12,  36,  44,  70,  78, 102, 110,   6,  14,  38,  46,
     80,  88, 112, 120,  16,  24,  48,  56,  82,  90, 114, 122,  18,  26,  50,  58,
     84,  92, 116, 124,  20,  28,  52,  60,  86,  94, 118, 126,  22,  30,  54,  62,
      1,   9,  33,  41,  65,  73,  97, 105,   3,  11,  35,  43,  67,  75,  99, 107,
      5,  13,  37,  45,  69,  77, 101, 109,   7,  15,  39,  47,  71,  79, 103, 111,
     17,  25,  49,  57,  81,  89, 113, 121,  19,  27,  51,  59,  83,  91, 115, 123,
     21,  29,  53,  61,  85,  93, 117, 125,  23,  31,  55,  63,  87,  95, 119, 127 };

uint gs_block(uint bp, uint page, uint b) { return (bp + page * 32u + b) & 0x3FFFu; }

uint gs_addr32(uint bp, uint bw, uint x, uint y)
{
    uint page = (y >> 5) * bw + (x >> 6);
    uint b = blk32[((y >> 3) & 3) * 8 + ((x >> 3) & 7)];
    return gs_block(bp, page, b) * 64u + col32[(y & 7) * 8 + (x & 7)];
}

uint gs_addr32z(uint bp, uint bw, uint x, uint y)
{
    uint page = (y >> 5) * bw + (x >> 6);
    uint b = blk32[((y >> 3) & 3) * 8 + ((x >> 3) & 7)] ^ 24u;
    return gs_block(bp, page, b) * 64u + col32[(y & 7) * 8 + (x & 7)];
}

// 16-bit formats: swz picks the block order (0 = CT16, 1 = CT16S), xr flips the z layouts
uint gs_addr16x(uint bp, uint bw, uint x, uint y, uint swz, uint xr)
{
    uint page = (y >> 6) * bw + (x >> 6);
    uint i = ((y >> 3) & 7) * 4 + ((x >> 4) & 3);
    uint b = (swz ? blk16s[i] : blk16[i]) ^ xr;
    return gs_block(bp, page, b) * 128u + col16[(y & 7) * 16 + (x & 15)];
}

uint gs_addr8(uint bp, uint bw, uint x, uint y)
{
    uint page = (y >> 6) * (bw >> 1) + (x >> 7);
    uint b = blk32[((y >> 4) & 3) * 8 + ((x >> 4) & 7)];
    uint c = (y >> 2) & 3;
    uint i = (y & 3) * 16 + (x & 15);
    uint o = (c & 1) ? col8_odd[i] : col8_even[i];
    return gs_block(bp, page, b) * 256u + c * 64u + o;
}

uint gs_addr4(uint bp, uint bw, uint x, uint y)
{
    uint page = (y >> 7) * (bw >> 1) + (x >> 7);
    uint b = blk16[((y >> 4) & 7) * 4 + ((x >> 5) & 3)];
    uint c = (y >> 2) & 3;
    uint i = (y & 3) * 32 + (x & 31);
    uint o = (c & 1) ? col4_odd[i] : col4_even[i];
    return gs_block(bp, page, b) * 512u + c * 128u + o;
}

// Where a pixel lives: the word, the bit shift of the pixel inside it and its bit mask.
void gs_locate(uint psm, uint bp, uint bw, uint x, uint y, out uint word, out uint shift, out uint mask)
{
    uint a;
    shift = 0; mask = 0xFFFFFFFFu;
    switch (psm) {
    case PSMZ32: case PSMZ24:
        word = gs_addr32z(bp, bw, x, y);
        if (psm == PSMZ24) mask = 0xFFFFFFu;
        break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
        a = gs_addr16x(bp, bw, x, y, (psm & 8) ? 1u : 0u, (psm & 0x30) == 0x30 ? 24u : 0u);
        word = a >> 1; shift = (a & 1) * 16; mask = 0xFFFFu;
        break;
    case PSMT8:
        a = gs_addr8(bp, bw, x, y);
        word = a >> 2; shift = (a & 3) * 8; mask = 0xFFu;
        break;
    case PSMT4:
        a = gs_addr4(bp, bw, x, y);
        word = a >> 3; shift = (a & 7) * 4; mask = 0xFu;
        break;
    case PSMT8H:  word = gs_addr32(bp, bw, x, y); shift = 24; mask = 0xFFu; break;
    case PSMT4HL: word = gs_addr32(bp, bw, x, y); shift = 24; mask = 0xFu; break;
    case PSMT4HH: word = gs_addr32(bp, bw, x, y); shift = 28; mask = 0xFu; break;
    case PSMCT24: word = gs_addr32(bp, bw, x, y); mask = 0xFFFFFFu; break;
    default:      word = gs_addr32(bp, bw, x, y); break;
    }
}
