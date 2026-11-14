/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <limits.h>
#include <memory.h>

#include "lcd.h"
#include "common/stdlcd/fonts.h"
#include "common/stdlcd/utf8.h"

#if !defined(SIMU)
  #define assert(x)
#else
  #include <assert.h>
#endif

#if !defined(BOOT)
  #include "opentx.h"
  #include "hal/switch_driver.h"
  #include "hal/adc_driver.h"
  #include "switches.h"
#endif

pixel_t displayBuf[DISPLAY_BUFFER_SIZE] __DMA;
#if !defined(BOOT) && defined(NV3007_LCD)
#include "font_cn_14x16.h"
uint8_t hiResGlyphMask[DISPLAY_BUFFER_SIZE] __DMA;
static coord_t protectCjkColX = -1;
#endif

inline bool lcdIsPointOutside(coord_t x, coord_t y)
{
  return (x<0 || x>=LCD_W || y<0 || y>=LCD_H);
}

void lcdClear()
{
  memset(displayBuf, 0, DISPLAY_BUFFER_SIZE * sizeof(pixel_t));
#if !defined(BOOT) && defined(NV3007_LCD)
  memset(hiResGlyphMask, 0, DISPLAY_BUFFER_SIZE);
#endif
}

coord_t lcdLastRightPos;
coord_t lcdLastLeftPos;
coord_t lcdNextPos;

struct PatternData
{
  uint8_t width;
  uint8_t height;
  const uint8_t * data;
};

uint8_t getPatternWidth(const PatternData * pattern)
{
  uint8_t result = 0;
  uint8_t lines = (pattern->height+7)/8;
  const uint8_t * data = pattern->data;
  for (int8_t i=0; i<pattern->width; i++) {
    for (uint8_t j=0; j<lines; j++) {
      if (data[j] != 0xff) {
        result += 1;
        break;
      }
    }
    data += lines;
  }
  return result;
}

void lcdPutPattern(coord_t x, coord_t y, const uint8_t * pattern, uint8_t width, uint8_t height, LcdFlags flags)
{
  bool blink = false;
  bool inv = false;
  if (flags & BLINK) {
    if (BLINK_ON_PHASE) {
      if (flags & INVERS)
        inv = true;
      else {
        blink = true;
      }
    }
  }
  else if (flags & INVERS) {
    inv = true;
  }

  uint8_t lines = (height+7)/8;
  assert(lines <= 5);

  for (int8_t i = 0; i < int8_t(width + 2); i++) {
    if (x < LCD_W) {
      uint8_t b[5] = { 0 };
      if (i == 0) {
        if (x == 0 || !inv) {
          lcdNextPos++;
          continue;
        }
        else {
          // we need to work on the previous x when INVERS
          x--;
        }
      }
      else if (i <= width) {
        uint8_t skip = true;
        for (uint8_t j=0; j<lines; j++) {
          b[j] = *(pattern++); /*top byte*/
          if (b[j] != 0xFF) {
            skip = false;
          }
        }
        if (skip) {
          if (flags & FIXEDWIDTH) {
            for (uint8_t j=0; j<lines; j++) {
              b[j] = 0;
            }
          }
          else {
            continue;
          }
        }
      }

      for (int8_t j = -1; j <= int8_t(height); j++) {
        bool plot;
        if (j < 0 || (j == height && FONTSIZE(flags) != SMLSIZE)) {
          plot = false;
          if (height >= 12)
            continue;
          if (j < 0 && !inv)
            continue;
          if (y + j < 0)
            continue;
        }
        else {
          uint8_t line = (j / 8);
          uint8_t pixel = (j % 8);
          plot = b[line] & (1 << pixel);
        }
        if (inv)
          plot = !plot;
        if (!blink) {
          if (flags & VERTICAL)
            lcdDrawPoint(y+j, LCD_H-x, plot ? FORCE : ERASE);
          else
            lcdDrawPoint(x, y+j, plot ? FORCE : ERASE);
        }
      }
    }

    x++;
    lcdNextPos++;
  }
}

