/***************************************************************************************
 *  Genesis Plus
 *  CD compatible ROM/RAM cartridge support
 *
 *  Copyright (C) 2012-2021 Eke-Eke (Genesis Plus GX)
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

/*--------------------------------------------------------------------------*/
/* backup RAM cartridge (max. 512KB)                                        */
/*--------------------------------------------------------------------------*/
/* VitAdhoc: counts every write to the backup RAM (scd.c) and the RAM cartridge, so that the player can tell "the save may have changed" without
 * reading and hashing up to 520 KB (md_host_sram_generation) */
unsigned int md_cd_save_gen;

static unsigned int cart_ram_read_byte(unsigned int address)
{
  /* LSB only */
  if (address & 1)
  {
    return scd.cartridge.area[(address >> 1) & scd.cartridge.mask];
  }
  
  return 0xff;
}

static unsigned int cart_ram_read_word(unsigned int address)
{
  return (scd.cartridge.area[(address >> 1) & scd.cartridge.mask] | 0xff00);
}

static void cart_ram_write_byte(unsigned int address, unsigned int data)
{
  /* LSB only */
  if (address & 1)
  {
    scd.cartridge.area[(address >> 1) & scd.cartridge.mask] = data;
    md_cd_save_gen++;
  }
}

static void cart_ram_write_word(unsigned int address, unsigned int data)
{
  scd.cartridge.area[(address >> 1) & scd.cartridge.mask] = data & 0xff;
  md_cd_save_gen++;
}


/*--------------------------------------------------------------------------*/
/* no cartridge in the slot (VitAdhoc: RAM cartridge "Off")                  */
/*--------------------------------------------------------------------------*/
/* An empty cartridge port is not a ROM cartridge: the BIOS reads the RAM cartridge's size id at $400001 and an empty slot answers $FF there
 * (as everywhere else in $400000-$7FFFFF); nothing is writable. */
static unsigned int cart_none_read_byte(unsigned int address) { return 0xff; }
static unsigned int cart_none_read_word(unsigned int address) { return 0xffff; }

/*--------------------------------------------------------------------------*/
/* backup RAM cartridge ID                                                  */
/*--------------------------------------------------------------------------*/

static unsigned int cart_id_read_byte(unsigned int address)
{
  /* LSB only */
  if (address & 1)
  {
    return scd.cartridge.id;
  }

  return 0xff;
}

static unsigned int cart_id_read_word(unsigned int address)
{
  return (scd.cartridge.id | 0xff00);
}


/*--------------------------------------------------------------------------*/
/* backup RAM cartridge write protection                                    */
/*--------------------------------------------------------------------------*/

static unsigned int cart_prot_read_byte(unsigned int address)
{
  /* LSB only */
  if (address & 1)
  {
    return scd.cartridge.prot;
  }

  return 0xff;
}

static unsigned int cart_prot_read_word(unsigned int address)
{
  return (scd.cartridge.prot | 0xff00);
}

static void cart_prot_write_byte(unsigned int address, unsigned int data)
{
  /* LSB only */
  if (address & 1)
  {
    int i;

    if (data & 1)
    {
      /* cartridge is write enabled */
      for (i=0x60; i<0x70; i++)
      {
        m68k.memory_map[i].write8  = cart_ram_write_byte;
        m68k.memory_map[i].write16 = cart_ram_write_word;
        zbank_memory_map[i].write  = cart_ram_write_byte;
      }
    }
    else
    {
      /* cartridge is write protected */
      for (i=0x60; i<0x70; i++)
      {
        m68k.memory_map[i].write8  = m68k_unused_8_w;
        m68k.memory_map[i].write16 = m68k_unused_16_w;
        zbank_memory_map[i].write  = zbank_unused_w;
      }
    }

    scd.cartridge.prot = data;
  }
}

