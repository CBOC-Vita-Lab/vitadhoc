/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/*
 * CBOC-Vita local addition (not part of upstream Snes9x): a pre-decoded block
 * interpreter for the Super FX (GSU). It runs the same cycle model as the
 * reference interpreter in fxinst.cpp and gives the same result bit for bit
 * (registers, flags, RAM, the cycle count of every slice, the cache state);
 * it only gets there faster. Written for this tree, with the instruction
 * semantics taken from fxinst.cpp (Snes9x licence); no GPL code is used.
 *
 * How it works
 *   Code is cut into blocks at the first byte it is entered at (program bank +
 *   the address the program counter holds). A block is a straight run of
 *   instructions ending in a branch / loop / jump / write to R15 (with its
 *   delay slot), STOP, CACHE, RPIX or a length limit. Each block becomes a short
 *   array of ops:
 *     - the prefixes (ALT1-3, TO, FROM, WITH) are folded into the op that
 *       consumes them: the op carries its source and destination register and
 *       the ALT variant is picked at decode time, so no prefix is executed at
 *       run time and the status register / source / destination pointers are not
 *       touched inside a block;
 *     - R15 is not advanced per op: ops that read it get a K_PC op in front,
 *       jumps load it, the block end sets it to base + length;
 *     - the cycle cost of the instruction fetches is a property of the block
 *       (which cache lines it touches and whether they are filled), worked out
 *       once per "epoch" (a run of slices with the same cost constants and cache
 *       state) and added in one step when the block is entered; the data
 *       access costs (RAM, ROM buffer, multiplies, plots) are static per op;
 *     - TO/FROM/WITH/ALT fusion, the pipe byte and the delay slot are kept
 *       exactly: the byte after a branch is fetched from the target, not from
 *       the delay slot.
 *   The run loop of the reference stops between two instructions as soon as the
 *   cycle budget is used up. A block is only run when its whole cost (up to the
 *   delay slot) still fits in what is left, so no stop can fall inside it; the
 *   last piece of a slice, and anything unusual, is run by the reference
 *   interpreter one instruction at a time, which also resumes cleanly when a
 *   slice ended between a prefix and its instruction.
 *
 *   Code in the game pak RAM (banks 70-73) is never cached: the reference
 *   interpreter runs it. The ROM is constant, so a block never goes stale; a
 *   cheat that patches the ROM calls fx_blocksReset().
 *
 *   The cache RAM of the chip (3100-32ff) only drives the timing model in this
 *   emulation (code is fetched from the program bank, not from the cache RAM),
 *   so CPU writes to it need no invalidation here.
 *
 *   FX_SHADOW (test builds only): every slice is run twice, by this
 *   interpreter and by the reference, from the same state; the whole chip state
 *   and the RAM must match afterwards.
 */

#include "snes9x.h"
#include "fxinst.h"
#include "fxemu.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int fx_fast = 1;

