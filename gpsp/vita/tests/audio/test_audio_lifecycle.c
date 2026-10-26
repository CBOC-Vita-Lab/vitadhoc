/* Compiles verbatim platform audio sections, not a model of them.
 * The runner supplies audio_lifecycle_native.inc and the real Vita SDK
 * declarations; only the kernel calls below are fault-injection doubles.
 * AUDIO_TEST_HOST selects audio_lifecycle_host.inc and pthread doubles.
 * AUDIO_TEST_LEGACY_ONLY reproduces the old void-stop timeout failure. */
#ifdef AUDIO_TEST_HOST
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static struct { int realtime; const char *wav; } opt;
static uint64_t sim_ns;
static int create_rc, join_rc;
static unsigned create_count, join_count, step_count;
static void *(*thread_entry)(void *);

static uint64_t real_ns(void) { return 0; }
static uint32_t fe_crc32(uint32_t crc, const void *data, size_t len)
{
   (void)data; (void)len;
   return crc;
}

static int create_double(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*entry)(void *), void *arg)
{
   assert(attr == NULL && arg == NULL);
   create_count++;
   thread_entry = entry;
   if (!create_rc)
      *thread = (pthread_t)(uintptr_t)1;
   return create_rc;
}

static int join_double(pthread_t thread, void **result)
{
   assert(thread == (pthread_t)(uintptr_t)1 && result == NULL);
   join_count++;
   return join_rc;
}

#define pthread_create create_double
#define pthread_join join_double
#include "audio_lifecycle_host.inc"
#undef pthread_create
#undef pthread_join

static int host_step(void) { step_count++; return 1; }
static int other_host_step(void) { return 1; }

static void reset_host_case(void)
{
   assert(audio_step == NULL);
#ifndef AUDIO_TEST_LEGACY_ONLY
   assert(!au_live);
#endif
   opt.realtime = 1;
   opt.wav = NULL;
   create_rc = join_rc = 0;
   create_count = join_count = step_count = 0;
   thread_entry = NULL;
   sim_ns = 0;
   wav_rate = 48000;
   wav_grain = 512;
}

static void host_join_failure_retains_ownership(void)
{
   reset_host_case();
   assert(plat_audio_thread_start(host_step) == 0);
   join_rc = 1;
   plat_audio_thread_stop();
   assert(audio_step == host_step);
   assert(__atomic_load_n(&au_run, __ATOMIC_ACQUIRE) == 0);
#ifndef AUDIO_TEST_LEGACY_ONLY
   assert(au_live);
   assert(plat_audio_thread_start(other_host_step) < 0);
   assert(audio_step == host_step && create_count == 1);
   assert(plat_audio_thread_stop_checked() == 0);
   assert(au_live && audio_step == host_step);
   join_rc = 0;
   assert(plat_audio_thread_stop_checked() == 1);
   assert(!au_live && audio_step == NULL && join_count == 3);
#endif
}

#ifndef AUDIO_TEST_LEGACY_ONLY
static void host_normal_start_stop(void)
{
   reset_host_case();
   assert(plat_audio_thread_stop_checked() == 1);
   assert(plat_audio_close_checked() == 1);
   assert(plat_audio_thread_start(host_step) == 0);
   assert(au_live && __atomic_load_n(&au_run, __ATOMIC_ACQUIRE) == 1);
   assert(plat_audio_thread_start(other_host_step) < 0);
   assert(audio_step == host_step && create_count == 1);
   assert(thread_entry(NULL) == NULL && step_count == 1);
   assert(plat_audio_thread_stop_checked() == 1);
   assert(!au_live && audio_step == NULL && join_count == 1);
   plat_audio_thread_stop();
   assert(join_count == 1);
}

static void host_create_failure_and_simulated_stop(void)
{
   reset_host_case();
   create_rc = 1;
   assert(plat_audio_thread_start(host_step) < 0);
   assert(!au_live && audio_step == NULL);
   assert(__atomic_load_n(&au_run, __ATOMIC_ACQUIRE) == 0);
   assert(plat_audio_thread_stop_checked() == 1 && join_count == 0);
   opt.realtime = 0;
   assert(plat_audio_thread_start(host_step) == 0);
   assert(!au_live && audio_step == host_step);
   assert(plat_audio_thread_start(other_host_step) < 0);
   sim_advance(20000000);
   assert(step_count == 1);
   assert(plat_audio_thread_stop_checked() == 1);
   assert(audio_step == NULL && join_count == 0);
}
#endif

