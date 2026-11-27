/****************************************************************************
 *  Genesis Plus
 *  CFI-compliant flash memory (29xxx) support
 *
 *  Limitations:
 *   - 16-bit mode only
 *   - only support commonly used commands (read/program/erase/autoselect/query)
 *   - write-protection configuration based on cartridge mapping (only backup RAM is rewritable)
 *   - writable sectors size fixed to 64KB
 *   - instantaneous program/erase operations
 *   - only return manufacturer & device codes in autoselect mode
 *   - only return identification, system interface information, device geometry definition and primary extended table in CFI query mode
 *
 *  Copyright (C) 2025-2026 Eke-Eke (Genesis Plus GX)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/

#include "shared.h"
#include "flash_cfi.h"

#define CFI_QUERY_TABLE_LEN 0x41

typedef enum
{
  FLASH_READ = 0,
  FLASH_UNLOCKED,
  FLASH_AUTOSELECT,
  FLASH_PROGRAM,
  FLASH_ERASE_INIT,
  FLASH_ERASE_UNLOCKED,
  FLASH_CFI_QUERY,
  FLASH_OTP_ENABLED,
  FLASH_OTP_EXIT_UNLOCKED
} T_CFI_MODE;

typedef struct
{
  uint32 addr;                          /* latched address register */
  uint32 data;                          /* latched data register */
  T_CFI_MODE mode;                      /* current operating mode */
  const uint8 *cfi_data;                /* CFI query data array */
  const uint16 *id;                     /* Manufacturer & Device id codes */
  uint16 otp_area[FLASH_OTP_AREA_SIZE]; /* Secured Silicon Sector / Extended Block area */
  uint8 readmask;                       /* Autoselect mode read address mask */
} T_FLASH_CFI;

static T_FLASH_CFI flash;

/* The cartridge ROM this chip rewrites (a bank swapped out of the battery RAM window, an erased bank): a state does not hold the ROM, so the
 * first change of each 64KB bank keeps its original and the "carry" block of a state holds the banks as they are now. */
#define FLASH_MUT_MAX 4
static struct { uint32 off; uint8 *orig; } fmut[FLASH_MUT_MAX];
static int fmut_n;

static void flash_forget(void)
{
  while (fmut_n > 0) free(fmut[--fmut_n].orig);
}

/* before every write into a bank: base is the bank's memory (the battery RAM or the ROM) */
static void flash_touch(uint8 *base)
{
  int i;
  if (base < cart.rom || base >= cart.rom + MAXROMSIZE) return;
  uint32 off = (uint32)(base - cart.rom) & ~0xffffu;
  for (i = 0; i < fmut_n; i++) if (fmut[i].off == off) return;
  if (fmut_n >= FLASH_MUT_MAX) return;
  fmut[fmut_n].orig = (uint8 *)malloc(0x10000);
  if (!fmut[fmut_n].orig) return;
  memcpy(fmut[fmut_n].orig, cart.rom + off, 0x10000);
  fmut[fmut_n].off = off;
  fmut_n++;
}

int flash_cfi_carry_size(void)
{
  return 4 + 0x10000 + FLASH_MUT_MAX * (4 + 0x10000);      /* count, the save window (sram.sram: not in the core's state on these cartridges), the banks */
}

/* The banks changed so far, as they are (save) or put back as the state had them (load; banks the state does not list return to the original) */
int flash_cfi_carry(uint8 *p, int load)
{
  int i, j, n = flash_cfi_carry_size();
  if (!n || !p) return n;
  if (!load)
  {
    memset(p, 0, n);
    memcpy(p + 4, sram.sram, 0x10000);
    for (i = 0; i < fmut_n; i++)       /* only the banks that differ from the ROM file (so equal machines give equal bytes) */
    {
      if (!memcmp(cart.rom + fmut[i].off, fmut[i].orig, 0x10000)) continue;
      uint8 *q = p + 4 + 0x10000 + (int)p[0] * (4 + 0x10000);
      q[0] = (uint8)(fmut[i].off >> 16);
      memcpy(q + 4, cart.rom + fmut[i].off, 0x10000);
      p[0]++;
    }
    return n;
  }
  int cnt = p[0] > FLASH_MUT_MAX ? 0 : p[0];
  memcpy(sram.sram, p + 4, 0x10000);
  for (j = 0; j < cnt; j++)
  {
    const uint8 *q = p + 4 + 0x10000 + j * (4 + 0x10000);
    flash_touch(cart.rom + ((uint32)q[0] << 16));
  }
  for (i = 0; i < fmut_n; i++)
  {
    const uint8 *src = fmut[i].orig;
    for (j = 0; j < cnt; j++)
    {
      const uint8 *q = p + 4 + 0x10000 + j * (4 + 0x10000);
      if (((uint32)q[0] << 16) == fmut[i].off) { src = q + 4; break; }
    }
    memcpy(cart.rom + fmut[i].off, src, 0x10000);
  }
  return n;
}

