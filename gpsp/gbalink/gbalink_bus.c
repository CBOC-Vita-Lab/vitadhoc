/* gbalink_bus.c -- the wire between the two GBA link ports (one copy).
 *
 * Two queues of time-stamped events, one per direction.  A port posts while
 * it runs; only one core runs at a time, so a queue never changes under the
 * core reading it.  Events from one sender are in its time order (its clock
 * only moves forward).  See gbalink.h.
 */
#include <string.h>

#include "../gbalink.h"

#define BUS_CAP 1024u   /* events in flight per direction; the stepper keeps
                           the two clocks within a frame of each other, and a
                           port posts only on a change, so a few dozen is the
                           realistic peak.  Full = the session stops. */

typedef struct
{
   gbalink_event ev[BUS_CAP];
   unsigned head, count;
} bus_queue;

static bus_queue q[GBALINK_SLOTS];      /* q[s] = events FOR slot s */
static gbalink_bus_stats bst;

void gbalink_bus_reset(void)
{
   memset(q, 0, sizeof(q));
   memset(&bst, 0, sizeof(bst));
}

int gbalink_bus_post(unsigned from_slot, const gbalink_event *ev)
{
   bus_queue *d = &q[(from_slot ^ 1) & 1];
   if (d->count >= BUS_CAP)
   {
      bst.overflow++;
      return -1;
   }
   d->ev[(d->head + d->count) % BUS_CAP] = *ev;
   d->count++;
   bst.posted[from_slot & 1]++;
   if (d->count > bst.depth_max)
      bst.depth_max = d->count;
   return 0;
}

static int is_reply(const gbalink_event *ev)
{
   return ev->kind == GBALINK_EV_MP_DATA || ev->kind == GBALINK_EV_N_DATA;
}

/* Replies are taken out of order by gbalink_bus_take_reply; whatever is left
 * at the head that is a reply nobody asked for is dropped here (a reply to a
 * transfer this port abandoned). */
const gbalink_event *gbalink_bus_peek(unsigned to_slot)
{
   bus_queue *d = &q[to_slot & 1];
   unsigned i;
   for (i = 0; i < d->count; i++)
   {
      const gbalink_event *ev = &d->ev[(d->head + i) % BUS_CAP];
      if (!is_reply(ev))
         return ev;
   }
   return NULL;
}

void gbalink_bus_pop(unsigned to_slot)
{
   /* Remove the event gbalink_bus_peek returned: the first non-reply. */
   bus_queue *d = &q[to_slot & 1];
   unsigned i;
   for (i = 0; i < d->count; i++)
   {
      unsigned at = (d->head + i) % BUS_CAP;
      if (!is_reply(&d->ev[at]))
      {
         unsigned j;
         for (j = i; j > 0; j--)   /* shift the replies in front up by one */
            d->ev[(d->head + j) % BUS_CAP] = d->ev[(d->head + j - 1) % BUS_CAP];
         d->head = (d->head + 1) % BUS_CAP;
         d->count--;
         return;
      }
   }
}

int gbalink_bus_take_reply(unsigned to_slot, unsigned kind, gbalink_event *out)
{
   bus_queue *d = &q[to_slot & 1];
   unsigned i;
   for (i = 0; i < d->count; i++)
   {
      unsigned at = (d->head + i) % BUS_CAP;
      if (d->ev[at].kind == kind)
      {
         unsigned j;
         *out = d->ev[at];
         for (j = i; j > 0; j--)
            d->ev[(d->head + j) % BUS_CAP] = d->ev[(d->head + j - 1) % BUS_CAP];
         d->head = (d->head + 1) % BUS_CAP;
         d->count--;
         return 1;
      }
   }
   return 0;
}

void gbalink_bus_get_stats(gbalink_bus_stats *out)
{
   *out = bst;
}

/* ---- the wire in a save state of a pair (gbalink.h) ------------------------
 * Counts, then the queued events oldest first: a few dozen events at most,
 * so the image is small whatever BUS_CAP is. */
#define BUS_STATE_MAGIC 0x31534247u     /* "GBS1" */

unsigned gbalink_bus_state_size(void)
{
   return (unsigned)(3 * sizeof(uint32_t) +
                     (q[0].count + q[1].count) * sizeof(gbalink_event));
}

unsigned gbalink_bus_state_save(void *out, unsigned cap)
{
   unsigned need = gbalink_bus_state_size(), s, i;
   uint8_t *p = (uint8_t *)out;
   uint32_t hdr[3];
   if (cap < need)
      return 0;
   hdr[0] = BUS_STATE_MAGIC;
   hdr[1] = q[0].count;
   hdr[2] = q[1].count;
   memcpy(p, hdr, sizeof(hdr));
   p += sizeof(hdr);
   for (s = 0; s < GBALINK_SLOTS; s++)
      for (i = 0; i < q[s].count; i++, p += sizeof(gbalink_event))
         memcpy(p, &q[s].ev[(q[s].head + i) % BUS_CAP], sizeof(gbalink_event));
   return need;
}

int gbalink_bus_state_load(const void *in, unsigned n)
{
   const uint8_t *p = (const uint8_t *)in;
   uint32_t hdr[3];
   unsigned s, i;
   if (n < sizeof(hdr))
      return -1;
   memcpy(hdr, p, sizeof(hdr));
   p += sizeof(hdr);
   if (hdr[0] != BUS_STATE_MAGIC || hdr[1] > BUS_CAP || hdr[2] > BUS_CAP ||
       n != sizeof(hdr) + (hdr[1] + hdr[2]) * sizeof(gbalink_event))
      return -1;
   gbalink_bus_reset();
   for (s = 0; s < GBALINK_SLOTS; s++)
   {
      q[s].count = hdr[1 + s];
      for (i = 0; i < q[s].count; i++, p += sizeof(gbalink_event))
         memcpy(&q[s].ev[i], p, sizeof(gbalink_event));
   }
   return 0;
}
