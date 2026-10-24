/* vadhoc_fake.c -- see vadhoc_fake.h.  A radio cell made of UDP loopback
 * sockets: station NN's PDP socket is 127.0.0.1:base+NN, a datagram carries
 * the PDP source and destination ports in a 4-byte prefix, and a console's
 * MAC is read back from the UDP source port. */
#include "vadhoc_fake.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static struct
{
   int      station;
   uint16_t base;
   int      peers[16], n_peers;
   vadhoc_fake_opts o;

   int mod_net, mod_adhoc;       /* loaded by a call here */
   int net, netctl, adhoc, ctl;  /* initialised by a call here */
   int dlg_open, dlg_polls, dlg_mode, dlg_done;
   int dlg_aborted;              /* this dialog was closed by Abort */
   unsigned dlg_timeout;
   char dlg_group[9], dlg_id[10];
   int connected;                /* in the cell */
   int sock_fd, sock_port, socks;
   char err[160];                /* first protocol violation */
} F = { .sock_fd = -1, .o = { 1, 5, SCE_COMMON_DIALOG_RESULT_OK, 0, 0 } };

static void violation(const char *what)
{
   if (!F.err[0])
      snprintf(F.err, sizeof(F.err), "%s", what);
}

void vadhoc_fake_setup(int station, uint16_t base_port, const int *peers, int n)
{
   int i;
   F.station = station;
   F.base = base_port;
   F.n_peers = n > 16 ? 16 : n;
   for (i = 0; i < F.n_peers; i++)
      F.peers[i] = peers[i];
}

void vadhoc_fake_options(const vadhoc_fake_opts *o) { F.o = *o; }

int vadhoc_fake_balance(char *why, size_t n)
{
   const char *m = NULL;
   if (F.err[0]) m = F.err;
   else if (F.socks) m = "a PDP socket was never deleted";
   else if (F.dlg_open) m = "the dialog was never terminated";
   else if (F.connected) m = "still in the cell (no sceNetCtlAdhocDisconnect)";
   else if (F.ctl) m = "sceNetAdhocctlTerm missing";
   else if (F.adhoc) m = "sceNetAdhocTerm missing";
   else if (F.netctl) m = "sceNetCtlTerm missing";
   else if (F.net) m = "sceNetTerm missing";
   else if (F.mod_adhoc) m = "PSPNET_ADHOC module still loaded";
   else if (F.mod_net) m = "NET module still loaded";
   if (m && why && n)
      snprintf(why, n, "%s", m);
   return m ? -1 : 0;
}

void vadhoc_fake_last_dialog(int *mode, char group[9], char id[10],
                             unsigned *timeout_us)
{
   if (mode) *mode = F.dlg_mode;
   if (group) memcpy(group, F.dlg_group, 9);
   if (id) memcpy(id, F.dlg_id, 10);
   if (timeout_us) *timeout_us = F.dlg_timeout;
}

/* ---- modules / stack ------------------------------------------------------ */

int sceSysmoduleIsLoaded(int id)
{
   if (F.o.stack_already_up && id == SCE_SYSMODULE_NET)
      return SCE_SYSMODULE_LOADED;
   if (id == SCE_SYSMODULE_NET) return F.mod_net ? SCE_SYSMODULE_LOADED : (int)0x805A1001;
   if (id == SCE_SYSMODULE_PSPNET_ADHOC) return F.mod_adhoc ? SCE_SYSMODULE_LOADED : (int)0x805A1001;
   return (int)0x805A1000;
}

int sceSysmoduleLoadModule(int id)
{
   int *m = id == SCE_SYSMODULE_NET ? &F.mod_net :
            id == SCE_SYSMODULE_PSPNET_ADHOC ? &F.mod_adhoc : NULL;
   if (!m || *m || (F.o.stack_already_up && id == SCE_SYSMODULE_NET))
   {
      violation("module loaded twice / unknown module");
      return (int)0x805A1002;
   }
   *m = 1;
   return 0;
}

