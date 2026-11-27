/* Cygne
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Dox dox@space.pl
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <string.h>

#include "../mednafen-types.h"
#include "wswan-memory.h"

/* 1bpp -> 8-byte pixel-strip expansion tables (normal and H-flipped).
 * Combining two planes via lut_norm[b0] | (lut_norm[b1] << 1) produces
 * the exact 2bpp row without carry across bytes, shrinking .bss from
 * 1 MiB to 4 KiB so tile decoding stays in L1 cache. */
static uint64 lut_2bpp_norm[256];
static uint64 lut_2bpp_flip[256];
static int lut_2bpp_ready;
uint8	wsTCache[512*64];			
uint8	wsTCache2[512*64];			
uint8	wsTCacheFlipped[512*64];
uint8	wsTCacheFlipped2[512*64];
uint8	wsTCacheUpdate[512];		
uint8	wsTCacheUpdate2[512];		  
uint8	wsTileRow[8];
int	wsVMode;				

void WSWan_TCacheInvalidByAddr(uint32 ws_offset)
{
   if(wsVMode  && (ws_offset>=0x4000)&&(ws_offset<0x8000))
   {
      wsTCacheUpdate[(ws_offset-0x4000)>>5]=false; /*invalidate tile*/
      return;
   }
   else if((ws_offset>=0x2000)&&(ws_offset<0x4000))
   {
      wsTCacheUpdate[(ws_offset-0x2000)>>4]=false; /*invalidate tile*/
      return;
   }

   if(wsVMode  && (ws_offset>=0x8000)&&(ws_offset<0xc000))
   {
      wsTCacheUpdate2[(ws_offset-0x8000)>>5]=false; /*invalidate tile*/
      return;
   }
   else if((ws_offset>=0x4000)&&(ws_offset<0x6000))
   {
      wsTCacheUpdate2[(ws_offset-0x4000)>>4]=false; /*invalidate tile*/
      return;
   }
}

void wsSetVideo(int number,bool force)
{
   if((number!=wsVMode)||(force))
   { 
      wsVMode=number;
      memset(wsTCacheUpdate,0,512);
      memset(wsTCacheUpdate2,0,512);
   }
}

void wsMakeTiles(void)
{
   int x, k;
   if(lut_2bpp_ready)
      return;
   for(x=0;x<256;x++)
   {
      uint8 norm[8], flip[8];
      for(k=0;k<8;k++)
      {
         uint8 bit = (uint8)((x >> (7 - k)) & 1);
         norm[k] = bit;
         flip[7 - k] = bit;
      }
      memcpy(&lut_2bpp_norm[x], norm, 8);
      memcpy(&lut_2bpp_flip[x], flip, 8);
   }
   lut_2bpp_ready = 1;
}

