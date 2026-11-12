/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "memmap.h"

// CBOC-Vita: inlined fast path of S9xSA1GetByte (a plain ROM/RAM block); everything else calls the original
static alwaysinline uint8 S9xSA1GetByteF (uint32 address)
{
	uint8	*GetAddress = SA1.Map[(address & 0xffffff) >> MEMMAP_SHIFT];

	if (GetAddress >= (uint8 *) CMemory::MAP_LAST)
	{
		SA1.Cycles += SA1.MemSpeed;
		if (SA1Rec.on)
			SA1RecNote(GetAddress + (address & 0xffff));
		return (*(GetAddress + (address & 0xffff)));
	}

	return S9xSA1GetByte(address);
}

#define CPU								SA1
#define ICPU							SA1
#define Registers						SA1Registers
#define OpenBus							SA1OpenBus
#define S9xGetByte						S9xSA1GetByteF
#define S9xGetWord						S9xSA1GetWord
#define S9xSetByte						S9xSA1SetByte
#define S9xSetWord						S9xSA1SetWord
#define S9xSetPCBase					S9xSA1SetPCBase
#define S9xOpcodesM1X1					S9xSA1OpcodesM1X1
#define S9xOpcodesM1X0					S9xSA1OpcodesM1X0
#define S9xOpcodesM0X1					S9xSA1OpcodesM0X1
#define S9xOpcodesM0X0					S9xSA1OpcodesM0X0
#define S9xOpcodesE1					S9xSA1OpcodesE1
#define S9xOpcodesSlow					S9xSA1OpcodesSlow
#define S9xOpcode_IRQ					S9xSA1Opcode_IRQ
#define S9xOpcode_NMI					S9xSA1Opcode_NMI
#define S9xUnpackStatus					S9xSA1UnpackStatus
#define S9xPackStatus					S9xSA1PackStatus
#define S9xFixCycles					S9xSA1FixCycles
#define Immediate8						SA1Immediate8
#define Immediate16						SA1Immediate16
#define Relative						SA1Relative
#define RelativeLong					SA1RelativeLong
#define Absolute						SA1Absolute
#define AbsoluteLong					SA1AbsoluteLong
#define AbsoluteIndirect				SA1AbsoluteIndirect
#define AbsoluteIndirectLong			SA1AbsoluteIndirectLong
#define AbsoluteIndexedIndirect			SA1AbsoluteIndexedIndirect
#define Direct							SA1Direct
#define DirectIndirectIndexed			SA1DirectIndirectIndexed
#define DirectIndirectIndexedLong		SA1DirectIndirectIndexedLong
#define DirectIndexedIndirect			SA1DirectIndexedIndirect
#define DirectIndexedX					SA1DirectIndexedX
#define DirectIndexedY					SA1DirectIndexedY
#define AbsoluteIndexedX				SA1AbsoluteIndexedX
#define AbsoluteIndexedY				SA1AbsoluteIndexedY
#define AbsoluteLongIndexedX			SA1AbsoluteLongIndexedX
#define DirectIndirect					SA1DirectIndirect
#define DirectIndirectLong				SA1DirectIndirectLong
#define StackRelative					SA1StackRelative
#define StackRelativeIndirectIndexed	SA1StackRelativeIndirectIndexed

#define SA1_OPCODES

#include "cpuops.cpp"

static inline void S9xSA1UpdateTimer (void);

/* CBOC-Vita: replaying the SA-1's idle loops.
 *
 * The SA-1 is run after every instruction of the SNES CPU, up to the same moment
 * (S9xSA1MainLoop below), so each call runs one or two SA-1 instructions. Most of
 * them are a wait: Super Mario RPG and Kirby Super Star spend about 80% of their
 * SA-1 instructions in a loop that reads a byte until the SNES CPU changes it.
 *
 * Such a loop is recorded once: the SA-1's whole state after each of its
 * instructions, the cycles each one took, and the plain memory it read. It is a
 * proven idle loop when one pass ends in exactly the state it started from, wrote
 * nothing and read nothing but plain memory in ROM-resident code. From then on, as
 * long as the SA-1 is still exactly at one of the recorded points and every byte the
 * loop reads still holds what it held, a call does not interpret the instructions:
 * it works out where the same instructions would have stopped (the same cycle rule
 * as the loop below) and puts that recorded state in place. Interrupts are still
 * looked at first, the timer is still updated after, and anything that does not
 * match ends the replay; the result is the same as running every instruction. */
