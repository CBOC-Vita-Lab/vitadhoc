/*
 * playerkit in-game menu (the control centre). Same pages, order and wording
 * as the GBA side's in-game menu:
 *
 *   Resume | Save states | Play together | Cheats | Settings | Reset game | Game list | Exit
 *   Settings > Picture | Controls | Speed
 *
 * Data driven: the host says which features it has (Caps) and gets callbacks;
 * rows it does not support are shown greyed with the reason, not hidden. Pad
 * navigation with repeat and touch (every row and button is a target at least
 * 80 px tall, with a pressed highlight and a tap flash).
 *
 * Usage per frame while open:  ev = menu.update(input);  (host draws its frozen game picture)  menu.draw(gfx, text);
 *
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 */
#ifndef PK_MENU_H
#define PK_MENU_H

#include "pk_gfx.h"
#include "pk_input.h"
#include "pk_notice.h"
#include "pk_states.h"
#include <stdint.h>
#include <string>
#include <vector>

namespace pk {

/* what the host supports; a bit missing = the row is greyed with a reason */
enum Caps : uint32_t {
	CAP_STATES = 1u << 0,
	CAP_UNDO = 1u << 1,           /* undo of a state load (needs capture_state) */
	CAP_RESET = 1u << 2,
	CAP_SHAPE = 1u << 3,
	CAP_WIDE = 1u << 4,           /* wide view is wired (otherwise a greyed "coming" row) */
	CAP_QUALITY = 1u << 5,
	CAP_SCANLINES = 1u << 6,
	CAP_LCD_GRID = 1u << 7,
	CAP_GLOW = 1u << 8,
	CAP_PIC_PER_GAME = 1u << 9,   /* "Apply to" for the picture settings */
	CAP_CONTROLS = 1u << 10,      /* remapping */
	CAP_TURBO = 1u << 11,
	CAP_FF = 1u << 12,
	CAP_FF_SOUND = 1u << 13,
	CAP_FF_MODE = 1u << 14,
	CAP_REWIND = 1u << 15,
	CAP_CHEATS = 1u << 16,
	CAP_PLAY_TOGETHER = 1u << 17,
	CAP_ALL_BASIC = CAP_STATES | CAP_UNDO | CAP_RESET | CAP_SHAPE | CAP_QUALITY | CAP_SCANLINES | CAP_LCD_GRID | CAP_GLOW |
	                CAP_PIC_PER_GAME | CAP_CONTROLS | CAP_TURBO
};

/* rows whose value the host owns; ids are stable */
enum RowId {
	ROW_PIC_SHAPE = 0, ROW_PIC_WIDE, ROW_PIC_QUALITY, ROW_PIC_SCANLINES, ROW_PIC_GRID, ROW_PIC_GLOW, ROW_PIC_APPLY,
	ROW_CTL_STICK_L, ROW_CTL_STICK_R, ROW_CTL_REAR, ROW_CTL_MAPPING, ROW_CTL_TURBO_A, ROW_CTL_TURBO_B, ROW_CTL_TURBO_MODE,
	ROW_CTL_APPLY, ROW_CTL_RESET,
	ROW_SPD_FF, ROW_SPD_SOUND, ROW_SPD_MODE, ROW_SPD_REWIND,
	ROW_COUNT
};

/* the values (RowId order): ROW_PIC_SHAPE 0..4 = Original, Fit, Zoom, Full screen, Pixel perfect; ROW_PIC_WIDE 0..2 = Auto, On, Off;
 * ROW_PIC_QUALITY 0 Sharp, 1 Smooth; scanlines/grid/glow 0..10 (0 = Off); ROW_*_APPLY 0 All games, 1 This game;
 * ROW_SPD_FF 0 = 2x, 1 = 3x; ROW_SPD_SOUND 0 Off 1 On; ROW_SPD_MODE 0 Hold 1 Toggle; ROW_SPD_REWIND 0 Off 1 On. */

struct CheatItem {
	std::string name;
	bool on;
	bool available;     /* false: shown greyed (a code this player cannot use) */
	CheatItem() : on(false), available(true) {}
};

class MenuHost {
public:
	virtual ~MenuHost() {}
	/* a picker or slider changed (the menu already holds the new value) */
	virtual void on_change(int row_id, int value) { (void) row_id; (void) value; }
	/* "Reset game" was confirmed; the menu closes after it */
	virtual void on_reset() {}
	/* the machine's state as bytes, for Save here and for the undo copy; false if it cannot */
	virtual bool capture_state(std::vector<uint8_t> &out) { (void) out; return false; }
	/* put a state back; false if the core refused it */
	virtual bool restore_state(const uint8_t *data, size_t n) { (void) data; (void) n; return false; }
	/* the picture on screen now, for the thumbnails (valid until the next call) */
	virtual bool current_frame(Frame *f) { (void) f; return false; }
	virtual int64_t play_seconds() { return -1; }
	/* cheats */
	virtual void list_cheats(std::vector<CheatItem> &out) { (void) out; }
	virtual void set_cheat(int index, bool on) { (void) index; (void) on; }
	/* a state was saved / loaded / deleted through the menu (for the session report) */
	virtual void on_state_touched(int slot) { (void) slot; }
	/* the profile or a mapping changed and was stored (rc) */
	virtual void on_controls_changed(WriteResult rc) { (void) rc; }
};

struct MenuConfig {
	std::string game_name;          /* title at the top */
	std::string console_label;      /* "Super Nintendo" */
	std::string exit_label = "Exit";
	std::string exit_help = "Close VitAdhoc";
	std::string play_together_reason = "Not available for this console: the player has no link";
	std::string unsupported_reason = "Not available in this player";
	uint32_t caps = CAP_ALL_BASIC;
	uint32_t accent = 0x7B6CFF;     /* 0xRRGGBB */
	bool enter_is_cross = true;
	int players = 4;                /* pads the mapping page offers */
};

enum MenuEvent { MENU_NONE = 0, MENU_RESUME, MENU_GAME_LIST, MENU_EXIT, MENU_RESET_DONE };

struct TouchPoint {
	bool down = false;
	float x = 0, y = 0;     /* native 960 x 544 */
};

struct MenuInput {
	PadSample pads[MAX_PLAYERS];
	TouchPoint touch;
	int dt_ms = 16;
};

class Menu {
public:
	Menu();
	~Menu();
	/* `states` may be null (then Save states is greyed). The Gfx given to draw() must outlive the Menu, or call release_textures() first. */
	void configure(const MenuConfig &c, MenuHost *host, States *states);
	/* the pad profile editing; any of them may be null (then Controls is greyed) */
	void attach_input(Mapper *mapper, ProfileStore *store, const PadModel *model, Profile *profile);
	void set_value(int row_id, int value);
	int value(int row_id) const;
	void set_row_note(int row_id, const std::string &reason);   /* greys the row with this reason ("" clears) */

