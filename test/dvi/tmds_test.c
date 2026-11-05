/*
 * Host unit test for src/dvi/tmds.c
 *
 * Build and run on a PC (not the Pico):
 *
 *   cc -O2 -Wall -I src/dvi test/dvi/tmds_test.c src/dvi/tmds.c -o tmds_test && ./tmds_test
 *
 * Checks that every level symbol is a DC-balanced, decodable data symbol, and that
 * tmdsEncodeLine() produces lanes that a DVI receiver decodes back to the source
 * pixels, for both pixel-doubled lines and full-resolution (80 column) lines.
 */

#include "tmds.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { ++failures; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int popcount(uint32_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

static int isControl(uint32_t s)
{
  return s == TMDS_CTRL_00 || s == TMDS_CTRL_01 || s == TMDS_CTRL_10 || s == TMDS_CTRL_11;
}

/* reference DVI 1.0 encoder with running disparity, used to validate strict mode */
static uint16_t specEncode(uint8_t d, int *cnt)
{
  int n1d = popcount(d);
  int xnor = n1d > 4 || (n1d == 4 && !(d & 1));
  uint32_t qm = d & 1, prev = d & 1;
  for (int i = 1; i < 8; ++i)
  {
    uint32_t bit = prev ^ ((d >> i) & 1);
    if (xnor) bit ^= 1;
    qm |= bit << i;
    prev = bit;
  }
  int q8 = !xnor;
  int n1 = popcount(qm), n0 = 8 - n1;
  uint16_t out;
  if (*cnt == 0 || n1 == n0)
  {
    out = (uint16_t)((q8 ? qm : (~qm & 0xff)) | (q8 << 8) | ((!q8) << 9));
    *cnt += q8 ? (n1 - n0) : (n0 - n1);
  }
  else if ((*cnt > 0 && n1 > n0) || (*cnt < 0 && n0 > n1))
  {
    out = (uint16_t)((~qm & 0xff) | (q8 << 8) | (1 << 9));
    *cnt += 2 * q8 + (n0 - n1);
  }
  else
  {
    out = (uint16_t)(qm | (q8 << 8));
    *cnt += -2 * (!q8) + (n1 - n0);
  }
  return out;
}

static void checkTables(int strict)
{
  tmdsInitTables(strict);
  printf("%s symbols:\n", strict ? "strict" : "relaxed");
  for (int level = 0; level < 16; ++level)
  {
    uint16_t s = tmdsLevelSymbol(level);
    uint8_t v = tmdsLevelValue(level);
    int err = (int)v - level * 17;
    printf("  level %2d -> value %3d (%+d)  symbol 0x%03x\n", level, v, err, s);

    CHECK(s != 0, "level %d has no symbol", level);
    CHECK(s < 0x400, "level %d symbol out of range", level);
    CHECK(popcount(s) == 5, "level %d symbol 0x%03x not DC balanced", level, s);
    CHECK(!isControl(s), "level %d symbol 0x%03x is a control token", level, s);
    CHECK(tmdsDecodeSymbol(s) == v, "level %d symbol 0x%03x decodes to %d not %d", level, s, tmdsDecodeSymbol(s), v);
    if (!strict)
      CHECK(abs(err) <= 3, "level %d error %d too large", level, err);
    else
    {
      int cnt = 0;
      CHECK(specEncode(v, &cnt) == s && cnt == 0, "level %d strict symbol isn't spec encoder output", level);
    }
  }
  if (!strict)
  {
    CHECK(tmdsLevelValue(0) == 0, "relaxed black isn't 0");
    CHECK(tmdsLevelValue(15) == 255, "relaxed white isn't 255");
  }
}

/* pixel: bits 0-3 red, 4-7 green, 8-11 blue, 12-15 junk (renderer puts green there) */
static uint16_t makePixel(int r, int g, int b) { return (uint16_t)(r | (g << 4) | (b << 8) | (g << 12)); }

static void checkLine(int doubled, unsigned seed)
{
  enum { PAIRS = 320 };
  uint16_t px[PAIRS * 2];
  uint32_t lane[3][PAIRS];

  srand(seed);
  for (int i = 0; i < PAIRS * 2; i += 2)
  {
    px[i] = makePixel(rand() & 15, rand() & 15, rand() & 15);
    px[i + 1] = doubled ? px[i] : makePixel(rand() & 15, rand() & 15, rand() & 15);
    if ((rand() & 3) == 0 && i >= 2) { px[i] = px[i - 2]; px[i + 1] = px[i - 1]; }  // exercise the run path
  }

  uint32_t src[PAIRS];
  for (int i = 0; i < PAIRS; ++i) src[i] = px[2 * i] | ((uint32_t)px[2 * i + 1] << 16);

  tmdsEncodeLine(src, lane[0], lane[1], lane[2], PAIRS);

  // decode the serial stream: 2 symbols per word, bits 0-9 first, 20-31 unused
  int disparity[3] = { 0, 0, 0 };
  for (int i = 0; i < PAIRS * 2; ++i)
  {
    const int shifts[3] = { 8, 4, 0 };  // lane 0 blue, lane 1 green, lane 2 red
    for (int l = 0; l < 3; ++l)
    {
      uint32_t word = lane[l][i / 2];
      CHECK((word >> 20) == 0, "lane %d word %d has bits above 19", l, i / 2);
      uint16_t sym = (word >> ((i & 1) * 10)) & 0x3ff;
      disparity[l] += 2 * popcount(sym) - 10;
      uint8_t got = tmdsDecodeSymbol(sym);
      uint8_t want = tmdsLevelValue((px[i] >> shifts[l]) & 15);
      CHECK(got == want, "%s line, pixel %d lane %d: got %d want %d", doubled ? "doubled" : "full", i, l, got, want);
    }
  }
  for (int l = 0; l < 3; ++l)
    CHECK(disparity[l] == 0, "lane %d running disparity %d", l, disparity[l]);
}

int main(void)
{
  // (control tokens 0x154/0x2ab are not DC balanced - that is per the DVI spec, which
  //  resets the encoder disparity during blanking, so only data symbols are checked)

  // decoder sanity: every form of every value round-trips
  for (int v = 0; v < 256; ++v)
    for (int f = 0; f < 4; ++f)
      CHECK(tmdsDecodeSymbol(tmdsEncodeSymbol(v, f & 1, f & 2)) == v, "round trip %d form %d", v, f);

  checkTables(1);
  for (unsigned seed = 1; seed < 50; ++seed) { checkLine(1, seed); checkLine(0, seed); }

  checkTables(0);
  for (unsigned seed = 1; seed < 50; ++seed) { checkLine(1, seed); checkLine(0, seed); }

  printf(failures ? "\n%d FAILURES\n" : "\nall tests passed\n", failures);
  return failures != 0;
}
