/* snes_twin --audio-sim: see audio_sim.cpp. Copyright CBOC-Vita. MIT licence. */
#ifndef AUDIO_SIM_H
#define AUDIO_SIM_H

#include <string>

struct AudioSimArgs {
	std::string scenario;    /* S1 S2 S2+ S2- S3 S4 S5 S6 S7 all */
	bool legacy = false;     /* the first player's audio behaviour (A/B) */
	bool tone = true;        /* synthetic tone instead of the core's audio (the ROM must be loaded when false) */
	const char *wav = nullptr;
	double minutes = 0;      /* override the scenario's length */
	double kp = -1, ki = -1; /* controller gains override (>= 0) */
	bool verbose = false;    /* one line per virtual second */
};

/* Returns 0 when every scenario passed, 1 when one failed, 2 on a usage error. */
int audio_sim_run(const AudioSimArgs &a);

#endif