/* CFI query data arrays */
static const uint8 cfi_query[MAX_FLASH_CFI_SUPPORTED_TYPES][CFI_QUERY_TABLE_LEN] =
{
  /* M29W320EB */
  {
    0x51,0x52,0x59,0x02,0x00,0x40,0x00,0x00,0x00,0x00,0x00,
    0x27,0x36,0xB5,0xC5,0x04,0x00,0x0A,0x00,0x04,0x00,0x03,0x00,
    0x16,0x02,0x00,0x00,0x00,0x02,0x07,0x00,0x20,0x00,0x3e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,
    0x50,0x52,0x49,0x31,0x30,0x00,0x02,0x01,0x01,0x04,0x00,0x00,0x00,0xB5,0xC5,0x02,0x00
  },

  /* S29GL064N (model 04) */
  {
    0x51,0x52,0x59,0x02,0x00,0x40,0x00,0x00,0x00,0x00,0x00,
    0x27,0x36,0x00,0x00,0x07,0x07,0x0A,0x00,0x03,0x05,0x04,0x00,
    0x17,0x02,0x00,0x05,0x00,0x02,0x07,0x00,0x20,0x00,0x7e,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,
    0x50,0x52,0x49,0x31,0x33,0x00,0x02,0x01,0x00,0x08,0x00,0x00,0x02,0xB5,0xC5,0x02,0x01
  }
};

/* Manufacturer & Device id codes */
static const uint16 flash_id[MAX_FLASH_CFI_SUPPORTED_TYPES][16] =
{
  /* M29W320EB */
  {0x0020,0x2257,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000},

  /* S29GL064N (model 04) */
  {0x0001,0x227E,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,0x2210,0x2200}
};

/* Autoselect mode address read masks (max value = 0x0F) */
static const uint8 flash_readmask[MAX_FLASH_CFI_SUPPORTED_TYPES] =
{
  0x03, /* M29W320EB */
  0x0F  /* S29GL064N (model 04) */
};

void flash_cfi_init(T_FLASH_CFI_TYPE type, const uint16 *otp_data)
{
  memset(&flash, 0x00, sizeof(flash));
  flash_forget();
  flash.cfi_data  = cfi_query[type];
  flash.id        = flash_id[type];
  flash.readmask  = flash_readmask[type];
  if (otp_data)
  {
    memcpy(flash.otp_area, otp_data, sizeof(flash.otp_area));
  }
}

