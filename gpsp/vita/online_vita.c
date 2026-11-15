/* online_vita.c -- one online connection (2.0); see online_vita.h.
 *
 * W0 scaffold: every call says "not available" (nothing begins, the stage
 * stays ON_IDLE).  Package M1 writes the state machine of the plan's 2.4. */
#include <string.h>

#include "online_vita.h"

void online_setup(const online_cfg *cfg) { (void)cfg; }

int online_host(lan_transport *t, const online_desc *room)
{
   (void)t; (void)room;
   return -1;
}

int online_join(lan_transport *t, const char *code, const online_desc *me)
{
   (void)t; (void)code; (void)me;
   return -1;
}

int online_step(void) { return ON_IDLE; }

void online_get(online_view *out)
{
   if (out)
      memset(out, 0, sizeof *out);
}

void online_linked(void) { }
void online_stop(void) { }
int  online_active(void) { return 0; }

int online_card_begin(const char *code)
{
   (void)code;
   return -1;
}

int online_card_poll(online_reply *card)
{
   if (card)
      memset(card, 0, sizeof *card);
   return 0;
}

void online_card_cancel(void) { }