void wsGetTile(uint32 number,uint32 line,int flipv,int fliph,int bank)
{
   uint32		t_adr,t_index,i;
   uint8		byte0,byte1,byte2,byte3;

   if((!bank)||(!(wsVMode &0x07)))
   {
#ifdef TCACHE_OFF
      wsTCacheUpdate[number]=false;
#endif

      if(!wsTCacheUpdate[number])
      {
         wsTCacheUpdate[number]=true;
         switch(wsVMode)
         {
            case 7:
               t_adr=0x4000+(number<<5);
               t_index=number<<6;
               for(i=0;i<8;i++)
               {
                  byte0=wsRAM[t_adr++];
                  byte1=wsRAM[t_adr++];
                  byte2=wsRAM[t_adr++];
                  byte3=wsRAM[t_adr++];
                  wsTCache[t_index]=byte0>>4;
                  wsTCacheFlipped[t_index++]=byte3&15;
                  wsTCache[t_index]=byte0&15;
                  wsTCacheFlipped[t_index++]=byte3>>4;
                  wsTCache[t_index]=byte1>>4;
                  wsTCacheFlipped[t_index++]=byte2&15;
                  wsTCache[t_index]=byte1&15;
                  wsTCacheFlipped[t_index++]=byte2>>4;
                  wsTCache[t_index]=byte2>>4;
                  wsTCacheFlipped[t_index++]=byte1&15;
                  wsTCache[t_index]=byte2&15;
                  wsTCacheFlipped[t_index++]=byte1>>4;
                  wsTCache[t_index]=byte3>>4;
                  wsTCacheFlipped[t_index++]=byte0&15;
                  wsTCache[t_index]=byte3&15;
                  wsTCacheFlipped[t_index++]=byte0>>4;
               }
               break;

            case 6:
               t_adr=0x4000+(number<<5);
               t_index=number<<6;
               for(i=0;i<8;i++)
               {
                  byte0=wsRAM[t_adr++];
                  byte1=wsRAM[t_adr++];
                  byte2=wsRAM[t_adr++];
                  byte3=wsRAM[t_adr++];
                  wsTCache[t_index]=((byte0>>7)&1)|(((byte1>>7)&1)<<1)|(((byte2>>7)&1)<<2)|(((byte3>>7)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0)&1)|(((byte1)&1)<<1)|(((byte2)&1)<<2)|(((byte3)&1)<<3);
                  wsTCache[t_index]=((byte0>>6)&1)|(((byte1>>6)&1)<<1)|(((byte2>>6)&1)<<2)|(((byte3>>6)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0>>1)&1)|(((byte1>>1)&1)<<1)|(((byte2>>1)&1)<<2)|(((byte3>>1)&1)<<3);
                  wsTCache[t_index]=((byte0>>5)&1)|(((byte1>>5)&1)<<1)|(((byte2>>5)&1)<<2)|(((byte3>>5)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0>>2)&1)|(((byte1>>2)&1)<<1)|(((byte2>>2)&1)<<2)|(((byte3>>2)&1)<<3);
                  wsTCache[t_index]=((byte0>>4)&1)|(((byte1>>4)&1)<<1)|(((byte2>>4)&1)<<2)|(((byte3>>4)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0>>3)&1)|(((byte1>>3)&1)<<1)|(((byte2>>3)&1)<<2)|(((byte3>>3)&1)<<3);
                  wsTCache[t_index]=((byte0>>3)&1)|(((byte1>>3)&1)<<1)|(((byte2>>3)&1)<<2)|(((byte3>>3)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0>>4)&1)|(((byte1>>4)&1)<<1)|(((byte2>>4)&1)<<2)|(((byte3>>4)&1)<<3);
                  wsTCache[t_index]=((byte0>>2)&1)|(((byte1>>2)&1)<<1)|(((byte2>>2)&1)<<2)|(((byte3>>2)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0>>5)&1)|(((byte1>>5)&1)<<1)|(((byte2>>5)&1)<<2)|(((byte3>>5)&1)<<3);
                  wsTCache[t_index]=((byte0>>1)&1)|(((byte1>>1)&1)<<1)|(((byte2>>1)&1)<<2)|(((byte3>>1)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0>>6)&1)|(((byte1>>6)&1)<<1)|(((byte2>>6)&1)<<2)|(((byte3>>6)&1)<<3);
                  wsTCache[t_index]=((byte0)&1)|(((byte1)&1)<<1)|(((byte2)&1)<<2)|(((byte3)&1)<<3);
                  wsTCacheFlipped[t_index++]=((byte0>>7)&1)|(((byte1>>7)&1)<<1)|(((byte2>>7)&1)<<2)|(((byte3>>7)&1)<<3);
               }
               break;

            default:
               if(!lut_2bpp_ready)
                  wsMakeTiles();
               t_adr=0x2000+(number<<4);
               t_index=number<<6;
               for(i=0;i<8;i++)
               {
                  uint64 row, row_f;
                  byte0=wsRAM[t_adr++];
                  byte1=wsRAM[t_adr++];
                  row = lut_2bpp_norm[byte0] | (lut_2bpp_norm[byte1] << 1);
                  row_f = lut_2bpp_flip[byte0] | (lut_2bpp_flip[byte1] << 1);
                  memcpy(&wsTCache[t_index], &row, 8);
                  memcpy(&wsTCacheFlipped[t_index], &row_f, 8);
                  t_index += 8;
               }
         }
      }
      if(flipv)
         line=7-line;
      if(fliph)
         memcpy(&wsTileRow[0],&wsTCacheFlipped[(number<<6)|(line<<3)],8);
      else
         memcpy(&wsTileRow[0],&wsTCache[(number<<6)|(line<<3)],8);
   }
   else
   {

#ifdef TCACHE_OFF
      wsTCacheUpdate2[number]=false;
#endif

      if(!wsTCacheUpdate2[number])
      {
         wsTCacheUpdate2[number]=true;
         switch(wsVMode)
         {
            case 7:
               t_adr=0x8000+(number<<5);
               t_index=number<<6;
               for(i=0;i<8;i++)
               {
                  byte0=wsRAM[t_adr++];
                  byte1=wsRAM[t_adr++];
                  byte2=wsRAM[t_adr++];
                  byte3=wsRAM[t_adr++];
                  wsTCache2[t_index]=byte0>>4;
                  wsTCacheFlipped2[t_index++]=byte3&15;
                  wsTCache2[t_index]=byte0&15;
                  wsTCacheFlipped2[t_index++]=byte3>>4;
                  wsTCache2[t_index]=byte1>>4;
                  wsTCacheFlipped2[t_index++]=byte2&15;
                  wsTCache2[t_index]=byte1&15;
                  wsTCacheFlipped2[t_index++]=byte2>>4;
                  wsTCache2[t_index]=byte2>>4;
                  wsTCacheFlipped2[t_index++]=byte1&15;
                  wsTCache2[t_index]=byte2&15;
                  wsTCacheFlipped2[t_index++]=byte1>>4;
                  wsTCache2[t_index]=byte3>>4;
                  wsTCacheFlipped2[t_index++]=byte0&15;
                  wsTCache2[t_index]=byte3&15;
                  wsTCacheFlipped2[t_index++]=byte0>>4;
               }
               break;
            case 6:
               t_adr=0x8000+(number<<5);
               t_index=number<<6;
               for(i=0;i<8;i++)
               {
                  byte0=wsRAM[t_adr++];
                  byte1=wsRAM[t_adr++];
                  byte2=wsRAM[t_adr++];
                  byte3=wsRAM[t_adr++];
                  wsTCache2[t_index]=((byte0>>7)&1)|(((byte1>>7)&1)<<1)|(((byte2>>7)&1)<<2)|(((byte3>>7)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0)&1)|(((byte1)&1)<<1)|(((byte2)&1)<<2)|(((byte3)&1)<<3);
                  wsTCache2[t_index]=((byte0>>6)&1)|(((byte1>>6)&1)<<1)|(((byte2>>6)&1)<<2)|(((byte3>>6)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0>>1)&1)|(((byte1>>1)&1)<<1)|(((byte2>>1)&1)<<2)|(((byte3>>1)&1)<<3);
                  wsTCache2[t_index]=((byte0>>5)&1)|(((byte1>>5)&1)<<1)|(((byte2>>5)&1)<<2)|(((byte3>>5)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0>>2)&1)|(((byte1>>2)&1)<<1)|(((byte2>>2)&1)<<2)|(((byte3>>2)&1)<<3);
                  wsTCache2[t_index]=((byte0>>4)&1)|(((byte1>>4)&1)<<1)|(((byte2>>4)&1)<<2)|(((byte3>>4)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0>>3)&1)|(((byte1>>3)&1)<<1)|(((byte2>>3)&1)<<2)|(((byte3>>3)&1)<<3);
                  wsTCache2[t_index]=((byte0>>3)&1)|(((byte1>>3)&1)<<1)|(((byte2>>3)&1)<<2)|(((byte3>>3)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0>>4)&1)|(((byte1>>4)&1)<<1)|(((byte2>>4)&1)<<2)|(((byte3>>4)&1)<<3);
                  wsTCache2[t_index]=((byte0>>2)&1)|(((byte1>>2)&1)<<1)|(((byte2>>2)&1)<<2)|(((byte3>>2)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0>>5)&1)|(((byte1>>5)&1)<<1)|(((byte2>>5)&1)<<2)|(((byte3>>5)&1)<<3);
                  wsTCache2[t_index]=((byte0>>1)&1)|(((byte1>>1)&1)<<1)|(((byte2>>1)&1)<<2)|(((byte3>>1)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0>>6)&1)|(((byte1>>6)&1)<<1)|(((byte2>>6)&1)<<2)|(((byte3>>6)&1)<<3);
                  wsTCache2[t_index]=((byte0)&1)|(((byte1)&1)<<1)|(((byte2)&1)<<2)|(((byte3)&1)<<3);
                  wsTCacheFlipped2[t_index++]=((byte0>>7)&1)|(((byte1>>7)&1)<<1)|(((byte2>>7)&1)<<2)|(((byte3>>7)&1)<<3);
               }
               break;
            default:
               if(!lut_2bpp_ready)
                  wsMakeTiles();
               t_adr=0x4000+(number<<4);
               t_index=number<<6;
               for(i=0;i<8;i++)
               {
                  uint64 row, row_f;
                  byte0=wsRAM[t_adr++];
                  byte1=wsRAM[t_adr++];
                  row = lut_2bpp_norm[byte0] | (lut_2bpp_norm[byte1] << 1);
                  row_f = lut_2bpp_flip[byte0] | (lut_2bpp_flip[byte1] << 1);
                  memcpy(&wsTCache2[t_index], &row, 8);
                  memcpy(&wsTCacheFlipped2[t_index], &row_f, 8);
                  t_index += 8;
               }
         }	
      }
      if(flipv)
         line=7-line;
      if(fliph)
         memcpy(&wsTileRow[0],&wsTCacheFlipped2[(number<<6)|(line<<3)],8);
      else
         memcpy(&wsTileRow[0],&wsTCache2[(number<<6)|(line<<3)],8);
   }
}