int sceSysmoduleUnloadModule(int id)
{
   int *m = id == SCE_SYSMODULE_NET ? &F.mod_net :
            id == SCE_SYSMODULE_PSPNET_ADHOC ? &F.mod_adhoc : NULL;
   if (!m || !*m)
   {
      violation("unloaded a module this code did not load");
      return (int)0x805A1003;
   }
   if (id == SCE_SYSMODULE_NET && (F.net || F.netctl))
      violation("NET module unloaded with the stack still initialised");
   if (id == SCE_SYSMODULE_PSPNET_ADHOC && (F.adhoc || F.ctl))
      violation("ADHOC module unloaded with the library still initialised");
   *m = 0;
   return 0;
}

int sceNetInit(SceNetInitParam *param)
{
   if (F.o.stack_already_up || F.net)
      return (int)SCE_NET_ERROR_EBUSY;
   if (!param || !param->memory || param->size < 64 * 1024)
      violation("sceNetInit without a pool");
   if (!F.mod_net)
      violation("sceNetInit before the NET module");
   F.net = 1;
   return 0;
}

int sceNetTerm(void)
{
   if (!F.net) violation("sceNetTerm on a stack this code did not start");
   if (F.netctl) violation("sceNetTerm before sceNetCtlTerm");
   if (F.adhoc) violation("sceNetTerm before sceNetAdhocTerm");
   F.net = 0;
   return 0;
}

int sceNetCtlInit(void)
{
   if (F.o.stack_already_up || F.netctl)
      return (int)0x80412102;
   if (!F.net) violation("sceNetCtlInit before sceNetInit");
   F.netctl = 1;
   return 0;
}

void sceNetCtlTerm(void)
{
   if (!F.netctl) violation("sceNetCtlTerm on a netctl this code did not start");
   if (F.ctl) violation("sceNetCtlTerm before sceNetAdhocctlTerm");
   F.netctl = 0;
}

int sceNetCtlAdhocGetState(int *state)
{
   *state = F.connected ? SCE_NETCTL_STATE_CONNECTED : SCE_NETCTL_STATE_DISCONNECTED;
   return 0;
}

int sceNetCtlAdhocDisconnect(void)
{
   if (F.socks) violation("disconnect with a PDP socket still open");
   F.connected = 0;
   return 0;
}

int sceNetAdhocInit(void)
{
   if (!F.mod_adhoc) violation("sceNetAdhocInit before the ADHOC module");
   if (F.adhoc) return (int)0x80410713;
   F.adhoc = 1;
   return 0;
}

int sceNetAdhocTerm(void)
{
   if (!F.adhoc) violation("sceNetAdhocTerm without init");
   if (F.ctl) violation("sceNetAdhocTerm before sceNetAdhocctlTerm");
   if (F.socks) violation("sceNetAdhocTerm with a PDP socket open");
   F.adhoc = 0;
   return 0;
}

int sceNetAdhocctlInit(const SceNetAdhocctlAdhocId *id)
{
   int i;
   if (!F.adhoc) violation("sceNetAdhocctlInit before sceNetAdhocInit");
   if (F.ctl) return (int)0x80410B07;
   if (!id || id->type != SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED)
      violation("adhoc id type is not RESERVED");
   for (i = 0; id && i < 9; i++)
      if (!id->data[i])
         violation("adhoc id shorter than 9 characters");
   F.ctl = 1;
   return 0;
}

int sceNetAdhocctlTerm(void)
{
   if (!F.ctl) violation("sceNetAdhocctlTerm without init");
   if (F.dlg_open) violation("sceNetAdhocctlTerm with the dialog open");
   F.ctl = 0;
   return 0;
}

static void station_mac(int nn, unsigned char mac[6])
{
   mac[0] = 0x02; mac[1] = mac[2] = mac[3] = mac[4] = 0; mac[5] = (unsigned char)nn;
}

int sceNetAdhocctlGetEtherAddr(SceNetEtherAddr *addr)
{
   if (!F.connected)
      return (int)0x80410B08;            /* not valid before the cell */
   station_mac(F.station, addr->data);
   return 0;
}

int sceNetAdhocctlGetPeerList(int *buflen, void *buf)
{
   SceNetAdhocctlPeerInfo *out = (SceNetAdhocctlPeerInfo *)buf;
   int i, n = F.connected ? F.n_peers : 0, cap;
   if (!F.ctl) return (int)0x80410B08;
   if (!buf)
   {
      *buflen = n * (int)sizeof(*out);
      return 0;
   }
   cap = *buflen / (int)sizeof(*out);
   if (n > cap) n = cap;
   for (i = 0; i < n; i++)
   {
      memset(&out[i], 0, sizeof(out[i]));
      out[i].next = i + 1 < n ? &out[i + 1] : NULL;
      snprintf(out[i].nickname.data, sizeof(out[i].nickname.data),
               "Vita%02d", F.peers[i]);
      station_mac(F.peers[i], out[i].macAddr.data);
   }
   *buflen = n * (int)sizeof(*out);
   return 0;
}