LcdFlags getCharPattern(PatternData * pattern, unsigned char c, LcdFlags flags)
{
#if !defined(BOOT)
  static const uint8_t fontWidth[] = { 5, 3, 5, 8, 10, 22, 5 };
  static const uint8_t fontHeight[] = { 7, 5, 6, 12, 16, 38, 7 };

  uint32_t fontsize = FONTSIZE(flags);
  unsigned char c_remapped = 0;

  if (fontsize == DBLSIZE || (flags & BOLD)) {
    // To save space only some DBLSIZE and BOLD chars are available
    // c has to be remapped. All non existing chars mapped to 0 (space)
    if (c >= ',' && c <= ':')
      c_remapped = c - ',' + 1;
    else if (c >= 'A' && c <= 'Z')
      c_remapped = c - 'A' + 16;
    else if (c >= 'a' && c <= 'z')
      c_remapped = c - 'a' + 42;
    else if (c == '_')
      c_remapped = 4;
    else if (c != ' ')
      flags &= ~BOLD; // For BOLD use Standard font if character is not in BOLD
  }

  uint8_t fontIdx = fontsize >> 8;
  if (fontIdx == 0 && flags & BOLD) fontIdx = 6;

  pattern->width = fontWidth[fontIdx];
  pattern->height = fontHeight[fontIdx];
  int charSize = (pattern->height + 7) / 8 * pattern->width;

  switch (fontIdx) {
    case 0: // Standard
      pattern->data = &font_5x7[(c - FONT_BASE_START) * charSize];
      break;
    case 1: // TINSIZE
      pattern->data = &font_3x5[(c - FONT_BASE_START) * charSize];
      break;
    case 2: // SMLSIZE
      // Adjust language special characters offset
      if (c >= FONT_LANG_START)
        c = c - (FONT_SYMS_CNT - FONT_SYMS_CNT_4x6);
      pattern->data = &font_4x6[(c - FONT_BASE_START) * charSize];
      break;
    case 3: // MDLSIZE
      // Adjust language special characters offset
      if (c >= FONT_LANG_START)
        c = c - FONT_SYMS_CNT;
      pattern->data = &font_8x10[(c - FONT_BASE_START) * charSize];
      break;
    case 4: // DBLSIZE
      // Adjust language special characters offset and symbols offset
      if (c >= FONT_LANG_START)
        c_remapped = c - (FONT_BASE_CNT - FONT_BASE_CNT_10x14) - (FONT_SYMS_CNT - FONT_SYMS_CNT_10x14) - FONT_BASE_START;
      else if (c >= FONT_SYMS_START)
        c_remapped = c - (FONT_BASE_CNT - FONT_BASE_CNT_10x14) - FONT_BASE_START;
      pattern->data = &font_10x14[(c_remapped) * charSize];
      break;
    case 5: // XXLSIZE
      pattern->data = &font_22x38_num[(c - '0' + 5) * charSize];
      break;
    case 6: // BOLD
      pattern->data = &font_5x7_B[c_remapped * charSize];
      break;
  };
#else
  pattern->width = 5;
  pattern->height = 7;
  pattern->data = &font_5x7[(c - FONT_BASE_START) * 5];
#endif
  return flags;
}

uint8_t getCharWidth(char c, LcdFlags flags)
{
  PatternData pattern;
  getCharPattern(&pattern, c, flags);
  return getPatternWidth(&pattern);
}

void lcdDrawChar(coord_t x, coord_t y, uint8_t c, LcdFlags flags)
{
  lcdNextPos = x - 1;
  PatternData pattern;
  flags = getCharPattern(&pattern, c, flags);
  lcdPutPattern(x, y, pattern.data, pattern.width, pattern.height, flags);
}

void lcdDrawChar(coord_t x, coord_t y, uint8_t c)
{
  lcdDrawChar(x, y, c, 0);
}

#if !defined(BOOT) && defined(NV3007_LCD)
static bool isCjkUtf8(const char * s, uint16_t & unicode)
{
  const uint8_t b0 = (uint8_t)s[0];
  if ((b0 & 0xF0) != 0xE0) return false;
  const uint8_t b1 = (uint8_t)s[1];
  if ((b1 & 0xC0) != 0x80) return false;
  const uint8_t b2 = (uint8_t)s[2];
  if ((b2 & 0xC0) != 0x80) return false;
  unicode = ((uint16_t)(b0 & 0x0F) << 12) | ((uint16_t)(b1 & 0x3F) << 6) | (uint16_t)(b2 & 0x3F);
  return unicode >= 0x3000;
}

static int findCnGlyph(uint16_t unicode)
{
  int lo = 0;
  int hi = CN_FONT_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t code = cnFontCodes[mid];
    if (code == unicode) return mid;
    if (code < unicode) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}

static inline void setHiResSubpixel(int16_t px, int16_t py)
{
  if (px < 0 || px >= LCD_W * 2 || py < 0 || py >= LCD_H * 2) return;
  coord_t lx = px >> 1;
  coord_t ly = py >> 1;
  size_t idx = (size_t)(ly >> 1) * LCD_W + lx;
  uint8_t subBit = (uint8_t)(((py & 1) << 1) | (px & 1));
  hiResGlyphMask[idx] |= (ly & 1) ? (uint8_t)(1u << (4 + subBit)) : (uint8_t)(1u << subBit);
}

static void lcdDrawChineseChar(coord_t x, coord_t y, uint16_t unicode, LcdFlags flags, bool afterCjk)
{
  bool blink = false;
  bool inv = false;
  if (flags & BLINK) {
    if (BLINK_ON_PHASE) {
      if (flags & INVERS)
        inv = true;
      else
        blink = true;
    }
  }
  else if (flags & INVERS) {
    inv = true;
  }

  uint8_t scale = (FONTSIZE(flags) == DBLSIZE) ? 2 : 1;
  uint8_t logW = CN_LOGICAL_W * scale;
  uint8_t logH = 8 * scale;

  if (!blink) {
    int8_t startDx = (inv && x > 0 && !afterCjk) ? -1 : 0;
    int8_t startDy = (inv && y > 0 && scale == 1) ? -1 : 0;
    LcdFlags bgAtt = inv ? FORCE : ERASE;
    for (int8_t dx = startDx; dx < (int8_t)logW; dx++) {
      for (int8_t dy = startDy; dy < (int8_t)logH; dy++) {
        lcdDrawPoint(x + dx, y + dy, bgAtt);
      }
    }

    int16_t basePx = x * 2;
    int16_t basePy = y * 2;
    int glyphIdx = findCnGlyph(unicode);
    if (glyphIdx >= 0) {
      for (uint8_t col = 0; col < CN_FONT_W; col++) {
        uint16_t colBits = cnFontBitmaps[glyphIdx][col];
        if (!colBits) continue;
        for (uint8_t row = 0; row < CN_FONT_H; row++) {
          if (colBits & (1u << row)) {
            if (scale == 1) {
              setHiResSubpixel(basePx + col, basePy + row);
            }
            else {
              int16_t px = basePx + col * 2;
              int16_t py = basePy + row * 2;
              setHiResSubpixel(px, py);
              setHiResSubpixel(px + 1, py);
              setHiResSubpixel(px, py + 1);
              setHiResSubpixel(px + 1, py + 1);
            }
          }
        }
      }
    }
    else {
      for (uint8_t row = 2; row <= 13; row++) {
        for (uint8_t col = 2; col <= 11; col++) {
          if (row == 2 || row == 13 || col == 2 || col == 11) {
            if (scale == 1) {
              setHiResSubpixel(basePx + col, basePy + row);
            }
            else {
              int16_t px = basePx + col * 2;
              int16_t py = basePy + row * 2;
              setHiResSubpixel(px, py);
              setHiResSubpixel(px + 1, py);
              setHiResSubpixel(px, py + 1);
              setHiResSubpixel(px + 1, py + 1);
            }
          }
        }
      }
    }
  }

  lcdNextPos = x + logW;
}
#endif