struct SA1Point
{
	SSA1Registers	r;
	uint8			c, z, n, v, ob;
	bool8			wfi;
	uint32			spb, sdb, flags;
	uint8			*pcbase;
	int32			memspeed, memspeed2, off;
	struct SOpcodes	*ops;
	uint8			*lens;
};

static struct
{
	int			state;		// 0 watching, 1 recording, 2 replaying
	uint32		head;		// the loop's first instruction
	uint32		last_land;	// the last place a backward jump landed, and how often in a row
	int			same, backoff;
	uint32		eff0;
	int			n;			// recorded points: 0 is the start, n the start again
	int			cur;		// the point the SA-1 is at while replaying
	bool		virt;		// its registers are still to be put in place (S9xSA1ReplaySync)
	SA1Point	pt[17];
	int32		step[17];	// cycles from point i to the next, and which point that is
	uint8		next[17];
	uint8		nrd;
	uint8		*rd_at[16];
	uint8		rd_val[16];
} SA1L;

#ifdef SA1_REPLAY_STATS
#include <cstdio>
#include <cstdlib>
static unsigned long long st_calls, st_replay, st_rec, st_proven, st_fail_same, st_fail_rd, st_abort;
static void st_dump(void) { fprintf(stderr, "sa1 replay: calls=%llu replayed=%llu rec=%llu proven=%llu fail_same=%llu fail_rd=%llu abort=%llu\n", st_calls, st_replay, st_rec, st_proven, st_fail_same, st_fail_rd, st_abort); }
#define ST(x) (x)
#else
#define ST(x)
#endif

static inline void sa1_take (SA1Point &p, int32 off)
{
	p.r = SA1Registers;
	p.c = SA1._Carry; p.z = SA1._Zero; p.n = SA1._Negative; p.v = SA1._Overflow; p.ob = SA1OpenBus;
	p.wfi = SA1.WaitingForInterrupt;
	p.spb = SA1.ShiftedPB; p.sdb = SA1.ShiftedDB; p.flags = SA1.Flags;
	p.pcbase = SA1.PCBase; p.memspeed = SA1.MemSpeed; p.memspeed2 = SA1.MemSpeedx2; p.off = off;
	p.ops = SA1.S9xOpcodes; p.lens = SA1.S9xOpLengths;
}

static inline bool sa1_same (const SA1Point &p)
{
	const SSA1Registers &r = SA1Registers;
	return p.r.PC.xPBPC == r.PC.xPBPC && p.r.A.W == r.A.W && p.r.X.W == r.X.W && p.r.Y.W == r.Y.W &&
		p.r.P.W == r.P.W && p.r.S.W == r.S.W && p.r.D.W == r.D.W && p.r.DB == r.DB &&
		p.c == SA1._Carry && p.z == SA1._Zero && p.n == SA1._Negative && p.v == SA1._Overflow &&
		p.ob == SA1OpenBus && p.wfi == SA1.WaitingForInterrupt &&
		p.spb == SA1.ShiftedPB && p.sdb == SA1.ShiftedDB && p.flags == SA1.Flags &&
		p.pcbase == SA1.PCBase && p.memspeed == SA1.MemSpeed && p.memspeed2 == SA1.MemSpeedx2 &&
		p.ops == SA1.S9xOpcodes && p.lens == SA1.S9xOpLengths;
}

static inline void sa1_put (const SA1Point &p)
{
	SA1Registers = p.r;
	SA1._Carry = p.c; SA1._Zero = p.z; SA1._Negative = p.n; SA1._Overflow = p.v; SA1OpenBus = p.ob;
	SA1.WaitingForInterrupt = p.wfi;
	SA1.ShiftedPB = p.spb; SA1.ShiftedDB = p.sdb; SA1.Flags = p.flags;
	SA1.PCBase = p.pcbase; SA1.MemSpeed = p.memspeed; SA1.MemSpeedx2 = p.memspeed2;
	SA1.S9xOpcodes = p.ops; SA1.S9xOpLengths = p.lens;
}

bool8 S9xSA1RomPatched = FALSE;

static inline bool sa1_code_in_rom (void)
{
	if (S9xSA1RomPatched)
		return false;
#ifdef CBOC_NO_SA1_REPLAY	/* build switch: never records a loop, so every SA-1 instruction is interpreted (the exactness reference) */
	return false;
#endif
	return SA1.PCBase && SA1.PCBase + 0x10000 > Memory.ROM && SA1.PCBase < Memory.ROM + Memory.CalculatedSize;
}

