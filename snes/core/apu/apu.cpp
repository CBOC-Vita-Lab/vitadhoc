/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <cmath>
#include <vector>
#include "../snes9x.h"
#include "apu.h"
#include "../msu1.h"
#include "../snapshot.h"
#include "../display.h"
#include "resampler.h"
#include "../s9x_prof.h"
#include "../s9x_mt.h"

#include "bapu/snes/snes.hpp"

static const int APU_DEFAULT_INPUT_RATE = 31950; // ~59.94Hz
static const int APU_SAMPLE_BLOCK       = 48;
static const int APU_NUMERATOR_NTSC     = 15664;
static const int APU_DENOMINATOR_NTSC   = 328125;
static const int APU_NUMERATOR_PAL      = 34176;
static const int APU_DENOMINATOR_PAL    = 709379;

// Max number of samples we'll ever generate before call to port API and
// moving the samples to the resampler.
// This is 535 sample frames, which corresponds to 1 video frame + some leeway
// for use with SoundSync, multiplied by 2, for left and right samples.
static const int MINIMUM_BUFFER_SIZE = 550 * 2;

namespace SNES {
#include "bapu/dsp/blargg_endian.h"
CPU cpu;
} // namespace SNES

namespace spc {
static apu_callback callback = NULL;
static void *callback_data = NULL;

static bool8 sound_in_sync = true;
static bool8 sound_enabled = false;

static Resampler resampler;

static int32 reference_time;
static uint32 remainder;

static const int timing_hack_numerator = 256;
static int timing_hack_denominator = 256;
/* Set these to NTSC for now. Will change to PAL in S9xAPUTimingSetSpeedup
   if necessary on game load. */
static uint32 ratio_numerator = APU_NUMERATOR_NTSC;
static uint32 ratio_denominator = APU_DENOMINATOR_NTSC;

static double dynamic_rate_multiplier = 1.0;
} // namespace spc

namespace msu {
// Always 16-bit, Stereo; 1.5x dsp buffer to never overflow
static Resampler resampler;
static std::vector<int16_t> resampler_buffer;
} // namespace msu

static void UpdatePlaybackRate(void);
static void SPCSnapshotCallback(void);
static inline int S9xAPUGetClock(int32);
static inline int S9xAPUGetClockRemainder(int32);

/* ---------------------------------------------------------------------------
 * Optional APU thread (CBOC-Vita addition, see snes/README.md "APU thread").
 *
 * Snes9x already runs the SPC700 lazily: the 65816 side only ever asks "run the
 * APU up to CPU.Cycles" (S9xAPUExecute), "write port" and "read port", and ends
 * every scanline with a DSP catch-up. This keeps exactly that contract, but the
 * requests become a command stream consumed in order by a worker thread, so the
 * worker performs bit-for-bit the same operations the single-thread build does.
 * Only a port read has to wait (for the worker to reach that point); writes and
 * time slices are fire-and-forget. Time arithmetic (the 15664/328125 ratio and
 * its remainder) stays on the 65816 side, so the cycle counts are identical.
 * ------------------------------------------------------------------------- */