#if defined(FX_LIMIT) && defined(FX_FAST) && defined(FX_LINEFLAGS) && defined(__GNUC__) && \
    (!defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define FXF_ON 1
#endif

#ifndef FXF_ON

uint32 fx_run (uint32 n) { return fx_run_ref(n); }
void fx_blocksReset (void) {}

#else

// ---------------------------------------------------------------------------
// Op kinds. The order of the branch group (BRA..BVS) follows opcodes 05..0f.
#define FXK_LIST(X) \
	X(STOP) X(CACHE) X(LSR) X(ROL) X(ASR) X(DIV2) X(ROR) X(SEX) X(LOB) X(HIB) X(SWAP) X(NOT) X(MERGE) \
	X(FMULT) X(LMULT) \
	X(ADD) X(ADC) X(SUB) X(SBC) X(CMP) X(AND) X(BIC) X(OR) X(XOR) X(MULT) X(UMULT) \
	X(ADDI) X(ADCI) X(SUBI) X(ANDI) X(BICI) X(ORI) X(XORI) X(MULTI) X(UMULTI) \
	X(MOVE) X(MOVES) X(INC) X(DEC) X(LINK) X(IBT) X(IWT) \
	X(LDW) X(LDB) X(STW) X(STB) X(SBK) X(LMS) X(SMS) X(LM) X(SM) \
	X(GETB) X(GETBH) X(GETBL) X(GETBS) X(GETC) X(COLOR) X(CMODE) X(RAMB) X(ROMB) \
	X(PLOT) X(RPIX) X(READR14) X(PC) X(DFETCH) \
	X(BRA) X(BGE) X(BLT) X(BNE) X(BEQ) X(BPL) X(BMI) X(BCC) X(BCS) X(BVC) X(BVS) X(LOOP) X(JMP) X(LJMP) \
	X(END_SEQ) X(END_J)

enum
{
#define X(n) K_##n,
	FXK_LIST(X)
#undef X
	K_COUNT
};

// kind properties
enum { P_RS = 1, P_RN = 2, P_WD = 4, P_WN = 8, P_DOK = 16 };	// reads SREG, reads R[n], writes DREG, writes R[n], allowed in a delay slot
// data cost classes (cycles are the run constants s_K[])
enum { DC_NONE, DC_M1, DC_M2, DC_PLOT, DC_RPIX, DC_MULT, DC_FMULT, DC_N };

struct FxOp
{
	const void	*h;			// label of the handler
	uint8		d, s, n, a;	// DREG index, SREG index, operand register, extra bits
	uint16		imm, off;	// immediate; offset of R15 from the block base
	uint32		aux;
};

struct FxBlock
{
	FxBlock		*next;
	uint32		key;		// program bank << 16 | address of the first fetch (R15 at entry)
	uint32		epoch;		// the cost fields are valid for this epoch
	uint32		cost;		// cycles to add when the block is entered (fetches + data), steady state
	uint32		fit;		// cost up to the delay slot: the part the budget must cover
	uint16		pc;
	uint16		ps;			// prefix state at entry
	uint16		nfetch;		// bytes fetched by the instructions of the block (the delay slot's own fetch is dynamic)
	uint16		nops;
	uint16		ninstr;
	uint8		first;		// the pipe byte the block expects at entry
	uint8		slow;		// 1: run by the reference interpreter
	uint8		hasD;		// the block ends with a delay slot
	uint8		clsD;
	uint8		cnt[DC_N];
	FxOp		ops[1];
};

enum { MAXI = 24, MAXOPS = 100, HBITS = 12 };

static const void	*s_lab[K_COUNT];
static uint8		s_kflags[K_COUNT], s_kcls[K_COUNT];
static bool			s_ready;
static FxBlock		*s_tab[1 << HBITS];
static uint8		*s_arena;
static size_t		s_used, s_cap = 1536 * 1024;
static uint32		s_epoch = 1;
static uint32		s_K[DC_N];
// what the epoch was valid for
static uint32		s_lastMask = 0xffffffffu, s_lastCBR, s_lastLim, s_lastCache, s_lastK[DC_N];
#ifdef FX_STATS
static uint64		st_blocks, st_instr, st_slow1, st_tails, st_tailsteps, st_decodes, st_flush, st_cuts, st_why[4], st_dhist[256], st_thist[256], st_pipehist[256], st_slowops[256];
#endif

static void fxf_init (void)
{
	if (s_ready)
		return;
	memset(s_kflags, 0, sizeof s_kflags);
	memset(s_kcls, 0, sizeof s_kcls);
	// reads SREG (and writes DREG): the unary and ALU group
	static const uint8 un[] = { K_LSR, K_ROL, K_ASR, K_DIV2, K_ROR, K_SEX, K_LOB, K_HIB, K_SWAP, K_NOT, K_FMULT, K_LMULT,
	                            K_ADDI, K_ADCI, K_SUBI, K_ANDI, K_BICI, K_ORI, K_XORI, K_MULTI, K_UMULTI };
	for (unsigned i = 0; i < sizeof un; i++)
		s_kflags[un[i]] = P_RS | P_WD | P_DOK;
	static const uint8 al[] = { K_ADD, K_ADC, K_SUB, K_SBC, K_AND, K_BIC, K_OR, K_XOR, K_MULT, K_UMULT };
	for (unsigned i = 0; i < sizeof al; i++)
		s_kflags[al[i]] = P_RS | P_RN | P_WD | P_DOK;
	s_kflags[K_CMP] = P_RS | P_RN | P_DOK;
	s_kflags[K_MERGE] = P_WD | P_DOK;
	s_kflags[K_MOVE] = P_RS | P_WN;
	s_kflags[K_MOVES] = P_RN | P_WD;
	s_kflags[K_INC] = P_DOK; s_kflags[K_DEC] = P_DOK;
	s_kflags[K_IBT] = P_WN; s_kflags[K_IWT] = P_WN;
	s_kflags[K_LDW] = P_WD | P_DOK; s_kflags[K_LDB] = P_WD | P_DOK;
	s_kflags[K_STW] = P_RS | P_DOK; s_kflags[K_STB] = P_RS | P_DOK; s_kflags[K_SBK] = P_RS | P_DOK;
	s_kflags[K_LMS] = P_WN; s_kflags[K_SMS] = P_RN; s_kflags[K_LM] = P_WN; s_kflags[K_SM] = P_RN;
	s_kflags[K_GETB] = P_WD | P_DOK; s_kflags[K_GETBS] = P_WD | P_DOK;
	s_kflags[K_GETBH] = P_RS | P_WD | P_DOK; s_kflags[K_GETBL] = P_RS | P_WD | P_DOK;
	s_kflags[K_GETC] = P_DOK; s_kflags[K_COLOR] = P_RS | P_DOK; s_kflags[K_CMODE] = P_RS | P_DOK;
	s_kflags[K_RAMB] = P_RS | P_DOK; s_kflags[K_ROMB] = P_RS | P_DOK;
	s_kflags[K_PLOT] = P_DOK;
	s_kflags[K_RPIX] = P_WD;
	s_kflags[K_LINK] = 0;
	// jumps read operands: handled by the decoder (they are terminators)
	s_kflags[K_JMP] = P_RN; s_kflags[K_LJMP] = P_RN | P_RS;
	// data classes
	s_kcls[K_STB] = s_kcls[K_LDB] = s_kcls[K_GETB] = s_kcls[K_GETBH] = s_kcls[K_GETBL] = s_kcls[K_GETBS] = DC_M1;
	s_kcls[K_STW] = s_kcls[K_LDW] = s_kcls[K_SBK] = s_kcls[K_LMS] = s_kcls[K_SMS] = s_kcls[K_LM] = s_kcls[K_SM] = DC_M2;
	s_kcls[K_PLOT] = DC_PLOT; s_kcls[K_RPIX] = DC_RPIX;
	s_kcls[K_MULT] = s_kcls[K_UMULT] = s_kcls[K_MULTI] = s_kcls[K_UMULTI] = DC_MULT;
	s_kcls[K_FMULT] = s_kcls[K_LMULT] = DC_FMULT;
	s_ready = true;
}

void fx_blocksReset (void)
{
	memset(s_tab, 0, sizeof s_tab);
	s_used = 0;
	s_epoch++;
	s_lastMask = 0xffffffffu;
#ifdef FX_STATS
	st_flush++;
#endif
}

static uint32 fxf_exec (uint32 cyc, bool init);
#ifdef FX_SHADOW
static int sh_trace;
static uint64 sh_slices, sh_bad;
static void sh_state (const char *t)
{
	fprintf(stderr, "%s cyc %u rb %02x R15 %x pipe %02x sfr %04x sg %x z %x c %x ov %x S%d D%d |", t, GSU.vCycles, GSU.vRomBuffer, GSU.avReg[15], GSU.vPipe, GSU.vStatusReg, GSU.vSign, GSU.vZero, GSU.vCarry, GSU.vOverflow,
	    (int) (GSU.pvSreg - GSU.avReg), (int) (GSU.pvDreg - GSU.avReg));
	for (int i = 0; i < 15; i++) fprintf(stderr, " %x", GSU.avReg[i]);
	fprintf(stderr, "\n");
}
#endif

// ---------------------------------------------------------------------------
// Decoder

enum { T_NORMAL, T_PREFIX_ALT, T_PREFIX_WITH, T_PREFIX_TO, T_PREFIX_FROM, T_BRANCH, T_LOOP, T_JMP, T_LJMP, T_STOP, T_CACHE, T_NOP };

struct Ins
{
	int		kind, type, len;
	uint32	n, aux, imm;
};

static void classify (const uint8 *code, uint32 ia0, uint32 pos, uint32 alt, Ins &I)
{
#define CB(o) code[(ia0 + (o)) & 0xffff]
	uint32 opb = CB(pos);
	I.kind = -1; I.type = T_NORMAL; I.len = 1; I.n = opb & 15; I.aux = 0; I.imm = 0;
	int a1 = alt & 1;
	switch (opb >> 4)
	{
		case 0x0:
			if (opb == 0) { I.type = T_STOP; I.kind = K_STOP; }
			else if (opb == 1) I.type = T_NOP;
			else if (opb == 2) { I.type = T_CACHE; I.kind = K_CACHE; }
			else if (opb == 3) I.kind = K_LSR;
			else if (opb == 4) I.kind = K_ROL;
			else { I.type = T_BRANCH; I.kind = K_BRA + (int) (opb - 5); I.len = 2; I.aux = (uint32) SEX8(CB(pos + 1)); }
			break;
		case 0x1: I.type = T_PREFIX_TO; break;
		case 0x2: I.type = T_PREFIX_WITH; break;
		case 0x3:
			if (opb <= 0x3b) I.kind = a1 ? K_STB : K_STW;
			else if (opb == 0x3c) { I.type = T_LOOP; I.kind = K_LOOP; }
			else I.type = T_PREFIX_ALT;
			break;
		case 0x4:
			if (opb <= 0x4b) I.kind = a1 ? K_LDB : K_LDW;
			else if (opb == 0x4c) I.kind = a1 ? K_RPIX : K_PLOT;
			else if (opb == 0x4d) I.kind = K_SWAP;
			else if (opb == 0x4e) I.kind = a1 ? K_CMODE : K_COLOR;
			else I.kind = K_NOT;
			break;
		case 0x5: I.kind = (alt == 0) ? K_ADD : (alt == 1) ? K_ADC : (alt == 2) ? K_ADDI : K_ADCI; break;
		case 0x6: I.kind = (alt == 0) ? K_SUB : (alt == 1) ? K_SBC : (alt == 2) ? K_SUBI : K_CMP; break;
		case 0x7:
			if (opb == 0x70) I.kind = K_MERGE;
			else I.kind = (alt == 0) ? K_AND : (alt == 1) ? K_BIC : (alt == 2) ? K_ANDI : K_BICI;
			break;
		case 0x8: I.kind = (alt == 0) ? K_MULT : (alt == 1) ? K_UMULT : (alt == 2) ? K_MULTI : K_UMULTI; break;
		case 0x9:
			if (opb == 0x90) I.kind = K_SBK;
			else if (opb <= 0x94) { I.kind = K_LINK; I.aux = opb & 15; }
			else if (opb == 0x95) I.kind = K_SEX;
			else if (opb == 0x96) I.kind = a1 ? K_DIV2 : K_ASR;
			else if (opb == 0x97) I.kind = K_ROR;
			else if (opb <= 0x9d) { I.type = a1 ? T_LJMP : T_JMP; I.kind = a1 ? K_LJMP : K_JMP; }
			else if (opb == 0x9e) I.kind = K_LOB;
			else I.kind = a1 ? K_LMULT : K_FMULT;
			break;
		case 0xa:
			I.len = 2;
			if (alt == 0) { I.kind = K_IBT; I.aux = (uint32) SEX8(CB(pos + 1)); }
			else if (alt == 2) { I.kind = K_SMS; I.aux = ((uint32) CB(pos + 1)) << 1; }
			else { I.kind = K_LMS; I.aux = ((uint32) CB(pos + 1)) << 1; }
			break;
		case 0xb: I.type = T_PREFIX_FROM; break;
		case 0xc:
			if (opb == 0xc0) I.kind = K_HIB;
			else I.kind = (alt == 0) ? K_OR : (alt == 1) ? K_XOR : (alt == 2) ? K_ORI : K_XORI;
			break;
		case 0xd:
			if (opb <= 0xde) I.kind = K_INC;
			else I.kind = (alt <= 1) ? K_GETC : (alt == 2) ? K_RAMB : K_ROMB;
			break;
		case 0xe:
			if (opb <= 0xee) I.kind = K_DEC;
			else I.kind = (alt == 0) ? K_GETB : (alt == 1) ? K_GETBH : (alt == 2) ? K_GETBL : K_GETBS;
			break;
		default:
			I.len = 3;
			{
				uint32 w = ((uint32) CB(pos + 1)) | (((uint32) CB(pos + 2)) << 8);
				I.aux = w;
				I.kind = (alt == 0) ? K_IWT : (alt == 2) ? K_SM : K_LM;
			}
			break;
	}
	I.imm = I.n;	// the #n of the immediate forms
}

struct Dec
{
	FxOp	ops[MAXOPS];
	int		n;
	uint8	cnt[DC_N];
};

static void fxf_emit (Dec &D, int k, uint32 dd, uint32 ss, uint32 nn, uint32 aux, uint32 imm, uint32 off, uint32 a)
{
	FxOp &o = D.ops[D.n++];
	o.h = s_lab[k];
	o.d = (uint8) dd; o.s = (uint8) ss; o.n = (uint8) nn; o.a = (uint8) a;
	o.imm = (uint16) imm; o.off = (uint16) off; o.aux = aux;
	D.cnt[s_kcls[k]]++;
}

static bool fxf_isRam (const uint8 *p)
{
	return p >= GSU.pvRam && p < GSU.pvRam + (size_t) GSU.nRamBanks * 0x10000;
}

static inline uint32 fxf_hash (uint32 key, uint32 ps) { return ((key ^ (ps * 0x2545F491u)) * 0x9E3779B1u) >> (32 - HBITS); }

static inline uint32 fxf_pack (uint32 alt, uint32 b, uint32 s, uint32 d) { return alt | (b << 2) | (s << 3) | (d << 7); }

static FxBlock *fxf_store (const Dec &D, uint32 key, uint32 ps, uint32 pc, uint32 first, uint32 nfetch, uint32 ninstr, bool hasD, int clsD, bool slow)
{
	size_t sz = sizeof(FxBlock) + (D.n > 1 ? (D.n - 1) : 0) * sizeof(FxOp);
	sz = (sz + 15) & ~(size_t) 15;
	if (!s_arena)
		s_arena = (uint8 *) malloc(s_cap);
	if (!s_arena)
		return NULL;
	if (s_used + sz > s_cap)
		fx_blocksReset();
	FxBlock *b = (FxBlock *) (s_arena + s_used);
	s_used += sz;
	memset(b, 0, sizeof(FxBlock));
	b->key = key; b->ps = (uint16) ps; b->pc = (uint16) pc; b->first = (uint8) first; b->nfetch = (uint16) nfetch;
	b->nops = (uint16) D.n; b->ninstr = (uint16) ninstr; b->slow = slow ? 1 : 0; b->hasD = hasD ? 1 : 0; b->clsD = (uint8) clsD;
	memcpy(b->cnt, D.cnt, sizeof b->cnt);
	if (D.n)
		memcpy(b->ops, D.ops, D.n * sizeof(FxOp));
	uint32 h = fxf_hash(key, ps);
	b->next = s_tab[h];
	s_tab[h] = b;
	return b;
}

// Decode the block that starts with the pipe byte at pc - 1, entered with the prefix state ps
// (ALT bits, B, source and destination register; see fxf_pack).
static FxBlock *fxf_decode (uint32 pbr, uint32 pc, uint32 ps)
{
	const uint8 *code = GSU.apvRomBank[pbr & 0xff];
	uint32 key = (pbr << 16) | pc;
	uint32 ia0 = (pc - 1) & 0xffff;
	Dec D;
	D.n = 0;
	memset(D.cnt, 0, sizeof D.cnt);
#ifdef FX_STATS
	st_decodes++;
#endif
	if (fxf_isRam(code))
		return fxf_store(D, key, ps, pc, 0, 0, 0, false, 0, true);

	uint32 pos = 0, ninstr = 0, fetchLen = 0;
	uint32 alt = ps & 3, b = (ps >> 2) & 1, s = (ps >> 3) & 15, d = (ps >> 7) & 15;
	bool hasD = false, done = false, slow = false;
	int clsD = 0;

	// end the block before the instruction at pos (state as it is now), or make it a slow block when that is the start
	auto cut = [&]()
	{
		if (pos == 0) { slow = true; D.n = 0; memset(D.cnt, 0, sizeof D.cnt); }
		else { fxf_emit(D, K_END_SEQ, 0, 0, 0, pos, fxf_pack(alt, b, s, d), 0, 0); fetchLen = pos; }
		done = true;
	};
	// the R15 reader prelude
	auto needPC = [&](int k, uint32 ss, uint32 nn, uint32 posMain)
	{
		uint8 f = s_kflags[k];
		if (((f & P_RS) && ss == 15) || ((f & P_RN) && nn == 15))
			fxf_emit(D, K_PC, 0, 0, 0, 0, 0, posMain, 0);
	};
	// The delay slot instruction after a jump-like op, executed in prefix state st = (a2, b2, s2, d2).
	// false (and nothing emitted): the block has to end before the jump.
	auto delaySlot = [&](uint32 dpos, uint32 a2, uint32 b2, uint32 s2, uint32 d2) -> bool
	{
		Ins I;
		classify(code, ia0, dpos, a2, I);
#ifdef FX_STATS
		struct R_ { bool ok; const uint8 *c; uint32 p; ~R_() { if (!ok) st_dhist[c[p & 0xffff]]++; } } rj = { false, code, ia0 + dpos };
#endif
		uint32 nst = 0;
		int k = -1, nn = 0;
		uint32 dd = d2, ss = s2, aux = 0, imm = 0;
		bool r14 = false;
		switch (I.type)
		{
			case T_PREFIX_ALT: nst = fxf_pack(a2 | ((I.n == 0xd) ? 1 : (I.n == 0xe) ? 2 : 3), 0, s2, d2); break;
			case T_PREFIX_WITH: nst = fxf_pack(a2, 1, I.n, I.n); break;
			case T_PREFIX_TO:
				if (!b2) nst = fxf_pack(a2, 0, s2, I.n);
				else
				{
					if (I.n == 15 || s2 == 15) return false;
					k = K_MOVE; nn = I.n; r14 = (I.n == 14);
				}
				break;
			case T_PREFIX_FROM:
				if (!b2) nst = fxf_pack(a2, 0, I.n, d2);
				else
				{
					if (I.n == 15 || d2 == 15) return false;
					k = K_MOVES; nn = I.n; r14 = (d2 == 14);
				}
				break;
			case T_NOP: break;
			case T_NORMAL:
			{
				if (I.len != 1 || I.kind == K_RPIX || I.kind == K_LINK)
					return false;
				uint8 f = s_kflags[I.kind];
				if (!(f & P_DOK))
					return false;
				if (((f & P_RN) && I.n == 15) || ((f & P_RS) && s2 == 15) || ((f & P_WD) && d2 == 15))
					return false;
				k = I.kind; nn = I.n; aux = I.aux; imm = I.imm;
				r14 = ((f & P_WD) && d2 == 14) || ((k == K_INC || k == K_DEC) && I.n == 14);
				break;
			}
			default:
				return false;
		}
		fxf_emit(D, K_DFETCH, 0, 0, 0, 0, 0, 0, 0);
		if (k >= 0)
		{
			fxf_emit(D, k, dd, ss, nn, aux, imm, 0, 0);
			if (r14)
				fxf_emit(D, K_READR14, 0, 0, 0, 0, 0, 0, 0);
			clsD = s_kcls[k];
		}
		fxf_emit(D, K_END_J, 0, 0, 0, 0, nst, 0, 0);
#ifdef FX_STATS
		rj.ok = true;
#endif
		return true;
	};
	// A terminator op T with its delay slot. consumes: T clears the prefix state (every one but the branches).
	auto term = [&](int k, int len, uint32 dd, uint32 ss, uint32 nn, uint32 aux, uint32 imm, uint32 off, bool consumes) -> void
	{
		int n0 = D.n;
		uint8 c0[DC_N];
		memcpy(c0, D.cnt, sizeof c0);
		needPC(k, ss, nn, pos);
		fxf_emit(D, k, dd, ss, nn, aux, imm, off, 0);
		bool ok = consumes ? delaySlot(pos + len, 0, 0, 0, 0) : delaySlot(pos + len, alt, b, s, d);
		if (ok)
		{
			hasD = true;
			fetchLen = pos + len;
			pos += len + 1;
			done = true;
		}
		else
		{
			D.n = n0;
			memcpy(D.cnt, c0, sizeof c0);
			cut();
		}
	};
	auto clear = [&]() { alt = b = s = d = 0; };

	while (!done)
	{
		if (ninstr >= MAXI)
		{
			cut();
			break;
		}
		Ins I;
		classify(code, ia0, pos, alt, I);
		ninstr++;
		switch (I.type)
		{
			case T_PREFIX_ALT:
				alt |= (I.n == 0xd) ? 1 : (I.n == 0xe) ? 2 : 3;
				b = 0;
				pos += 1;
				continue;
			case T_PREFIX_WITH:
				b = 1; s = d = I.n;
				pos += 1;
				continue;
			case T_PREFIX_TO:
				if (!b)
				{
					d = I.n;
					pos += 1;
					continue;
				}
				// MOVE: R[n] = SREG (the prefix state is consumed)
				if (I.n == 15)
					term(K_MOVE, 1, 0, s, 15, 0, 0, 0, true);
				else
				{
					needPC(K_MOVE, s, I.n, pos);
					fxf_emit(D, K_MOVE, 0, s, I.n, 0, 0, 0, 0);
					if (I.n == 14)
						fxf_emit(D, K_READR14, 0, 0, 0, 0, 0, 0, 0);
					pos += 1;
					clear();
				}
				continue;
			case T_PREFIX_FROM:
				if (!b)
				{
					s = I.n;
					pos += 1;
					continue;
				}
				// MOVES: DREG = R[n], with flags
				if (d == 15)
					term(K_MOVES, 1, d, s, I.n, 0, 0, 0, true);
				else
				{
					needPC(K_MOVES, s, I.n, pos);
					fxf_emit(D, K_MOVES, d, s, I.n, 0, 0, 0, 0);
					if (d == 14)
						fxf_emit(D, K_READR14, 0, 0, 0, 0, 0, 0, 0);
					pos += 1;
					clear();
				}
				continue;
			case T_NOP:
				pos += 1;
				clear();
				continue;
			case T_STOP:
				fxf_emit(D, K_STOP, 0, 0, 0, pos + 1, 0, 0, 0);
				fetchLen = pos + 1;
				pos += 1;
				done = true;
				continue;
			case T_CACHE:
				fxf_emit(D, K_CACHE, 0, 0, 0, pos, 0, 0, 0);
				fxf_emit(D, K_END_SEQ, 0, 0, 0, pos + 1, 0, 0, 0);
				fetchLen = pos + 1;
				pos += 1;
				done = true;
				continue;
			case T_BRANCH:
				// taken: base + pos + 1 + offset; not taken: base + pos + 2. The branches leave the prefix state alone.
				term(I.kind, 2, 0, 0, 0, pos + 1 + I.aux, 0, pos + 2, false);
				continue;
			case T_LOOP:
				term(K_LOOP, 1, 0, 0, 0, pos + 1, 0, 0, true);
				continue;
			case T_JMP:
				term(K_JMP, 1, 0, s, I.n, 0, 0, 0, true);
				continue;
			case T_LJMP:
				term(K_LJMP, 1, 0, s, I.n, 0, 0, 0, true);
				continue;
			default:
				break;
		}
		// a normal op
		int k = I.kind;
		uint8 f = s_kflags[k];
		uint32 dd = d, ss = s;
		if (k == K_RPIX && dd == 15)
		{
			ninstr--;
			cut();
			continue;
		}
		if (k == K_RPIX)
		{
			// the reference leaves the prefix state alone when the pixel is off screen: imm is the state to continue with
			fxf_emit(D, K_RPIX, dd, ss, 0, 0, fxf_pack(alt, b, s, d), pos + 1, 0);
			if (dd == 14)
				fxf_emit(D, K_READR14, 0, 0, 0, 0, 0, 0, 0);
			fxf_emit(D, K_END_SEQ, 0, 0, 0, pos + 1, 0, 0, 0);
			fetchLen = pos + 1;
			pos += 1;
			done = true;
			continue;
		}
		bool wr15 = ((f & P_WD) && dd == 15) || ((f & P_WN) && I.n == 15);
		uint32 aux = I.aux;
		if (k == K_LINK)
			aux = pos + I.aux;	// R11 = R15 + n, R15 = base + pos here
		if (wr15)
		{
			term(k, I.len, dd, ss, I.n, aux, I.imm, 0, true);
			continue;
		}
		needPC(k, ss, I.n, pos);
		fxf_emit(D, k, dd, ss, I.n, aux, I.imm, 0, 0);
		if (((f & P_WD) && dd == 14) || ((k == K_INC || k == K_DEC || k == K_IBT || k == K_IWT || k == K_LMS || k == K_LM) && I.n == 14))
			fxf_emit(D, K_READR14, 0, 0, 0, 0, 0, 0, 0);
		pos += I.len;
		clear();
	}
	if (slow)
		return fxf_store(D, key, ps, pc, 0, 0, 0, false, 0, true);
	return fxf_store(D, key, ps, pc, code[ia0], fetchLen, ninstr, hasD, clsD, false);
}

// ---------------------------------------------------------------------------
// Costs

static void fxf_constants (void)
{
	uint32 cMem = GSU.vCostMem, cMult = GSU.vCostMult, cFmult = GSU.vCostFmult;
	s_K[DC_NONE] = 0;
	s_K[DC_M1] = cMem;
	s_K[DC_M2] = cMem << 1;
	s_K[DC_MULT] = cMult;
	s_K[DC_FMULT] = cFmult;
	switch (GSU.vMode)
	{
		case 0: s_K[DC_PLOT] = ((cMem << 1) >> 3) + 1; s_K[DC_RPIX] = cMem << 1; break;
		case 3: s_K[DC_PLOT] = cMem + 1; s_K[DC_RPIX] = cMem << 3; break;
		default: s_K[DC_PLOT] = ((cMem << 2) >> 3) + 1; s_K[DC_RPIX] = cMem << 2; break;
	}
}

// Cost of a block entered now: the cache line fills it triggers are applied to the line flags only
// when the block can run (it is entered only if the budget covers it up to the delay slot).
static bool fxf_prepare (FxBlock *b, uint32 cyc, uint32 *add)
{
	uint8 tmp[32];
	memcpy(tmp, fx_lineOk, 32);
	const uint32 cMem = GSU.vCostMem, cCache = GSU.vCostCache, cbr = GSU.vCacheBaseReg & 0xffff;
	uint32 fc = 0, fills = 0, a = b->pc, rem = b->nfetch;
	while (rem)
	{
		uint32 n = 16 - (a & 15);
		if (n > rem) n = rem;
		uint32 fca = (a - cbr) & 0xffff;
		if (fca < fx_cacheLimit)
		{
			uint32 li = fca >> 4;
			if (!tmp[li]) { tmp[li] = 1; fills++; }
			fc += n * cCache;
		}
		else
			fc += n * cMem;
		a = (a + n) & 0xffff;
		rem -= n;
	}
	uint32 dall = 0;
	for (int i = 1; i < DC_N; i++)
		dall += b->cnt[i] * s_K[i];
	uint32 dpre = dall - (b->hasD ? s_K[b->clsD] : 0);
	uint32 pen = fills * (cMem << 4);
	if (cyc + fc + pen + dpre >= fx_limit)
		return false;
	memcpy(fx_lineOk, tmp, 32);
	b->epoch = s_epoch;
	b->fit = fc + dpre;
	b->cost = fc + dall;
	*add = fc + pen + dall;
	return true;
}

// ---------------------------------------------------------------------------
// Execution

#define SZ(v) do { GSU.vSign = (v); GSU.vZero = (v); } while (0)
#define S		R[o->s]
#define Dst		R[o->d]
#define NR		R[o->n]
#ifdef FX_SHADOW
#define NEXT	do { if (sh_trace) { fprintf(stderr, "  op %d done: rb %02x sg %x z %x c %x ov %x |", (int) (o - blk->ops), GSU.vRomBuffer, GSU.vSign, GSU.vZero, GSU.vCarry, GSU.vOverflow); for (int i_ = 0; i_ < 15; i_++) fprintf(stderr, " %x", R[i_]); fprintf(stderr, "\n"); } ++o; goto *o->h; } while (0)
#else
#define NEXT	do { ++o; goto *o->h; } while (0)
#endif
#define TST_S	(GSU.vSign & 0x8000)
#define TST_Z	(USEX16(GSU.vZero) == 0)
#define TST_OV	(GSU.vOverflow >= 0x8000 || GSU.vOverflow < -0x8000)
#define TST_CY	(GSU.vCarry & 1)

static inline void fxf_pair (uint8 *p, uint32 vv, uint32 pm)
{
	uint16 w;
	memcpy(&w, p, 2);
	w = (uint16) ((w & ~vv) | (pm & vv));
	memcpy(p, &w, 2);
}
static const uint16 s_pm[4] = { 0x0000, 0x00ff, 0xff00, 0xffff };

#define PIPE_FROM_R15()	(GSU.vPipe = PRGBANK(R[15] - 1))

#define PS_MASK		(FLG_ALT1 | FLG_ALT2 | FLG_B)
static inline uint32 fxf_derive (void)
{
	return ((GSU.vStatusReg >> 8) & 3) | (((GSU.vStatusReg >> 12) & 1) << 2) | ((uint32) (GSU.pvSreg - GSU.avReg) << 3) | ((uint32) (GSU.pvDreg - GSU.avReg) << 7);
}
static inline void fxf_materialize (uint32 ps)
{
	GSU.vStatusReg = (GSU.vStatusReg & ~(uint32) PS_MASK) | ((ps & 3) << 8) | (((ps >> 2) & 1) << 12);
	GSU.pvSreg = &GSU.avReg[(ps >> 3) & 15];
	GSU.pvDreg = &GSU.avReg[(ps >> 7) & 15];
}

static uint32 fxf_exec (uint32 cyc, bool init)
{
#define X(n) &&L_##n,
	static const void *const labs[K_COUNT] = { FXK_LIST(X) };
#undef X
	if (init)
	{
		memcpy(s_lab, labs, sizeof labs);
		return 0;
	}

	uint32 *const R = GSU.avReg;
	const uint32 mode = GSU.vMode;
	const uint32 cMem = GSU.vCostMem, cCache = GSU.vCostCache;
	FxOp *o;
	FxBlock *blk;
	uint32 base, pc, key, pbr, add, ps;

	// the prefix state (ALT bits, B, source and destination register) lives in ps while blocks run
	ps = fxf_derive();

next:
	if (cyc >= fx_limit)
		goto done_m;
	pc = R[15] & 0xffff;
	pbr = GSU.vPrgBankReg;
	key = (pbr << 16) | pc;
	blk = s_tab[fxf_hash(key, ps)];
	while (blk && (blk->key != key || blk->ps != ps))
		blk = blk->next;
	if (!blk)
	{
		blk = fxf_decode(pbr, pc, ps);
		if (!blk)
			goto slow1;
	}
	if (blk->slow || GSU.vPipe != blk->first)
	{
#ifdef FX_STATS
		if (blk->slow) st_why[1]++; else { st_why[2]++; st_pipehist[GSU.vPipe]++; }
#endif
		goto slow1;
	}
	if (blk->epoch == s_epoch)
	{
#ifdef FX_SHADOW
		{
			FxBlock t_ = *blk; uint32 add_; uint8 lo_[32]; memcpy(lo_, fx_lineOk, 32);
			if (fxf_prepare(&t_, 0, &add_) && (add_ != blk->cost || t_.fit != blk->fit))
			{
				fprintf(stderr, "EPOCH COST blk %02x:%04x cached cost %u fit %u now %u fit %u cbr %04x lim %u cC %u cM %u slice %llu\n", (unsigned) pbr, (unsigned) blk->pc, blk->cost, blk->fit, add_, t_.fit, GSU.vCacheBaseReg, fx_cacheLimit, GSU.vCostCache, GSU.vCostMem, (unsigned long long) sh_slices);
				abort();
			}
			memcpy(fx_lineOk, lo_, 32);
		}
		{
			uint32 a_ = blk->pc, rem_ = blk->nfetch, cbr_ = GSU.vCacheBaseReg & 0xffff;
			while (rem_)
			{
				uint32 n_ = 16 - (a_ & 15); if (n_ > rem_) n_ = rem_;
				uint32 fca_ = (a_ - cbr_) & 0xffff;
				if (fca_ < fx_cacheLimit && !fx_lineOk[fca_ >> 4])
				{
					fprintf(stderr, "EPOCH STALE blk %02x:%04x line %u cbr %04x lim %u epoch %u slice %llu\n", (unsigned) pbr, (unsigned) blk->pc, fca_ >> 4, cbr_, fx_cacheLimit, s_epoch, (unsigned long long) sh_slices);
					abort();
				}
				a_ = (a_ + n_) & 0xffff; rem_ -= n_;
			}
		}
#endif
		if (cyc + blk->fit >= fx_limit)
			goto tail;
		add = blk->cost;
	}
	else if (!fxf_prepare(blk, cyc, &add))
		goto tail;
	cyc += add;
#ifdef FX_SHADOW
	if (sh_trace) { fprintf(stderr, "F blk %02x:%04x pipe %02x cyc %u -> %u nops %u ninstr %u R0-3 %x %x %x %x bytes", (unsigned) pbr, pc, GSU.vPipe, cyc - add, cyc, blk->nops, blk->ninstr, R[0], R[1], R[2], R[3]); for (int i = -1; i < 40; i++) fprintf(stderr, " %02x", PRGBANK(pc + i)); fprintf(stderr, "\n"); }
#endif
#ifdef FX_STATS
	st_blocks++; st_instr += blk->ninstr;
#endif
	base = R[15];
	o = blk->ops;
	goto *o->h;

slow1:
	GSU.vCycles = cyc;
	fxf_materialize(ps);
#ifdef FX_STATS
	st_slowops[GSU.vPipe]++;
#endif
#ifdef FX_SHADOW
	if (sh_trace) fprintf(stderr, "F slow R15 %x pipe %02x cyc %u sfr %04x\n", R[15], GSU.vPipe, cyc, GSU.vStatusReg);
#endif
	{
		uint32 cbr0 = GSU.vCacheBaseReg, lim0 = fx_cacheLimit;
		FX_STEP;
		// a reference CACHE / LJMP flushed the line flags: cached block costs assumed the old ones
		if (GSU.vCacheBaseReg != cbr0 || fx_cacheLimit != lim0)
			s_epoch++;
	}
	cyc = GSU.vCycles;
	ps = fxf_derive();
#ifdef FX_STATS
	st_slow1++;
#endif
	goto next;

tail:
	GSU.vCycles = cyc;
	fxf_materialize(ps);
#ifdef FX_STATS
	st_tails++;
	{ uint32 c0 = GSU.vCycles; (void) c0; }
#endif
	{
		uint32 cbr0 = GSU.vCacheBaseReg, lim0 = fx_cacheLimit;
		while (GSU.vCycles < fx_limit)
		{
#ifdef FX_SHADOW
			if (sh_trace) sh_state("F tail");
#endif
			FX_STEP;
#ifdef FX_STATS
			st_tailsteps++;
#endif
		}
		// a reference CACHE / LJMP in the tail flushed the line flags the cached costs assumed
		if (GSU.vCacheBaseReg != cbr0 || fx_cacheLimit != lim0)
			s_epoch++;
	}
	cyc = GSU.vCycles;
	goto done;

done_m:
	fxf_materialize(ps);
done:
	return cyc;

// ---- handlers -----------------------------------------------------------

L_END_SEQ:
	R[15] = base + o->aux;
	PIPE_FROM_R15();
	ps = o->imm;
	goto next;
L_END_J:
	R[15] += 1;
	PIPE_FROM_R15();
	ps = o->imm;
	goto next;

L_PC:
	R[15] = base + o->off;
	NEXT;
L_READR14:
	GSU.vRomBuffer = ROM(R[14]);
	NEXT;
L_DFETCH:
	{
		// the first fetch of the delay slot instruction: from the address in R15 (target or fall through)
		uint32 fca = USEX16(R[15] - GSU.vCacheBaseReg);
		if (fca < fx_cacheLimit)
		{
			uint32 li = fca >> 4;
			if (!fx_lineOk[li]) { fx_lineOk[li] = 1; cyc += cMem << 4; }
			cyc += cCache;
		}
		else
			cyc += cMem;
	}
	NEXT;

L_STOP:
	CF(G);
	fx_limit = 0;
	GSU.vCounter = 0;
	GSU.vInstCount = GSU.vCounter;
	if (!(GSU.pvRegisters[GSU_CFGR] & 0x80))
		SF(IRQ);
	GSU.vPlotOptionReg = 0;
	GSU.vPipe = 1;
	R[15] = base + o->aux;
	ps = 0;
	goto next;

L_CACHE:
	{
		uint32 c = (base + o->aux) & 0xfff0;
		if (GSU.vCacheBaseReg != c || !GSU.bCacheActive)
		{
			fx_flushCache();
			GSU.vCacheBaseReg = c;
			GSU.bCacheActive = TRUE;
			fx_cacheLimit = 512;
			s_epoch++;
		}
	}
	NEXT;

// unary group
L_LSR:  { uint32 sv = S; GSU.vCarry = sv & 1; uint32 v = USEX16(sv) >> 1; Dst = v; SZ(v); } NEXT;
L_ROL:  { uint32 sv = S; uint32 v = USEX16((sv << 1) + GSU.vCarry); GSU.vCarry = (sv >> 15) & 1; Dst = v; SZ(v); } NEXT;
L_ASR:  { uint32 sv = S; GSU.vCarry = sv & 1; uint32 v = (uint32) (SEX16(sv) >> 1); Dst = v; SZ(v); } NEXT;
L_DIV2: { uint32 sv = S; int32 s = SEX16(sv); GSU.vCarry = s & 1; uint32 v; if (s == -1) v = 0; else v = (uint32) (s >> 1); Dst = v; SZ(v); } NEXT;
L_ROR:  { uint32 sv = S; uint32 v = (USEX16(sv) >> 1) | (GSU.vCarry << 15); GSU.vCarry = sv & 1; Dst = v; SZ(v); } NEXT;
L_SEX:  { uint32 v = (uint32) SEX8(S); Dst = v; SZ(v); } NEXT;
L_LOB:  { uint32 v = USEX8(S); Dst = v; GSU.vSign = v << 8; GSU.vZero = v << 8; } NEXT;
L_HIB:  { uint32 v = USEX8(S >> 8); Dst = v; GSU.vSign = v << 8; GSU.vZero = v << 8; } NEXT;
L_SWAP: { uint8 c = (uint8) S; uint8 d = (uint8) (S >> 8); uint32 v = (((uint32) c) << 8) | ((uint32) d); Dst = v; SZ(v); } NEXT;
L_NOT:  { uint32 v = ~S; Dst = v; SZ(v); } NEXT;
L_MERGE:
	{
		uint32 v = (R[7] & 0xff00) | ((R[8] & 0xff00) >> 8);
		Dst = v;
		GSU.vOverflow = (v & 0xc0c0) << 16;
		GSU.vZero = !(v & 0xf0f0);
		GSU.vSign = ((v | (v << 8)) & 0x8000);
		GSU.vCarry = (v & 0xe0e0) != 0;
	}
	NEXT;
L_FMULT:
	{
		uint32 c = (uint32) (SEX16(S) * SEX16(R[6]));
		uint32 v = c >> 16;
		Dst = v;
		SZ(v);
		GSU.vCarry = (c >> 15) & 1;
	}
	NEXT;
L_LMULT:
	{
		uint32 c = (uint32) (SEX16(S) * SEX16(R[6]));
		R[4] = c;
		uint32 v = c >> 16;
		Dst = v;
		SZ(v);
		GSU.vCarry = (R[4] >> 15) & 1;
	}
	NEXT;

// register operand group
L_ADD:
	{
		uint32 sv = S, nv = NR;
		int32 s = SUSEX16(sv) + SUSEX16(nv);
		GSU.vCarry = s >= 0x10000;
		GSU.vOverflow = ~(sv ^ nv) & (nv ^ s) & 0x8000;
		SZ(s);
		Dst = s;
	}
	NEXT;
L_ADC:
	{
		uint32 sv = S, nv = NR;
		int32 s = SUSEX16(sv) + SUSEX16(nv) + SEX16(GSU.vCarry);
		GSU.vCarry = s >= 0x10000;
		GSU.vOverflow = ~(sv ^ nv) & (nv ^ s) & 0x8000;
		SZ(s);
		Dst = s;
	}
	NEXT;
L_SUB:
	{
		uint32 sv = S, nv = NR;
		int32 s = SUSEX16(sv) - SUSEX16(nv);
		GSU.vCarry = s >= 0;
		GSU.vOverflow = (sv ^ nv) & (sv ^ s) & 0x8000;
		SZ(s);
		Dst = s;
	}
	NEXT;
L_SBC:
	{
		uint32 sv = S, nv = NR;
		int32 s = SUSEX16(sv) - SUSEX16(nv) - (SUSEX16(GSU.vCarry ^ 1));
		GSU.vCarry = s >= 0;
		GSU.vOverflow = (sv ^ nv) & (sv ^ s) & 0x8000;
		SZ(s);
		Dst = s;
	}
	NEXT;
L_CMP:
	{
		uint32 sv = S, nv = NR;
		int32 s = SUSEX16(sv) - SUSEX16(nv);
		GSU.vCarry = s >= 0;
		GSU.vOverflow = (sv ^ nv) & (sv ^ s) & 0x8000;
		SZ(s);
	}
	NEXT;
L_AND:  { uint32 v = S & NR;  Dst = v; SZ(v); } NEXT;
L_BIC:  { uint32 v = S & ~NR; Dst = v; SZ(v); } NEXT;
L_OR:   { uint32 v = S | NR;  Dst = v; SZ(v); } NEXT;
L_XOR:  { uint32 v = S ^ NR;  Dst = v; SZ(v); } NEXT;
L_MULT:  { uint32 v = (uint32) (SEX8(S) * SEX8(NR)); Dst = v; SZ(v); } NEXT;
L_UMULT: { uint32 v = USEX8(S) * USEX8(NR); Dst = v; SZ(v); } NEXT;

// immediate group (#n in o->imm)
L_ADDI:
	{
		uint32 sv = S; int32 imm = o->imm;
		int32 s = SUSEX16(sv) + imm;
		GSU.vCarry = s >= 0x10000;
		GSU.vOverflow = ~(sv ^ imm) & (imm ^ s) & 0x8000;
		SZ(s);
		Dst = s;
	}
	NEXT;
L_ADCI:
	{
		uint32 sv = S; int32 imm = o->imm;
		int32 s = SUSEX16(sv) + imm + SEX16(GSU.vCarry);
		GSU.vCarry = s >= 0x10000;
		GSU.vOverflow = ~(sv ^ imm) & (imm ^ s) & 0x8000;
		SZ(s);
		Dst = s;
	}
	NEXT;
L_SUBI:
	{
		uint32 sv = S; int32 imm = o->imm;
		int32 s = SUSEX16(sv) - imm;
		GSU.vCarry = s >= 0;
		GSU.vOverflow = (sv ^ imm) & (sv ^ s) & 0x8000;
		SZ(s);
		Dst = s;
	}
	NEXT;
L_ANDI:  { uint32 v = S & o->imm;  Dst = v; SZ(v); } NEXT;
L_BICI:  { uint32 v = S & ~(uint32) o->imm; Dst = v; SZ(v); } NEXT;
L_ORI:   { uint32 v = S | o->imm;  Dst = v; SZ(v); } NEXT;
L_XORI:  { uint32 v = S ^ o->imm;  Dst = v; SZ(v); } NEXT;
L_MULTI:  { uint32 v = (uint32) (SEX8(S) * ((int32) o->imm)); Dst = v; SZ(v); } NEXT;
L_UMULTI: { uint32 v = USEX8(S) * ((uint32) o->imm); Dst = v; SZ(v); } NEXT;

L_MOVE:
	NR = S;
	NEXT;
L_MOVES:
	{
		uint32 v = NR;
		Dst = v;
		GSU.vOverflow = (v & 0x80) << 16;
		SZ(v);
	}
	NEXT;
L_INC:
	NR += 1;
	GSU.vSign = NR;
	GSU.vZero = NR;
	NEXT;
L_DEC:
	NR -= 1;
	GSU.vSign = NR;
	GSU.vZero = NR;
	NEXT;
L_LINK:
	R[11] = base + o->aux;
	NEXT;
L_IBT:
	NR = o->aux;
	NEXT;
L_IWT:
	NR = o->aux;
	NEXT;

// RAM access
L_LDW:
	{
		uint32 a = NR;
		GSU.vLastRamAdr = a;
		uint32 v = (uint32) RAM(a);
		v |= ((uint32) RAM(a ^ 1)) << 8;
		Dst = v;
	}
	NEXT;
L_LDB:
	{
		uint32 a = NR;
		GSU.vLastRamAdr = a;
		uint32 v = (uint32) RAM(a);
		Dst = v;
	}
	NEXT;
L_STW:
	{
		uint32 a = NR, sv = S;
		GSU.vLastRamAdr = a;
		RAM(a) = (uint8) sv;
		RAM(a ^ 1) = (uint8) (sv >> 8);
	}
	NEXT;
L_STB:
	{
		uint32 a = NR, sv = S;
		GSU.vLastRamAdr = a;
		RAM(a) = (uint8) sv;
	}
	NEXT;
L_SBK:
	{
		uint32 sv = S;
		RAM(GSU.vLastRamAdr) = (uint8) sv;
		RAM(GSU.vLastRamAdr ^ 1) = (uint8) (sv >> 8);
	}
	NEXT;
L_LMS:
	{
		uint32 a = o->aux;
		GSU.vLastRamAdr = a;
		uint32 v = (uint32) RAM(a);
		v |= ((uint32) RAM(a + 1)) << 8;
		NR = v;
	}
	NEXT;
L_SMS:
	{
		uint32 a = o->aux, v = NR;
		GSU.vLastRamAdr = a;
		RAM(a) = (uint8) v;
		RAM(a + 1) = (uint8) (v >> 8);
	}
	NEXT;
L_LM:
	{
		uint32 a = o->aux;
		GSU.vLastRamAdr = a;
		uint32 v = RAM(a);
		v |= USEX8(RAM(a ^ 1)) << 8;
		NR = v;
	}
	NEXT;
L_SM:
	{
		uint32 a = o->aux, v = NR;
		GSU.vLastRamAdr = a;
		RAM(a) = (uint8) v;
		RAM(a ^ 1) = (uint8) (v >> 8);
	}
	NEXT;

// ROM buffer
L_GETB:  Dst = (uint32) GSU.vRomBuffer; NEXT;
L_GETBH: Dst = USEX8(S) | (USEX8(GSU.vRomBuffer) << 8); NEXT;
L_GETBL: Dst = (S & 0xff00) | USEX8(GSU.vRomBuffer); NEXT;
L_GETBS: Dst = (uint32) SEX8(GSU.vRomBuffer); NEXT;

// colour, mode, banks
L_GETC:
	{
		uint8 c = GSU.vRomBuffer;
		if (GSU.vPlotOptionReg & PLOT_HIGHNIBBLE)
			c = (c & 0xf0) | (c >> 4);
		if (GSU.vPlotOptionReg & PLOT_FREEZEHIGH)
		{
			GSU.vColorReg &= 0xf0;
			GSU.vColorReg |= c & 0x0f;
		}
		else
			GSU.vColorReg = USEX8(c);
	}
	NEXT;
L_COLOR:
	{
		uint8 c = (uint8) S;
		if (GSU.vPlotOptionReg & PLOT_HIGHNIBBLE)
			c = (c & 0xf0) | (c >> 4);
		if (GSU.vPlotOptionReg & PLOT_FREEZEHIGH)
		{
			GSU.vColorReg &= 0xf0;
			GSU.vColorReg |= c & 0x0f;
		}
		else
			GSU.vColorReg = USEX8(c);
	}
	NEXT;
L_CMODE:
	GSU.vPlotOptionReg = S;
	if (GSU.vPlotOptionReg & PLOT_OBJECT)
		GSU.vScreenHeight = 256;
	else
		GSU.vScreenHeight = GSU.vScreenRealHeight;
	fx_computeScreenPointers();
	NEXT;
L_RAMB:
	GSU.vRamBankReg = S & (FX_RAM_BANKS - 1);
	GSU.pvRamBank = GSU.apvRamBank[GSU.vRamBankReg & 0x3];
	NEXT;
L_ROMB:
	GSU.vRomBankReg = USEX8(S) & 0x7f;
	GSU.pvRomBank = GSU.apvRomBank[GSU.vRomBankReg];
	NEXT;

// pixels
L_PLOT:
	{
		uint32 x = USEX8(R[1]);
		uint32 y = USEX8(R[2]);
		R[1]++;
		if (y >= GSU.vScreenHeight)
			NEXT;
		uint32 c;
		if (mode != 3)
		{
			if (!(GSU.vPlotOptionReg & PLOT_TRANSPARENT) && !(COLR & 0xf))
				NEXT;
			if (GSU.vPlotOptionReg & PLOT_DITHER)
				c = ((x ^ y) & 1) ? (uint8) (GSU.vColorReg >> 4) : (uint8) GSU.vColorReg;
			else
				c = (uint8) GSU.vColorReg;
		}
		else
		{
			c = (uint8) GSU.vColorReg;
			if (!(GSU.vPlotOptionReg & PLOT_TRANSPARENT))
			{
				if ((GSU.vPlotOptionReg & PLOT_FREEZEHIGH) && !(c & 0xf)) NEXT;
				if (!(GSU.vPlotOptionReg & PLOT_FREEZEHIGH) && !c) NEXT;
			}
		}
		uint8 *a = GSU.apvScreen[y >> 3] + GSU.x[x >> 3] + ((y & 7) << 1);
		uint32 v = 128 >> (x & 7);
		uint32 vv = v | (v << 8);
		fxf_pair(a, vv, s_pm[c & 3]);
		if (mode != 0)
		{
			fxf_pair(a + 0x10, vv, s_pm[(c >> 2) & 3]);
			if (mode == 3)
			{
				fxf_pair(a + 0x20, vv, s_pm[(c >> 4) & 3]);
				fxf_pair(a + 0x30, vv, s_pm[(c >> 6) & 3]);
			}
		}
	}
	NEXT;
L_RPIX:
	{
		uint32 x = USEX8(R[1]);
		uint32 y = USEX8(R[2]);
		if (y >= GSU.vScreenHeight)
		{
			// the reference leaves the prefix state alone here (no CLRFLAGS): carry on with it
			R[15] = base + o->off;
			PIPE_FROM_R15();
			ps = o->imm;
			goto next;
		}
		uint8 *a = GSU.apvScreen[y >> 3] + GSU.x[x >> 3] + ((y & 7) << 1);
		uint32 v = 128 >> (x & 7);
		uint32 r = 0;
		r |= ((uint32) ((a[0] & v) != 0)) << 0;
		r |= ((uint32) ((a[1] & v) != 0)) << 1;
		if (mode != 0)
		{
			r |= ((uint32) ((a[0x10] & v) != 0)) << 2;
			r |= ((uint32) ((a[0x11] & v) != 0)) << 3;
			if (mode == 3)
			{
				r |= ((uint32) ((a[0x20] & v) != 0)) << 4;
				r |= ((uint32) ((a[0x21] & v) != 0)) << 5;
				r |= ((uint32) ((a[0x30] & v) != 0)) << 6;
				r |= ((uint32) ((a[0x31] & v) != 0)) << 7;
				GSU.vZero = r;
			}
		}
		Dst = r;
	}
	NEXT;

// control flow: they set R15 (the delay slot op, DFETCH and END_J follow)
L_BRA: R[15] = base + o->aux; NEXT;
L_BGE: R[15] = ((TST_S != 0) == (TST_OV != 0)) ? base + o->aux : base + o->off; NEXT;
L_BLT: R[15] = ((TST_S != 0) != (TST_OV != 0)) ? base + o->aux : base + o->off; NEXT;
L_BNE: R[15] = (!TST_Z) ? base + o->aux : base + o->off; NEXT;
L_BEQ: R[15] = (TST_Z) ? base + o->aux : base + o->off; NEXT;
L_BPL: R[15] = (!TST_S) ? base + o->aux : base + o->off; NEXT;
L_BMI: R[15] = (TST_S) ? base + o->aux : base + o->off; NEXT;
L_BCC: R[15] = (!TST_CY) ? base + o->aux : base + o->off; NEXT;
L_BCS: R[15] = (TST_CY) ? base + o->aux : base + o->off; NEXT;
L_BVC: R[15] = (!TST_OV) ? base + o->aux : base + o->off; NEXT;
L_BVS: R[15] = (TST_OV) ? base + o->aux : base + o->off; NEXT;
L_LOOP:
	GSU.vSign = GSU.vZero = --R[12];
	if ((uint16) R[12] != 0)
		R[15] = R[13];
	else
		R[15] = base + o->aux;
	NEXT;
L_JMP:
	R[15] = NR;
	NEXT;
L_LJMP:
	{
		GSU.vPrgBankReg = NR & 0x7f;
		GSU.pvPrgBank = GSU.apvRomBank[GSU.vPrgBankReg];
		uint32 t = S;
		uint32 c = t & 0xfff0;
		fx_flushCache();
		GSU.vCacheBaseReg = c;
		GSU.bCacheActive = TRUE;
		fx_cacheLimit = 512;
		s_epoch++;
		R[15] = t;
	}
	NEXT;
}

// ---------------------------------------------------------------------------

#ifdef FX_STATS
static void fxf_dumpStats (void)
{
	fprintf(stderr, "fx_stats: blocks %llu instr %llu slow1 %llu (unclean %llu slowblk %llu pipemismatch %llu) tails %llu tailsteps %llu decodes %llu flushes %llu\n",
	    (unsigned long long) st_blocks, (unsigned long long) st_instr, (unsigned long long) st_slow1, (unsigned long long) st_why[0], (unsigned long long) st_why[1],
	    (unsigned long long) st_why[2], (unsigned long long) st_tails, (unsigned long long) st_tailsteps, (unsigned long long) st_decodes, (unsigned long long) st_flush);
	const char *nm[3] = { "delay-slot rejected (opcode)", "pipe mismatch (pipe)", "slow step (pipe byte executed)" };
	uint64 *h[3] = { st_dhist, st_pipehist, st_slowops };
	for (int k = 0; k < 3; k++)
	{
		fprintf(stderr, "  %s:", nm[k]);
		for (int r = 0; r < 12; r++)
		{
			int best = -1;
			for (int i = 0; i < 256; i++)
				if (h[k][i] && (best < 0 || h[k][i] > h[k][best])) best = i;
			if (best < 0) break;
			fprintf(stderr, " %02x:%llu", best, (unsigned long long) h[k][best]);
			h[k][best] = 0;
		}
		fprintf(stderr, "\n");
	}
}
#endif

static uint32 fxf_run (uint32 nInstructions)
{
	if (!s_ready)
	{
		fxf_init();
		fxf_exec(0, true);
	}
#ifdef FX_STATS
	static bool sreg;
	if (!sreg) { sreg = true; atexit(fxf_dumpStats); }
#endif
	fx_cacheLimit = GSU.bCacheActive ? 512 : 0;
	for (int i = 0; i < 32; i++)
		fx_lineOk[i] = (GSU.vCacheMask >> i) & 1;
	fxf_constants();
	// a new epoch when anything the cached costs depend on is not what the last run left
	bool same = GSU.vCacheMask == s_lastMask && GSU.vCacheBaseReg == s_lastCBR && fx_cacheLimit == s_lastLim;
	for (int i = 0; i < DC_N && same; i++)
		same = s_K[i] == s_lastK[i];
	same = same && GSU.vCostCache == s_lastCache && GSU.vCostMem == s_lastK[DC_M1];
	if (!same)
		s_epoch++;
	GSU.vCycles = 0;
	fx_limit = TF(G) ? nInstructions : 0;
	uint32 cyc = fxf_exec(0, false);
	GSU.vCycles = cyc;
	uint32 m = 0;
	for (int i = 0; i < 32; i++)
		m |= (uint32) fx_lineOk[i] << i;
	GSU.vCacheMask = m;
	s_lastMask = m; s_lastCBR = GSU.vCacheBaseReg; s_lastLim = fx_cacheLimit;
	for (int i = 0; i < DC_N; i++)
		s_lastK[i] = s_K[i];
	s_lastCache = GSU.vCostCache;
	return 0;
}

#ifdef FX_SHADOW
#include <vector>
static void sh_dump (void)
{
	fprintf(stderr, "fx_shadow: %llu slices compared, %llu mismatches\n", (unsigned long long) sh_slices, (unsigned long long) sh_bad);
#ifdef FX_STATS
	fprintf(stderr, "fx_stats: blocks %llu instr %llu slow1 %llu tails %llu tailsteps %llu decodes %llu flushes %llu\n",
	    (unsigned long long) st_blocks, (unsigned long long) st_instr, (unsigned long long) st_slow1, (unsigned long long) st_tails,
	    (unsigned long long) st_tailsteps, (unsigned long long) st_decodes, (unsigned long long) st_flush);
#endif
}
struct ShSnap { std::vector<uint8> g, ram; uint8 lo[32]; uint32 cl, lim; };
static void sh_take (ShSnap &s)
{
	s.g.assign((uint8 *) &GSU, (uint8 *) &GSU + sizeof GSU);
	s.ram.assign(GSU.pvRam, GSU.pvRam + (size_t) GSU.nRamBanks * 0x10000);
	memcpy(s.lo, fx_lineOk, 32);
	s.cl = fx_cacheLimit; s.lim = fx_limit;
}
static void sh_put (const ShSnap &s)
{
	memcpy((uint8 *) &GSU, s.g.data(), sizeof GSU);
	memcpy(GSU.pvRam, s.ram.data(), s.ram.size());
	memcpy(fx_lineOk, s.lo, 32);
	fx_cacheLimit = s.cl; fx_limit = s.lim;
}
uint32 fx_run (uint32 n)
{
	if (!(fx_fast && GSU.bCycleMode && (GSU.vCacheBaseReg & 15) == 0))
		return fx_run_ref(n);
	static bool reg;
	if (!reg) { reg = true; atexit(sh_dump); }
	ShSnap s0, s1;
	sh_take(s0);
	uint32 r = fxf_run(n);
	sh_take(s1);
	sh_put(s0);
	fx_run_ref(n);
	ShSnap s2;
	sh_take(s2);
	sh_slices++;
	if (s1.g != s2.g || s1.ram != s2.ram || memcmp(s1.lo, s2.lo, 32) || s1.cl != s2.cl || s1.lim != s2.lim)
	{
		sh_bad++;
		// find the smallest budget that already differs, and report the state it starts from
		{
			uint32 lo = 1;
			for (; lo <= n; lo++)
			{
				sh_put(s0); s_epoch++;
				fxf_run(lo);
				ShSnap f; sh_take(f);
				sh_put(s0);
				fx_run_ref(lo);
				ShSnap r; sh_take(r);
				if (f.g != r.g || f.ram != r.ram || memcmp(f.lo, r.lo, 32))
				{
					fprintf(stderr, "fx_shadow: smallest differing budget %u (slice start: R15=%04x PBR=%02x pipe=%02x sfr=%04x cyc-limit cache base %04x lim %u)\n",
					    lo, (unsigned) (((const FxRegs_s *) s0.g.data())->avReg[15] & 0xffff), (unsigned) ((const FxRegs_s *) s0.g.data())->vPrgBankReg,
					    (unsigned) ((const FxRegs_s *) s0.g.data())->vPipe, (unsigned) ((const FxRegs_s *) s0.g.data())->vStatusReg,
					    (unsigned) ((const FxRegs_s *) s0.g.data())->vCacheBaseReg, lo);
					const FxRegs_s *F = (const FxRegs_s *) f.g.data(), *Rr = (const FxRegs_s *) r.g.data();
					for (int i = 0; i < 16; i++)
						if (F->avReg[i] != Rr->avReg[i]) fprintf(stderr, "   R%d fast %x ref %x\n", i, F->avReg[i], Rr->avReg[i]);
#define CMPF(x) if (F->x != Rr->x) fprintf(stderr, "   " #x " fast %x ref %x\n", (unsigned) F->x, (unsigned) Rr->x)
					CMPF(vCycles); CMPF(vStatusReg); CMPF(vSign); CMPF(vZero); CMPF(vCarry); CMPF(vOverflow); CMPF(vPipe); CMPF(vRomBuffer);
					CMPF(vColorReg); CMPF(vPlotOptionReg); CMPF(vLastRamAdr); CMPF(vCacheMask); CMPF(vCacheBaseReg); CMPF(vCounter);
					CMPF(vRomBankReg); CMPF(vRamBankReg); CMPF(vPrgBankReg);
					if ((const uint8 *) F->pvSreg - (const uint8 *) F->avReg != (const uint8 *) Rr->pvSreg - (const uint8 *) Rr->avReg) fprintf(stderr, "   pvSreg differs\n");
					if ((const uint8 *) F->pvDreg - (const uint8 *) F->avReg != (const uint8 *) Rr->pvDreg - (const uint8 *) Rr->avReg) fprintf(stderr, "   pvDreg differs\n");
					// traces of both at the smallest differing budget
					sh_put(s0); s_epoch++;
					sh_trace = 1;
					fprintf(stderr, "--- fast\n");
					fxf_run(lo);
					fprintf(stderr, "--- ref\n");
					sh_put(s0);
					fx_cacheLimit = GSU.bCacheActive ? 512 : 0;
					for (int i = 0; i < 32; i++) fx_lineOk[i] = (GSU.vCacheMask >> i) & 1;
					GSU.vCycles = 0; fx_limit = TF(G) ? lo : 0;
					while (GSU.vCycles < fx_limit)
					{
						sh_state("R step");
						FX_STEP;
					}
					sh_trace = 0;
					break;
				}
			}
			sh_put(s0);
			fprintf(stderr, "  slice-start regs:");
			for (int i = 0; i < 16; i++) fprintf(stderr, " %x", ((const FxRegs_s *) s0.g.data())->avReg[i]);
			fprintf(stderr, "\n");
		}
		if (sh_bad <= 5)
		{
			fprintf(stderr, "fx_shadow: MISMATCH in slice %llu (budget %u)\n", (unsigned long long) sh_slices, n);
			for (size_t i = 0; i < s1.g.size(); i++)
				if (s1.g[i] != s2.g[i]) { fprintf(stderr, "  GSU byte %zu: fast %02x ref %02x\n", i, s1.g[i], s2.g[i]); if (i > 20 && sh_bad > 1) break; }
			for (size_t i = 0, c = 0; i < s1.ram.size() && c < 8; i++)
				if (s1.ram[i] != s2.ram[i]) { fprintf(stderr, "  RAM %zx: fast %02x ref %02x\n", i, s1.ram[i], s2.ram[i]); c++; }
		}
		abort();
	}
	return r;
}
#else
uint32 fx_run (uint32 n)
{
	if (fx_fast && GSU.bCycleMode && (GSU.vCacheBaseReg & 15) == 0)
		return fxf_run(n);
	return fx_run_ref(n);
}
#endif

#endif
