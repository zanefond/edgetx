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

#include "board.h"
#include "debug.h"
#include "lcd.h"

#include "hal/abnormal_reboot.h"

#if !defined(BOOT)
  #include "opentx.h"
#endif

#if defined(NV3007_LCD)

#define RESET_WAIT_DELAY_MS            300 // Wait time after LCD reset before power-on finish
#define WAIT_FOR_DMA_END()             do { } while (lcd_busy)

#define LCD_NCS_HIGH()                 LCD_NCS_GPIO->BSRRL = LCD_NCS_GPIO_PIN
#define LCD_NCS_LOW()                  LCD_NCS_GPIO->BSRRH = LCD_NCS_GPIO_PIN

#define LCD_A0_HIGH()                  LCD_SPI_GPIO->BSRRL = LCD_A0_GPIO_PIN
#define LCD_A0_LOW()                   LCD_SPI_GPIO->BSRRH = LCD_A0_GPIO_PIN

#define LCD_RST_HIGH()                 LCD_RST_GPIO->BSRRL = LCD_RST_GPIO_PIN
#define LCD_RST_LOW()                  LCD_RST_GPIO->BSRRH = LCD_RST_GPIO_PIN

#define NV3007_PHYS_W                  428
#define NV3007_PHYS_H                  142
#define NV3007_ACTIVE_X                2
#define NV3007_ACTIVE_Y                7
#define NV3007_LINE_PIXELS             (LCD_W * 2 * 2)
#define NV3007_LINE_BYTES              (NV3007_LINE_PIXELS * 2)

#define RGB565_SWAP(r, g, b)           ((uint16_t)(((uint16_t)(((r) & 0xF8) | ((g) >> 5))) | ((uint16_t)((((g) & 0x1C) << 3) | ((b) >> 3)) << 8)))

bool lcdInitFinished = false;
volatile bool lcd_busy = false;
static uint16_t nv3007Lut[16];
static uint16_t nv3007LineBuf[2][NV3007_LINE_PIXELS] __DMA;
#if !defined(BOOT)
static bool nv3007Flip = false;
static int8_t nv3007LastTheme = -1;
static int8_t nv3007LastFlip = -1;
#endif

void lcdInitFinish();

static void nv3007SpiSendByte(uint8_t byte)
{
  while ((LCD_SPI->SR & SPI_SR_TXE) == 0) {
  }
  (void)LCD_SPI->DR;
  LCD_SPI->DR = byte;
  while ((LCD_SPI->SR & SPI_SR_RXNE) == 0) {
  }
  (void)LCD_SPI->DR;
  while (LCD_SPI->SR & SPI_SR_BSY) {
  }
}

void lcdWriteCommand(uint8_t byte)
{
  LCD_A0_LOW();
  LCD_NCS_LOW();
  nv3007SpiSendByte(byte);
  LCD_NCS_HIGH();
}

static void lcdWriteData(uint8_t byte)
{
  LCD_A0_HIGH();
  LCD_NCS_LOW();
  nv3007SpiSendByte(byte);
  LCD_NCS_HIGH();
}

