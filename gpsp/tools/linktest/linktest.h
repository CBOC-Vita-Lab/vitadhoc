/* linktest.h -- shared between main.c (ROM, Thumb) and iwram.c (IWRAM, ARM).
 * See README.md for what the ROM does and how the host reads its verdict. */
#ifndef LINKTEST_H
#define LINKTEST_H

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed int     s32;

#define REG16(a) (*(volatile u16 *)(a))
#define REG32(a) (*(volatile u32 *)(a))

#define DISPCNT     REG16(0x04000000)
#define DISPSTAT    REG16(0x04000004)
#define VCOUNT      REG16(0x04000006)
#define DMA3SAD     REG32(0x040000D4)
#define DMA3DAD     REG32(0x040000D8)
#define DMA3CNT     REG32(0x040000DC)
#define TM0D        REG16(0x04000100)
#define TM0CNT      REG16(0x04000102)
#define TM1D        REG16(0x04000104)
#define TM1CNT      REG16(0x04000106)
#define TM2D        REG16(0x04000108)
#define TM2CNT      REG16(0x0400010A)
#define TM3D        REG16(0x0400010C)
#define TM3CNT      REG16(0x0400010E)
#define SIODATA32   REG32(0x04000120)
#define SIOMULTI0   REG16(0x04000120)
#define SIOMULTI1   REG16(0x04000122)
#define SIOCNT      REG16(0x04000128)
#define SIOMLT_SEND REG16(0x0400012A)   /* = SIODATA8 */
#define KEYINPUT    REG16(0x04000130)
#define RCNT        REG16(0x04000134)
#define IE          REG16(0x04000200)
#define IF          REG16(0x04000202)
#define IME         REG16(0x04000208)

#define IRQ_VBLANK  0x0001
#define IRQ_VCOUNT  0x0004
#define IRQ_TIMER2  0x0020
#define IRQ_TIMER3  0x0040
#define IRQ_SERIAL  0x0080

/* The child changes what it drives on the cable only at this scanline (its
 * VCOUNT interrupt): early in the frame, so that the parent -- which looks at
 * child-driven state only in its V-blank interrupt, line 160 -- sees it
 * settled however late the pair stepper delivers it (README "lookahead"). */
#define SAFE_LINE 6

/* ---- the verdict, at a fixed address: EWRAM 0x02000000 ------------------ */
struct results
{
   u32 magic;          /* 00 'LKT1' */
   u32 role;           /* 04 0 undecided, 1 parent, 2 child, 3 no partner */
   u32 frame;          /* 08 V-blanks seen */
   u32 loops;          /* 0C whole passes through every phase */
   u32 phase;          /* 10 phase in progress (1..NPH) */
   u32 fail_total;     /* 14 every failed check */
   u32 ok_total;       /* 18 every passed check */
   u32 rx_sum;         /* 1C running checksum of every word received */
   u32 xfers_mp;       /* 20 multi-player transfers completed */
   u32 xfers_n;        /* 24 normal-mode transfers completed */
   u32 peer_ok;        /* 28 passed checks the other side reported */
   u32 peer_fail;      /* 2C phases the other side reported a failure in */
   u32 smc_ok;         /* 30 self-modifying-code checks passed */
   u32 smc_fail;       /* 34 ... failed */
   u32 timeouts;       /* 38 a phase gave up waiting */
   u32 tmr_mix;        /* 3C checksum of timer readings taken at transfers */
   u32 irq_vbl;        /* 40 */
   u32 irq_vcnt;       /* 44 */
   u32 irq_tm2;        /* 48 */
   u32 irq_tm3;        /* 4C */
   u32 irq_sio;        /* 50 */
   u32 keys_seen;      /* 54 OR of every button seen pressed */
   u32 keys_sum;       /* 58 checksum of the key words this side sent */
   u32 peer_keys_sum;  /* 5C checksum of the key words received */
   u32 first_fail;     /* 60 first failure: phase<<24 | code<<16 | index */
   u32 first_got;      /* 64 */
   u32 first_exp;      /* 68 */
   u32 gpio_irqs;      /* 6C SI falling-edge interrupts (child) */
   u32 pad[4];         /* 70 */
   u32 ok[16];         /* 80 passed checks by phase */
   u32 fail[16];       /* C0 failed checks by phase */
};
extern volatile struct results R;      /* linker: 0x02000000 */
#define RESULT_MAGIC 0x31544B4Cu

#define EWCODE ((u8 *)0x02000400)        /* where the EWRAM routine runs */

enum { ROLE_NONE = 0, ROLE_PARENT = 1, ROLE_CHILD = 2, ROLE_SOLO = 3 };
enum { K_MPDATA = 0, K_MPKEYS, K_NORM, K_NC, K_GPIO };
enum { PACE_NONE = 0, PACE_VBL, PACE_TMR, PACE_POLL, PACE_NTMR, PACE_GTMR };

struct phdef { u8 kind, n, pace, arg; };   /* arg: baud bits / data bits */
#define NPH 10
#define PH_SOLO 11
extern const struct phdef phases[NPH + 1];

#define GPIO_A_STEPS 32
#define GPIO_B_STEPS 8
#define GP_SD(k) ((0x6A39C5D2u >> (k)) & 1)
#define GP_SO(k) ((0x9D3C1E57u >> (k)) & 1)
#define GP_B(j)  ((0xB4u >> (j)) & 1)

/* ---- iwram.c / crt0.s: all of this lives and runs in IWRAM (ARM) -------- */
#define IWFN __attribute__((long_call))
extern volatile u32 role, phase, loop_no, pace, vbl_count, vbl_flag;
extern volatile u32 sw_step, sw_kind, gpio_child_state, my_keys, peer_keys;
IWFN void mp_start(void);
IWFN void mp_complete(void);
IWFN u32  seq(u32 r, u32 p, u32 i);
IWFN void account(u32 p, u32 good, u32 code, u32 idx, u32 got, u32 exp);
IWFN u32  mix_iw(u32 x);                 /* crt0.s; patched at mix_iw_patch */
IWFN u32  selfmod_iw(u32 imm);           /* crt0.s; rewrites itself */
IWFN u32  swi_iw(u32 num, u32 den);      /* crt0.s; Div + Sqrt SWIs from IWRAM */
extern u32 mix_iw_patch[1];
extern const u16 ew_img[], ew_img_patch[], ew_img_end[];

enum { GC_IDLE = 0, GC_POLL, GC_POLLED, GC_DRIVE, GC_DONE };

#endif
