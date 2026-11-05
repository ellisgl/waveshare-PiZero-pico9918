/*
 * Project: pico9918 - dvi
 *
 * TMDS symbol tables and scanline encoder.
 *
 * This code is licensed under the MIT license
 */

#include "tmds.h"

#ifdef PICO_BUILD
#include "pico.h"
#else
#define __time_critical_func(f) f
#endif

uint32_t __attribute__((aligned(4))) tmdsPairTable[256];

static uint16_t levelSymbol[16];
static uint8_t  levelValue[16];

static int popcount10(uint32_t s)
{
  int n = 0;
  for (int i = 0; i < 10; ++i) n += (s >> i) & 1;
  return n;
}

static int transitions10(uint32_t s)
{
  int n = 0;
  for (int i = 1; i < 10; ++i) n += ((s >> i) ^ (s >> (i - 1))) & 1;
  return n;
}

static bool isControlToken(uint32_t s)
{
  return s == TMDS_CTRL_00 || s == TMDS_CTRL_01 || s == TMDS_CTRL_10 || s == TMDS_CTRL_11;
}

uint16_t tmdsEncodeSymbol(uint8_t value, bool xnor, bool invert)
{
  uint32_t prev = value & 1;
  uint32_t q = prev;
  for (int i = 1; i < 8; ++i)
  {
    uint32_t bit = prev ^ ((value >> i) & 1);
    if (xnor) bit ^= 1;
    q |= bit << i;
    prev = bit;
  }
  if (invert) q ^= 0xff;
  q |= (xnor ? 0u : 1u) << 8;
  q |= (invert ? 1u : 0u) << 9;
  return (uint16_t)q;
}

uint8_t tmdsDecodeSymbol(uint16_t symbol)
{
  uint32_t data = symbol & 0xff;
  if (symbol & 0x200) data ^= 0xff;
  const bool xorMode = (symbol & 0x100) != 0;

  uint32_t d = data & 1;
  for (int i = 1; i < 8; ++i)
  {
    uint32_t bit = ((data >> i) ^ (data >> (i - 1))) & 1;
    if (!xorMode) bit ^= 1;
    d |= bit << i;
  }
  return (uint8_t)d;
}

/* the q_m mode a spec-compliant DVI encoder picks for this value */
static bool specUsesXnor(uint8_t value)
{
  int ones = 0;
  for (int i = 0; i < 8; ++i) ones += (value >> i) & 1;
  return ones > 4 || (ones == 4 && (value & 1) == 0);
}

/*
 * find a DC-balanced symbol for an exact 8-bit value. returns 0 if there isn't one.
 * (0 is never a valid balanced symbol, so it doubles as "none")
 */
static uint16_t balancedSymbolFor(uint8_t value, bool strict)
{
  const bool specXnor = specUsesXnor(value);

  // spec encoder: when q_m is balanced, q_out[9] = !q_m[8] and data is inverted iff q_m[8] == 0
  uint16_t spec = tmdsEncodeSymbol(value, specXnor, specXnor);
  if (popcount10(spec) == 5 && !isControlToken(spec))
    return spec;

  if (strict)
    return 0;

  // any other decodable form (the receiver only looks at bits 8 and 9)
  uint16_t best = 0;
  for (int form = 0; form < 4; ++form)
  {
    uint16_t s = tmdsEncodeSymbol(value, form & 1, form & 2);
    if (popcount10(s) != 5 || isControlToken(s)) continue;
    if (!best || transitions10(s) < transitions10(best)) best = s;
  }
  return best;
}

void tmdsInitTables(bool strict)
{
  for (int level = 0; level < 16; ++level)
  {
    const int target = level * 17;  // 4-bit -> 8-bit (0x0 -> 0x00, 0xf -> 0xff)
    uint16_t symbol = 0;
    int value = target;

    for (int dist = 0; dist < 128 && !symbol; ++dist)
    {
      // try the brighter/darker candidate that keeps us furthest from clipping first
      const int sign = (target < 128) ? 1 : -1;
      const int candidates[2] = { target + sign * dist, target - sign * dist };
      for (int c = 0; c < (dist ? 2 : 1) && !symbol; ++c)
      {
        if (candidates[c] < 0 || candidates[c] > 255) continue;
        symbol = balancedSymbolFor((uint8_t)candidates[c], strict);
        value = candidates[c];
      }
    }

    levelSymbol[level] = symbol;
    levelValue[level] = (uint8_t)value;
  }

  for (int i = 0; i < 256; ++i)
  {
    tmdsPairTable[i] = (uint32_t)levelSymbol[i & 0x0f] | ((uint32_t)levelSymbol[i >> 4] << 10);
  }
}

uint16_t tmdsLevelSymbol(uint8_t level)
{
  return levelSymbol[level & 0x0f];
}

uint8_t tmdsLevelValue(uint8_t level)
{
  return levelValue[level & 0x0f];
}

/*
 * portable reference encoder. on RP2040 tmdsEncodeLine() is the assembly
 * version in tmds_encode_rp2040.S, which must produce identical output.
 */
void tmdsEncodeLineGeneric(const uint32_t *src, uint32_t *lane0, uint32_t *lane1, uint32_t *lane2, uint32_t pairCount)
{
  if (!pairCount) return;

  const uint32_t *tbl = tmdsPairTable;
  uint32_t prev = ~src[0];  // force a lookup on the first pair
  uint32_t b = 0, g = 0, r = 0;

  // runs of identical pixel pairs (borders, backgrounds) reuse the previous lookup
  for (uint32_t i = 0; i < pairCount; ++i)
  {
    const uint32_t w = src[i];
    if (w != prev)
    {
      prev = w;
      b = tbl[((w >> 8) & 0x0f) | ((w >> 20) & 0xf0)];
      g = tbl[((w >> 4) & 0x0f) | ((w >> 16) & 0xf0)];
      r = tbl[( w       & 0x0f) | ((w >> 12) & 0xf0)];
    }
    lane0[i] = b;
    lane1[i] = g;
    lane2[i] = r;
  }
}

#if !PICO_RP2040
void __time_critical_func(tmdsEncodeLine)(const uint32_t *src, uint32_t *lane0, uint32_t *lane1, uint32_t *lane2, uint32_t pairCount)
{
  tmdsEncodeLineGeneric(src, lane0, lane1, lane2, pairCount);
}
#endif