void flash_cfi_write(unsigned int address, unsigned int data)
{
  /* 64KB bank index */
  unsigned int index = (address >> 15) & 0xff;

  if (flash.mode == FLASH_PROGRAM)
  {
    /* assume any writable 64KB bank except first one is not write-protected */
    if (index > 0)
    {
      /* assume any programmed bank should be mapped to SRAM (max 64KB) */
      if (m68k.memory_map[index].base != sram.sram)
      {
        /* if not, assume save data sector is switched (see https://gitlab.com/doragasu/sgdk-flash-save/-/blob/master/src/saveman.c) */
        int i = 0x7f;
        do
        {
          if (m68k.memory_map[i].base == sram.sram)
          {
             /* 64KB bank currently mapped to SRAM is mapped back to cartridge ROM */
             m68k.memory_map[i].base = cart.rom + (i<<16);
             
             /* copy current SRAM data to cartridge ROM (should eventually be copied back to SRAM then normally erased) */
             flash_touch(m68k.memory_map[i].base);
             memcpy(m68k.memory_map[i].base, sram.sram, 0x10000);
             break;
          }
        } while (--i > 0);

        /* copy programmed 64KB bank content to SRAM (should normally be filled with 0xFF) */
        memcpy(sram.sram, m68k.memory_map[index].base, 0x10000);

        /* programmed 64KB bank is now mapped to SRAM */
        m68k.memory_map[index].base = sram.sram;
      }

      /* write data to SRAM (flash programming can only clear bits set to 1) */
      WRITE_WORD(sram.sram, (address << 1) & 0xfffe, data & READ_WORD(sram.sram, (address << 1) & 0xfffe));
    }

    /* reset to default read mode */
    flash.mode = FLASH_READ;
  }
  else
  {
    /* only A0-A11 and D0-D7 are decoded */
    address &= 0xfff;
    data &= 0xff;

    /* detect RESET command (always valid except in OTP area read mode) */
    if ((data == 0xf0) && (flash.mode < FLASH_OTP_ENABLED))
    {
      /* reset to default read mode */
      flash.mode = FLASH_READ;
    }
    else
    {
      switch (flash.mode)
      {
        case FLASH_UNLOCKED:
        {
          /* detect supported commands */
          if (address == 0x555)
          {
            if (data == 0x80)
            {
              /* ERASE command initialization */
              flash.mode = FLASH_ERASE_INIT;
            }
            else if (data == 0x88)
            {
              /* ENTER SECURED SILICON SECTOR REGION / EXTENDED BLOCK command */
              flash.mode = FLASH_OTP_ENABLED;
            }
            else if (data == 0x90)
            {
              /* AUTOSELECT command */
              flash.mode = FLASH_AUTOSELECT;
            }
            else if (data == 0xA0)
            {
              /* PROGRAM command */
              flash.mode = FLASH_PROGRAM;
            }
          }
          break;
        }

        case FLASH_OTP_ENABLED:
        {
          /* detect UNLOCK sequence */
          if ((flash.addr == 0x555) && (flash.data == 0xAA) && (address == 0x2AA) && (data == 0x55))
          {
            flash.mode = FLASH_OTP_EXIT_UNLOCKED;
          }
          break;
        }

        case FLASH_OTP_EXIT_UNLOCKED:
        {
          /* detect EXIT SECURED SILICON SECTOR REGION / EXTENDED BLOCK command */
          if ((flash.addr == 0x555) && (flash.data == 0x90) && (data == 0x00))
          {
            /* reset to default read mode */
            flash.mode = FLASH_READ;
          }
          break;
        }

        case FLASH_ERASE_INIT:
        {
          /* detect UNLOCK sequence */
          if ((flash.addr == 0x555) && (flash.data == 0xAA) && (address == 0x2AA) && (data == 0x55))
          {
            flash.mode = FLASH_ERASE_UNLOCKED;
          }
          break;
        }

        case FLASH_ERASE_UNLOCKED:
        {
          /* detect CHIP ERASE command */
          if ((address == 0x555) && (data == 0x10))
          {
            int i;

            /* assume any writable 64KB bank except first one is not write-protected (max. 8MB flash memory) */
            for (i=0x01; i<0x80; i++)
            {
              /* only unprotected sectors can be erased */
              if (m68k.memory_map[i].write16 != m68k_unused_16_w)
              {
                flash_touch(m68k.memory_map[i].base);
                memset(m68k.memory_map[i].base, 0xff, 0x10000);
                break;
              }
            }

            /* reset to default read mode */
            flash.mode = FLASH_READ;
          }

          /* detect SECTOR ERASE command */
          else if (data == 0x30)
          {
            /* assume any writable 64KB bank except first one is not write-protected */
            if (index > 0)
            {
              /* for simplicity, erase whole 64KB bank (assume 64KB writable sectors only) */
              flash_touch(m68k.memory_map[index].base);
              memset(m68k.memory_map[index].base, 0xff, 0x10000);
            }
 
            /* reset to default read mode */
            flash.mode = FLASH_READ;
          }
          break;
        }

        default:
        {
          /* detect UNLOCK sequence */
          if ((flash.addr == 0x555) && (flash.data == 0xAA) && (address == 0x2AA) && (data == 0x55))
          {
            flash.mode = FLASH_UNLOCKED;
          }

          /* detect CFI QUERY command */
          else if ((address == 0x55) && (data == 0x98))
          {
            flash.mode = FLASH_CFI_QUERY;
          }
          break;
        }
      }
    }
  }

  /* latch address & data */
  flash.addr = address;
  flash.data = data;
}

unsigned int flash_cfi_read(unsigned int address)
{
  /* detect current read mode */
  switch (flash.mode)
  {
    case FLASH_OTP_ENABLED:
    case FLASH_OTP_EXIT_UNLOCKED:
    {
      /* OTP area */
      return flash.otp_area[address & (FLASH_OTP_AREA_SIZE - 1)];
    }

    case FLASH_AUTOSELECT:
    {
      /* Manufacturer and Device ID codes */
      return flash.id[address & flash.readmask];
    }

    case FLASH_CFI_QUERY:
    {
      /* CFI data array (identification, system interface information, device geometry definition and primary extended query table only) */
      int index = address - 0x10;
      if ((index >= 0x00) && (index < CFI_QUERY_TABLE_LEN))
      {
        /* D8-D15 are forced to 0 */
        return flash.cfi_data[index];
      }

      /* unsupported address */
      return 0x00;
    }

    default:
    {
      /* default read mode */
      if (m68k.memory_map[(address>>15)&0xff].base == sram.sram)
      {
        /* backup RAM (always stored in big endian format) */
        return READ_WORD(sram.sram, (address << 1) & 0xfffe);
      }
      else
      {
        /* cartridge ROM */
        return *(uint16 *)(m68k.memory_map[(address>>15)&0xff].base + ((address << 1) & 0xfffe));
      }
    }
  }
}

int flash_cfi_context_save(uint8 *state)
{
  int bufferptr = 0;

  save_param(&flash.addr, sizeof(flash.addr));
  save_param(&flash.data, sizeof(flash.data));
  save_param(&flash.mode, sizeof(flash.mode));

  return bufferptr;
}

int flash_cfi_context_load(uint8 *state)
{
  int bufferptr = 0;

  load_param(&flash.addr, sizeof(flash.addr));
  load_param(&flash.data, sizeof(flash.data));
  load_param(&flash.mode, sizeof(flash.mode));

  return bufferptr;
}