namespace apumt {
enum { OP_RUN, OP_WRITE, OP_ENDSCAN, OP_JOB };
struct Cmd { int32 op; int32 a; uintptr_t b, c; };
static const unsigned RING = 8192, MASK = RING - 1, PUBLISH_EVERY = 24, CHUNK = 256;
static Cmd ring[RING];
static unsigned prod, pub;               /* 65816 side: next slot, last published slot */
static std::atomic<unsigned> head;       /* published (free running counter) */
static std::atomic<unsigned> tail;       /* consumed; changed only while xlock is held */
static std::atomic<int> xlock;           /* whoever holds it may run the APU */
static std::atomic<int> want;            /* the 65816 side asks the worker to let go of the lock */
static std::atomic<int> sleeping, quit;
static s9xmt::Sem wake;
static s9xmt::Thread thr;
static bool on, started;
static std::atomic<uint64_t> busy_ns, cmds, helped, lockwaits, lockwait_ns;
static int spin_idle = 3000;
static int core_hint = -1, prio_hint = 0x40;

static inline bool try_lock(void)
{
	int z = 0;
	return xlock.compare_exchange_strong(z, 1, std::memory_order_acquire);
}
static inline void unlock(void) { xlock.store(0, std::memory_order_release); }

static inline void publish(void)
{
	if (pub == prod) return;
	pub = prod;
	head.store(prod, std::memory_order_seq_cst);
	if (sleeping.load(std::memory_order_seq_cst)) wake.post();
}

static void exec(const Cmd &c)
{
	switch (c.op) {
	case OP_RUN:
		SNES::smp.clock -= c.a;
		SNES::smp.enter();
		break;
	case OP_WRITE:
		SNES::cpu.port_write(c.a >> 8, c.a & 0xff);
		break;
	case OP_ENDSCAN:
		SNES::dsp.synchronize();
		if (spc::resampler.space_filled() >= APU_SAMPLE_BLOCK)
			S9xLandSamples();
		break;
	case OP_JOB:
		((void (*)(void *)) c.b)((void *) c.c);
		break;
	}
}

/* The 65816 side needs the APU state "as of now" (a port read, a state save,
 * a full ring): it takes the lock as soon as the worker finishes its current
 * chunk and runs everything still queued itself. That is never slower than the
 * single-thread build, and it avoids a thread hand-over for every poll of a
 * port. */
static void help_drain(void)
{
	publish();
	if (tail.load(std::memory_order_acquire) == prod) return;
	uint64_t t0 = 0;
	want.store(1, std::memory_order_seq_cst);
	if (!try_lock()) {
		t0 = s9xmt::now_ns();
		int i = 0;
		while (!try_lock()) { if (++i < 20000) s9xmt::cpu_relax(); else s9xmt::yield_cpu(); }
		lockwaits.fetch_add(1, std::memory_order_relaxed);
		lockwait_ns.fetch_add(s9xmt::now_ns() - t0, std::memory_order_relaxed);
	}
	unsigned t = tail.load(std::memory_order_relaxed), n = 0;
	while (t != prod) { exec(ring[t & MASK]); t++; n++; }
	tail.store(t, std::memory_order_release);
	unlock();
	want.store(0, std::memory_order_seq_cst);
	helped.fetch_add(n, std::memory_order_relaxed);
}

static inline void push(const Cmd &c)
{
	while (prod - tail.load(std::memory_order_acquire) >= RING) help_drain();
	ring[prod & MASK] = c;
	prod++;
	if (prod - pub >= PUBLISH_EVERY || c.op == OP_ENDSCAN) publish();
}

static void thread_main(void *)
{
	for (;;) {
		if (head.load(std::memory_order_acquire) != tail.load(std::memory_order_acquire)) {
			if (want.load(std::memory_order_acquire) || !try_lock()) { s9xmt::cpu_relax(); continue; }
			unsigned h = head.load(std::memory_order_acquire), t = tail.load(std::memory_order_relaxed), n = 0;
			uint64_t t0 = s9xmt::now_ns();
			while (t != h && n < CHUNK) {
				bool endscan = ring[t & MASK].op == OP_ENDSCAN;
				exec(ring[t & MASK]);
				t++; n++;
				if (endscan) break;
			}
			tail.store(t, std::memory_order_release);
			unlock();
			cmds.fetch_add(n, std::memory_order_relaxed);
			busy_ns.fetch_add(s9xmt::now_ns() - t0, std::memory_order_relaxed);
			continue;
		}
		int i = 0;
		for (; i < spin_idle; i++) {
			if (head.load(std::memory_order_acquire) != tail.load(std::memory_order_acquire)) break;
			s9xmt::cpu_relax();
		}
		if (i < spin_idle) continue;
		sleeping.store(1, std::memory_order_seq_cst);
		if (head.load(std::memory_order_seq_cst) != tail.load(std::memory_order_seq_cst)) { sleeping.store(0); continue; }
		if (quit.load()) { sleeping.store(0); break; }
		wake.wait();
		sleeping.store(0);
	}
}
} // namespace apumt

void S9xAPUMTSync(void)
{
	if (apumt::on) apumt::help_drain();
}

bool S9xAPUMTActive(void) { return apumt::on; }

void S9xAPUMTPost(void (*fn)(void *), void *arg)
{
	apumt::Cmd c = { apumt::OP_JOB, 0, (uintptr_t) fn, (uintptr_t) arg };
	apumt::push(c);
	apumt::publish();
}

void S9xAPUMTSetSpin(int n) { apumt::spin_idle = n; }

void S9xAPUMTConfig(int core, int prio)
{
	apumt::core_hint = core;
	apumt::prio_hint = prio;
}