uint8_t getTextWidth(const char * s, uint8_t len, LcdFlags flags)
{
  uint8_t width = 0;
#if !defined(BOOT) && defined(NV3007_LCD)
  bool unlimited = (len == 0);
  uint8_t cnW = CN_LOGICAL_W * ((FONTSIZE(flags) == DBLSIZE) ? 2 : 1);
  while (s && *s && (unlimited || len > 0)) {
    uint16_t unicode = 0;
    if ((unlimited || len >= 3) && isCjkUtf8(s, unicode)) {
      width += cnW;
      s += 3;
      if (!unlimited) len -= 3;
      continue;
    }
    uint8_t rem = unlimited ? 255 : (len - 1);
    unsigned char c = map_utf8_char(s, rem);
    if (!c) {
      break;
    }
    if (!unlimited) {
      len = rem;
    }
    width += getCharWidth(c, flags) + 1;
    s++;
  }
#else
  for (int i = 0; len == 0 || i < len; ++i) {
    unsigned char c = map_utf8_char(s, len);
    if (!c) {
      break;
    }
    width += getCharWidth(c, flags) + 1;
    s++;
  }
#endif
  return width;
}

void lcdDrawSizedText(coord_t x, coord_t y, const char * s, uint8_t len, LcdFlags flags)
{
  const coord_t orig_x = x;
  const uint8_t orig_len = len;
  uint32_t fontsize = FONTSIZE(flags);
  bool setx = false;
  uint8_t width = 0;
#if !defined(BOOT) && defined(NV3007_LCD)
  bool afterCjk = false;
#endif

  if (flags & RIGHT) {
    width = getTextWidth(s, len, flags);
    x -= width;
  }
  else if (flags & CENTERED) {
    width = getTextWidth(s, len, flags);
    x -= width / 2;
  }

  while (len--) {
    unsigned char c = *s;

    if (setx) {
      x = c;
      setx = false;
#if !defined(BOOT) && defined(NV3007_LCD)
      afterCjk = false;
#endif
    }
    else if (!c) {
      break;
    }
    else if (c >= 0x20) {
#if !defined(BOOT)
#if defined(NV3007_LCD)
      uint16_t unicode = 0;
      if (len >= 2 && isCjkUtf8(s, unicode)) {
        lcdDrawChineseChar(x, y, unicode, flags, afterCjk);
        x = lcdNextPos;
        afterCjk = true;
        s += 2;
        len -= 2;
        s++;
        continue;
      }
      if (afterCjk && (flags & INVERS) && x > 0) {
        protectCjkColX = x - 1;
      }
#endif
      // UTF8 detection
      c = map_utf8_char(s, len);
      if (!c) break;
      
      if (c == 46 && FONTSIZE(flags) == TINSIZE) { // '.' handling
        if (((flags & BLINK) && BLINK_ON_PHASE) || ((!(flags & BLINK) && (flags & INVERS)))) {
          lcdDrawSolidVerticalLine(x, y-1, 5);
          lcdDrawPoint(x, y + 5);
        }
        else {
          lcdDrawPoint(x, y + 5 -1 , flags);
        }
        x += 2;
      } else {
        lcdDrawChar(x, y, c, flags);
        x = lcdNextPos;
      }
#if defined(NV3007_LCD)
      protectCjkColX = -1;
      afterCjk = false;
#endif
#else
      lcdDrawChar(x, y, c, flags);
      x = lcdNextPos;
#endif
    }
    else if (c == 0x1F) {  //X-coord prefix
      setx = true;
    }
    else if (c == 0x1E) {  //NEWLINE
      len = orig_len;
      x = orig_x;
      y += FH;
      if (fontsize == DBLSIZE)
        y += FH;
      else if (fontsize == MIDSIZE)
        y += 4;
      else if (fontsize == SMLSIZE)
        y--;
#if !defined(BOOT) && defined(NV3007_LCD)
      afterCjk = false;
#endif
      if (y >= LCD_H) break;
    }
    else if (c == 0x1D) {  // TAB
      x |= 0x3F;
      x += 1;
#if !defined(BOOT) && defined(NV3007_LCD)
      afterCjk = false;
#endif
    }
    else {
      x += (c * FW/2); // EXTENDED SPACE
#if !defined(BOOT) && defined(NV3007_LCD)
      afterCjk = false;
#endif
    }
    s++;
  }
#if !defined(BOOT) && defined(NV3007_LCD)
  protectCjkColX = -1;
#endif

  lcdLastRightPos = x;
  lcdNextPos = x;
  if (fontsize == MIDSIZE) {
    lcdLastRightPos += 1;
  }

  if (flags & RIGHT) {
    lcdLastRightPos -= width;
    lcdNextPos -= width;
    lcdLastLeftPos = lcdLastRightPos;
    lcdLastRightPos = orig_x;
  }
  else {
    lcdLastLeftPos = orig_x;
  }
}

