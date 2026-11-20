/* SPDX-License-Identifier: MIT
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 * pk_ra_plat.h on the console: the web-request thread (256 KiB of stack: SceHttp and SceSsl run the TLS handshake on the caller's stack),
 * the network stack and the Wi-Fi state. Written for the players; takes nothing from the main app. */
#include "pk_ra_plat.h"

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <stdlib.h>

#define PK_RA_NET_POOL (256 * 1024)

static SceUID g_th = -1;
static void (*g_fn)(void *);
static void *g_arg;
static int g_net;

static int entry(SceSize args, void *argp)
{
	(void) args; (void) argp;
	g_fn(g_arg);
	return 0;
}

int plat_net_start(void (*fn)(void *), void *arg)
{
	if (g_th >= 0) return -1;
	g_fn = fn;
	g_arg = arg;
	g_th = sceKernelCreateThread("pk_ra_net", entry, 0xB8, 0x40000, 0, 0, NULL);     /* below the writer: whatever is left over */
	if (g_th < 0 || sceKernelStartThread(g_th, 0, NULL) < 0) {
		if (g_th >= 0) sceKernelDeleteThread(g_th);
		g_th = -1;
		return -1;
	}
	return 0;
}

int plat_net_join(uint32_t timeout_us)
{
	SceUInt tmo = timeout_us;
	if (g_th < 0) return 1;
	if (sceKernelWaitThreadEnd(g_th, NULL, &tmo) < 0) return 0;
	sceKernelDeleteThread(g_th);
	g_th = -1;
	return 1;
}

void plat_thread_sleep_us(uint32_t us) { sceKernelDelayThread(us); }

int pk_ra_net_up(void)
{
	SceNetInitParam p;
	int rc;
	if (g_net) return 0;
	if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED && sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return -1;
	p.memory = malloc(PK_RA_NET_POOL);
	p.size = PK_RA_NET_POOL;
	p.flags = 0;
	if (!p.memory) return -1;
	rc = sceNetInit(&p);
	if (rc < 0) {
		free(p.memory);     /* somebody was first (the two-player link): their stack serves */
		if ((unsigned) rc != (unsigned) SCE_NET_ERROR_EBUSY) return -1;
	}
	rc = sceNetCtlInit();
	if (rc < 0 && (unsigned) rc != 0x80412102u) return -1;     /* already up is fine */
	g_net = 1;
	return 0;
}

int pk_ra_net_online(void)
{
	int st = 0;
	if (!g_net) return 0;
	return sceNetCtlInetGetState(&st) >= 0 && st == SCE_NETCTL_STATE_CONNECTED;
}