static inline void sa1_stop_recording (int backoff)
{
	SA1Rec.on = FALSE;
	SA1L.state = 0;
	SA1L.backoff = backoff;
	SA1L.same = 0;
}

/* Replays the proven loop up to `target` cycles: the SA-1 goes on from recorded point to
 * recorded point exactly as the loop below would (an instruction starts while the
 * cycles are below the target). With `keep`, the registers are put in place; without,
 * only the point is remembered (SA1L.virt) and S9xSA1ReplaySync puts them in place when
 * anything needs them. False when the loop no longer applies. */
static alwaysinline bool sa1_replay (int32 target, bool keep)
{
	if (!SA1L.virt && !sa1_same(SA1L.pt[SA1L.cur]))
	{
		ST(st_fail_same++);
		return false;
	}
	for (int i = 0; i < SA1L.nrd; i++)
		if (*SA1L.rd_at[i] != SA1L.rd_val[i])
		{
			ST(st_fail_rd++);
			S9xSA1ReplaySync();
			return false;
		}
	ST(st_replay++);
	const int	n = SA1L.n;
	const int32	d = SA1L.pt[n].off;
	int32		c = SA1.Cycles;
	int			cur = SA1L.cur;
	if (target - c > d)
	{
		// whole passes, each started below the target: c += ((target - c - 1) / d) * d. The quotient is almost
		// always 1 to 3 here (a call covers one SNES CPU instruction), and the Vita's Cortex-A9 has no divide
		// instruction (a library call of dozens of cycles, on every replayed call), so subtract while it is small.
		int32	r = target - c - 1;			// >= d here
		if (r < 8 * d)
		{
			do { c += d; r -= d; } while (r >= d);
		}
		else
			c += (r / d) * d;
	}
	while (c < target)
	{
		c += SA1L.step[cur];
		cur = SA1L.next[cur];
	}
	SA1.Cycles = c;
	SA1L.cur = cur;
	if (keep)
	{
		sa1_put(SA1L.pt[cur]);
		SA1L.virt = false;
	}
	else
		SA1L.virt = true;
	return true;
}

void S9xSA1ReplaySync (void)
{
	if (SA1L.virt)
	{
		sa1_put(SA1L.pt[SA1L.cur]);
		SA1L.virt = false;
	}
}

void S9xSA1ReplayReset (void)
{
	S9xSA1ReplaySync();
	S9xSA1ReplayDrop();
}

/* forget the loop without putting its registers in place (the registers are being
 * reset or loaded from a state) */
void S9xSA1ReplayDrop (void)
{
	SA1L.virt = false;
	SA1Rec.on = FALSE;
	SA1L.state = 0;
	SA1L.same = 0;
	SA1L.last_land = 0xffffffff;
}

/* Called after every SNES CPU instruction. While a proven idle loop still applies, and
 * nothing could interrupt it (no SA-1 timer interrupt set up; anything the SNES CPU
 * writes to the SA-1's registers ends the replay), the SA-1 only moves on along the
 * loop's points and its timer is updated, as the full call below does. */
void S9xSA1Run (void)
{
	if (SA1L.state == 2 && !(Memory.FillRAM[0x2200] & 0x60) && !(Memory.FillRAM[0x2210] & 0x03))
	{
		#undef CPU
		const int32	target = CPU.Cycles * 3;
		#define CPU SA1
		if (sa1_replay(target, false))
		{
			S9xSA1UpdateTimer();
			return;
		}
	}
	S9xSA1MainLoop();
}

