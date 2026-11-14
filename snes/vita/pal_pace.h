/*
 * Frames owed per display refresh for a console whose frame rate is below the display's (SNES PAL: 50.007 Hz on a
 * 59.94 Hz Vita screen). One frame per refresh would run the game 20 % fast, so a refresh runs 0 or 1 frames: 5 of
 * every 6, spread evenly by an exact accumulator of frame time against refresh time (the NES core's mux does the
 * same in gpsp/nescore/gbcore_mux.c). A refresh that owes none shows the last picture again. A refresh the loop
 * missed (elapsed > 1) owes the frames of all the refreshes it covers. Header only so the Linux twin proves the very
 * code the Vita runs. Copyright CBOC-Vita. MIT licence.
 */
#ifndef SNES_PAL_PACE_H
#define SNES_PAL_PACE_H

struct FramePace {
	double ratio = 1.0, acc = 0.0;
	/* fps: the console's frame rate; display_hz: the screen's. A console at or above the display rate keeps one per refresh. */
	void set(double fps, double display_hz) { ratio = fps / display_hz; acc = 0.0; }
	/* frames to run for `refreshes` display refreshes */
	int owed(unsigned refreshes)
	{
		int n = 0;
		for (unsigned i = 0; i < refreshes; i++) {
			acc += ratio;
			if (acc >= 1.0 - 1e-9) { acc -= 1.0; n++; }
		}
		return n;
	}
};

#endif
