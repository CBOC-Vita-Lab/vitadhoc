/* ThreadSanitizer run of the frame hand-over mailbox (make tsan, part of make test).
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "handoff_stress.h"

int main()
{
	int bad = 0;
	struct { const char *name; pkstress::Cfg c; } runs[6];
	runs[0].name = "full, depth 1"; runs[0].c.depth = 1; runs[0].c.modes = false;
	runs[1].name = "full, depth 2"; runs[1].c.depth = 2;
	runs[2].name = "full, depth 4, no delays"; runs[2].c.depth = 4; runs[2].c.consumer_delay_us = 0; runs[2].c.producer_delay_us = 0;
	runs[3].name = "all modes, depth 2"; runs[3].c.depth = 2; runs[3].c.modes = true; runs[3].c.seed = 3;
	runs[4].name = "all modes, depth 3, slow consumer"; runs[4].c.depth = 3; runs[4].c.modes = true; runs[4].c.consumer_delay_us = 600; runs[4].c.seed = 5; runs[4].c.frames = 1200;
	runs[5].name = "all modes, depth 1, fixed"; runs[5].c.depth = 1; runs[5].c.modes = true; runs[5].c.adaptive = false; runs[5].c.seed = 9;
	for (int i = 0; i < 6; i++) {
		pkstress::Out o = pkstress::run(runs[i].c);
		printf("tsan run %d (%s): produced %u shown %u dropped %u failures %d\n", i, runs[i].name, o.produced, o.shown, o.dropped, o.failures);
		bad += o.failures;
	}
	printf("handoff_tsan: %s\n", bad ? "FAILED" : "ok");
	return bad ? 1 : 0;
}
