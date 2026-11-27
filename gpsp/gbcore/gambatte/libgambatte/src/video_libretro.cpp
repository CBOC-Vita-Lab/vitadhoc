/***************************************************************************
 *   Copyright (C) 2007 by Sindre Aamås                                    *
 *   aamas@stud.ntnu.no                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License version 2 as     *
 *   published by the Free Software Foundation.                            *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License version 2 for more details.                *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   version 2 along with this program; if not, write to the               *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include "video.h"
#include "gbadhoc.h"
#include "savestate.h"
#include <cstring>
#include <algorithm>
#include <cmath>

/* GBC colour correction factors */
#define GBC_CC_LUM 0.94f
#define GBC_CC_R   0.82f
#define GBC_CC_G   0.665f
#define GBC_CC_B   0.73f
#define GBC_CC_RG  0.125f
#define GBC_CC_RB  0.195f
#define GBC_CC_GR  0.24f
#define GBC_CC_GB  0.075f
#define GBC_CC_BR  -0.06f
#define GBC_CC_BG  0.21f

namespace gambatte
{
   void LCD::setDmgPaletteColor(const unsigned index, const video_pixel_t rgb32)
   {
      dmgColorsRgb32_[index] = rgb32;
   }

   void LCD::setDmgPalette(video_pixel_t *const palette, const video_pixel_t *const dmgColors, const unsigned data)
   {
      palette[0] = dmgColors[data      & 3];
      palette[1] = dmgColors[data >> 2 & 3];
      palette[2] = dmgColors[data >> 4 & 3];
      palette[3] = dmgColors[data >> 6 & 3];
   }

   void LCD::setColorCorrection(bool colorCorrection_)
   {
      colorCorrection = colorCorrection_;
      refreshPalettes();
   }
   
   void LCD::setColorCorrectionMode(unsigned colorCorrectionMode_)
   {
      // Note: We only have two modes, so could make 'colorCorrectionMode'
      // a bool... but may want to add other modes in the future
      // (e.g. a special GBA colour correction mode for when the GBA
      // flag is set in Shantae/Zelda: Oracle/etc.)
      colorCorrectionMode = colorCorrectionMode_;
      refreshPalettes();
   }
   
   void LCD::setColorCorrectionBrightness(float colorCorrectionBrightness_)
   {
      colorCorrectionBrightness = colorCorrectionBrightness_;
      refreshPalettes();
   }
   
   void LCD::setDarkFilterLevel(unsigned darkFilterLevel_)
   {
      darkFilterLevel = darkFilterLevel_;
      refreshPalettes();
   }

   LCD::LCD(const unsigned char *const oamram, const unsigned char *const vram, const VideoInterruptRequester memEventRequester) :
      ppu_(nextM0Time_, oamram, vram),
      eventTimes_(memEventRequester),
      statReg_(0),
      m2IrqStatReg_(0),
      m1IrqStatReg_(0),
      /* Color-correction defaults must be initialized BEFORE the
       * constructor body runs, because setColorCorrection(true)
       * below calls refreshPalettes() -> gbcToRgb32(), which
       * reads colorCorrectionMode/colorCorrectionBrightness/
       * darkFilterLevel. Previously these members were left
       * uninitialized at this point and gbcToRgb32 invoked UB
       * during construction. The values get overwritten by
       * check_variables() once core options are read, but the
       * construction-time UB still fires under UBSan. */
      colorCorrection(true),
      colorCorrectionMode(0),
      colorCorrectionBrightness(0.5f),
      darkFilterLevel(0)
   {
      std::memset( bgpData_, 0, sizeof  bgpData_);
      std::memset(objpData_, 0, sizeof objpData_);
      /* dmgColorsGBC_ is consumed by refreshPalettes() in the
       * CGB-running-DMG-game path; if uninitialized it would
       * produce garbage colours until the game wrote a palette. */
      std::memset(dmgColorsGBC_, 0, sizeof dmgColorsGBC_);

      for (std::size_t i = 0; i < sizeof(dmgColorsRgb32_) / sizeof(dmgColorsRgb32_[0]); ++i)
      {
#ifdef VIDEO_RGB565
         uint16_t dmgColors[4]={0xFFFF, //11111 111111 11111
            0xAD55, //10101 101010 10101
            0x52AA, //01010 010101 01010
            0x0000};//00000 000000 00000
         setDmgPaletteColor(i, dmgColors[i&3]);
#elif defined(VIDEO_ABGR1555)
         uint16_t dmgColors[4]={0xEFFF, //11111 111111 11111
            0x56B5, //10101 10101 10101
            0x294A, //01010 01010 01010
            0x0000};//00000 000000 00000
         setDmgPaletteColor(i, dmgColors[i&3]);
#else
         setDmgPaletteColor(i, (3 - (i & 3)) * 85 * 0x010101);
#endif
      }

      reset(oamram, vram, false);
      /* PPUFrameBuf is default-constructed with NULL buffer and
       * pitch 0; the frontend will call setVideoBuffer with the
       * real buffer via cpu.setVideoBuffer once retro_run begins,
       * so the previous setVideoBuffer(0, 160) here was a no-op
       * with a confusing pitch. */

      setColorCorrection(true);
   }