bool S9xAPUMTSet(bool enable)
{
	using namespace apumt;
	if (enable == on) return on;
	if (enable) {
		if (Settings.MSU1) return false; /* MSU-1 audio is generated from the DSP callback and read by the 65816 */
		if (!started) { wake.init(); started = true; }
		prod = pub = 0;
		head.store(0); tail.store(0); sleeping.store(0); want.store(0); xlock.store(0); quit.store(0);
		busy_ns = 0; cmds = 0; helped = 0; lockwaits = 0; lockwait_ns = 0;
		if (const char *e = getenv("SNES_MT_SPIN")) spin_idle = atoi(e);
		if (!thr.start(thread_main, 0, "snes_apu", core_hint, prio_hint)) return false;
		on = true;
	} else {
		S9xAPUMTSync();
		quit.store(1);
		publish();
		wake.post();
		thr.join();
		on = false;
	}
	return on;
}

void S9xAPUMTStats(uint64_t *busy, uint64_t *cmds, uint64_t *helped, uint64_t *lockwaits, uint64_t *lockwait_ns)
{
	*busy = apumt::busy_ns.load(); *cmds = apumt::cmds.load(); *helped = apumt::helped.load();
	*lockwaits = apumt::lockwaits.load(); *lockwait_ns = apumt::lockwait_ns.load();
}

bool8 S9xMixSamples(uint8 *dest, int sample_count)
{
    int16 *out = (int16 *)dest;

    if (Settings.Mute)
    {
        memset(out, 0, sample_count << 1);
        S9xClearSamples();
        spc::sound_in_sync = true;
        return true;
    }

    if (spc::resampler.avail() < sample_count)
    {
        memset(out, 0, sample_count << 1);
        return false;
    }

    spc::resampler.read((short *)out, sample_count);

    if (Settings.MSU1)
    {
        if ((int)msu::resampler_buffer.size() < sample_count)
            msu::resampler_buffer.resize(sample_count);

        msu::resampler.read(msu::resampler_buffer.data(), sample_count);
        for (int i = 0; i < sample_count; ++i)
        {
            int32 mixed = (int32)out[i] + msu::resampler_buffer[i];
            out[i] = ((int16)mixed != mixed) ? (mixed >> 31) ^ 0x7fff : mixed;
        }
    }

    if (spc::resampler.space_empty() >= 535 * 2 || !Settings.SoundSync ||
        Settings.TurboMode || Settings.Mute)
        spc::sound_in_sync = true;
    else
        spc::sound_in_sync = false;

    return true;
}

int S9xGetSampleCount(void)
{
	int avail = spc::resampler.avail();
	if (Settings.MSU1) // return minimum available samples, otherwise we can run into the assert above due to partial sample generation in msu1
		avail = Resampler::min(avail, msu::resampler.avail());
    return avail;
}

void S9xLandSamples(void)
{
    if (spc::callback != NULL)
        spc::callback(spc::callback_data);

    if (spc::resampler.space_empty() >= 535 * 2 || !Settings.SoundSync ||
        Settings.TurboMode || Settings.Mute)
        spc::sound_in_sync = true;
    else
        spc::sound_in_sync = false;
}

void S9xClearSamples(void)
{
    spc::resampler.clear();
    if (Settings.MSU1)
        msu::resampler.clear();
}

bool8 S9xSyncSound(void)
{
    if (!Settings.SoundSync || spc::sound_in_sync)
        return true;

    S9xLandSamples();

    return (spc::sound_in_sync);
}

void S9xSetSamplesAvailableCallback(apu_callback callback, void *data)
{
    spc::callback = callback;
    spc::callback_data = data;
}

void S9xUpdateDynamicRate(int avail, int buffer_size)
{
    S9xAPUMTSync();
    spc::dynamic_rate_multiplier = 1.0 + (Settings.DynamicRateLimit * (buffer_size - 2 * avail)) /
                                             (double)(1000 * buffer_size);

    UpdatePlaybackRate();
}

static void UpdatePlaybackRate(void)
{
    if (Settings.SoundInputRate == 0)
        Settings.SoundInputRate = APU_DEFAULT_INPUT_RATE;

    double time_ratio = (double)Settings.SoundInputRate * spc::timing_hack_numerator / (Settings.SoundPlaybackRate * spc::timing_hack_denominator);

    if (Settings.DynamicRateControl)
    {
        time_ratio *= spc::dynamic_rate_multiplier;
    }

    spc::resampler.time_ratio(time_ratio);

    if (Settings.MSU1)
    {
        time_ratio = time_ratio * 44100 / 32040;
        msu::resampler.time_ratio(time_ratio);
    }
}