void lcdDrawSizedText(coord_t x, coord_t y, const char * s, uint8_t len)
{
  lcdDrawSizedText(x, y, s, len, 0);
}

void lcdDrawCenteredText(coord_t y, const char * s, LcdFlags flags)
{
#if !defined(BOOT) && defined(NV3007_LCD)
  coord_t x = (LCD_W - getTextWidth(s, 0, flags)) / 2;
#else
  coord_t x = (LCD_W - getTextWidth(s, flags)) / 2;
#endif
  lcdDrawText(x, y, s, flags);
}

void lcdDrawText(coord_t x, coord_t y, const char * s, LcdFlags flags)
{
  lcdDrawSizedText(x, y, s, 255, flags);
}

void lcdDrawText(coord_t x, coord_t y, const char * s)
{
  lcdDrawText(x, y, s, 0);
}

void lcdDrawTextAlignedLeft(coord_t y, const char * s)
{
  lcdDrawText(0, y, s);
}

#if !defined(BOOT)
void lcdDrawTextAtIndex(coord_t x, coord_t y, const char *const *s,uint8_t idx, LcdFlags flags)
{
  lcdDrawSizedText(x, y, s[idx], UINT8_MAX, flags);
}

void lcdDrawHexNumber(coord_t x, coord_t y, uint32_t val, LcdFlags flags)
{
  for (int i=12; i>=0; i-=4) {
    char c = (val >> i) & 0xf;
    c = c>9 ? c+'A'-10 : c+'0';
    lcdDrawChar(x, y, c, flags);
    x = lcdNextPos;
  }
}

void lcdDrawNumber(coord_t x, coord_t y, int32_t val, LcdFlags flags)
{
  lcdDrawNumber(x, y, val, flags, 0);
}

void lcdDrawNumber(coord_t x, coord_t y, int32_t val, LcdFlags flags, uint8_t len)
{
  char str[16+1];
  char *s = str+16;
  *s = '\0';
  int idx = 0;
  int mode = MODE(flags);
  bool neg = false;

  if (val == INT_MAX) {
    flags &= ~(LEADING0 | PREC1 | PREC2);
    lcdDrawText(x, y, "INT_MAX", flags);
    return;
  }

  if (val < 0) {
    if (val == INT_MIN) {
      flags &= ~(LEADING0 | PREC1 | PREC2);
      lcdDrawText(x, y, "INT_MIN", flags);
      return;
    }
    val = -val;
    neg = true;
  }
  do {
    *--s = '0' + (val % 10);
    ++idx;
    val /= 10;
    if (mode!=0 && idx==mode) {
      mode = 0;
      *--s = '.';
      if (val==0) {
        *--s = '0';
      }
    }
  } while (val!=0 || mode>0 || (mode==MODE(LEADING0) && idx<len));
  if (neg) {
    *--s = '-';
  }
  flags &= ~(LEADING0 | PREC1 | PREC2);
  lcdDrawText(x, y, s, flags);
}
#endif

void lcdDrawSolidHorizontalLine(coord_t x, coord_t y, coord_t w, LcdFlags att)
{
  if (w < 0) { x += w; w = -w; }
  lcdDrawHorizontalLine(x, y, w, 0xff, att);
}

#if !defined(BOOT)
void lcdDrawLine(coord_t x1, coord_t y1, coord_t x2, coord_t y2, uint8_t pat, LcdFlags att)
{
  int dx = x2-x1;      /* the horizontal distance of the line */
  int dy = y2-y1;      /* the vertical distance of the line */
  int dxabs = abs(dx);
  int dyabs = abs(dy);
  int sdx = sgn(dx);
  int sdy = sgn(dy);
  int x = dyabs>>1;
  int y = dxabs>>1;
  int px = x1;
  int py = y1;

  if (dxabs >= dyabs) {
    /* the line is more horizontal than vertical */
    for (int i=0; i<=dxabs; i++) {
      if ((1<<(px%8)) & pat) {
        lcdDrawPoint(px, py, att);
      }
      y += dyabs;
      if (y>=dxabs) {
        y -= dxabs;
        py += sdy;
      }
      px += sdx;
    }
  }
  else {
    /* the line is more vertical than horizontal */
    for (int i=0; i<=dyabs; i++) {
      if ((1<<(py%8)) & pat) {
        lcdDrawPoint(px, py, att);
      }
      x += dxabs;
      if (x >= dyabs) {
        x -= dyabs;
        px += sdx;
      }
      py += sdy;
    }
  }
}
#endif

void lcdDrawSolidVerticalLine(coord_t x, coord_t y, coord_t h, LcdFlags att)
{
  lcdDrawVerticalLine(x, y, h, SOLID, att);
}

