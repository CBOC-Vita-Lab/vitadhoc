/* pad_port_pick.h -- which sceCtrl port is player 2's pad (plat.h has the
 * rule and why).  Pure: the port types as sceCtrlGetControllerPortInfo
 * reports them (0 = nobody; 1 handheld pad, 2 PS TV virtual, 4 DualShock 3,
 * 8 DualShock 4) and whether this is a PS TV, so the host build tests it. */
#ifndef GBADHOC_PAD_PORT_PICK_H
#define GBADHOC_PAD_PORT_PICK_H

/* The port (1..4) of player 2's controller, or 0 for none. */
static inline int plat_pick_pad2_port(const unsigned char type[5], int is_tv)
{
   int p;
   /* ports 2, 3, 4 first: on a PS TV port 1 is the first pad again, and on a
    * handheld that pairs two pads the second one is 2 */
   for (p = 2; p <= 4; p++)
      if (type[p] != 0)
         return p;
   /* one external pad on a handheld: port 1, the console's own controls stay
    * player 1's */
   if (!is_tv && type[1] != 0)
      return 1;
   return 0;
}

#endif