bool8 S9xInitSound(int buffer_ms)
{
    S9xAPUMTSync();
    // The resampler and spc unit use samples (16-bit short) as arguments.
    int buffer_size_samples = MINIMUM_BUFFER_SIZE;
    int requested_buffer_size_samples = Settings.SoundPlaybackRate * buffer_ms * 2 / 1000;

    if (requested_buffer_size_samples > buffer_size_samples)
        buffer_size_samples = requested_buffer_size_samples;

    spc::resampler.resize(buffer_size_samples);
    msu::resampler.resize(buffer_size_samples * 3 / 2);

    SNES::dsp.spc_dsp.set_output(&spc::resampler);
    S9xMSU1SetOutput(&msu::resampler);

    UpdatePlaybackRate();

    spc::sound_enabled = S9xOpenSoundDevice();

    return (spc::sound_enabled);
}

void S9xSetSoundControl(uint8 voice_switch)
{
    S9xAPUMTSync();
    SNES::dsp.spc_dsp.set_stereo_switch(voice_switch << 8 | voice_switch);
}

void S9xSetSoundMute(bool8 mute)
{
    Settings.Mute = mute;
    if (!spc::sound_enabled)
        Settings.Mute = true;
}

void S9xDumpSPCSnapshot(void)
{
    S9xAPUMTSync();
    SNES::dsp.spc_dsp.dump_spc_snapshot();
}

static void SPCSnapshotCallback(void)
{
    S9xSPCDump(S9xGetFilenameInc((".spc"), SPC_DIR).c_str());
    printf("Dumped key-on triggered spc snapshot.\n");
}

bool8 S9xInitAPU(void)
{
    S9xAPUMTSync();
    spc::resampler.clear();
    msu::resampler.clear();

    return true;
}

void S9xDeinitAPU(void)
{
    S9xAPUMTSync();
    S9xMSU1DeInit();
    msu::resampler_buffer.clear();
}

static inline int S9xAPUGetClock(int32 cpucycles)
{
    return (spc::ratio_numerator * (cpucycles - spc::reference_time) + spc::remainder) /
           spc::ratio_denominator;
}

static inline int S9xAPUGetClockRemainder(int32 cpucycles)
{
    return (spc::ratio_numerator * (cpucycles - spc::reference_time) + spc::remainder) %
           spc::ratio_denominator;
}

uint8 S9xAPUReadPort(int port)
{
    S9XP_COUNT(APU_PORT);
    S9xAPUExecute();
    S9xAPUMTSync(); /* the value the APU has produced up to this cycle */
    return ((uint8)SNES::smp.port_read(port & 3));
}

void S9xAPUWritePort(int port, uint8 byte)
{
    S9XP_COUNT(APU_PORT);
    S9xAPUExecute();
    if (apumt::on)
    {
        apumt::Cmd c = { apumt::OP_WRITE, ((port & 3) << 8) | byte, 0, 0 };
        apumt::push(c);
        return;
    }
    SNES::cpu.port_write(port & 3, byte);
}

void S9xAPUSetReferenceTime(int32 cpucycles)
{
    spc::reference_time = cpucycles;
}

static void S9xAPUExecute_impl(void);
void S9xAPUExecute(void)
{
    S9XP_COUNT(APU_RUN);
    if (apumt::on)
    {
        int cycles = S9xAPUGetClock(CPU.Cycles);
        spc::remainder = S9xAPUGetClockRemainder(CPU.Cycles);
        S9xAPUSetReferenceTime(CPU.Cycles);
        /* merge with a still unpublished run request: two back-to-back runs equal one */
        if (apumt::prod != apumt::pub && apumt::ring[(apumt::prod - 1) & apumt::MASK].op == apumt::OP_RUN)
            apumt::ring[(apumt::prod - 1) & apumt::MASK].a += cycles;
        else
        {
            apumt::Cmd c = { apumt::OP_RUN, cycles, 0, 0 };
            apumt::push(c);
        }
        return;
    }
    S9XP_ENTER(APU);
    S9xAPUExecute_impl();
    S9XP_LEAVE();
}