void lcdDrawRect(coord_t x, coord_t y, coord_t w, coord_t h, uint8_t pat, LcdFlags att)
{
  lcdDrawVerticalLine(x, y, h, pat, att);
  lcdDrawVerticalLine(x+w-1, y, h, pat, att);
  if (~att & ROUND) { x+=1; w-=2; }
  lcdDrawHorizontalLine(x, y+h-1, w, pat, att);
  lcdDrawHorizontalLine(x, y, w, pat, att);
}

#if !defined(BOOT)
void lcdDrawFilledRect(coord_t x, coord_t y, coord_t w, coord_t h, uint8_t pat, LcdFlags att)
{
  for (coord_t i=y; i<y+h; i++) {
    if ((att&ROUND) && (i==y || i==y+h-1))
      lcdDrawHorizontalLine(x+1, i, w-2, pat, att);
    else
      lcdDrawHorizontalLine(x, i, w, pat, att);
    pat = (pat >> 1) + ((pat & 1) << 7);
  }
}

void drawTelemetryTopBar()
{
  drawModelName(0, 0, g_model.header.name, g_eeGeneral.currModel, 0);
  uint8_t att = (IS_TXBATT_WARNING() ? BLINK : 0);
  putsVBat(12*FW, 0, att);
  if (g_model.timers[0].mode) {
    TimerData *timer =  &g_model.timers[0];
    int32_t val = 0;
    if (g_model.timers[0].showElapsed)
      val = timer->start - timersStates[0].val;
    else
      val = timersStates[0].val;
    LcdFlags att = (val < 0 ? BLINK : 0) | TIMEHOUR;
    drawTimer(18*FW, 0, val, att, att);
    lcdDrawText(18*FW, 0, "T1:", RIGHT);
  }
  if (g_model.timers[1].mode) {
    TimerData *timer =  &g_model.timers[1];
    int32_t val = 0;
    if (g_model.timers[1].showElapsed)
      val = timer->start - timersStates[1].val;
    else
      val = timersStates[1].val;
    LcdFlags att = (val < 0 ? BLINK : 0) | TIMEHOUR;
    drawTimer(28*FW, 0, val, att, att);
    lcdDrawText(28*FW, 0, "T2:", RIGHT);
  }
  lcdInvertLine(0);
}

void drawRtcTime(coord_t x, coord_t y, LcdFlags att)
{
  drawTimer(x, y, getValue(MIXSRC_TX_TIME), att, att);
}

void drawTimer(coord_t x, coord_t y, int32_t tme, LcdFlags att, LcdFlags att2)
{
  div_t qr;

  if (att & RIGHT) {
    att -= RIGHT;
    if (att & DBLSIZE)
      x -= 5*(2*FWNUM)-4;
    else if (att & MIDSIZE)
      x -= 5*8-4;
    else
      x -= 5*FWNUM+1;
  }

  if (tme < 0) {
    lcdDrawChar(x - ((att & DBLSIZE) ? FW+2 : ((att & MIDSIZE) ? FW+0 : FWNUM)), y, '-', att);
    tme = -tme;
  }

  qr = div((int)tme, 60);

#define separator ':'
  if (att & TIMEHOUR) {
    div_t qr2 = div(qr.quot, 60);
    if (qr2.quot < 100) {
      lcdDrawNumber(x, y, qr2.quot, att|LEADING0|LEFT, 2);
    }
    else {
      lcdDrawNumber(x, y, qr2.quot, att|LEFT);
    }
    lcdDrawChar(lcdNextPos, y, separator, att);
    qr.quot = qr2.rem;
    x = lcdNextPos;
  }

  lcdDrawNumber(x, y, qr.quot, att|LEADING0|LEFT, 2);
  if (att & TIMEBLINK)
    lcdDrawChar(lcdNextPos, y, separator, BLINK);
  else
    lcdDrawChar(lcdNextPos, y, separator, att&att2);
  lcdDrawNumber(lcdNextPos, y, qr.rem, (att2|LEADING0) & (~RIGHT), 2);
}

// TODO to be optimized with drawValueWithUnit
void putsVolts(coord_t x, coord_t y, uint16_t volts, LcdFlags att)
{
  lcdDrawNumber(x, y, (int16_t)volts, (~NO_UNIT) & (att | ((att&PREC2)==PREC2 ? 0 : PREC1)));
  if (~att & NO_UNIT) lcdDrawChar(lcdLastRightPos, y, 'V', att);
}

void putsVBat(coord_t x, coord_t y, LcdFlags att)
{
  putsVolts(x, y, g_vbat100mV, att);
}

void drawMainControlLabel(coord_t x, coord_t y, uint8_t idx, LcdFlags att)
{
  lcdDrawSizedText(x, y, getMainControlLabel(idx), UINT8_MAX, att);
}