/* ---- dialog --------------------------------------------------------------- */

#define FAKE_MAGIC 0xC0D1A109u

void sceNetCheckDialogParamInit(SceNetCheckDialogParam *param)
{
   memset(param, 0, sizeof(*param));
   param->sdkVersion = 0x03570011;
   param->magic = FAKE_MAGIC + (unsigned)(uintptr_t)param;
}

int sceNetCheckDialogInit(SceNetCheckDialogParam *p)
{
   if (!F.ctl) violation("dialog before sceNetAdhocctlInit");
   if (F.dlg_open) return (int)0x80020401;   /* BUSY */
   if (p->magic != FAKE_MAGIC + (unsigned)(uintptr_t)p)
   {
      violation("dialog parameter block was copied after ParamInit");
      return (int)0x80100C01;
   }
   if (p->mode < 5 || p->mode > 7 || !p->groupName)
      return (int)0x80100C07;                /* INVALID_PSPADHOC_PARAM */
   F.dlg_open = 1;
   F.dlg_done = 0;
   F.dlg_aborted = 0;
   F.dlg_polls = 0;
   F.dlg_mode = p->mode;
   F.dlg_timeout = p->timeoutUs;
   memset(F.dlg_group, 0, sizeof(F.dlg_group));
   memcpy(F.dlg_group, p->groupName->data, 8);
   memset(F.dlg_id, 0, sizeof(F.dlg_id));
   memcpy(F.dlg_id, p->npCommunicationId.data, 9);
   return 0;
}

int sceNetCheckDialogGetStatus(void)
{
   if (!F.dlg_open)
      return SCE_COMMON_DIALOG_STATUS_NONE;
   if (!F.dlg_done && ++F.dlg_polls >= F.o.dialog_frames)
   {
      F.dlg_done = 1;
      if (F.o.dialog_result == SCE_COMMON_DIALOG_RESULT_OK)
         F.connected = 1;
   }
   return F.dlg_done ? SCE_COMMON_DIALOG_STATUS_FINISHED
                     : SCE_COMMON_DIALOG_STATUS_RUNNING;
}

int sceNetCheckDialogAbort(void)
{
   if (!F.dlg_open) return (int)0x80020404;
   if (!F.dlg_done)
   {
      /* This dialog's outcome, not the option: the next dialog a program
       * opens (the twin: a second session in one run) starts afresh. */
      F.dlg_done = 1;
      F.dlg_aborted = 1;
   }
   return 0;
}

int sceNetCheckDialogGetResult(SceNetCheckDialogResult *r)
{
   if (!F.dlg_open || !F.dlg_done) return (int)0x80020410;   /* NOT_FINISHED */
   r->result = F.dlg_aborted ? SCE_COMMON_DIALOG_RESULT_ABORTED
                             : F.o.dialog_result;
   return 0;
}

int sceNetCheckDialogTerm(void)
{
   if (!F.dlg_open)
   {
      violation("sceNetCheckDialogTerm without a dialog");
      return (int)0x80020404;
   }
   F.dlg_open = 0;
   return 0;
}

/* ---- PDP ------------------------------------------------------------------- */

int sceNetAdhocPdpCreate(const SceNetEtherAddr *saddr, SceUShort16 sport,
                         unsigned int bufsize, int flag)
{
   struct sockaddr_in a;
   unsigned char me[6];
   int fd;
   (void)flag;
   if (!F.adhoc || !F.connected)
      return (int)0x8041070B;                /* NOT_CONNECTED */
   station_mac(F.station, me);
   if (memcmp(saddr->data, me, 6) || !sport || bufsize < 0x2000 || F.socks)
      return (int)0x80410711;                /* INVALID_ARG */
   fd = socket(AF_INET, SOCK_DGRAM, 0);
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   a.sin_port = htons((uint16_t)(F.base + F.station));
   if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0)
   {
      if (fd >= 0) close(fd);
      return (int)0x8041070A;                /* PORT_IN_USE */
   }
   F.sock_fd = fd;
   F.sock_port = sport;
   F.socks = 1;
   return 7;                                 /* a socket id */
}