int main(void)
{
   host_join_failure_retains_ownership();
#ifndef AUDIO_TEST_LEGACY_ONLY
   host_normal_start_stop();
   host_create_failure_and_simulated_stop();
#else
   (void)other_host_step;
   (void)sim_advance;
#endif
   puts("audio host lifecycle fault-injection tests passed");
   return 0;
}

#else
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr/thread.h>

#define PRIO_AUDIO 0x50

static SceUID create_result;
static int start_result, wait_result, delete_result;
static unsigned create_calls, start_calls, wait_calls, delete_calls, watch_calls;
static SceUID last_wait_id, last_delete_id;
static SceKernelThreadEntry entry_seen;
static int open_result, write_result;
static unsigned drain_failures, release_failures;
static unsigned open_calls, write_calls, drain_calls, release_calls, sleep_calls;
static int16_t pcm[1024];

void plat_sleep_us(uint32_t us);

int sceAudioOutOpenPort(SceAudioOutPortType type, int len, int rate,
                       SceAudioOutMode mode)
{
   assert(type == SCE_AUDIO_OUT_PORT_TYPE_MAIN);
   assert(len == 512 && rate == 48000 && mode == SCE_AUDIO_OUT_MODE_STEREO);
   open_calls++;
   return open_result;
}

int sceAudioOutSetVolume(int port, SceAudioOutChannelFlag ch, int *vol)
{
   assert(port == open_result);
   assert(ch == (SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH));
   assert(vol[0] == SCE_AUDIO_VOLUME_0DB && vol[1] == SCE_AUDIO_VOLUME_0DB);
   return 0;
}

int sceAudioOutOutput(int port, const void *buf)
{
   assert(port == open_result);
   if (buf)
   {
      assert(buf == pcm);
      write_calls++;
      return write_result;
   }
   drain_calls++;
   if (drain_failures)
   {
      drain_failures--;
      return -1;
   }
   return 0;
}

int sceAudioOutReleasePort(int port)
{
   assert(port == open_result);
   release_calls++;
   if (release_failures)
   {
      release_failures--;
      return -1;
   }
   return 0;
}

static void plat_thread_watch(const char *tag, int thid)
{
   assert(strcmp(tag, "audio") == 0);
   assert(thid == create_result);
   watch_calls++;
}

SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry,
                            int priority, SceSize stack, SceUInt attr,
                            int affinity, const SceKernelThreadOptParam *option)
{
   assert(strcmp(name, "gbadhoc_audio") == 0);
   assert(priority == PRIO_AUDIO && stack == 0x10000);
   assert(attr == 0 && affinity == 0 && option == NULL);
   create_calls++;
   entry_seen = entry;
   return create_result;
}

int sceKernelStartThread(SceUID thid, SceSize n, void *p)
{
   assert(thid == create_result && n == 0 && p == NULL);
   start_calls++;
   return start_result;
}

int sceKernelWaitThreadEnd(SceUID thid, int *status, SceUInt *timeout)
{
   assert(status == NULL && timeout != NULL && *timeout == 2000000);
   wait_calls++;
   last_wait_id = thid;
   return wait_result;
}

int sceKernelDeleteThread(SceUID thid)
{
   delete_calls++;
   last_delete_id = thid;
   return delete_result;
}

#include "audio_lifecycle_native.inc"

void plat_sleep_us(uint32_t us)
{
   assert(us == 100000);
   assert(g_port == open_result);
   sleep_calls++;
}

static unsigned step_a_calls, step_b_calls;

static int step_a(void) { step_a_calls++; return 1; }
static int step_b(void) { step_b_calls++; return 1; }

static void reset_case(void)
{
   /* Each case first proves the previous one retired its UID. */
   assert(g_audio_thid < 0);
   assert(g_port < 0);
   create_result = 42;
   start_result = wait_result = delete_result = 0;
   create_calls = start_calls = wait_calls = delete_calls = watch_calls = 0;
   step_a_calls = step_b_calls = 0;
   last_wait_id = last_delete_id = -1;
   entry_seen = NULL;
   open_result = 7;
   write_result = 0;
   drain_failures = release_failures = 0;
   open_calls = write_calls = drain_calls = release_calls = sleep_calls = 0;
}

