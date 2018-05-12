#include <stdint.h>

typedef uint16_t u16;
typedef uint32_t u32;

#if MAXSTRINGLEN != 41
#error This test is hard-coded for MAXSTRINGLEN==41
#endif

#define UPTO5  '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5'
#define UPTO6  UPTO5, '6'
#define UPTO7  UPTO6, '7'
#define UPTO8  UPTO7, '8'
#define UPTO9  UPTO8, '9'


static u16 s16_len1_0[] = {UPTO8, 0x5A, 0}; // Z
static u16 s16_len1_1[] = {UPTO9, 0x5A, 0};

static u16 s16_len2_0[] = {UPTO7, 0x3A9, 0}; // Ω
static u16 s16_len2_1[] = {UPTO8, 0x3A9, 0};
static u16 s16_len2_2[] = {UPTO9, 0x3A9, 0};

static u16 s16_len3_0[] = {UPTO6, 0x263A, 0}; // ☺
static u16 s16_len3_1[] = {UPTO7, 0x263A, 0};
static u16 s16_len3_2[] = {UPTO8, 0x263A, 0};
static u16 s16_len3_3[] = {UPTO9, 0x263A, 0};

static u16 s16_len4_0[] = {UPTO5, 0xD83D, 0xDE08, 0}; // 😈
static u16 s16_len4_1[] = {UPTO6, 0xD83D, 0xDE08, 0};
static u16 s16_len4_2[] = {UPTO7, 0xD83D, 0xDE08, 0};
static u16 s16_len4_3[] = {UPTO8, 0xD83D, 0xDE08, 0};
static u16 s16_len4_4[] = {UPTO9, 0xD83D, 0xDE08, 0};

static u16 *utf16_strings[] = {
  s16_len1_0, s16_len1_1,
  s16_len2_0, s16_len2_1, s16_len2_2,
  s16_len3_0, s16_len3_1, s16_len3_2, s16_len3_3,
  s16_len4_0, s16_len4_1, s16_len4_2, s16_len4_3, s16_len4_4,
  0
};


static u32 s32_len1_0[] = {UPTO8, 0x5A, 0}; // Z
static u32 s32_len1_1[] = {UPTO9, 0x5A, 0};

static u32 s32_len2_0[] = {UPTO7, 0x3A9, 0}; // Ω
static u32 s32_len2_1[] = {UPTO8, 0x3A9, 0};
static u32 s32_len2_2[] = {UPTO9, 0x3A9, 0};

static u32 s32_len3_0[] = {UPTO6, 0x263A, 0}; // ☺
static u32 s32_len3_1[] = {UPTO7, 0x263A, 0};
static u32 s32_len3_2[] = {UPTO8, 0x263A, 0};
static u32 s32_len3_3[] = {UPTO9, 0x263A, 0};

static u32 s32_len4_0[] = {UPTO5, 0x1F608, 0}; // 😈
static u32 s32_len4_1[] = {UPTO6, 0x1F608, 0};
static u32 s32_len4_2[] = {UPTO7, 0x1F608, 0};
static u32 s32_len4_3[] = {UPTO8, 0x1F608, 0};
static u32 s32_len4_4[] = {UPTO9, 0x1F608, 0};

static u32 *utf32_strings[] = {
  s32_len1_0, s32_len1_1,
  s32_len2_0, s32_len2_1, s32_len2_2,
  s32_len3_0, s32_len3_1, s32_len3_2, s32_len3_3,
  s32_len4_0, s32_len4_1, s32_len4_2, s32_len4_3, s32_len4_4,
  0
};


int main()
{
    return 0;
}