int sceNetAdhocPdpDelete(int id, int flag)
{
   (void)flag;
   if (id != 7 || !F.socks)
   {
      violation("PdpDelete of a socket that is not open");
      return (int)0x80410701;
   }
   close(F.sock_fd);
   F.sock_fd = -1;
   F.socks = 0;
   return 0;
}

static void fake_tx(int nn, SceUShort16 dport, const void *data, int len)
{
   unsigned char pkt[2048];
   struct sockaddr_in a;
   pkt[0] = (unsigned char)F.sock_port; pkt[1] = (unsigned char)(F.sock_port >> 8);
   pkt[2] = (unsigned char)dport;       pkt[3] = (unsigned char)(dport >> 8);
   memcpy(pkt + 4, data, (size_t)len);
   memset(&a, 0, sizeof(a));
   a.sin_family = AF_INET;
   a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   a.sin_port = htons((uint16_t)(F.base + nn));
   sendto(F.sock_fd, pkt, (size_t)len + 4, MSG_DONTWAIT, (struct sockaddr *)&a,
          sizeof(a));
}

int sceNetAdhocPdpSend(int id, const SceNetEtherAddr *daddr, SceUShort16 dport,
                       const void *data, int len, unsigned int timeout, int flag)
{
   static const unsigned char ff[6] = { 255, 255, 255, 255, 255, 255 };
   static const unsigned char zero[6] = { 0 };
   int i;
   (void)timeout; (void)flag;
   if (id != 7 || !F.socks) return (int)0x80410701;
   if (!memcmp(daddr->data, zero, 6)) return (int)0x80410702;
   if (!dport) return (int)0x80410703;
   if (len < 0 || len > 1444) return (int)0x80410705;
   if (!memcmp(daddr->data, ff, 6))
   {
      /* "Broadcast never fails": fanned out to the cell's consoles */
      if (F.o.broadcast_works)
         for (i = 0; i < F.n_peers; i++)
            fake_tx(F.peers[i], dport, data, len);
      return 0;
   }
   if (daddr->data[0] == 0x02)
      fake_tx(daddr->data[5], dport, data, len);
   return 0;                                  /* unknown MAC: fakes success */
}

int sceNetAdhocPdpRecv(int id, SceNetEtherAddr *saddr, SceUShort16 *sport,
                       void *buf, int *len, unsigned int timeout, int flag)
{
   unsigned char pkt[2048];
   struct sockaddr_in from;
   socklen_t flen = sizeof(from);
   struct pollfd p;
   ssize_t n;
   int cap = *len;

   if (id != 7 || !F.socks) return (int)0x80410701;
   for (;;)
   {
      if (!(flag & SCE_NET_ADHOC_F_NONBLOCK))
      {
         p.fd = F.sock_fd; p.events = POLLIN; p.revents = 0;
         if (poll(&p, 1, timeout ? (int)(timeout / 1000u) : -1) <= 0)
            return (int)SCE_ERROR_NET_ADHOC_TIMEOUT;
      }
      n = recvfrom(F.sock_fd, pkt, sizeof(pkt), MSG_DONTWAIT,
                   (struct sockaddr *)&from, &flen);
      if (n < 0)
         return (int)((flag & SCE_NET_ADHOC_F_NONBLOCK)
                         ? SCE_ERROR_NET_ADHOC_WOULD_BLOCK
                         : SCE_ERROR_NET_ADHOC_TIMEOUT);
      if (n < 4 || (pkt[2] | (pkt[3] << 8)) != F.sock_port)
         continue;                            /* another PDP port: not ours */
      break;
   }
   n -= 4;
   if ((int)n > cap)
   {
      *len = (int)n;
      return (int)SCE_ERROR_NET_ADHOC_NOT_ENOUGH_SPACE;
   }
   memcpy(buf, pkt + 4, (size_t)n);
   station_mac(ntohs(from.sin_port) - F.base, saddr->data);
   *sport = (SceUShort16)(pkt[0] | (pkt[1] << 8));
   if (F.o.recv_returns_size)
      return (int)n;                          /* *len left as it was */
   *len = (int)n;
   return 0;
}