static void timeout_keeps_uid_and_blocks_restart(void)
{
   reset_case();
   assert(plat_audio_thread_start(step_a) == 0);
   assert(g_audio_step == step_a);
   wait_result = -1;
   plat_audio_thread_stop();
   assert(g_audio_thid == 42);
   assert(__atomic_load_n(&g_audio_stop, __ATOMIC_ACQUIRE) == 1);
   assert(wait_calls == 1 && last_wait_id == 42 && delete_calls == 0);
   assert(plat_audio_thread_start(step_b) < 0);
   assert(g_audio_step == step_a);
   assert(__atomic_load_n(&g_audio_stop, __ATOMIC_ACQUIRE) == 1);
   assert(create_calls == 1 && start_calls == 1);
   assert(entry_seen(0, NULL) == 0);
   assert(step_a_calls == 0 && step_b_calls == 0);
#ifndef AUDIO_TEST_LEGACY_ONLY
   assert(plat_audio_thread_stop_checked() == 0);
   assert(g_audio_thid == 42 && delete_calls == 0);
   wait_result = 0;
   assert(plat_audio_thread_stop_checked() == 1);
   assert(g_audio_thid < 0 && last_delete_id == 42);
   assert(plat_audio_thread_start(step_b) == 0);
   assert(g_audio_step == step_b);
   assert(__atomic_load_n(&g_audio_stop, __ATOMIC_ACQUIRE) == 0);
   assert(entry_seen(0, NULL) == 0 && step_b_calls == 1);
   assert(plat_audio_thread_stop_checked() == 1);
#endif
}

#ifndef AUDIO_TEST_LEGACY_ONLY
static void normal_start_stop_and_no_thread(void)
{
   reset_case();
   assert(plat_audio_thread_stop_checked() == 1);
   assert(wait_calls == 0 && delete_calls == 0);
   assert(plat_audio_thread_start(step_a) == 0);
   assert(watch_calls == 1);
   assert(plat_audio_thread_start(step_b) < 0);
   assert(g_audio_step == step_a);
   assert(__atomic_load_n(&g_audio_stop, __ATOMIC_ACQUIRE) == 0);
   assert(entry_seen(0, NULL) == 0 && step_a_calls == 1);
   assert(plat_audio_thread_stop_checked() == 1);
   assert(wait_calls == 1 && delete_calls == 1 && g_audio_thid < 0);
   plat_audio_thread_stop();
   assert(wait_calls == 1 && delete_calls == 1);
}

static void create_failure_leaves_no_consumer(void)
{
   reset_case();
   create_result = -10;
   assert(plat_audio_thread_start(step_a) < 0);
   assert(start_calls == 0 && wait_calls == 0 && delete_calls == 0);
   assert(plat_audio_thread_stop_checked() == 1);
   create_result = 43;
   assert(plat_audio_thread_start(step_b) == 0);
   assert(plat_audio_thread_stop_checked() == 1);
}

static void start_failure_retires_dormant_uid(void)
{
   reset_case();
   start_result = -2;
   assert(plat_audio_thread_start(step_a) < 0);
   assert(delete_calls == 1 && g_audio_thid < 0);
   assert(plat_audio_thread_stop_checked() == 1);
   assert(wait_calls == 0);
   start_result = 0;
   assert(plat_audio_thread_start(step_b) == 0);
   assert(plat_audio_thread_stop_checked() == 1);
}

static void delete_failure_preserves_joined_uid(void)
{
   reset_case();
   assert(plat_audio_thread_start(step_a) == 0);
   delete_result = -3;
   assert(plat_audio_thread_stop_checked() == 0);
   assert(g_audio_thid == 42 && wait_calls == 1 && delete_calls == 1);
   assert(__atomic_load_n(&g_audio_stop, __ATOMIC_ACQUIRE) == 1);
   assert(plat_audio_thread_start(step_b) < 0);
   assert(g_audio_step == step_a && create_calls == 1);
   delete_result = 0;
   assert(plat_audio_thread_stop_checked() == 1);
   assert(g_audio_thid < 0);
}