	/* opens on the control centre; nav_player is the pad that asked for it */
	void open(int nav_player = 0);
	bool is_open() const { return open_; }
	MenuEvent update(const MenuInput &in);
	void draw(Gfx *g, Text *t);
	void release_textures();
	Notices &notices() { return notices_; }

	/* introspection, for tests */
	enum PageId { PAGE_TOP = 0, PAGE_STATES, PAGE_SETTINGS, PAGE_PICTURE, PAGE_CONTROLS, PAGE_MAPPING, PAGE_SPEED, PAGE_CHEATS };
	PageId page() const { return page_; }
	int cursor() const { return cursor_; }
	void goto_page(PageId p);
	void select_slot(int slot);        /* 1..10 */
	int selected_slot() const { return slot_; }
	bool modal_open() const { return modal_ != 0; }
	int modal_kind() const { return modal_; }
	enum { REGION_CARD = 1, REGION_ROW, REGION_ARROW_L, REGION_ARROW_R, REGION_SLIDER, REGION_SLOT, REGION_BUTTON, REGION_OUTSIDE };
	struct Region {
		float x, y, w, h;      /* the touch area */
		int kind, a;
		float vx, vy, vw, vh;  /* what is drawn (for the pressed highlight and the flash) */
		float radius;
	};
	const std::vector<Region> &regions() const { return regions_; }
	float min_touch_height() const;    /* smallest height among regions fully on screen and not clipped, for tests */
	int row_count() const { return (int) rows_.size(); }
	const std::string &row_label(int i) const { return rows_[(size_t) i].label; }
	std::string row_value_text(int i) const;
	bool row_enabled(int i) const { return rows_[(size_t) i].enabled; }
	const std::string &row_reason(int i) const { return rows_[(size_t) i].reason; }

private:
	enum { K_CHOICE = 1, K_SLIDER, K_ACTION, K_LINK, K_INFO };
	struct Row {
		int id;                       /* RowId, or -1 */
		int kind;
		std::string label, help, value_text;
		std::vector<std::string> choices;
		int vmin, vmax, value;
		bool enabled;
		std::string reason;
		int tag;                      /* a PageId for links, an action index for mapping rows, a cheat index */
		Row() : id(-1), kind(K_INFO), vmin(0), vmax(0), value(0), enabled(true), tag(0) {}
	};
	enum { M_NONE = 0, M_OVERWRITE, M_DELETE, M_LIST, M_EXIT, M_RESET, M_CTL_RESET, M_CAPTURE };
	enum { CARD_RESUME = 0, CARD_STATES, CARD_TOGETHER, CARD_CHEATS, CARD_SETTINGS, CARD_RESET, CARD_LIST, CARD_EXIT, CARD_COUNT };
	enum { BTN_PRIMARY = 1, BTN_SECONDARY, BTN_LOAD, BTN_SAVE, BTN_DELETE, BTN_UNDO, BTN_CLEAR, BTN_CANCEL };