void drawSource(coord_t x, coord_t y, uint32_t idx, LcdFlags att)
{
  if (idx == MIXSRC_NONE) {
    lcdDrawText(x, y, STR_EMPTY, att);
  }
  else if (idx <= MIXSRC_LAST_INPUT) {
    lcdDrawChar(x+2, y+1, CHR_INPUT, TINSIZE);
    lcdDrawFilledRect(x, y, 7, 7);
    if (ZEXIST(g_model.inputNames[idx-MIXSRC_FIRST_INPUT]))
      lcdDrawSizedText(x+8, y, g_model.inputNames[idx-MIXSRC_FIRST_INPUT], LEN_INPUT_NAME, att);
    else
      lcdDrawNumber(x+8, y, idx, att|LEADING0|LEFT, 2);
  }

  else if (idx <= MIXSRC_LAST_LUA) {
    div_t qr = div(idx-MIXSRC_FIRST_LUA, MAX_SCRIPT_OUTPUTS);
#if defined(LUA_MODEL_SCRIPTS)
    if (qr.quot < MAX_SCRIPTS && qr.rem < scriptInputsOutputs[qr.quot].outputsCount) {
      lcdDrawChar(x+2, y+1, '1'+qr.quot, TINSIZE);
      lcdDrawFilledRect(x, y, 7, 7);
      lcdDrawSizedText(x+8, y, scriptInputsOutputs[qr.quot].outputs[qr.rem].name, att & STREXPANDED ? 9 : 4, att);
    }
    else
#endif
    {
      drawStringWithIndex(x, y, "LUA", qr.quot+1, att);
      lcdDrawChar(lcdLastRightPos, y, 'a'+qr.rem, att);
    }
  }
  else {
    lcdDrawText(x, y, getSourceString(idx), att);
  }
}

void putsChn(coord_t x, coord_t y, uint8_t idx, LcdFlags att)
{
  drawStringWithIndex(x, y, STR_CH, idx, att);
}

void putsChnLetter(coord_t x, coord_t y, uint8_t idx, LcdFlags att)
{
  lcdDrawText(x, y, getAnalogShortLabel(idx), att);
}

void drawModelName(coord_t x, coord_t y, char *name, uint8_t id, LcdFlags att)
{
  uint8_t len = sizeof(g_model.header.name);
  while (len>0 && !name[len-1]) --len;
  if (len==0) {
    drawStringWithIndex(x, y, STR_MODEL, id+1, att|LEADING0);
  }
  else {
    lcdDrawSizedText(x, y, name, sizeof(g_model.header.name), att);
  }
}

void drawSwitch(coord_t x, coord_t y, int32_t idx, LcdFlags flags, bool autoBold)
{
  char s[8];
  getSwitchPositionName(s, idx);
  if (autoBold && idx != SWSRC_NONE && getSwitch(idx))
    flags |= BOLD;
  lcdDrawText(x, y, s, flags);
}

void drawCurveName(coord_t x, coord_t y, int8_t idx, LcdFlags flags)
{
  char s[8];
  getCurveString(s, idx);
  lcdDrawText(x, y, s, flags);
}

void drawTimerMode(coord_t x, coord_t y, swsrc_t mode, LcdFlags att)
{
  if (mode >= 0) {
    if (mode < TMRMODE_COUNT)
      return lcdDrawTextAtIndex(x, y, STR_VTMRMODES, mode, att);
    else
      mode -= (TMRMODE_COUNT-1);
  }
  drawSwitch(x, y, mode, att);
}

void drawGPSCoord(coord_t x, coord_t y, int32_t value, const char * direction, LcdFlags att, bool seconds=true)
{
  uint32_t absvalue = abs(value);
  att &= ~RIGHT;
  if (x > 10) x-=10;
  lcdDrawNumber(x, y, absvalue / 1000000, att); // ddd
  lcdDrawChar(lcdLastRightPos, y, STR_CHAR_BW_DEGREE, att);
  absvalue = absvalue % 1000000;
  absvalue *= 60;
  if (g_eeGeneral.gpsFormat == 0 || !seconds) {
    lcdDrawNumber(lcdNextPos, y, absvalue / 1000000, att|LEFT|LEADING0, 2); // mm before '.'
    lcdDrawSolidVerticalLine(lcdLastRightPos, y, 2);
    lcdLastRightPos += 1;
    if (seconds) {
      absvalue %= 1000000;
      absvalue *= 60;
      absvalue /= 10000;
      lcdDrawNumber(lcdLastRightPos+2, y, absvalue, att|LEFT|PREC2);
      lcdDrawSolidVerticalLine(lcdLastRightPos, y, 2);
      lcdDrawSolidVerticalLine(lcdLastRightPos+2, y, 2);
      lcdLastRightPos += 3;
    }
  }
  else {
    absvalue /= 10000;
    lcdDrawNumber(lcdLastRightPos+FW, y, absvalue, att|LEFT|PREC2); // mm.mmm
  }
  lcdDrawSizedText(lcdLastRightPos+1, y, direction + (value>=0 ? 0 : 1), 1);
}

void drawDate(coord_t x, coord_t y, TelemetryItem & telemetryItem, LcdFlags att)
{
  att &= ~FONTSIZE_MASK;
  if (BLINK_ON_PHASE) {
    lcdDrawNumber(x, y, telemetryItem.datetime.hour, att|LEADING0, 2);
    lcdDrawText(lcdNextPos, y, ":", att);
    lcdDrawNumber(lcdNextPos, y, telemetryItem.datetime.min, att|LEADING0, 2);
    lcdDrawText(lcdNextPos, y, ":", att);
    lcdDrawNumber(lcdNextPos, y, telemetryItem.datetime.sec, att|LEADING0, 2);
  }
  else {
    lcdDrawNumber(x, y, telemetryItem.datetime.year, att|LEADING0, 4);
    lcdDrawText(lcdNextPos, y, "-", att);
    lcdDrawNumber(lcdNextPos, y, telemetryItem.datetime.month, att|LEADING0, 2);
    lcdDrawText(lcdNextPos, y, "-", att);
    lcdDrawNumber(lcdNextPos, y, telemetryItem.datetime.day, att|LEADING0,2);
  }
}

