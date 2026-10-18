/* vadhoc_fake.h -- the handful of PS Vita SDK names transport_vadhoc.c uses,
 * for building that file on a PC (-DVADHOC_HOST_FAKE).  Same names, same
 * signatures and the same structure sizes where the transport depends on
 * them as /opt/vitasdk/arm-vita-eabi/include/psp2/{pspnet_adhoc,
 * pspnet_adhocctl,netcheck_dialog,common_dialog,sysmodule}.h and
 * psp2/net/{net,netctl}.h; implemented by vadhoc_fake.c over UDP loopback.
 *
 * The fake follows the PSP library's documented behaviour (docs/
 * ADHOC-NOTES.md): it is what transport_vadhoc.c ASSUMES the firmware does,
 * so a test passing here says the transport is right IF the assumptions
 * hold -- never that they hold.
 */
#ifndef VADHOC_FAKE_H
#define VADHOC_FAKE_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned short SceUShort16;

typedef struct SceNetEtherAddr { unsigned char data[6]; } SceNetEtherAddr;

#define SCE_NET_ADHOCCTL_ADHOCID_LEN   9
#define SCE_NET_ADHOCCTL_GROUPNAME_LEN 8
#define SCE_NET_ADHOCCTL_NICKNAME_LEN  128
enum { SCE_NET_ADHOCCTL_ADHOCTYPE_PRODUCT_ID = 0,
       SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED = 1 };

typedef struct SceNetAdhocctlAdhocId
{
   int type;
   char data[SCE_NET_ADHOCCTL_ADHOCID_LEN];
   unsigned char padding[3];
} SceNetAdhocctlAdhocId;
typedef struct SceNetAdhocctlGroupName { char data[SCE_NET_ADHOCCTL_GROUPNAME_LEN]; } SceNetAdhocctlGroupName;
typedef struct SceNetAdhocctlNickname { char data[SCE_NET_ADHOCCTL_NICKNAME_LEN]; } SceNetAdhocctlNickname;
typedef struct SceNetAdhocctlPeerInfo
{
   struct SceNetAdhocctlPeerInfo *next;
   struct SceNetAdhocctlNickname nickname;
   struct SceNetEtherAddr macAddr;
   unsigned char padding[6];
   unsigned long long lastRecv;
} SceNetAdhocctlPeerInfo;

typedef struct SceNetInitParam { void *memory; int size; int flags; } SceNetInitParam;

#define SCE_NET_ERROR_EBUSY                    0x80410110
#define SCE_ERROR_NET_ADHOC_NOT_ENOUGH_SPACE   0x80410706
#define SCE_ERROR_NET_ADHOC_WOULD_BLOCK        0x80410709
#define SCE_ERROR_NET_ADHOC_TIMEOUT            0x80410715
#define SCE_NET_ADHOC_F_NONBLOCK               0x0001

enum { SCE_SYSMODULE_LOADED = 0 };
enum { SCE_SYSMODULE_NET = 0x0001, SCE_SYSMODULE_PSPNET_ADHOC = 0x0043 };

enum { SCE_NETCTL_STATE_DISCONNECTED, SCE_NETCTL_STATE_CONNECTING,
       SCE_NETCTL_STATE_FINALIZING, SCE_NETCTL_STATE_CONNECTED };

enum { SCE_COMMON_DIALOG_STATUS_NONE = 0, SCE_COMMON_DIALOG_STATUS_RUNNING = 1,
       SCE_COMMON_DIALOG_STATUS_FINISHED = 2 };
enum { SCE_COMMON_DIALOG_RESULT_OK, SCE_COMMON_DIALOG_RESULT_USER_CANCELED,
       SCE_COMMON_DIALOG_RESULT_ABORTED };
enum { SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN = 5,
       SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CREATE = 6,
       SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_JOIN = 7 };

typedef struct SceNpCommunicationId { char data[9]; char term; unsigned char num; char dummy; } SceNpCommunicationId;
typedef struct SceNetCheckDialogParam
{
   unsigned sdkVersion;
   unsigned magic;                    /* stands for commonParam's */
   int mode;
   SceNpCommunicationId npCommunicationId;
   SceNetAdhocctlGroupName *groupName;
   unsigned timeoutUs;
} SceNetCheckDialogParam;
typedef struct SceNetCheckDialogResult { int result; unsigned char reserved[128]; } SceNetCheckDialogResult;

/* As the real one: zero the block and seal it with a magic that depends on
 * the block's ADDRESS -- so the fake can catch a copied parameter block. */
void sceNetCheckDialogParamInit(SceNetCheckDialogParam *param);
int  sceNetCheckDialogInit(SceNetCheckDialogParam *param);
int  sceNetCheckDialogGetStatus(void);
int  sceNetCheckDialogAbort(void);
int  sceNetCheckDialogGetResult(SceNetCheckDialogResult *result);
int  sceNetCheckDialogTerm(void);

int  sceSysmoduleLoadModule(int id);
int  sceSysmoduleUnloadModule(int id);
int  sceSysmoduleIsLoaded(int id);

int  sceNetInit(SceNetInitParam *param);
int  sceNetTerm(void);
int  sceNetCtlInit(void);
void sceNetCtlTerm(void);
int  sceNetCtlAdhocGetState(int *state);
int  sceNetCtlAdhocDisconnect(void);

int  sceNetAdhocInit(void);
int  sceNetAdhocTerm(void);
int  sceNetAdhocctlInit(const SceNetAdhocctlAdhocId *adhocId);
int  sceNetAdhocctlTerm(void);
int  sceNetAdhocctlGetPeerList(int *buflen, void *buf);
int  sceNetAdhocctlGetEtherAddr(SceNetEtherAddr *addr);

int  sceNetAdhocPdpCreate(const SceNetEtherAddr *saddr, SceUShort16 sport,
                          unsigned int bufsize, int flag);
int  sceNetAdhocPdpSend(int id, const SceNetEtherAddr *daddr, SceUShort16 dport,
                        const void *data, int len, unsigned int timeout, int flag);
int  sceNetAdhocPdpRecv(int id, SceNetEtherAddr *saddr, SceUShort16 *sport,
                        void *buf, int *len, unsigned int timeout, int flag);
int  sceNetAdhocPdpDelete(int id, int flag);

/* ---- the test's handles on the fake --------------------------------------- */

/* This process is console `station` (1..15; its MAC is 02:00:00:00:00:NN) in
 * a cell whose consoles' PDP sockets live at UDP 127.0.0.1:base_port+NN.
 * `peers` are the other stations the dialog "finds". */
void vadhoc_fake_setup(int station, uint16_t base_port, const int *peers, int n);

typedef struct vadhoc_fake_opts
{
   int broadcast_works;      /* 0: a send to FF:..:FF goes nowhere */
   int dialog_frames;        /* GetStatus calls before FINISHED */
   int dialog_result;        /* SCE_COMMON_DIALOG_RESULT_* or an error code */
   int stack_already_up;     /* sceNetInit -> EBUSY, sceNetCtlInit ->
                                0x80412102, the modules read as loaded */
   int recv_returns_size;    /* PdpRecv returns the byte count and leaves
                                *len alone (the alternative to H1) */
} vadhoc_fake_opts;
void vadhoc_fake_options(const vadhoc_fake_opts *o);

/* 0 when everything the transport initialised it also terminated, in an
 * order the real stack would accept; else a message in why. */
int  vadhoc_fake_balance(char *why, size_t n);
/* What the last dialog was opened with. */
void vadhoc_fake_last_dialog(int *mode, char group[9], char id[10],
                             unsigned *timeout_us);

#endif /* VADHOC_FAKE_H */