	/* page and rows */
	void build_rows();
	void set_page(PageId p, bool push);
	void back();
	void clamp_cursor();
	void ensure_visible();
	float view_h() const;
	float content_h() const;
	void set_row_value(Row &r, int v);
	void apply_value(Row &r, int v);
	void activate_row(int i);
	void step_row(int i, int dir);
	bool card_visible(int c) const;
	int step_card(int from, int dir) const;
	int visible_card_count() const;
	bool card_enabled(int c, std::string *why) const;
	void activate_card(int c);
	std::string card_help(int c) const;
	void controls_changed();
	void map_row_text(Row &r, int action) const;
	/* states */
	void refresh_slots();
	void do_save(int slot);
	void do_load(int slot);
	void do_delete(int slot);
	void do_undo();
	void request_save(int slot);
	/* modal */
	void open_modal(int kind, int arg);
	void modal_answer(bool yes);
	void begin_capture(int action);
	void capture_update(const MenuInput &in);
	/* input */
	void layout();
	void grow_to_pitch();
	void pad_update(const MenuInput &in);
	void touch_update(const MenuInput &in);
	void tap(const Region &r, float x);
	int hit(float x, float y) const;
	void add_region(float x, float y, float w, float h, int kind, int a, float radius, bool clip_to_view);
	void notify(const std::string &text, NoticeKind k);
	/* drawing */
	void draw_backdrop(Gfx *g);
	void draw_header(Gfx *g, Text *t, const char *title, const char *sub);
	void draw_hints(Gfx *g, Text *t);
	void draw_top(Gfx *g, Text *t);
	void draw_rows(Gfx *g, Text *t);
	void draw_states(Gfx *g, Text *t);
	void draw_modal(Gfx *g, Text *t);
	void draw_overlays(Gfx *g);
	void upload_textures(Gfx *g);
	uint32_t accent(int a) const;
	uint32_t accent_deep() const;

	MenuConfig cfg_;
	MenuHost *host_;
	States *states_;
	Mapper *mapper_;
	ProfileStore *store_;
	const PadModel *model_;
	Profile *profile_;
	int vals_[ROW_COUNT];
	std::string notes_[ROW_COUNT];
	bool open_;
	PageId page_;
	std::vector<PageId> stack_;
	std::vector<int> cursor_stack_;
	int cursor_;
	int slot_;
	int nav_player_;
	int modal_, modal_arg_;
	std::vector<Row> rows_;
	std::vector<CheatItem> cheats_;
	std::vector<Region> regions_;
	float scroll_, scroll_target_;
	int age_ms_;
	int map_player_;
	/* pad repeat and edges */
	uint32_t prev_buttons_, ignore_mask_;
	bool first_update_;
	int rep_ms_[4];
	/* touch */
	bool t_down_;
	int t_region_;
	float t_x0_, t_y0_, t_x_, t_y_, t_scroll0_;
	bool t_scrolling_, t_slider_;
	Region flash_;
	int flash_ms_;
	bool have_flash_;
	/* states data */
	SlotInfo slots_[STATE_SLOTS];
	std::vector<uint16_t> thumb_px_[STATE_SLOTS];
	std::vector<uint16_t> shot_px_;
	int shot_w_, shot_h_, shot_slot_;
	bool undo_avail_;
	/* textures (made in draw) */
	Gfx *gfx_;
	Texture *thumb_tex_[STATE_SLOTS];
	Texture *shot_tex_;
	bool tex_dirty_;
	/* capture */
	int cap_action_, cap_ms_, cap_window_ms_;
	uint32_t cap_bits_;
	/* results */
	MenuEvent pending_;
	Notices notices_;
};

}  // namespace pk

#endif