static void S9xAPUExecute_impl(void)
{
    int cycles = S9xAPUGetClock(CPU.Cycles);
    spc::remainder = S9xAPUGetClockRemainder(CPU.Cycles);
    SNES::smp.clock -= cycles;
    SNES::smp.enter();

    S9xAPUSetReferenceTime(CPU.Cycles);
}

void S9xAPUEndScanline(void)
{
    S9xAPUExecute();
    if (apumt::on)
    {
        apumt::Cmd c = { apumt::OP_ENDSCAN, 0, 0, 0 };
        apumt::push(c);
        return;
    }
    S9XP_ENTER(APU);
    SNES::dsp.synchronize();

    if (spc::resampler.space_filled() >= APU_SAMPLE_BLOCK)
        S9xLandSamples();
    S9XP_LEAVE();
}

void S9xAPUTimingSetSpeedup(int ticks)
{
    S9xAPUMTSync();
    if (ticks != 0)
        printf("APU speedup hack: %d\n", ticks);

    spc::timing_hack_denominator = 256 - ticks;

    spc::ratio_numerator = Settings.PAL ? APU_NUMERATOR_PAL : APU_NUMERATOR_NTSC;
    spc::ratio_denominator = Settings.PAL ? APU_DENOMINATOR_PAL : APU_DENOMINATOR_NTSC;
    spc::ratio_denominator = spc::ratio_denominator * spc::timing_hack_denominator / spc::timing_hack_numerator;

    UpdatePlaybackRate();
}

void S9xResetAPU(void)
{
    S9xAPUMTSync();
    spc::reference_time = 0;
    spc::remainder = 0;

    SNES::cpu.reset();
    SNES::smp.power();
    SNES::dsp.power();
    SNES::dsp.spc_dsp.set_spc_snapshot_callback(SPCSnapshotCallback);

    S9xClearSamples();
}

void S9xSoftResetAPU(void)
{
    S9xAPUMTSync();
    spc::reference_time = 0;
    spc::remainder = 0;
    SNES::cpu.reset();
    SNES::smp.reset();
    SNES::dsp.reset();

    S9xClearSamples();
}

void S9xAPUSaveState(uint8 *block)
{
    S9xAPUMTSync();
    uint8 *ptr = block;

    SNES::smp.save_state(&ptr);
    SNES::dsp.save_state(&ptr);

    SNES::set_le32(ptr, spc::reference_time);
    ptr += sizeof(int32);
    SNES::set_le32(ptr, spc::remainder);
    ptr += sizeof(int32);
    SNES::set_le32(ptr, SNES::dsp.clock);
    ptr += sizeof(int32);
    memcpy(ptr, SNES::cpu.registers, 4);
    ptr += sizeof(int32);

    memset(ptr, 0, SPC_SAVE_STATE_BLOCK_SIZE - (ptr - block));
}

void S9xAPULoadState(uint8 *block)
{
    S9xAPUMTSync();
    uint8 *ptr = block;

    SNES::smp.load_state(&ptr);
    SNES::dsp.load_state(&ptr);
    spc::reference_time = SNES::get_le32(ptr);
    ptr += sizeof(int32);
    spc::remainder = SNES::get_le32(ptr);
    ptr += sizeof(int32);
    SNES::dsp.clock = SNES::get_le32(ptr);
    ptr += sizeof(int32);
    memcpy(SNES::cpu.registers, ptr, 4);
}

static void to_var_from_buf(uint8 **buf, void *var, size_t size)
{
    memcpy(var, *buf, size);
    *buf += size;
}