void drawTelemScreenDate(coord_t x, coord_t y, source_t sensor, LcdFlags att)
{
  sensor = (sensor-MIXSRC_FIRST_TELEM) / 3;
	TelemetryItem & telemetryItem = telemetryItems[sensor];

  att &= ~FONTSIZE_MASK;
  lcdDrawNumber(x, y+1, telemetryItem.datetime.sec, att|LEADING0, 2);
  lcdDrawText(lcdNextPos, y+1, ":", att);
  lcdDrawNumber(lcdNextPos, y+1, telemetryItem.datetime.min, att|LEADING0, 2);
  lcdDrawText(lcdNextPos, y+1, ":", att);
  lcdDrawNumber(lcdNextPos, y+1, telemetryItem.datetime.hour, att|LEADING0, 2);

  lcdDrawNumber(x, y+9, telemetryItem.datetime.day, att|LEADING0, 2);
  lcdDrawText(lcdNextPos, y+9, "-", att);
  lcdDrawNumber(lcdNextPos, y+9, telemetryItem.datetime.month, att|LEADING0, 2);
  lcdDrawText(lcdNextPos, y+9, "-", att);
  lcdDrawNumber(lcdNextPos, y+9, telemetryItem.datetime.year, att|LEADING0,4);
}

void drawGPSSensorValue(coord_t x, coord_t y, TelemetryItem & telemetryItem, LcdFlags att)
{
  if (att & DBLSIZE) {
    x -= (g_eeGeneral.gpsFormat == 0 ? 54 : 51);
    att &= ~FONTSIZE_MASK;
    drawGPSCoord(x, y, telemetryItem.gps.latitude, "NS", att, true);
    drawGPSCoord(x, y+FH, telemetryItem.gps.longitude, "EW", att, true);
  }
  else {
    drawGPSCoord(x, y, telemetryItem.gps.latitude, "NS", att, false);
    drawGPSCoord(lcdLastRightPos+2*FWNUM, y, telemetryItem.gps.longitude, "EW", att, false);
  }
}

#endif // BOOT

void lcdSetContrast(bool useDefault)
{
#if defined(BOOT)
  lcdSetRefVolt(LCD_CONTRAST_DEFAULT);
#else
  lcdSetRefVolt(useDefault ? LCD_CONTRAST_DEFAULT : g_eeGeneral.contrast);
#endif
}

void lcdMaskPoint(uint8_t *p, uint8_t mask, LcdFlags att)
{
  if ((p) >= DISPLAY_END) {
    return;
  }

  if (att&FILL_WHITE) {
    // TODO I could remove this, it's used for the top bar
    if (*p & 0x0F) mask &= 0xF0;
    if (*p & 0xF0) mask &= 0x0F;
  }

  if (att & FORCE) {
    *p |= mask;
  }
  else if (att & ERASE) {
    *p &= ~mask;
  }
  else {
    *p ^= mask;
  }
#if !defined(BOOT) && defined(NV3007_LCD)
  if (att & (FORCE | ERASE)) {
    size_t idx = (size_t)(p - displayBuf);
    if (!(att == FORCE && protectCjkColX >= 0 && (coord_t)(idx % LCD_W) == protectCjkColX)) {
      uint8_t clr = ((mask & 0x0F) ? 0x0F : 0) | ((mask & 0xF0) ? 0xF0 : 0);
      hiResGlyphMask[idx] &= ~clr;
    }
  }
#endif
}

#define PIXEL_GREY_MASK(y, att) (((y) & 1) ? (0xF0 - (COLOUR_MASK(att) >> 12)) : (0x0F - (COLOUR_MASK(att) >> 16)))

void lcdDrawPoint(coord_t x, coord_t y, LcdFlags att)
{
  if (lcdIsPointOutside(x, y))
    return;
  uint8_t *p = &displayBuf[ y / 2 * LCD_W + x ];
  uint8_t mask = PIXEL_GREY_MASK(y, att);
  lcdMaskPoint(p, mask, att);
}

void lcdDrawHorizontalLine(coord_t x, coord_t y, coord_t w, uint8_t pat, LcdFlags att)
{
  if (y < 0 || y >= LCD_H) return;
  if (x+w > LCD_W) {
    if (x >= LCD_W ) return;
    w = LCD_W - x;
  }

  uint8_t *p  = &displayBuf[ y / 2 * LCD_W + x ];
  uint8_t mask = PIXEL_GREY_MASK(y, att);
  while (w--) {
    if (pat&1) {
      lcdMaskPoint(p, mask, att);
      pat = (pat >> 1) | 0x80;
    }
    else {
      pat = pat >> 1;
    }
    p++;
  }
}

void lcdDrawVerticalLine(coord_t x, coord_t y, coord_t h, uint8_t pat, LcdFlags att)
{
  if (x >= LCD_W) return;
  if (y >= LCD_H) return;
  if (h<0) { y+=h; h=-h; }
  if (y<0) { h+=y; y=0; if (h<=0) return; }
  if (y+h > LCD_H) { h = LCD_H - y; }

  if (pat==DOTTED && !(y%2)) {
    pat = ~pat;
  }

  while (h--) {
    if (pat & 1) {
      lcdDrawPoint(x, y, att);
      pat = (pat >> 1) | 0x80;
    }
    else {
      pat = pat >> 1;
    }
    y++;
  }
}