void lcdHardwareInit()
{
  GPIO_InitTypeDef GPIO_InitStructure;

  LCD_SPI->CR1 = 0;
  LCD_SPI->CR1 = SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_CPOL | SPI_CR1_CPHA | LCD_SPI_PRESCALER;
  LCD_SPI->CR2 = 0;
  LCD_SPI->CR1 |= SPI_CR1_MSTR;
  LCD_SPI->CR1 |= SPI_CR1_SPE;

  LCD_NCS_HIGH();

  GPIO_InitStructure.GPIO_Pin = LCD_NCS_GPIO_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_Init(LCD_NCS_GPIO, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = LCD_RST_GPIO_PIN;
  GPIO_Init(LCD_RST_GPIO, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = LCD_A0_GPIO_PIN;
  GPIO_Init(LCD_SPI_GPIO, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = LCD_CLK_GPIO_PIN | LCD_MOSI_GPIO_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
  GPIO_Init(LCD_SPI_GPIO, &GPIO_InitStructure);

  GPIO_PinAFConfig(LCD_SPI_GPIO, LCD_MOSI_GPIO_PinSource, LCD_GPIO_AF);
  GPIO_PinAFConfig(LCD_SPI_GPIO, LCD_CLK_GPIO_PinSource, LCD_GPIO_AF);

  LCD_DMA_Stream->CR &= ~DMA_SxCR_EN;
  LCD_DMA->HIFCR = LCD_DMA_FLAGS;
  LCD_DMA_Stream->CR = DMA_SxCR_PL_0 | DMA_SxCR_MINC | DMA_SxCR_DIR_0;
  LCD_DMA_Stream->PAR = (uint32_t)&LCD_SPI->DR;
  LCD_DMA_Stream->M0AR = (uint32_t)nv3007LineBuf[0];
  LCD_DMA_Stream->NDTR = NV3007_LINE_BYTES;
  LCD_DMA_Stream->FCR = 0x05;

  NVIC_SetPriority(LCD_DMA_Stream_IRQn, 7);
  NVIC_EnableIRQ(LCD_DMA_Stream_IRQn);
}

static void nv3007BuildLut(uint8_t theme)
{
#if defined(BOOT)
  (void)theme;
  uint8_t br = 8, bg = 24, bb = 56;
  uint8_t fr = 255, fg = 255, fb = 255;
#else
  struct Nv3007Theme {
    uint8_t br, bg, bb;
    uint8_t fr, fg, fb;
  };
  static const Nv3007Theme themes[5] = {
    {  8,  24,  56, 255, 255, 255 }, // 0: Deep Blue BG / White FG
    { 170, 210, 140,  10,  20,  10 }, // 1: Classic Green LCD BG / Black FG
    { 245, 195, 110,  48,  20,   4 }, // 2: Warm Amber BG / Dark Brown FG
    { 255, 255, 255,   0,   0,   0 }, // 3: High-Contrast White BG / Black FG
    {   8,  42,  56, 235, 255, 255 }, // 4: Dark Cyan Night BG / Cyan-White FG
  };
  const Nv3007Theme & t = themes[theme % 5];
  uint8_t br = t.br, bg = t.bg, bb = t.bb;
  uint8_t fr = t.fr, fg = t.fg, fb = t.fb;
#endif
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t r = (uint8_t)(br + ((int16_t)(fr - br) * i) / 15);
    uint8_t g = (uint8_t)(bg + ((int16_t)(fg - bg) * i) / 15);
    uint8_t b = (uint8_t)(bb + ((int16_t)(fb - bb) * i) / 15);
    nv3007Lut[i] = RGB565_SWAP(r, g, b);
  }
}

static void nv3007SetWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
#if defined(BOOT)
  uint16_t yOff = 14;
#else
  uint16_t yOff = nv3007Flip ? 12 : 14;
#endif
  uint16_t ry1 = y1 + yOff;
  uint16_t ry2 = y2 + yOff;

  lcdWriteCommand(0x2A);
  lcdWriteData((uint8_t)(x1 >> 8));
  lcdWriteData((uint8_t)(x1 & 0xFF));
  lcdWriteData((uint8_t)(x2 >> 8));
  lcdWriteData((uint8_t)(x2 & 0xFF));

  lcdWriteCommand(0x2B);
  lcdWriteData((uint8_t)(ry1 >> 8));
  lcdWriteData((uint8_t)(ry1 & 0xFF));
  lcdWriteData((uint8_t)(ry2 >> 8));
  lcdWriteData((uint8_t)(ry2 & 0xFF));

  lcdWriteCommand(0x2C);
}

static void nv3007FillFullScreen(uint16_t colorSwap)
{
  nv3007SetWindow(0, 0, NV3007_PHYS_W - 1, NV3007_PHYS_H - 1);

  LCD_A0_HIGH();
  LCD_NCS_LOW();
  uint8_t b0 = (uint8_t)(colorSwap & 0xFF);
  uint8_t b1 = (uint8_t)(colorSwap >> 8);
  for (uint32_t i = 0; i < (uint32_t)NV3007_PHYS_W * NV3007_PHYS_H; i++) {
    while ((LCD_SPI->SR & SPI_SR_TXE) == 0) {
    }
    LCD_SPI->DR = b0;
    while ((LCD_SPI->SR & SPI_SR_TXE) == 0) {
    }
    LCD_SPI->DR = b1;
  }
  while (LCD_SPI->SR & SPI_SR_BSY) {
  }
  (void)LCD_SPI->DR;
  (void)LCD_SPI->SR;
  LCD_NCS_HIGH();
}

static void lcdStart()
{
  static const uint8_t nv3007InitSeq[] = {
    0xff, 1, 0xa5,
    0x9a, 1, 0x08,
    0x9b, 1, 0x08,
    0x9c, 1, 0xb0,
    0x9d, 1, 0x16,
    0x9e, 1, 0xc4,
    0x8f, 2, 0x55, 0x04,
    0x84, 1, 0x90,
    0x83, 1, 0x7b,
    0x85, 1, 0x33,
    0x60, 1, 0x00,
    0x70, 1, 0x00,
    0x61, 1, 0x02,
    0x71, 1, 0x02,
    0x62, 1, 0x04,
    0x72, 1, 0x04,
    0x6c, 1, 0x29,
    0x7c, 1, 0x29,
    0x6d, 1, 0x31,
    0x7d, 1, 0x31,
    0x6e, 1, 0x0f,
    0x7e, 1, 0x0f,
    0x66, 1, 0x21,
    0x76, 1, 0x21,
    0x68, 1, 0x3a,
    0x78, 1, 0x3a,
    0x63, 1, 0x07,
    0x73, 1, 0x07,
    0x64, 1, 0x05,
    0x74, 1, 0x05,
    0x65, 1, 0x02,
    0x75, 1, 0x02,
    0x67, 1, 0x23,
    0x77, 1, 0x23,
    0x69, 1, 0x08,
    0x79, 1, 0x08,
    0x6a, 1, 0x13,
    0x7a, 1, 0x13,
    0x6b, 1, 0x13,
    0x7b, 1, 0x13,
    0x6f, 1, 0x00,
    0x7f, 1, 0x00,
    0x50, 1, 0x00,
    0x52, 1, 0xd6,
    0x53, 1, 0x08,
    0x54, 1, 0x08,
    0x55, 1, 0x1e,
    0x56, 1, 0x1c,
    0xa0, 3, 0x2b, 0x24, 0x00,
    0xa1, 1, 0x87,
    0xa2, 1, 0x86,
    0xa5, 1, 0x00,
    0xa6, 1, 0x00,
    0xa7, 1, 0x00,
    0xa8, 1, 0x36,
    0xa9, 1, 0x7e,
    0xaa, 1, 0x7e,
    0xb9, 1, 0x85,
    0xba, 1, 0x84,
    0xbb, 1, 0x83,
    0xbc, 1, 0x82,
    0xbd, 1, 0x81,
    0xbe, 1, 0x80,
    0xbf, 1, 0x01,
    0xc0, 1, 0x02,
    0xc1, 1, 0x00,
    0xc2, 1, 0x00,
    0xc3, 1, 0x00,
    0xc4, 1, 0x33,
    0xc5, 1, 0x7e,
    0xc6, 1, 0x7e,
    0xc8, 2, 0x33, 0x33,
    0xc9, 1, 0x68,
    0xca, 1, 0x69,
    0xcb, 1, 0x6a,
    0xcc, 1, 0x6b,
    0xcd, 2, 0x33, 0x33,
    0xce, 1, 0x6c,
    0xcf, 1, 0x6d,
    0xd0, 1, 0x6e,
    0xd1, 1, 0x6f,
    0xab, 2, 0x03, 0x67,
    0xac, 2, 0x03, 0x6b,
    0xad, 2, 0x03, 0x68,
    0xae, 2, 0x03, 0x6c,
    0xb3, 1, 0x00,
    0xb4, 1, 0x00,
    0xb5, 1, 0x00,
    0xb6, 1, 0x32,
    0xb7, 1, 0x7e,
    0xb8, 1, 0x7e,
    0xe0, 1, 0x00,
    0xe1, 2, 0x03, 0x0f,
    0xe2, 1, 0x04,
    0xe3, 1, 0x01,
    0xe4, 1, 0x0e,
    0xe5, 1, 0x01,
    0xe6, 1, 0x19,
    0xe7, 1, 0x10,
    0xe8, 1, 0x10,
    0xea, 1, 0x12,
    0xeb, 1, 0xd0,
    0xec, 1, 0x04,
    0xed, 1, 0x07,
    0xee, 1, 0x07,
    0xef, 1, 0x09,
    0xf0, 1, 0xd0,
    0xf1, 1, 0x0e,
    0xf9, 1, 0x17,
    0xf2, 4, 0x2c, 0x1b, 0x0b, 0x20,
    0xe9, 1, 0x29,
    0xec, 1, 0x04,
    0x35, 1, 0x00,
    0x44, 2, 0x00, 0x10,
    0x46, 1, 0x10,
    0xff, 1, 0x00,
    0x3a, 1, 0x05,
  };

  lcdWriteCommand(0x36);
#if defined(BOOT)
  lcdWriteData(0x70);
#else
  lcdWriteData(nv3007Flip ? 0xA0 : 0x70);
#endif

  uint16_t i = 0;
  while (i < sizeof(nv3007InitSeq)) {
    lcdWriteCommand(nv3007InitSeq[i++]);
    uint8_t n = nv3007InitSeq[i++];
    while (n--) {
      lcdWriteData(nv3007InitSeq[i++]);
    }
  }
}

void lcdReset()
{
  LCD_NCS_HIGH();
  LCD_RST_LOW();
  delay_ms(30);
  LCD_RST_HIGH();
  delay_ms(100);
}

void lcdInit()
{
  lcdHardwareInit();
  if (IS_LCD_RESET_NEEDED()) {
    lcdReset();
  }
}

void lcdInitFinish()
{
  lcdInitFinished = true;

  if (!WAS_RESET_BY_WATCHDOG_OR_SOFTWARE()) {
#if !defined(BOOT)
    while (g_tmr10ms < (RESET_WAIT_DELAY_MS / 10));
#else
    delay_ms(RESET_WAIT_DELAY_MS);
#endif
  }

#if defined(BOOT)
  nv3007BuildLut(0);
#else
  uint8_t defTheme = (LCD_CONTRAST_DEFAULT % 30) / 6;
  nv3007Flip = (LCD_CONTRAST_DEFAULT >= 30);
  nv3007LastTheme = (int8_t)defTheme;
  nv3007LastFlip = nv3007Flip ? 1 : 0;
  nv3007BuildLut(defTheme);
#endif

  lcdStart();
  lcdWriteCommand(0x11);
  delay_ms(220);
  nv3007FillFullScreen(nv3007Lut[0]);
  lcdWriteCommand(0x29);
  delay_ms(20);
}

void lcdOff()
{
  WAIT_FOR_DMA_END();
  lcdWriteCommand(0x28);
  delay_ms(20);
  lcdWriteCommand(0x10);
  delay_ms(3);
}

void lcdRefreshWait()
{
  WAIT_FOR_DMA_END();
}

static void nv3007FillLineBuf(uint8_t logicRow, uint16_t * dst)
{
  uint32_t rowOff = (uint32_t)(logicRow >> 1) * LCD_W;
  const uint8_t * src = &displayBuf[rowOff];
  bool oddRow = (logicRow & 1) != 0;
  uint16_t * row0 = dst;
  uint16_t * row1 = dst + LCD_W * 2;

  for (coord_t lx = 0; lx < LCD_W; lx++) {
    uint8_t b = src[lx];
    uint8_t g = oddRow ? (b >> 4) : (b & 0x0F);
    uint16_t cNorm = nv3007Lut[g];
#if !defined(BOOT)
    uint8_t mh = hiResGlyphMask[rowOff + lx];
    uint8_t m = oddRow ? (mh >> 4) : (mh & 0x0F);
    if (m) {
      uint16_t cFlip = nv3007Lut[g ^ 0x0F];
      row0[0] = (m & 1u) ? cFlip : cNorm;
      row0[1] = (m & 2u) ? cFlip : cNorm;
      row1[0] = (m & 4u) ? cFlip : cNorm;
      row1[1] = (m & 8u) ? cFlip : cNorm;
      row0 += 2;
      row1 += 2;
      continue;
    }
#endif
    row0[0] = cNorm;
    row0[1] = cNorm;
    row1[0] = cNorm;
    row1[1] = cNorm;
    row0 += 2;
    row1 += 2;
  }
}

static void nv3007StartLineDma(const uint16_t * buf)
{
  lcd_busy = true;
  LCD_DMA_Stream->CR &= ~DMA_SxCR_EN;
  while (LCD_DMA_Stream->CR & DMA_SxCR_EN) {
  }
  LCD_DMA->HIFCR = LCD_DMA_FLAGS;
  LCD_DMA_Stream->M0AR = (uint32_t)buf;
  LCD_DMA_Stream->NDTR = NV3007_LINE_BYTES;
  LCD_DMA_Stream->CR |= DMA_SxCR_EN | DMA_SxCR_TCIE;
  LCD_SPI->CR2 |= SPI_CR2_TXDMAEN;
}

void lcdRefresh(bool wait)
{
  (void)wait;
  if (!lcdInitFinished) {
    lcdInitFinish();
  }

  WAIT_FOR_DMA_END();

  nv3007SetWindow(NV3007_ACTIVE_X, NV3007_ACTIVE_Y,
                  NV3007_ACTIVE_X + LCD_W * 2 - 1, NV3007_ACTIVE_Y + LCD_H * 2 - 1);

  nv3007FillLineBuf(0, nv3007LineBuf[0]);

  LCD_A0_HIGH();
  LCD_NCS_LOW();
  nv3007StartLineDma(nv3007LineBuf[0]);

  for (uint8_t row = 1; row < LCD_H; row++) {
    uint16_t * nextBuf = nv3007LineBuf[row & 1];
    nv3007FillLineBuf(row, nextBuf);
    WAIT_FOR_DMA_END();
    nv3007StartLineDma(nextBuf);
  }

  WAIT_FOR_DMA_END();
  LCD_NCS_HIGH();
}

extern "C" void LCD_DMA_Stream_IRQHandler()
{
  DEBUG_INTERRUPT(INT_LCD);

  LCD_DMA_Stream->CR &= ~DMA_SxCR_TCIE;
  LCD_DMA->HIFCR |= LCD_DMA_FLAG_INT;
  LCD_SPI->CR2 &= ~SPI_CR2_TXDMAEN;
  LCD_DMA_Stream->CR &= ~DMA_SxCR_EN;

  while (LCD_SPI->SR & SPI_SR_BSY) {
  }
  (void)LCD_SPI->DR;
  (void)LCD_SPI->SR;
  lcd_busy = false;
}

void lcdSetRefVolt(uint8_t val)
{
  if (!lcdInitFinished) {
    lcdInitFinish();
  }
#if defined(BOOT)
  (void)val;
#else
  uint8_t theme = (val % 30) / 6;
  bool flip = (val >= 30);
  int8_t flipInt = flip ? 1 : 0;

  if ((int8_t)theme == nv3007LastTheme && flipInt == nv3007LastFlip) {
    return;
  }

  WAIT_FOR_DMA_END();

  nv3007BuildLut(theme);
  if (flipInt != nv3007LastFlip) {
    nv3007Flip = flip;
    lcdWriteCommand(0x36);
    lcdWriteData(nv3007Flip ? 0xA0 : 0x70);
  }
  nv3007LastTheme = (int8_t)theme;
  nv3007LastFlip = flipInt;
  nv3007FillFullScreen(nv3007Lut[0]);
#endif
}

#else

#if defined(OLED_SCREEN)
  #define LCD_CONTRAST_OFFSET            0
#elif defined(RADIO_FAMILY_JUMPER_T12) || defined(MANUFACTURER_RADIOMASTER) || defined(RADIO_COMMANDO8) || defined(RADIO_TPRO) || defined(RADIO_T12MAX)
  #define LCD_CONTRAST_OFFSET            -10
#else
  #define LCD_CONTRAST_OFFSET            160
#endif
#define RESET_WAIT_DELAY_MS            300 // Wait time after LCD reset before first command
#define WAIT_FOR_DMA_END()             do { } while (lcd_busy)

#define LCD_NCS_HIGH()                 LCD_NCS_GPIO->BSRRL = LCD_NCS_GPIO_PIN
#define LCD_NCS_LOW()                  LCD_NCS_GPIO->BSRRH = LCD_NCS_GPIO_PIN

#define LCD_A0_HIGH()                  LCD_SPI_GPIO->BSRRL = LCD_A0_GPIO_PIN
#define LCD_A0_LOW()                   LCD_SPI_GPIO->BSRRH = LCD_A0_GPIO_PIN

#define LCD_RST_HIGH()                 LCD_RST_GPIO->BSRRL = LCD_RST_GPIO_PIN
#define LCD_RST_LOW()                  LCD_RST_GPIO->BSRRH = LCD_RST_GPIO_PIN

bool lcdInitFinished = false;
void lcdInitFinish();

void lcdWriteCommand(uint8_t byte)
{
  LCD_A0_LOW();
  LCD_NCS_LOW();
  while ((SPI3->SR & SPI_SR_TXE) == 0) {
    // Wait
  }
  (void)SPI3->DR; // Clear receive
  LCD_SPI->DR = byte;
  while ((SPI3->SR & SPI_SR_RXNE) == 0) {
    // Wait
  }
  LCD_NCS_HIGH();
}

void lcdHardwareInit()
{
  GPIO_InitTypeDef GPIO_InitStructure;

  // APB1 clock / 2 = 133nS per clock
  LCD_SPI->CR1 = 0; // Clear any mode error
  LCD_SPI->CR1 = SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_CPOL | SPI_CR1_CPHA | LCD_SPI_PRESCALER;
  LCD_SPI->CR2 = 0;
  LCD_SPI->CR1 |= SPI_CR1_MSTR;	// Make sure in case SSM/SSI needed to be set first
  LCD_SPI->CR1 |= SPI_CR1_SPE;

  LCD_NCS_HIGH();

  GPIO_InitStructure.GPIO_Pin = LCD_NCS_GPIO_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
  GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_25MHz;
  GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
  GPIO_Init(LCD_NCS_GPIO, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = LCD_RST_GPIO_PIN;
  GPIO_Init(LCD_RST_GPIO, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = LCD_A0_GPIO_PIN;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init(LCD_SPI_GPIO, &GPIO_InitStructure);

  GPIO_InitStructure.GPIO_Pin = LCD_CLK_GPIO_PIN | LCD_MOSI_GPIO_PIN;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
  GPIO_Init(LCD_SPI_GPIO, &GPIO_InitStructure);

  GPIO_PinAFConfig(LCD_SPI_GPIO, LCD_MOSI_GPIO_PinSource, LCD_GPIO_AF);
  GPIO_PinAFConfig(LCD_SPI_GPIO, LCD_CLK_GPIO_PinSource, LCD_GPIO_AF);

  LCD_DMA_Stream->CR &= ~DMA_SxCR_EN; // Disable DMA
  LCD_DMA->HIFCR = LCD_DMA_FLAGS; // Write ones to clear bits
  LCD_DMA_Stream->CR =  DMA_SxCR_PL_0 | DMA_SxCR_MINC | DMA_SxCR_DIR_0;
  LCD_DMA_Stream->PAR = (uint32_t)&LCD_SPI->DR;
#if LCD_W == 128
  LCD_DMA_Stream->NDTR = LCD_W;
#else
  LCD_DMA_Stream->M0AR = (uint32_t)displayBuf;
  LCD_DMA_Stream->NDTR = LCD_W*LCD_H/8*4;
#endif
  LCD_DMA_Stream->FCR = 0x05; // DMA_SxFCR_DMDIS | DMA_SxFCR_FTH_0;

  NVIC_SetPriority(LCD_DMA_Stream_IRQn, 7);
  NVIC_EnableIRQ(LCD_DMA_Stream_IRQn);
}

#if defined(SSD1309_LCD)
void lcdColumnSet(unsigned char column)
{
  lcdWriteCommand(0x10|(column >> 4));
  lcdWriteCommand(0x00|(column & 0x0f));
}

void lcdPageSet(unsigned char page)
{
  lcdWriteCommand(0xb0 + page);
}
#endif

#if LCD_W == 128
void lcdStart()
{
#if defined(SSD1309_LCD)
   lcdWriteCommand(0xD5);  // Set Display Clock Divide Ratio / OSC Frequency
   lcdWriteCommand(0x80);  // Display Clock Divide Ratio / OSC Frequency
   lcdWriteCommand(0xA8);  // Set Multiplex Ratio
   lcdWriteCommand(0x3F);  // Multiplex Ratio for 128x64 (LCD_HEIGHT - 1)
   lcdWriteCommand(0xD3);  // Set Display Offset
   lcdWriteCommand(0x00);  // Display Offset (0)
   lcdWriteCommand(0x40);  // Set Display Start Line (0)
   lcdWriteCommand(0x8D);  // Set Charge Pump
   lcdWriteCommand(0x10);  // Charge Pump (0x10 External, 0x14 Internal DC/DC)
   lcdWriteCommand(0xA1);  // Set Segment Re-Map (Reversed)
   lcdWriteCommand(0xC8);  // Set Com Output Scan Direction (Reversed)
   lcdWriteCommand(0xDA);  // Set COM Hardware Configuration
   lcdWriteCommand(0x12);  // COM Hardware Configuration
   lcdWriteCommand(0xD9);  // Set Pre-Charge Period
   lcdWriteCommand(0x4F);  // Set Pre-Charge Period (A[7:4]:Phase 2, A[3:0]:Phase 1)
   lcdWriteCommand(0xDB);  // Set VCOMH Deselect Level
   lcdWriteCommand(0x20);  // VCOMH Deselect Level (0x00 ~ 0.65 x VCC, 0x10 ~ 0.71 x VCC, 0x20 ~ 0.77 x VCC, 0x30 ~ 0.83 x VCC)
   lcdWriteCommand(0xA4);  // Disable Entire Display On
   lcdWriteCommand(0xA6);  // Set Normal Display (not inverted)
   lcdWriteCommand(0x2E);  // Deactivate scroll
#else
#if defined(LCD_VERTICAL_INVERT)
  // T12 and TX12 have the screen inverted.
  lcdWriteCommand(0xe2); // (14) Soft reset
#if defined(LCD_HORIZONTAL_INVERT)
  lcdWriteCommand(0xa1); // Set seg
#else 
  lcdWriteCommand(0xa0); // Set seg
#endif
  lcdWriteCommand(0xc8); // Set com
  lcdWriteCommand(0xf8); // Set booster
  lcdWriteCommand(0x00); // 5x
  lcdWriteCommand(0xa3); // Set bias=1/6
  lcdWriteCommand(0x22); // Set internal rb/ra=5.0
  lcdWriteCommand(0x2f); // All built-in power circuits on
  lcdWriteCommand(0x24); // Power control set
  lcdWriteCommand(0x81); // Set contrast
  lcdWriteCommand(0x0A); // Set Vop
  lcdWriteCommand(0xa6); // Set display mode
#else
  lcdWriteCommand(0xe2); // (14) Soft reset
  lcdWriteCommand(0xa1); // Set seg
  lcdWriteCommand(0xc0); // Set com
  lcdWriteCommand(0xf8); // Set booster
  lcdWriteCommand(0x00); // 5x
  lcdWriteCommand(0xa3); // Set bias=1/6
  lcdWriteCommand(0x22); // Set internal rb/ra=5.0
  lcdWriteCommand(0x2f); // All built-in power circuits on
  lcdWriteCommand(0x81); // Set contrast
  lcdWriteCommand(0x36); // Set Vop
  lcdWriteCommand(0xa6); // Set display mode
#endif
#if defined(BOOT)
  lcdSetRefVolt(LCD_CONTRAST_DEFAULT);
#else
  lcdSetRefVolt(g_eeGeneral.contrast);
#endif
#endif
}
#else
void lcdStart()
{
  lcdWriteCommand(0x2F); // Internal pump control
  delay_ms(20);
  lcdWriteCommand(0x24); // Temperature compensation
  lcdWriteCommand(0xE9); // Set bias=1/10
  lcdWriteCommand(0x81); // Set Vop
#if defined(BOOT)
  lcdWriteCommand(LCD_CONTRAST_OFFSET + LCD_CONTRAST_DEFAULT);
#else
  lcdWriteCommand(LCD_CONTRAST_OFFSET + g_eeGeneral.contrast);
#endif
  lcdWriteCommand(0xA2); // Set line rate: 28KLPS
  lcdWriteCommand(0x28); // Set panel loading
  lcdWriteCommand(0x40); // Scroll line LSB
  lcdWriteCommand(0x50); // Scroll line MSB
  lcdWriteCommand(0x89); // RAM address control
  lcdWriteCommand(0xC0); // LCD mapping control
  lcdWriteCommand(0x04); // MX=0, MY=1
  lcdWriteCommand(0xD0); // Display pattern = 16-SCALE GRAY
  lcdWriteCommand(0xF1); // Set COM end
  lcdWriteCommand(0x3F); // 64

  lcdWriteCommand(0xF8); // Set Window Program Disable.

  lcdWriteCommand(0xF5); // Start row address of RAM program window. PAGE1
  lcdWriteCommand(0x00);
  lcdWriteCommand(0xF7); // End row address of RAM program window. PAGE32
  lcdWriteCommand(0x1F);
  lcdWriteCommand(0xF4); // Start column address of RAM program window.
  lcdWriteCommand(0x00);
  lcdWriteCommand(0xF6); // End column address of RAM program window. SEG212
  lcdWriteCommand(0xD3);
}

void lcdWriteAddress(uint8_t x, uint8_t y)
{
  lcdWriteCommand(x & 0x0Fu); // Set Column Address LSB CA[3:0]
  lcdWriteCommand((x >> 4u) | 0x10u); // Set Column Address MSB CA[7:4]

  lcdWriteCommand((y & 0x0Fu) | 0x60u); // Set Row Address LSB RA [3:0]
  lcdWriteCommand(((y >> 4u) & 0x0Fu) | 0x70u); // Set Row Address MSB RA [7:4]
}
#endif

volatile bool lcd_busy;

void lcdRefreshWait()
{
  WAIT_FOR_DMA_END();
}

void lcdRefresh(bool wait)
{
  if (!lcdInitFinished) {
    lcdInitFinish();
  }

#if LCD_W == 128
  uint8_t * p = displayBuf;
#if defined(LCD_W_OFFSET)
  lcdWriteCommand(LCD_W_OFFSET);
#endif
  for (uint8_t y=0; y < 8; y++, p+=LCD_W) {
#if defined(SSD1309_LCD)
    lcdPageSet(y);
    lcdColumnSet(0);
#else
    lcdWriteCommand(0x10); // Column addr 0
    lcdWriteCommand(0xB0 | y); // Page addr y
#if !defined(LCD_VERTICAL_INVERT)
    lcdWriteCommand(0x04);
#endif
#endif

    LCD_NCS_LOW();
    LCD_A0_HIGH();

    lcd_busy = true;
    LCD_DMA_Stream->CR &= ~DMA_SxCR_EN; // Disable DMA
    LCD_DMA->HIFCR = LCD_DMA_FLAGS; // Write ones to clear bits
    LCD_DMA_Stream->M0AR = (uint32_t)p;
    LCD_DMA_Stream->CR |= DMA_SxCR_EN | DMA_SxCR_TCIE; // Enable DMA & TC interrupts
    LCD_SPI->CR2 |= SPI_CR2_TXDMAEN;

    WAIT_FOR_DMA_END();

    LCD_NCS_HIGH();
    LCD_A0_HIGH();
  }
#else
  // Wait if previous DMA transfer still active
  WAIT_FOR_DMA_END();
  lcd_busy = true;

  lcdWriteAddress(0, 0);

  LCD_NCS_LOW();
  LCD_A0_HIGH();

  LCD_DMA_Stream->CR &= ~DMA_SxCR_EN; // Disable DMA
  LCD_DMA->HIFCR = LCD_DMA_FLAGS; // Write ones to clear bits

  LCD_DMA_Stream->CR |= DMA_SxCR_EN | DMA_SxCR_TCIE; // Enable DMA & TC interrupts
  LCD_SPI->CR2 |= SPI_CR2_TXDMAEN;
#endif
}

extern "C" void LCD_DMA_Stream_IRQHandler()
{
  DEBUG_INTERRUPT(INT_LCD);

  LCD_DMA_Stream->CR &= ~DMA_SxCR_TCIE; // Stop interrupt
  LCD_DMA->HIFCR |= LCD_DMA_FLAG_INT; // Clear interrupt flag
  LCD_SPI->CR2 &= ~SPI_CR2_TXDMAEN;
  LCD_DMA_Stream->CR &= ~DMA_SxCR_EN; // Disable DMA

  while (LCD_SPI->SR & SPI_SR_BSY) {
    /* Wait for SPI to finish sending data
    The DMA TX End interrupt comes two bytes before the end of SPI transmission,
    therefore we have to wait here.
    */
  }
  LCD_NCS_HIGH();
  lcd_busy = false;
}

/*
  Proper method for turning of LCD module. It must be used,
  otherwise we might damage LCD crystals in the long run!
*/
void lcdOff()
{
  WAIT_FOR_DMA_END();

  /*
  LCD Sleep mode is also good for draining capacitors and enables us
  to re-init LCD without any delay
  */
  lcdWriteCommand(0xAE); // LCD sleep
  delay_ms(3); // Wait for caps to drain
}

void lcdReset()
{
  LCD_NCS_HIGH();
  LCD_RST_LOW();
#if LCD_W == 128
  delay_ms(150);
#else
  delay_ms(1); // Only 3 us needed according to data-sheet, we use 1 ms
#endif
  LCD_RST_HIGH();
}

/*
  Starts LCD initialization routine. It should be called as
  soon as possible after the reset because LCD takes a lot of
  time to properly power-on.

  Make sure that delay_ms() is functional before calling this function!
*/
void lcdInit()
{
  lcdHardwareInit();

  if (IS_LCD_RESET_NEEDED()) {
    lcdReset();
  }
}

/*
  Finishes LCD initialization. It is called auto-magically when first LCD command is
  issued by the other parts of the code.
*/

#if defined(RADIO_X9DP2019) || defined(RADIO_X7ACCESS)
  #define LCD_DELAY_NEEDED() true
#else
  #define LCD_DELAY_NEEDED() (!WAS_RESET_BY_WATCHDOG_OR_SOFTWARE())
#endif

void lcdInitFinish()
{
  lcdInitFinished = true;

  /*
    LCD needs longer time to initialize in low temperatures. The data-sheet
    mentions a time of at least 150 ms. The delay of 1300 ms was obtained
    experimentally. It was tested down to -10 deg Celsius.

    The longer initialization time seems to only be needed for regular Taranis,
    the Taranis Plus (9XE) has been tested to work without any problems at -18 deg Celsius.
    Therefore the delay for T+ is lower.

    If radio is reset by watchdog or boot-loader the wait is skipped, but the LCD
    is initialized in any case.

    This initialization is needed in case the user moved power switch to OFF and
    then immediately to ON position, because lcdOff() was called. In any case the LCD
    initialization (without reset) is also recommended by the data sheet.
  */

  if (LCD_DELAY_NEEDED()) {
#if !defined(BOOT)
    while (g_tmr10ms < (RESET_WAIT_DELAY_MS / 10)); // wait measured from the power-on
#else
    delay_ms(RESET_WAIT_DELAY_MS);
#endif
  }

  lcdStart();
  lcdWriteCommand(0xAF); // dc2=1, IC into exit SLEEP MODE, dc3=1 gray=ON, dc4=1 Green Enhanc mode disabled
  delay_ms(20); // needed for internal DC-DC converter startup
}

void lcdSetRefVolt(uint8_t val)
{
  if (!lcdInitFinished) {
    lcdInitFinish();
  }

#if LCD_W != 128
  WAIT_FOR_DMA_END();
#endif

  lcdWriteCommand(0x81); // Set Vop
  lcdWriteCommand(val+LCD_CONTRAST_OFFSET); // 0-255
}

#endif // NV3007_LCD