static void start_and_delete_failure_preserves_dormant_uid(void)
{
   reset_case();
   start_result = -2;
   delete_result = -3;
   assert(plat_audio_thread_start(step_a) < 0);
   assert(g_audio_thid == 42 && delete_calls == 1);
   assert(__atomic_load_n(&g_audio_stop, __ATOMIC_ACQUIRE) == 1);
   assert(plat_audio_thread_start(step_b) < 0);
   assert(g_audio_step == step_a && create_calls == 1);
   assert(plat_audio_thread_stop_checked() == 0);
   assert(g_audio_thid == 42 && wait_calls == 0 && delete_calls == 2);
   delete_result = 0;
   assert(plat_audio_thread_stop_checked() == 1);
   assert(g_audio_thid < 0 && wait_calls == 0 && delete_calls == 3);
   start_result = 0;
   assert(plat_audio_thread_start(step_b) == 0);
   assert(plat_audio_thread_stop_checked() == 1);
}

static void checked_close_retires_only_after_drain_and_release(void)
{
   reset_case();
   assert(plat_audio_close_checked() == 1);
   assert(plat_audio_open(48000, 512) == 0);
   assert(plat_audio_open(48000, 512) < 0 && open_calls == 1);
   assert(plat_audio_write(pcm) == 0);
   drain_failures = 1;
   assert(plat_audio_close_checked() == 0);
   assert(g_port == 7 && drain_calls == 1 && release_calls == 0);
   assert(plat_audio_open(48000, 512) < 0 && open_calls == 1);
   release_failures = 1;
   assert(plat_audio_close_checked() == 0);
   assert(g_port == 7 && drain_calls == 2 && release_calls == 1);
   assert(plat_audio_open(48000, 512) < 0 && open_calls == 1);
   assert(plat_audio_close_checked() == 1);
   assert(g_port < 0 && drain_calls == 3 && release_calls == 2);
   assert(plat_audio_open(48000, 512) == 0);
   assert(plat_audio_close_checked() == 1);
   assert(drain_calls == 3);       /* No output: no potentially blocking drain. */
}

static void failed_write_keeps_storage_owned_until_close(void)
{
   reset_case();
   assert(plat_audio_open(48000, 512) == 0);
   write_result = -1;
   assert(plat_audio_write(pcm) < 0);
   drain_failures = 1;
   assert(plat_audio_close_checked() == 0);
   assert(g_port == 7 && drain_calls == 1 && release_calls == 0);
   assert(plat_audio_close_checked() == 1);
   assert(g_port < 0 && drain_calls == 2 && release_calls == 1);
}

static void consumer_retries_final_close_before_exit(void)
{
   reset_case();
   assert(plat_audio_open(48000, 512) == 0);
   assert(plat_audio_write(pcm) == 0);
   assert(plat_audio_thread_start(step_a) == 0);
   wait_result = -1;
   assert(plat_audio_thread_stop_checked() == 0);
   assert(g_audio_thid == 42 && g_port == 7);
   assert(drain_calls == 0 && release_calls == 0);
   drain_failures = release_failures = 1;
   assert(entry_seen(0, NULL) == 0);
   assert(step_a_calls == 0 && sleep_calls == 2);
   assert(drain_calls == 3 && release_calls == 2 && g_port < 0);
   wait_result = 0;
   assert(plat_audio_thread_stop_checked() == 1);
}

static void step_ending_consumer_closes_queued_port(void)
{
   reset_case();
   assert(plat_audio_open(48000, 512) == 0);
   assert(plat_audio_write(pcm) == 0);
   assert(plat_audio_thread_start(step_a) == 0);
   assert(entry_seen(0, NULL) == 0);
   assert(step_a_calls == 1 && g_port < 0);
   assert(drain_calls == 1 && release_calls == 1 && sleep_calls == 0);
   assert(plat_audio_thread_stop_checked() == 1);
}
#endif

int main(void)
{
   timeout_keeps_uid_and_blocks_restart();
#ifndef AUDIO_TEST_LEGACY_ONLY
   normal_start_stop_and_no_thread();
   create_failure_leaves_no_consumer();
   start_failure_retires_dormant_uid();
   delete_failure_preserves_joined_uid();
   start_and_delete_failure_preserves_dormant_uid();
   checked_close_retires_only_after_drain_and_release();
   failed_write_keeps_storage_owned_until_close();
   consumer_retries_final_close_before_exit();
   step_ending_consumer_closes_queued_port();
#endif
   puts("audio native lifecycle fault-injection tests passed");
   return 0;
}
#endif
