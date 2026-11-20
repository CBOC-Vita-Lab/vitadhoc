/*
 * Cheats of the Sega player: the list of a game (RetroArch's .cht format, <root>/cheats/<rom name>.cht), the decoding of what a
 * player types (Game Genie, Pro Action Replay, Fusion RAM / ROM, or the database's own parts) into the parts md_host_cheat_add
 * takes (gpsp/vita/tests/cheats/SEGA-CODES.md; the decoders are ports of tools/make_cheatdb.py's, which are copies of Genesis Plus
 * GX's decode_cheat). Portable: no console calls. Applying to the core is the caller's: it resets the core's list and adds the
 * parts of the switched-on entries (active_parts()).
 *
 * Copyright CBOC-Vita. MIT licence (see md/frontend/LICENSE).
 */
#ifndef MD_CHEATS_H
#define MD_CHEATS_H

#include <string>
#include <vector>
#include "savefile.h"

namespace mdp {

enum { CHEAT_E_FORMAT = -1, CHEAT_E_FULL = -2, CHEAT_E_TOO_LONG = -3, CHEAT_E_RANGE = -4, CHEAT_E_IO = -5, CHEAT_E_EXISTS = -6 };
enum { CHEATS_MAX = 64, CHEAT_PARTS_MAX = 16 };

/* md: true for the Mega Drive / Genesis (24 bit addresses, 16 bit words), false for Master System / Game Gear / SG-1000.
 * Returns the number of parts (1..16) and the canonical parts joined by '+' ("FFE4A2:0020+FFC843:0000", "C123:05:FF"), or a CHEAT_E_*. */
int cheat_canon(bool md, const std::string &text, std::string *parts);
/* the Genesis Game Genie letters -> address and word; false when a letter is not one of them */
bool md_gg_decode(const std::string &code, unsigned *address, unsigned *data);
/* Master System / Game Gear Game Genie "DDA-AAB" or "DDA-AAB-RRr" (text without dashes accepted); compare -1 = none */
bool sms_gg_decode(const std::string &code, unsigned *address, unsigned *data, int *compare);

struct CheatEntry {
	std::string label, code;     /* code: as stored in the file */
	std::string parts;           /* canonical parts, "" when the code is not usable on this system */
	std::string extra;           /* other keys of this entry in the file, kept when the file is written back */
	bool on = false, usable = false;
};

class CheatList {
public:
	CheatList() {}
	/* `path`: the .cht of the game; a file that cannot be read is left alone (writes are then refused). Returns the number of entries. */
	int load(const std::string &path, bool md);
	/* entries of another .cht (the app's database export for this game): those whose parts are not in the list yet are added, switched
	 * off. Returns how many were added. Not saved until something is changed. */
	int merge_file(const std::string &path);
	int size() const { return (int) e_.size(); }
	const CheatEntry &at(int i) const { return e_[(size_t) i]; }
	bool dirty() const { return dirty_; }
	bool blocked() const { return blocked_; }
	int set_on(int i, bool on);                                    /* marks dirty; 0 or CHEAT_E_RANGE */
	int add(const std::string &code, const std::string &label);    /* index or CHEAT_E_*; the new entry is on */
	int remove(int i);                                             /* 0 or CHEAT_E_RANGE */
	/* the file's text, and the whole write through SafeFile (verified backup of the old file first); 0 or CHEAT_E_IO */
	std::string text() const;
	int save(SafeFile &sf);
	const std::string &path() const { return path_; }
	const std::string &error() const { return err_; }
	/* the parts of every switched-on usable entry: one '+' joined string per entry (what the core is given) */
	std::vector<std::string> active_parts() const;
	bool is_md() const { return md_; }
	static std::string clean_label(const std::string &s);

private:
	void parse(const std::string &text, std::vector<CheatEntry> &out, std::string &global_extra) const;
	std::vector<CheatEntry> e_;
	std::string path_, global_extra_, err_;
	bool md_ = true, dirty_ = false, blocked_ = false;
};

}  // namespace mdp

#endif