void S9xSA1MainLoop (void)
{
	if (Memory.FillRAM[0x2200] & 0x60)
	{
		SA1.Cycles += 6; // FIXME
		S9xSA1UpdateTimer();
		return;
	}

	// SA-1 NMI
	if ((Memory.FillRAM[0x2200] & 0x10) && !(Memory.FillRAM[0x220b] & 0x10))
	{
		Memory.FillRAM[0x2301] |= 0x10;
		Memory.FillRAM[0x220b] |= 0x10;

		if (SA1.WaitingForInterrupt)
		{
			SA1.WaitingForInterrupt = FALSE;
			SA1Registers.PCw++;
		}

		S9xSA1Opcode_NMI();
	}
	else
	if (!SA1CheckFlag(IRQ))
	{
		// SA-1 Timer IRQ
		if ((Memory.FillRAM[0x220a] & 0x40) && !(Memory.FillRAM[0x220b] & 0x40))
		{
			Memory.FillRAM[0x2301] |= 0x40;

			if (SA1.WaitingForInterrupt)
			{
				SA1.WaitingForInterrupt = FALSE;
				SA1Registers.PCw++;
			}

			S9xSA1Opcode_IRQ();
		}
		else
		// SA-1 DMA IRQ
		if ((Memory.FillRAM[0x220a] & 0x20) && !(Memory.FillRAM[0x220b] & 0x20))
		{
			Memory.FillRAM[0x2301] |= 0x20;

			if (SA1.WaitingForInterrupt)
			{
				SA1.WaitingForInterrupt = FALSE;
				SA1Registers.PCw++;
			}

			S9xSA1Opcode_IRQ();
		}
		else
		// SA-1 IRQ
		if ((Memory.FillRAM[0x2200] & 0x80) && !(Memory.FillRAM[0x220b] & 0x80))
		{
			Memory.FillRAM[0x2301] |= 0x80;

			if (SA1.WaitingForInterrupt)
			{
				SA1.WaitingForInterrupt = FALSE;
				SA1Registers.PCw++;
			}

			S9xSA1Opcode_IRQ();
		}
	}

	#undef CPU
	int cycles = CPU.Cycles * 3;
	#define CPU SA1
#ifdef SA1_REPLAY_STATS
	if (!st_calls++) atexit(st_dump);
#endif

	if (SA1L.state == 2)
	{
		S9xSA1ReplaySync();
		if (sa1_replay(cycles, true))
		{
			S9xSA1UpdateTimer();
			return;
		}
		SA1L.state = 0;
		SA1L.backoff = 0;
	}
	else
	if (SA1L.state == 1 && !sa1_same(SA1L.pt[SA1L.n]))
		sa1_stop_recording(64);	// an interrupt or a reset came in between

	for (; SA1.Cycles < cycles && !(Memory.FillRAM[0x2200] & 0x60);)
	{
		const uint32	op_at = SA1Registers.PBPC;
		const int32		op_c = SA1.Cycles;
	#ifdef DEBUGGER
		if (SA1.Flags & TRACE_FLAG)
			S9xSA1Trace();
	#endif

		uint8				Op;
		struct SOpcodes	*Opcodes;

		if (SA1.PCBase)
		{
			SA1OpenBus = Op = SA1.PCBase[Registers.PCw];
			Opcodes = SA1.S9xOpcodes;
			SA1.Cycles += SA1.MemSpeed;
		}
		else
		{
			Op = S9xSA1GetByte(Registers.PBPC);
			Opcodes = S9xOpcodesSlow;
		}

		if ((SA1Registers.PCw & MEMMAP_MASK) + SA1.S9xOpLengths[Op] >= MEMMAP_BLOCK_SIZE)
		{
			uint32	oldPC = SA1Registers.PBPC;
			S9xSA1SetPCBase(SA1Registers.PBPC);
			SA1Registers.PBPC = oldPC;
			Opcodes = S9xSA1OpcodesSlow;
		}

		Registers.PCw++;
		(*Opcodes[Op].S9xOpcode)();

		if (SA1L.state == 1)
		{
			const int	k = ++SA1L.n;
			sa1_take(SA1L.pt[k], SA1L.pt[k - 1].off + (SA1.Cycles - op_c));
			if (SA1Registers.PBPC == SA1L.head)
			{
				SA1Rec.on = FALSE;
				SSA1Registers	r0 = SA1L.pt[0].r;
				const bool		same = SA1L.pt[k].off > 0 && SA1Effects == SA1L.eff0 && ({
					const SA1Point &a = SA1L.pt[0], &b = SA1L.pt[k];
					a.r.PC.xPBPC == b.r.PC.xPBPC && a.r.A.W == b.r.A.W && a.r.X.W == b.r.X.W &&
					a.r.Y.W == b.r.Y.W && a.r.P.W == b.r.P.W && a.r.S.W == b.r.S.W && a.r.D.W == b.r.D.W &&
					a.r.DB == b.r.DB && a.c == b.c && a.z == b.z && a.n == b.n && a.v == b.v && a.ob == b.ob &&
					a.wfi == b.wfi && a.spb == b.spb && a.sdb == b.sdb && a.flags == b.flags &&
					a.pcbase == b.pcbase && a.memspeed == b.memspeed && a.memspeed2 == b.memspeed2 &&
					a.ops == b.ops && a.lens == b.lens; });
				(void) r0;
				if (same)
				{
					SA1L.nrd = SA1Rec.n;
					for (int i = 0; i < SA1Rec.n; i++) { SA1L.rd_at[i] = SA1Rec.at[i]; SA1L.rd_val[i] = SA1Rec.val[i]; }
					for (int i = 0; i < k; i++)
					{
						SA1L.step[i] = SA1L.pt[i + 1].off - SA1L.pt[i].off;
						SA1L.next[i] = i + 1 == k ? 0 : i + 1;
					}
					SA1L.state = 2;
					SA1L.cur = 0;
					ST(st_proven++);
					break;		// the rest of this call is the replay's
				}
				sa1_stop_recording(256);
			}
			else
			if (k >= 16 || SA1Effects != SA1L.eff0)
				sa1_stop_recording(256);
		}
		else
		{
			const uint32	now_at = SA1Registers.PBPC;
			if (now_at <= op_at && op_at - now_at < 64)	// a jump back: a loop's start
			{
				if (SA1L.backoff)
					SA1L.backoff--;
				if (now_at != SA1L.last_land)
				{
					SA1L.last_land = now_at;
					SA1L.same = 0;
				}
				else
				if (++SA1L.same >= 2 && !SA1L.backoff && sa1_code_in_rom())
				{
					ST(st_rec++);
					SA1L.state = 1;
					SA1L.head = now_at;
					SA1L.n = 0;
					SA1L.eff0 = SA1Effects;
					sa1_take(SA1L.pt[0], 0);
					SA1Rec.on = TRUE;
					SA1Rec.n = 0;
				}
			}
		}
	}

	if (SA1L.state == 2 && SA1.Cycles < cycles && !(Memory.FillRAM[0x2200] & 0x60))
	{
		if (!sa1_replay(cycles, true))
		{
			SA1L.state = 0;
			SA1L.backoff = 0;
		}
	}

	S9xSA1UpdateTimer();
}

