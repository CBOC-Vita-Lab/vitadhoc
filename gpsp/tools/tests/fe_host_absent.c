/* fe_host_absent.c -- what a Game Boy host test of fe_host.c does not link.
 *
 * frontend-common/fe_host.c is the one production front end: its Game Boy
 * path goes through nescore/gbcore_mux.c (the NES and WonderSwan machines
 * answer the same calls), and its GBA path leans on gba_memory.c's
 * "instance A" hooks.  A test of the GB boot / save / state path links the
 * REAL fe_host.c, gbcore_mux.c and Game Boy core and takes the rest of
 * the app as absent:
 *
 *   - no NES or WonderSwan machine: nescore_create / wscore_create fail with
 *     a reason, and every other nescore_* / wscore_* call needs a machine,
 *     which none ever is (gbcore_mux.c tests its own pointer first) -- they
 *     abort so a mux that stops checking is a loud failure, not a pass;
 *   - no GBA cartridge hooks: the clock source, save-type hint and sensor
 *     reading gba_memory.c owns are inert.
 *
 * (The NES and WonderSwan cores have their own host suites: nes/twin,
 * wswan/twin.) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../../nescore/nescore.h"
#include "../../wscore/wscore.h"
#include "../../gbcore/gambatte_host.h"

#define NEVER(name) do { fprintf(stderr, "FAIL: %s called with no machine\n", name); abort(); } while (0)

/* ---- the NES, absent ---- */
void nescore_set_system_dir(const char *dir) { (void)dir; }
nescore_t *nescore_create(const char *rom_path, const char *srm_path,
                          const nescore_callbacks_t *cb)
{ (void)rom_path; (void)srm_path; (void)cb; return NULL; }
const char *nescore_error(void) { return "no NES core in this test"; }
int nescore_run_frame(nescore_t *c, unsigned b) { (void)c; (void)b; NEVER("nescore_run_frame"); return 0; }
void nescore_set_skip_render(nescore_t *c, int s) { (void)c; (void)s; NEVER("nescore_set_skip_render"); }
int nescore_repeat_frame(nescore_t *c) { (void)c; NEVER("nescore_repeat_frame"); return 0; }
void nescore_mix_next(nescore_t *c) { (void)c; NEVER("nescore_mix_next"); }
void nescore_shutdown(nescore_t *c) { (void)c; NEVER("nescore_shutdown"); }
unsigned nescore_sample_rate(const nescore_t *c) { (void)c; NEVER("nescore_sample_rate"); return 0; }
unsigned nescore_frame_width(const nescore_t *c) { (void)c; NEVER("nescore_frame_width"); return 0; }
unsigned nescore_frame_height(const nescore_t *c) { (void)c; NEVER("nescore_frame_height"); return 0; }
double nescore_fps(const nescore_t *c) { (void)c; NEVER("nescore_fps"); return 0; }
int nescore_rom_title(nescore_t *c, char *d, size_t n) { (void)c; (void)d; (void)n; NEVER("nescore_rom_title"); return 0; }
size_t nescore_save_ram_size(nescore_t *c) { (void)c; NEVER("nescore_save_ram_size"); return 0; }
int nescore_save_ram_read(nescore_t *c, void *d, size_t n) { (void)c; (void)d; (void)n; NEVER("nescore_save_ram_read"); return 0; }
int nescore_save_ram_write(nescore_t *c, const void *s, size_t n) { (void)c; (void)s; (void)n; NEVER("nescore_save_ram_write"); return 0; }
int nescore_save_ram_dirty(nescore_t *c) { (void)c; NEVER("nescore_save_ram_dirty"); return 0; }
size_t nescore_state_size(nescore_t *c) { (void)c; NEVER("nescore_state_size"); return 0; }
long nescore_state_save(nescore_t *c, void *d, size_t n) { (void)c; (void)d; (void)n; NEVER("nescore_state_save"); return 0; }
int nescore_state_load(nescore_t *c, const void *s, size_t n) { (void)c; (void)s; (void)n; NEVER("nescore_state_load"); return 0; }
uint32_t nescore_ra_read(nescore_t *c, uint32_t a, uint8_t *o, uint32_t n) { (void)c; (void)a; (void)o; (void)n; NEVER("nescore_ra_read"); return 0; }
int nescore_cheats_set(nescore_t *c, const char *s) { (void)c; (void)s; NEVER("nescore_cheats_set"); return 0; }
int nescore_peek(nescore_t *c, unsigned a) { (void)c; (void)a; NEVER("nescore_peek"); return 0; }
size_t nescore_ahead_size(nescore_t *c) { (void)c; NEVER("nescore_ahead_size"); return 0; }
long nescore_ahead_save(nescore_t *c, void *d, size_t n) { (void)c; (void)d; (void)n; NEVER("nescore_ahead_save"); return 0; }
int nescore_ahead_load(nescore_t *c, const void *s, size_t n) { (void)c; (void)s; (void)n; NEVER("nescore_ahead_load"); return 0; }
void nescore_set_quiet(nescore_t *c, int q) { (void)c; (void)q; NEVER("nescore_set_quiet"); }