   void LCD::doCgbColorChange(unsigned char *const pdata,
         video_pixel_t *const palette, unsigned index, const unsigned data)
   {
      pdata[index] = data;
      index >>= 1;
      palette[index] = gbcToRgb32(pdata[index << 1] | pdata[(index << 1) + 1] << 8);
   }

   void LCD::setVideoBuffer(video_pixel_t *const videoBuf, const int pitch)
   {
      ppu_.setFrameBuf(videoBuf, pitch);
   }

   static void clear(video_pixel_t *buf, const unsigned long color, const int dpitch)
   {
      unsigned lines = 144;

      while (lines--)
      {
         std::fill_n(buf, 160, color);
         buf += dpitch;
      }
   }

   void LCD::setDmgPaletteColor(const unsigned palNum, const unsigned colorNum, const video_pixel_t rgb32)
   {
      if (palNum > 2 || colorNum > 3)
         return;

      setDmgPaletteColor(palNum * 4 | colorNum, rgb32);
      refreshPalettes();
   }

   void LCD::updateScreen(const bool blanklcd, const unsigned long cycleCounter)
   {
      update(cycleCounter);

      if (blanklcd && ppu_.frameBuf().fb())
      {
         const video_pixel_t color = ppu_.cgb() ? gbcToRgb32(0xFFFF) : dmgColorsRgb32_[0];
         clear(ppu_.frameBuf().fb(), color, ppu_.frameBuf().pitch());
      }
   }

   // RGB range: [0,1]
   void LCD::darkenRgb(float &r, float &g, float &b)
   {
      // Note: This is *very* approximate...
      // - Should be done in linear colour space. It isn't.
      // - Should alter brightness by performing an RGB->HSL->RGB
      //   conversion. We just do simple linear scaling instead.
      // Basically, this is intended for use on devices that are
      // too weak to run shaders (i.e. why would you want a 'dark filter'
      // if your device supports proper LCD shaders?). We therefore
      // cut corners for the sake of performance...
      //
      // Constants
      // > Luminosity factors: photometric/digital ITU BT.709
      static const float lumaR = 0.2126f;
      static const float lumaG = 0.7152f;
      static const float lumaB = 0.0722f;
      // Calculate luminosity
      float luma = (lumaR * r) + (lumaG * g) + (lumaB * b);
      // Get 'darkness' scaling factor
      // > User set 'dark filter' level scaled by current luminosity
      //   (i.e. lighter colours affected more than darker colours)
      float darkFactor = 1.0f - ((static_cast<float>(darkFilterLevel) * 0.01f) * luma);
      darkFactor = darkFactor < 0.0f ? 0.0f : darkFactor;
      // Perform scaling...
      r = r * darkFactor;
      g = g * darkFactor;
      b = b * darkFactor;
   }

   video_pixel_t LCD::gbcToRgb32(const unsigned bgr15)
   {
      /* GBAdhoc: the frontend's own Game Boy Color colour model, the one the other core draws with. */
      return gambatte_gbc_pixel(bgr15);
   }

}