#undef IF_0_THEN_256
#define IF_0_THEN_256(n) ((uint8)((n)-1) + 1)
void S9xAPULoadBlarggState(uint8 *oldblock)
{
    S9xAPUMTSync();
    uint8 *ptr = oldblock;

    SNES::SPC_State_Copier copier(&ptr, to_var_from_buf);

    copier.copy(SNES::smp.apuram, 0x10000); // RAM

    uint8 regs_in[0x10];
    uint8 regs[0x10];
    uint16 pc, spc_time, dsp_time;
    uint8 a, x, y, psw, sp;

    copier.copy(regs, 0x10);    // REGS
    copier.copy(regs_in, 0x10); // REGS_IN

    // CPU Regs
    pc = copier.copy_int(0, sizeof(uint16));
    a = copier.copy_int(0, sizeof(uint8));
    x = copier.copy_int(0, sizeof(uint8));
    y = copier.copy_int(0, sizeof(uint8));
    psw = copier.copy_int(0, sizeof(uint8));
    sp = copier.copy_int(0, sizeof(uint8));
    copier.extra();

    // times
    spc_time = copier.copy_int(0, sizeof(uint16));
    dsp_time = copier.copy_int(0, sizeof(uint16));

    int cur_time = S9xAPUGetClock(CPU.Cycles);

    // spc_time is absolute, dsp_time is relative
    // smp.clock is relative, dsp.clock relative but counting upwards
    SNES::smp.clock = spc_time - cur_time;
    SNES::dsp.clock = -1 * dsp_time;

    // DSP
    SNES::dsp.load_state(&ptr);

    // Timers
    uint16 next_time[3];
    uint8 divider[3], counter[3];
    for (int i = 0; i < 3; i++)
    {
        next_time[i] = copier.copy_int(0, sizeof(uint16));
        divider[i] = copier.copy_int(0, sizeof(uint8));
        counter[i] = copier.copy_int(0, sizeof(uint8));
        copier.extra();
    }
    // construct timers out of available parts from blargg smp
    SNES::smp.timer0.enable = regs[1] >> 0 & 1;        // regs[1] = CONTROL
    SNES::smp.timer0.target = IF_0_THEN_256(regs[10]); // regs[10+i] = TiTARGET
    // blargg counts time, get ticks through timer frequency
    // (assume tempo = 256)
    SNES::smp.timer0.stage1_ticks = 128 - (next_time[0] - cur_time) / 128;
    SNES::smp.timer0.stage2_ticks = divider[0];
    SNES::smp.timer0.stage3_ticks = counter[0];

    SNES::smp.timer1.enable = regs[1] >> 1 & 1;
    SNES::smp.timer1.target = IF_0_THEN_256(regs[11]);
    SNES::smp.timer1.stage1_ticks = 128 - (next_time[1] - cur_time) / 128;
    SNES::smp.timer1.stage2_ticks = divider[0];
    SNES::smp.timer1.stage3_ticks = counter[0];

    SNES::smp.timer2.enable = regs[1] >> 2 & 1;
    SNES::smp.timer2.target = IF_0_THEN_256(regs[12]);
    SNES::smp.timer2.stage1_ticks = 16 - (next_time[2] - cur_time) / 16;
    SNES::smp.timer2.stage2_ticks = divider[0];
    SNES::smp.timer2.stage3_ticks = counter[0];

    copier.extra();

    SNES::smp.opcode_number = 0;
    SNES::smp.opcode_cycle = 0;

    SNES::smp.regs.pc = pc;
    SNES::smp.regs.sp = sp;
    SNES::smp.regs.B.a = a;
    SNES::smp.regs.x = x;
    SNES::smp.regs.B.y = y;

    // blargg's psw has same layout as byuu's flags
    SNES::smp.regs.p = psw;

    // blargg doesn't explicitly store iplrom_enable
    SNES::smp.status.iplrom_enable = regs[1] & 0x80;

    SNES::smp.status.dsp_addr = regs[2];

    SNES::smp.status.ram00f8 = regs_in[8];
    SNES::smp.status.ram00f9 = regs_in[9];

    // default to 0 - we are on an opcode boundary, shouldn't matter
    SNES::smp.rd = SNES::smp.wr = SNES::smp.dp = SNES::smp.sp = SNES::smp.ya = SNES::smp.bit = 0;

    spc::reference_time = SNES::get_le32(ptr);
    ptr += sizeof(int32);
    spc::remainder = SNES::get_le32(ptr);

    // blargg stores CPUIx in regs_in
    memcpy(SNES::cpu.registers, regs_in + 4, 4);
}

bool8 S9xSPCDump(const char *filename)
{
    S9xAPUMTSync();
    FILE *fs;
    uint8 buf[SPC_FILE_SIZE];
    size_t ignore;

    fs = fopen(filename, "wb");
    if (!fs)
        return false;

    S9xSetSoundMute(true);

    SNES::smp.save_spc(buf);

    ignore = fwrite(buf, SPC_FILE_SIZE, 1, fs);

    if (ignore == 0)
    {
        fprintf(stderr, "Couldn't write file %s.\n", filename);
    }

    fclose(fs);

    S9xSetSoundMute(false);

    return true;
}