/* ---- the WonderSwan, absent ---- */
wscore_t *wscore_create(const char *rom_path, unsigned rate,
                        const wscore_callbacks_t *cb)
{ (void)rom_path; (void)rate; (void)cb; return NULL; }
const char *wscore_error(void) { return "no WonderSwan core in this test"; }
int wscore_run_frame(wscore_t *c) { (void)c; NEVER("wscore_run_frame"); return 0; }
void wscore_set_skip_render(wscore_t *c, int s) { (void)c; (void)s; NEVER("wscore_set_skip_render"); }
void wscore_shutdown(wscore_t *c) { (void)c; NEVER("wscore_shutdown"); }
unsigned wscore_sample_rate(const wscore_t *c) { (void)c; NEVER("wscore_sample_rate"); return 0; }
unsigned wscore_frame_width(const wscore_t *c) { (void)c; NEVER("wscore_frame_width"); return 0; }
unsigned wscore_frame_height(const wscore_t *c) { (void)c; NEVER("wscore_frame_height"); return 0; }
double wscore_fps(const wscore_t *c) { (void)c; NEVER("wscore_fps"); return 0; }
int wscore_rom_title(wscore_t *c, char *d, size_t n) { (void)c; (void)d; (void)n; NEVER("wscore_rom_title"); return 0; }
size_t wscore_save_ram_size(wscore_t *c) { (void)c; NEVER("wscore_save_ram_size"); return 0; }
int wscore_save_ram_read(wscore_t *c, void *d, size_t n) { (void)c; (void)d; (void)n; NEVER("wscore_save_ram_read"); return 0; }
int wscore_save_ram_write(wscore_t *c, const void *s, size_t n) { (void)c; (void)s; (void)n; NEVER("wscore_save_ram_write"); return 0; }
size_t wscore_ahead_size(wscore_t *c) { (void)c; NEVER("wscore_ahead_size"); return 0; }
long wscore_ahead_save(wscore_t *c, void *d, size_t n) { (void)c; (void)d; (void)n; NEVER("wscore_ahead_save"); return 0; }
int wscore_ahead_load(wscore_t *c, const void *s, size_t n) { (void)c; (void)s; (void)n; NEVER("wscore_ahead_load"); return 0; }
void wscore_set_quiet(wscore_t *c, int q) { (void)c; (void)q; NEVER("wscore_set_quiet"); }
int wscore_save_ram_dirty(wscore_t *c) { (void)c; NEVER("wscore_save_ram_dirty"); return 0; }
size_t wscore_state_size(wscore_t *c) { (void)c; NEVER("wscore_state_size"); return 0; }
long wscore_state_save(wscore_t *c, void *d, size_t n) { (void)c; (void)d; (void)n; NEVER("wscore_state_save"); return 0; }
int wscore_state_load(wscore_t *c, const void *s, size_t n) { (void)c; (void)s; (void)n; NEVER("wscore_state_load"); return 0; }
uint32_t wscore_ra_read(wscore_t *c, uint32_t a, uint8_t *o, uint32_t n) { (void)c; (void)a; (void)o; (void)n; NEVER("wscore_ra_read"); return 0; }
int wscore_cheats_set(wscore_t *c, const char *s) { (void)c; (void)s; NEVER("wscore_cheats_set"); return 0; }
int wscore_peek(wscore_t *c, unsigned a) { (void)c; (void)a; NEVER("wscore_peek"); return 0; }