static alwaysinline void S9xSA1UpdateTimer (void) // FIXME
{
	SA1.PrevHCounter = SA1.HCounter;

	if (Memory.FillRAM[0x2210] & 0x80)
	{
		SA1.HCounter += (SA1.Cycles - SA1.PrevCycles);
		if (SA1.HCounter >= 0x800)
		{
			SA1.HCounter -= 0x800;
			SA1.PrevHCounter -= 0x800;
			if (++SA1.VCounter >= 0x200)
				SA1.VCounter = 0;
		}
	}
	else
	{
		SA1.HCounter += (SA1.Cycles - SA1.PrevCycles);
		if (SA1.HCounter >= Timings.H_Max_Master)
		{
			SA1.HCounter -= Timings.H_Max_Master;
			SA1.PrevHCounter -= Timings.H_Max_Master;
			if (++SA1.VCounter >= Timings.V_Max_Master)
				SA1.VCounter = 0;
		}
	}

	SA1.PrevCycles = SA1.Cycles;

	bool8	thisIRQ = Memory.FillRAM[0x2210] & 0x03;

	if (Memory.FillRAM[0x2210] & 0x01)
	{
		if (SA1.PrevHCounter >= SA1.HTimerIRQPos * ONE_DOT_CYCLE || SA1.HCounter < SA1.HTimerIRQPos * ONE_DOT_CYCLE)
			thisIRQ = FALSE;
	}

	if (Memory.FillRAM[0x2210] & 0x02)
	{
		if (SA1.VCounter != SA1.VTimerIRQPos * ONE_DOT_CYCLE)
			thisIRQ = FALSE;
	}

	// SA-1 Timer IRQ control
	if (!SA1.TimerIRQLastState && thisIRQ)
	{
		Memory.FillRAM[0x2301] |= 0x40;
		if (Memory.FillRAM[0x220a] & 0x40)
		{
			Memory.FillRAM[0x220b] &= ~0x40;
		#ifdef DEBUGGER
			S9xTraceFormattedMessage("--- SA-1 Timer IRQ triggered  prev HC:%04d  curr HC:%04d  HTimer:%d Pos:%04d  VTimer:%d Pos:%03d",
				SA1.PrevHCounter, SA1.HCounter,
				(Memory.FillRAM[0x2210] & 0x01) ? 1 : 0, SA1.HTimerIRQPos * ONE_DOT_CYCLE,
				(Memory.FillRAM[0x2210] & 0x02) ? 1 : 0, SA1.VTimerIRQPos);
		#endif
		}
	}

	SA1.TimerIRQLastState = thisIRQ;
}