static void cart_prot_write_word(unsigned int address, unsigned int data)
{
  int i;

  if (data & 1)
  {
    /* cartridge is write enabled */
    for (i=0x60; i<0x70; i++)
    {
      m68k.memory_map[i].write8  = cart_ram_write_byte;
      m68k.memory_map[i].write16 = cart_ram_write_word;
      zbank_memory_map[i].write  = cart_ram_write_byte;
    }
  }
  else
  {
    /* cartridge is write protected */
    for (i=0x60; i<0x70; i++)
    {
      m68k.memory_map[i].write8  = m68k_unused_8_w;
      m68k.memory_map[i].write16 = m68k_unused_16_w;
      zbank_memory_map[i].write  = zbank_unused_w;
    }
  }

  scd.cartridge.prot = data & 0xff;
}

/*--------------------------------------------------------------------------*/
/* ROM/RAM cartridge initialization                                         */
/*--------------------------------------------------------------------------*/
void cd_cart_init(void)
{
  int i;

  /* System boot mode */
  if (scd.cartridge.boot)
  {
    /* disable backup RAM cartridge when booting from cartridge (Mode 1) */
    scd.cartridge.id = 0;
  }
  else
  {
    /* enable 512K backup RAM cartridge when booting from CD (Mode 2) */
    scd.cartridge.id = cart_size;
  }

  /* RAM cartridge enabled ? */
  if (scd.cartridge.id)
  {
    /* disable cartridge backup memory */
    memset(&sram, 0, sizeof (T_SRAM));

    /* clear backup RAM */
    memset(scd.cartridge.area, 0x00, sizeof(scd.cartridge.area));

    /* backup RAM size mask */
    scd.cartridge.mask = (1 << (scd.cartridge.id + 13)) - 1;

    /* enable RAM cartridge write access */
    scd.cartridge.prot = 1;

    /* RAM cartridge ID register (read-only) */
    for (i=0x40; i<0x60; i++)
    {
      m68k.memory_map[i].base    = NULL;
      m68k.memory_map[i].read8   = cart_id_read_byte;
      m68k.memory_map[i].read16  = cart_id_read_word;
      m68k.memory_map[i].write8  = m68k_unused_8_w;
      m68k.memory_map[i].write16 = m68k_unused_16_w;
      zbank_memory_map[i].read   = cart_id_read_byte;
      zbank_memory_map[i].write  = zbank_unused_w;
    }

    /* RAM cartridge memory */
    for (i=0x60; i<0x70; i++)
    {
      m68k.memory_map[i].base    = NULL;
      m68k.memory_map[i].read8   = cart_ram_read_byte;
      m68k.memory_map[i].read16  = cart_ram_read_word;
      m68k.memory_map[i].write8  = cart_ram_write_byte;
      m68k.memory_map[i].write16 = cart_ram_write_word;
      zbank_memory_map[i].read   = cart_ram_read_byte;
      zbank_memory_map[i].write  = cart_ram_write_byte;
    }

    /* RAM cartridge write protection register */
    for (i=0x70; i<0x80; i++)
    {
      m68k.memory_map[i].base    = NULL;
      m68k.memory_map[i].read8   = cart_prot_read_byte;
      m68k.memory_map[i].read16  = cart_prot_read_word;
      m68k.memory_map[i].write8  = cart_prot_write_byte;
      m68k.memory_map[i].write16 = cart_prot_write_word;
      zbank_memory_map[i].read   = cart_prot_read_byte;
      zbank_memory_map[i].write  = cart_prot_write_byte;
    }
  }
  else if (scd.cartridge.boot)
  {
    /* initialize ROM cartridge */
    md_cart_init();
  }
  else
  {
    /* VitAdhoc: booting from CD without a RAM cartridge: the slot is empty. The cartridge port ($400000-$7FFFFF) reads $FF and ignores writes; no
       cartridge code runs (before, the first byte of the disc header was set up as a one byte ROM cartridge with cartridge SRAM) */
    for (i=0x40; i<0x80; i++)
    {
      m68k.memory_map[i].base    = NULL;
      m68k.memory_map[i].read8   = cart_none_read_byte;
      m68k.memory_map[i].read16  = cart_none_read_word;
      m68k.memory_map[i].write8  = m68k_unused_8_w;
      m68k.memory_map[i].write16 = m68k_unused_16_w;
      zbank_memory_map[i].read   = cart_none_read_byte;
      zbank_memory_map[i].write  = zbank_unused_w;
    }
    memset(&sram, 0, sizeof (T_SRAM));
  }
}