/* ---- the Accurate Game Boy core (Gambatte), absent ---- */
int gambatte_host_create(const uint8_t *rom, size_t size, int cgb, unsigned rate, const gambatte_host_cb_t *cb)
{ (void)rom; (void)size; (void)cgb; (void)rate; (void)cb; return -1; }
int gambatte_host_running(void) { return 0; }
const uint8_t *gambatte_host_header(void) { NEVER("gambatte_host_header"); return NULL; }
void gambatte_host_shutdown(void) { NEVER("gambatte_host_shutdown"); }
int gambatte_host_run_frame(uint16_t b) { (void)b; NEVER("gambatte_host_run_frame"); return 0; }
void gambatte_host_set_skip_render(int s) { (void)s; NEVER("gambatte_host_set_skip_render"); }
void gambatte_host_set_quiet(int q) { (void)q; NEVER("gambatte_host_set_quiet"); }
unsigned gambatte_host_sample_rate(void) { NEVER("gambatte_host_sample_rate"); return 0; }
unsigned gambatte_host_frame_width(void) { NEVER("gambatte_host_frame_width"); return 0; }
unsigned gambatte_host_frame_height(void) { NEVER("gambatte_host_frame_height"); return 0; }
int gambatte_host_rom_title(char *d, size_t n) { (void)d; (void)n; NEVER("gambatte_host_rom_title"); return 0; }
size_t gambatte_host_save_ram_size(void) { NEVER("gambatte_host_save_ram_size"); return 0; }
size_t gambatte_host_cart_ram_size(void) { NEVER("gambatte_host_cart_ram_size"); return 0; }
int gambatte_host_save_ram_read(void *d, size_t n) { (void)d; (void)n; NEVER("gambatte_host_save_ram_read"); return 0; }
int gambatte_host_save_ram_write(const void *s, size_t n) { (void)s; (void)n; NEVER("gambatte_host_save_ram_write"); return 0; }
size_t gambatte_host_state_size(void) { NEVER("gambatte_host_state_size"); return 0; }
long gambatte_host_state_save(void *d, size_t n) { (void)d; (void)n; NEVER("gambatte_host_state_save"); return 0; }
int gambatte_host_state_load(const void *s, size_t n) { (void)s; (void)n; NEVER("gambatte_host_state_load"); return 0; }
int gambatte_host_state_kind(const void *s, size_t n) { (void)s; (void)n; return 0; }
size_t gambatte_host_ahead_size(void) { NEVER("gambatte_host_ahead_size"); return 0; }
long gambatte_host_ahead_save(void *d, size_t n) { (void)d; (void)n; NEVER("gambatte_host_ahead_save"); return 0; }
int gambatte_host_ahead_load(const void *s, size_t n) { (void)s; (void)n; NEVER("gambatte_host_ahead_load"); return 0; }
void gambatte_host_set_dmg_colors(const uint16_t c[12]) { (void)c; }
void gambatte_host_set_color_correction(int on) { (void)on; }
void gambatte_host_set_wallclock(time_t (*w)(void)) { (void)w; }
int gambatte_host_cheats_set(const gambatte_host_cheat_t *c, unsigned n) { (void)c; (void)n; NEVER("gambatte_host_cheats_set"); return 0; }
uint32_t gambatte_host_ra_read(uint32_t a, uint8_t *o, uint32_t n) { (void)a; (void)o; (void)n; NEVER("gambatte_host_ra_read"); return 0; }
int gambatte_host_peek(uint16_t a, void *o, unsigned n) { (void)a; (void)o; (void)n; NEVER("gambatte_host_peek"); return 0; }
int gambatte_host_poke(uint16_t a, const void *d, unsigned n) { (void)a; (void)d; (void)n; NEVER("gambatte_host_poke"); return 0; }

/* ---- the GBA cartridge's hooks (gba_memory.c), inert ---- */
volatile long long gpsp_rtc_wall;
time_t (*gpsp_wallclock)(void);
int gpsp_force_save_type;
unsigned gpsp_save_size_hint;
const char *gpsp_backup_name(void) { return "none"; }
uint32_t gpsp_sensor_kinds(void) { return 0; }
void gpsp_sensor_set(uint32_t reading) { (void)reading; }
uint32_t gpsp_sensor_get(void) { return 0; }

/* ---- the netplay session (netpacket_host.c), none ---- */
int fe_np_session_active(void) { return 0; }