void lcdInvertLine(int8_t line)
{
  if (line < 0) return;
  if (line >= LCD_LINES) return;

  uint8_t *p  = &displayBuf[line * 4 * LCD_W];
  for (coord_t x=0; x<LCD_W*4; x++) {
    ASSERT_IN_DISPLAY(p);
    *p++ ^= 0xff;
  }
}

#if !defined(BOOT)
void lcdDraw1bitBitmap(coord_t x, coord_t y, const unsigned char * img, uint8_t idx, LcdFlags att)
{
  const unsigned char *q = img;
  uint8_t w    = *(q++);
  uint8_t hb   = (*(q++)+7) / 8;
  bool    inv  = (att & INVERS) ? true : (att & BLINK ? BLINK_ON_PHASE : false);
  q += idx*w*hb;
  for (uint8_t yb = 0; yb < hb; yb++) {
    for (uint8_t i=0; i<w; i++) {
      uint8_t b = *(q++);
      uint8_t val = inv ? ~b : b;
      for (int k=0; k<8; k++) {
        if (val & (1<<k)) {
          lcdDrawPoint(x+i, y+yb*8+k, 0);
        }
      }
    }
  }
}

void lcdDrawBitmap(coord_t x, coord_t y, const uint8_t * img, coord_t offset, coord_t width)
{
  const uint8_t *q = img;
  uint8_t w = *q++;
  if (!width || width > w) {
    width = w;
  }
  if (x+width > LCD_W) {
    if (x >= LCD_W ) return;
    width = LCD_W-x;
  }
  uint8_t rows = (*q++ + 1) / 2;

  for (uint8_t row=0; row<rows; row++) {
    q = img + 2 + row*w + offset;
    uint8_t *p = &displayBuf[(row + (y/2)) * LCD_W + x];
    for (coord_t i=0; i<width; i++) {
      if (p >= DISPLAY_END) return;
      uint8_t b = *q++;
      if (y & 1) {
        *p = (*p & 0x0f) + ((b & 0x0f) << 4);
#if defined(NV3007_LCD)
        hiResGlyphMask[p - displayBuf] &= 0x0f;
#endif
        if ((p+LCD_W) < DISPLAY_END) {
          *(p+LCD_W) = (*(p+LCD_W) & 0xf0) + ((b & 0xf0) >> 4);
#if defined(NV3007_LCD)
          hiResGlyphMask[(p + LCD_W) - displayBuf] &= 0xf0;
#endif
        }
      }
      else {
        *p = b;
#if defined(NV3007_LCD)
        hiResGlyphMask[p - displayBuf] = 0;
#endif
      }
      p++;
    }
  }
}

void lcdDrawRleBitmap(coord_t x, coord_t y, const uint8_t * img, coord_t offset, coord_t width, bool overlay)
{
  RleBitmap pic(img, offset);

  uint8_t w = pic.getWidth();
  if (!width || width > w) {
    width = w;
  }
  if (x+width > LCD_W) {
    if (x >= LCD_W ) return;
    width = LCD_W-x;
  }
  uint8_t rows = pic.getRows();

  for (uint8_t row=0; row<rows; row++) {
    uint8_t *p = &displayBuf[(row + (y/2)) * LCD_W + x];
    if (overlay) {
      for (coord_t i=0; i<width; i++) {
        if (p >= DISPLAY_END) return;
        uint8_t b = pic.getNext();
        if (y & 1) {
          if (b & 0x0f) {
            *p = (*p & 0x0f) + ((b & 0x0f) << 4);
#if defined(NV3007_LCD)
            hiResGlyphMask[p - displayBuf] &= 0x0f;
#endif
          }
          if ((p+LCD_W) < DISPLAY_END && (b & 0xf0)) {
            *(p+LCD_W) = (*(p+LCD_W) & 0xf0) + ((b & 0xf0) >> 4);
#if defined(NV3007_LCD)
            hiResGlyphMask[(p + LCD_W) - displayBuf] &= 0xf0;
#endif
          }
        }
        else {
          if (b & 0x0f) {
            *p = (*p & 0xf0) + (b & 0x0f);
#if defined(NV3007_LCD)
            hiResGlyphMask[p - displayBuf] &= 0xf0;
#endif
          }
          if (b & 0xf0) {
            *p = (*p & 0x0f) + (b & 0xf0);
#if defined(NV3007_LCD)
            hiResGlyphMask[p - displayBuf] &= 0x0f;
#endif
          }
        }
        p++;
      }
    } else {
      for (coord_t i=0; i<width; i++) {
        if (p >= DISPLAY_END) return;
        uint8_t b = pic.getNext();
        if (y & 1) {
          *p = (*p & 0x0f) + ((b & 0x0f) << 4);
#if defined(NV3007_LCD)
          hiResGlyphMask[p - displayBuf] &= 0x0f;
#endif
          if ((p+LCD_W) < DISPLAY_END) {
            *(p+LCD_W) = (*(p+LCD_W) & 0xf0) + ((b & 0xf0) >> 4);
#if defined(NV3007_LCD)
            hiResGlyphMask[(p + LCD_W) - displayBuf] &= 0xf0;
#endif
          }
        }
        else {
          *p = b;
#if defined(NV3007_LCD)
          hiResGlyphMask[p - displayBuf] = 0;
#endif
        }
        p++;
      }
    }
    pic.goToNextRow();
    pic.skip(offset);
  }
}
#endif

void lcdFlushed() {}
