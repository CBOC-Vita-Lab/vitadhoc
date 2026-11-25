/* See pk_menu.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_menu.h"
#include "pk_icons.h"
#include "pk_uilook.h"
#include "pk_i18n.h"
#include <math.h>
#include <string.h>
#include <algorithm>

namespace pk {

namespace {

/* the main app's colours (0xRRGGBB) */
const uint32_t C_BG = 0x060914, C_TEXT = 0xF5F7FC, C_TEXT2 = 0xA7B0C6, C_TEXT3 = 0x8F99B3, C_INK = 0x0A0F1F,
               C_SHEET = 0x0D1428, C_RED = 0xFF6B6B, C_PLATE = 0x1C2646;

/* layout, native pixels */
const float PANEL_X = 20, PANEL_Y = 96, PANEL_W = 920, PANEL_H = 432;
const float VX = 36, VY = 108, VW = 888;
const float PITCH = 80, RH = 72;
const float HELP_Y = 444;
const float CARD_W = 104, CARD_H = 124, CARD_GAP = 8, SHEET_H = 262;
const int SHEET_MS = 150;
const int ID_MAP_PLAYER = -100;
const int ID_CHEAT_ADD = -200;
const int ID_CHEAT_SEARCH = -201, ID_CS_SIZE = -202, ID_CS_NEW = -203, ID_CS_RESULTS = -204, ID_CS_OP = -210 /* .. -217 */;
const int TAG_MAP_RESET = -2;

uint32_t col(uint32_t rgb, int a = 255) { return PK_HEX(rgb, a); }

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

uint32_t mix_rgb(uint32_t a, uint32_t b, float t)
{
	int r = (int) (((a >> 16) & 255) * (1 - t) + ((b >> 16) & 255) * t), g = (int) (((a >> 8) & 255) * (1 - t) + ((b >> 8) & 255) * t),
	    bl = (int) ((a & 255) * (1 - t) + (b & 255) * t);
	return (uint32_t) ((r << 16) | (g << 8) | bl);
}

}  // namespace

Menu::Menu()
	: host_(0), states_(0), mapper_(0), store_(0), model_(0), profile_(0), open_(false), page_(PAGE_TOP), cursor_(0), slot_(1),
	  nav_player_(0), modal_(M_NONE), modal_arg_(0), scroll_(0), scroll_target_(0), age_ms_(0), map_player_(0), prev_buttons_(0),
	  ignore_mask_(0), first_update_(true), t_down_(false), t_region_(-1), t_x0_(0), t_y0_(0), t_x_(0), t_y_(0), t_scroll0_(0),
	  t_scrolling_(false), t_slider_(false), flash_ms_(0), have_flash_(false), shot_w_(0), shot_h_(0), shot_slot_(0), undo_avail_(false),
	  gfx_(0), shot_tex_(0), tex_dirty_(false), bt_hold_ms_(0), bt_touch_ms_(0), cap_action_(-1), cap_ms_(0), cap_window_ms_(0), cap_bits_(0), pending_(MENU_NONE)
{
	memset(&cs_, 0, sizeof cs_);
	lang_serial_ = i18n_serial();
	hints_left_ = 960;
	cs_ready_ = false; cs_size_ = 0; cs_op_ = -1; cs_n_ = 0;
	memset(vals_, 0, sizeof vals_);
	memset(allowed_, 0, sizeof allowed_);
	used_slots_ = 0;
	for (int i = 0; i < STATE_SLOTS; i++) { pending_slot_[i].on = false; pending_slot_[i].saved = 0; pending_slot_[i].play = -1; }
	memset(rep_ms_, 0, sizeof rep_ms_);
	for (int i = 0; i < STATE_SLOTS; i++) thumb_tex_[i] = 0;
	memset(&flash_, 0, sizeof flash_);
	vals_[ROW_PIC_SCANLINES] = vals_[ROW_PIC_GRID] = 0;
	vals_[ROW_PIC_GLOW] = 5;
	vals_[ROW_SPD_SOUND] = 1;
}

Menu::~Menu() { release_textures(); pk_cs_free(&cs_); }

void Menu::release_textures()
{
	if (gfx_) {
		for (int i = 0; i < STATE_SLOTS; i++) if (thumb_tex_[i]) { gfx_->tex_free(thumb_tex_[i]); thumb_tex_[i] = 0; }
		if (shot_tex_) { gfx_->tex_free(shot_tex_); shot_tex_ = 0; }
	}
	tex_dirty_ = true;
}

void Menu::configure(const MenuConfig &c, MenuHost *host, States *states)
{
	cfg_ = c;
	host_ = host;
	states_ = states;
	if (!(cfg_.caps & CAP_STATES) || !states_) cfg_.caps &= ~(CAP_STATES | CAP_UNDO);
}

void Menu::attach_input(Mapper *m, ProfileStore *s, const PadModel *model, Profile *p)
{
	mapper_ = m; store_ = s; model_ = model; profile_ = p;
}

Menu::HostRow *Menu::host_row(int id)
{
	for (size_t i = 0; i < host_rows_.size(); i++) if (host_rows_[i].id == id) return &host_rows_[i];
	return 0;
}

void Menu::set_value(int id, int v)
{
	if (HostRow *h = host_row(id)) { h->value = v; if (open_) build_rows(); return; }
	if (id < 0 || id >= ROW_COUNT) return;
	vals_[id] = v;
	if (open_) build_rows();
}

int Menu::value(int id) const
{
	for (size_t i = 0; i < host_rows_.size(); i++) if (host_rows_[i].id == id) return host_rows_[i].value;
	return (id >= 0 && id < ROW_COUNT) ? vals_[id] : 0;
}

void Menu::set_choices_allowed(int id, uint32_t mask)
{
	if (id >= 0 && id < ROW_COUNT) { allowed_[id] = mask; if (open_) build_rows(); }
}

void Menu::add_host_row(int page, int id, const std::string &label, const std::string &help,
                        const std::vector<std::string> &choices, int value, int before_row)
{
	if (id < ROW_HOST_BASE || choices.empty()) return;
	HostRow *h = host_row(id);
	HostRow n;
	n.page = page; n.id = id; n.before = before_row; n.label = label; n.help = help; n.choices = choices; n.value = value;
	if (h) *h = n; else host_rows_.push_back(n);
	if (open_) build_rows();
}

void Menu::refresh()
{
	if (!open_) return;
	if (page_ == PAGE_STATES) refresh_slots();
	build_rows();
	clamp_cursor();
	ensure_visible();
	layout();
}

void Menu::state_write_done(int slot, const SaveReport &rep)
{
	if (slot >= 1 && slot <= STATE_SLOTS) pending_slot_[slot - 1].on = false;
	if (rep.status != ST_OK) notify(rep.text(), NOTICE_ERROR);       /* the previous state in the slot is untouched */
	else {
		if (host_) host_->on_state_touched(slot);
		notify(strf(tr("menu.state_saved_to_slot"), slot), NOTICE_OK);
	}
	if (open_) { refresh_slots(); if (page_ == PAGE_STATES) select_slot(slot_); }
}

void Menu::set_row_note(int id, const std::string &reason)
{
	if (id >= 0 && id < ROW_COUNT) { notes_[id] = reason; if (open_) build_rows(); }
}

uint32_t Menu::accent(int a) const { return PK_HEX(cfg_.accent, a); }
/* the accent as TEXT on the sheet: lightened toward white until it reads at 4.5:1 (a dark accent, like the SNES violet, was 3.4:1) */
uint32_t Menu::accent_text() const
{
	auto lin = [](int c) { float v = (float) c / 255.0f; return v * v; };       /* gamma 2 stands in for sRGB's 2.4: no libm powf (it changed the console link layout) */
	auto lum = [&](uint32_t rgb) { return 0.2126f * lin((int) ((rgb >> 16) & 255)) + 0.7152f * lin((int) ((rgb >> 8) & 255)) + 0.0722f * lin((int) (rgb & 255)); };
	const float lb = lum(C_SHEET) + 0.03f;      /* the sheet is 92% opaque: a bright game shows through a little */
	uint32_t c = cfg_.accent;
	for (int i = 0; i <= 20; i++) {
		c = mix_rgb(cfg_.accent, 0xFFFFFF, (float) i * 0.05f);
		if ((lum(c) + 0.05f) / (lb + 0.05f) >= 4.9f) break;
	}
	return col(c);
}
uint32_t Menu::accent_deep() const { return col(mix_rgb(cfg_.accent, C_INK, 0.5f)); }

void Menu::notify(const std::string &text, NoticeKind k) { notices_.push(text, k); }

/* ---- opening and pages --------------------------------------------------------- */

void Menu::open(int nav_player)
{
	open_ = true;
	lang_serial_ = i18n_serial();
	nav_player_ = nav_player < 0 || nav_player >= MAX_PLAYERS ? 0 : nav_player;
	page_ = PAGE_TOP;
	stack_.clear();
	cursor_stack_.clear();
	cursor_ = 0;
	slot_ = 1;
	modal_ = M_NONE;
	scroll_ = scroll_target_ = 0;
	age_ms_ = 0;
	first_update_ = true;
	prev_buttons_ = 0;
	ignore_mask_ = 0;
	memset(rep_ms_, 0, sizeof rep_ms_);
	t_down_ = false;
	t_region_ = -1;
	have_flash_ = false;
	pending_ = MENU_NONE;
	map_player_ = 0;
	refresh_slots();
	build_rows();
	layout();
}

void Menu::goto_page(PageId p)
{
	stack_.clear();
	cursor_stack_.clear();
	if (p == PAGE_PICTURE || p == PAGE_CONTROLS || p == PAGE_SPEED) { stack_.push_back(PAGE_TOP); cursor_stack_.push_back(CARD_SETTINGS); stack_.push_back(PAGE_SETTINGS); cursor_stack_.push_back(0); }
	else if (p == PAGE_MAPPING) { stack_.push_back(PAGE_TOP); cursor_stack_.push_back(CARD_SETTINGS); stack_.push_back(PAGE_SETTINGS); cursor_stack_.push_back(1); stack_.push_back(PAGE_CONTROLS); cursor_stack_.push_back(3); }
	else if (p == PAGE_CHEAT_SEARCH) { stack_.push_back(PAGE_TOP); cursor_stack_.push_back(CARD_CHEATS); stack_.push_back(PAGE_CHEATS); cursor_stack_.push_back(0); }
	else if (p != PAGE_TOP) { stack_.push_back(PAGE_TOP); cursor_stack_.push_back(p == PAGE_STATES ? CARD_STATES : p == PAGE_CHEATS ? CARD_CHEATS : CARD_SETTINGS); }
	set_page(p, false);
}

void Menu::set_page(PageId p, bool push)
{
	if (push) { stack_.push_back(page_); cursor_stack_.push_back(cursor_); }
	page_ = p;
	cursor_ = 0;
	scroll_ = scroll_target_ = 0;
	if (p == PAGE_STATES) {
		refresh_slots();
		cursor_ = slot_ - 1;
	}
	if (p == PAGE_MAPPING) map_player_ = 0;
	build_rows();
	ensure_visible();
	scroll_ = scroll_target_;
	layout();
}

void Menu::back()
{
	if (stack_.empty()) { pending_ = MENU_RESUME; return; }
	PageId p = stack_.back();
	int c = cursor_stack_.back();
	stack_.pop_back();
	cursor_stack_.pop_back();
	page_ = p;
	cursor_ = c;
	scroll_ = scroll_target_ = 0;
	build_rows();
	clamp_cursor();
	ensure_visible();
	scroll_ = scroll_target_;
	layout();
}

float Menu::view_h() const { return page_ == PAGE_STATES ? 400.0f : 320.0f; }

float Menu::content_h() const
{
	if (page_ == PAGE_STATES) return STATE_SLOTS * PITCH;
	return (float) rows_.size() * PITCH;
}

void Menu::clamp_cursor()
{
	int n = page_ == PAGE_TOP ? (int) CARD_COUNT : page_ == PAGE_STATES ? (int) STATE_SLOTS : (int) rows_.size();
	if (n <= 0) { cursor_ = 0; return; }
	if (cursor_ < 0) cursor_ = 0;
	if (cursor_ >= n) cursor_ = n - 1;
}

void Menu::ensure_visible()
{
	if (page_ == PAGE_TOP) return;
	float top = cursor_ * PITCH, bot = top + PITCH;
	float maxs = std::max(0.0f, content_h() - view_h());
	if (top < scroll_target_) scroll_target_ = top;
	if (bot > scroll_target_ + view_h()) scroll_target_ = bot - view_h();
	scroll_target_ = clampf(scroll_target_, 0, maxs);
}

/* ---- the rows of each page -------------------------------------------------------- */

/* the main app's help for each Image quality (gpsp/psp/ui_psp.c, SET_QUALITY): one wording on every console */
static const char *quality_help(int v)
{
	switch (v) {
	case IMGQ_ORIGINAL: return tr("menu.the_game_s_own_pixels_evenly");
	case IMGQ_ENHANCED_2X: return tr("menu.smoothed_at_twice_the_game_s");
	case IMGQ_ENHANCED_3X: return tr("menu.smoothed_at_three_times_its_size");
	case IMGQ_ENHANCED_4X: return tr("menu.smoothed_at_four_times_its_size");
	case IMGQ_MMPX_2X: return tr("menu.mmpx_at_twice_its_size_crisp");
	default: return tr("menu.auto_adapts_to_keep_60_fps");
	}
}

static const char *help_for(int id)
{
	switch (id) {
	case ROW_PIC_SHAPE: return tr("menu.how_large_the_game_is_drawn");
	case ROW_PIC_WIDE: return tr("menu.shows_more_of_the_world_than");
	case ROW_PIC_QUALITY: return quality_help(IMGQ_AUTO);
	case ROW_PIC_SCANLINES: return tr("menu.dark_lines_between_the_rows_like");
	case ROW_PIC_GRID: return tr("menu.a_faint_pixel_grid_like_a");
	case ROW_PIC_GLOW: return tr("menu.a_soft_glow_beside_the_picture");
	case ROW_PIC_CRT: return tr("menu.coloured_stripes_like_a_crt_tube");
	case ROW_PIC_SCREEN: return tr("menu.which_vita_screen_this_is_oled");
	case ROW_UI_TEXT: return tr("menu.bigger_text_in_the_menus_of");
	case ROW_UI_CONTRAST: return tr("menu.brighter_text_and_clearer_edges_in");
	case ROW_UI_LANGUAGE: return tr("menu.ui.language_help");
	case ROW_PIC_APPLY: return tr("menu.keep_these_picture_settings_for_every");
	case ROW_CTL_STICK_L: return tr("menu.what_the_left_stick_does_nothing");
	case ROW_CTL_STICK_R: return tr("menu.what_the_right_stick_does_nothing");
	case ROW_CTL_REAR: return tr("menu.lets_the_rear_touch_panel_s");
	case ROW_CTL_MAPPING: return tr("menu.choose_which_vita_button_does_what");
	case ROW_CTL_BTNTEST: return tr("menu.shows_each_button_stick_and_touch");
	case ROW_CTL_TURBO_A: return tr("menu.how_fast_turbo_a_presses_10");
	case ROW_CTL_TURBO_B: return tr("menu.how_fast_turbo_b_presses_10");
	case ROW_CTL_TURBO_MODE: return tr("menu.hold_turbo_runs_while_the_button");
	case ROW_CTL_APPLY: return tr("menu.keep_these_controls_for_every_game");
	case ROW_CTL_RESET: return tr("menu.put_every_button_stick_and_turbo");
	case ROW_SPD_FF: return tr("menu.how_fast_it_runs_2x_and");
	case ROW_SPD_SOUND: return tr("menu.the_sound_while_fast_forwarding_off");
	case ROW_SPD_MODE: return tr("menu.hold_the_fast_forward_button_or");
	case ROW_SPD_REWIND: return tr("menu.hold_the_rewind_button_to_run");
	}
	return "";
}

void Menu::map_row_text(Row &r, int action) const
{
	r.label = action_label(*model_, action);
	r.value_text = phys_label(profile_->bind[map_player_][action], cfg_.enter_is_cross);
}

std::string Menu::exit_label() const { return cfg_.exit_label.empty() ? std::string(tr("menu.exit")) : std::string(tr(cfg_.exit_label.c_str())); }

void Menu::build_rows()
{
	rows_.clear();
	bool input_ok = mapper_ && store_ && model_ && profile_ && (cfg_.caps & CAP_CONTROLS);
	const std::string unsup = tr(cfg_.unsupported_reason.c_str());

	struct B {
		Menu *m;
		void choice(int id, const char *label, const char *const *ch, int n, uint32_t cap, int value, const std::string &reason = std::string())
		{
			Row r;
			r.id = id; r.kind = K_CHOICE; r.label = label;
			uint32_t mask = (id >= 0 && id < ROW_COUNT) ? m->allowed_[id] : 0;
			int v = value < 0 ? 0 : value > n - 1 ? n - 1 : value;
			if (mask) {
				int pos = 0;
				for (int i = 0; i < n; i++)
					if (mask & (1u << i)) { if (i == v) pos = (int) r.map.size(); r.choices.push_back(ch[i]); r.map.push_back(i); }
				if (r.choices.empty()) return;
				v = pos;
			} else {
				for (int i = 0; i < n; i++) r.choices.push_back(ch[i]);
			}
			r.vmin = 0; r.vmax = (int) r.choices.size() - 1; r.value = v;
			r.help = help_for(id);
			finish(r, cap, reason);
		}
		void slider(int id, const char *label, uint32_t cap, int value)
		{
			Row r;
			r.id = id; r.kind = K_SLIDER; r.label = label;
			r.vmin = 0; r.vmax = 10; r.value = value < 0 ? 0 : value > 10 ? 10 : value;
			r.help = help_for(id);
			finish(r, cap, std::string());
		}
		void finish(Row &r, uint32_t cap, const std::string &reason)
		{
			r.enabled = true;
			(void) reason;
			if (cap && !(m->cfg_.caps & cap)) return;      /* a row that cannot apply is hidden, not greyed */
			if (r.id >= 0 && r.id < ROW_COUNT && !m->notes_[r.id].empty()) { r.enabled = false; r.reason = m->notes_[r.id]; }
			m->rows_.push_back(r);
		}
	} b = { this };

	/* the main app's Screen size words (Fit, Stretch); index 1 is never offered (the players mask it: Fit is index 0 here) */
	const char *const kShapeNames[] = { tr("menu.shape.fit"), tr("menu.shape.fit"), tr("menu.shape.zoom"), tr("menu.shape.stretch"), tr("menu.shape.pixel_perfect") };
	const char *const kOnOff[] = { tr("menu.off"), tr("menu.on") };
	const char *const kWide[] = { tr("menu.auto"), tr("menu.on"), tr("menu.off") };
	const char *const kQuality[IMGQ_COUNT] = { tr("menu.auto"), tr("menu.original"), tr("menu.enhanced_2x"), tr("menu.enhanced_3x"), tr("menu.enhanced_4x"), tr("menu.mmpx_2x") };
	const char *const kApply[] = { tr("menu.all_games"), tr("menu.this_game") };
	const char *const kStick[] = { tr("menu.off"), tr("menu.d_pad"), tr("menu.buttons") };
	const char *const kRate[] = { tr("menu.10_a_second"), tr("menu.15_a_second"), tr("menu.30_a_second") };
	const char *const kHold[] = { tr("menu.hold"), tr("menu.toggle") };
	const char *const kFF[FFSPD_COUNT] = { "2x", "3x", tr("menu.unlimited"), tr("menu.unlimited_smooth") };

	switch (page_) {
	case PAGE_SETTINGS: {
		Row r;
		r.kind = K_LINK; r.enabled = true;
		r.tag = PAGE_PICTURE; r.label = tr("menu.picture");
		r.value_text = std::string(kShapeNames[vals_[ROW_PIC_SHAPE] < 0 || vals_[ROW_PIC_SHAPE] > 4 ? 0 : vals_[ROW_PIC_SHAPE]]) + ", " + kQuality[vals_[ROW_PIC_QUALITY] < 0 || vals_[ROW_PIC_QUALITY] >= IMGQ_COUNT ? 0 : vals_[ROW_PIC_QUALITY]];
		r.help = tr("menu.screen_size_image_quality_scanlines_lcd");
		rows_.push_back(r);
		r.tag = PAGE_CONTROLS; r.label = tr("menu.controls");
		r.value_text = (profile_ && model_ && !profile_is_default(*profile_, *model_, cfg_.enter_is_cross)) ? tr("menu.customised") : tr("menu.default");
		r.help = tr("menu.button_mapping_for_every_player_the");
		if (!input_ok) { r.enabled = false; r.reason = unsup; }
		rows_.push_back(r);
		r.enabled = true; r.reason.clear();
		r.tag = PAGE_SPEED; r.label = tr("menu.speed");
		{
			const HostRow *hs = 0;
			for (size_t i = 0; i < host_rows_.size() && !hs; i++) if (host_rows_[i].page == PAGE_SPEED) hs = &host_rows_[i];
			bool any = (cfg_.caps & (CAP_FF | CAP_REWIND)) != 0 || hs;
			if (cfg_.caps & CAP_FF) { r.value_text = trf("menu.speed.value_ff", kFF[vals_[ROW_SPD_FF] < 0 || vals_[ROW_SPD_FF] >= FFSPD_COUNT ? 0 : vals_[ROW_SPD_FF]]); r.help = tr("menu.fast_forward_and_sound_while_it"); }
			else if (hs) {
				int v = hs->value < 0 || hs->value >= (int) hs->choices.size() ? 0 : hs->value;
				r.value_text = std::string(tr(hs->label.c_str())) + ": " + tr(hs->choices[(size_t) v].c_str());
				r.help = tr("menu.how_the_game_is_run_and");
			}
			if (any) rows_.push_back(r);     /* a page with nothing on it is not offered */
		}
		r.enabled = true; r.reason.clear();
		r.tag = PAGE_INTERFACE; r.label = tr("menu.interface");
		r.value_text = vals_[ROW_UI_CONTRAST] ? trf("menu.ui.value_contrast", tr(vals_[ROW_UI_TEXT] ? "menu.ui.large_text" : "menu.ui.normal_text")) : std::string(tr(vals_[ROW_UI_TEXT] ? "menu.ui.large_text" : "menu.ui.normal_text"));
		r.help = tr("menu.text_size_and_high_contrast_for");
		rows_.push_back(r);
		break;
	}
	case PAGE_PICTURE:
		b.choice(ROW_PIC_SHAPE, tr("menu.screen_size"), kShapeNames, 5, CAP_SHAPE, vals_[ROW_PIC_SHAPE]);
		b.choice(ROW_PIC_WIDE, tr("menu.wide_view"), kWide, 3, CAP_WIDE, vals_[ROW_PIC_WIDE], tr("menu.coming"));
		b.choice(ROW_PIC_QUALITY, tr("menu.image_quality"), kQuality, IMGQ_COUNT, CAP_QUALITY, vals_[ROW_PIC_QUALITY]);
		for (size_t i = 0; i < rows_.size(); i++) if (rows_[i].id == ROW_PIC_QUALITY) rows_[i].help = quality_help(vals_[ROW_PIC_QUALITY]);
		b.slider(ROW_PIC_SCANLINES, tr("menu.scanlines"), CAP_SCANLINES, vals_[ROW_PIC_SCANLINES]);
		b.slider(ROW_PIC_GRID, tr("menu.lcd_grid"), CAP_LCD_GRID, vals_[ROW_PIC_GRID]);
		b.slider(ROW_PIC_CRT, tr("menu.crt_mask"), CAP_CRT, vals_[ROW_PIC_CRT]);
		b.slider(ROW_PIC_GLOW, tr("menu.ambient_glow"), CAP_GLOW, vals_[ROW_PIC_GLOW]);
		b.choice(ROW_PIC_APPLY, tr("menu.apply_to"), kApply, 2, CAP_PIC_PER_GAME, vals_[ROW_PIC_APPLY]);
		{
			/* global, like the app's Settings > Display > Screen: not part of "Apply to" */
			const char *const kScreen[] = { tr("menu.auto"), tr("menu.oled_pch_1000"), tr("menu.lcd_pch_2000"), tr("menu.ps_tv") };
			b.choice(ROW_PIC_SCREEN, tr("menu.screen"), kScreen, 4, 0, vals_[ROW_PIC_SCREEN]);
		}
		break;
	case PAGE_INTERFACE: {
		const char *const kText[] = { tr("menu.normal"), tr("menu.large") };
		b.choice(ROW_UI_TEXT, tr("menu.text_size"), kText, 2, 0, vals_[ROW_UI_TEXT]);
		b.choice(ROW_UI_CONTRAST, tr("menu.high_contrast"), kOnOff, 2, 0, vals_[ROW_UI_CONTRAST]);
		{
			const char *kLang[LANG_CHOICES];
			for (int i = 0; i < LANG_CHOICES; i++) kLang[i] = i == LANG_AUTO ? tr("lang.auto") : kLangNative[i];
			b.choice(ROW_UI_LANGUAGE, tr("menu.language"), kLang, LANG_CHOICES, 0, vals_[ROW_UI_LANGUAGE]);
		}
		break;
	}
	case PAGE_CONTROLS: {
		const uint32_t CAP_NEVER = 1u << 31;      /* a capability no host has: the row is always greyed */
		uint32_t cap = input_ok ? 0u : CAP_NEVER;
		int sl = profile_ ? profile_->stick_left : 1, sr = profile_ ? profile_->stick_right : 0;
		b.choice(ROW_CTL_STICK_L, tr("menu.left_stick"), kStick, 3, cap, sl);
		b.choice(ROW_CTL_STICK_R, tr("menu.right_stick"), kStick, 3, cap, sr);
		b.choice(ROW_CTL_REAR, tr("menu.rear_touch_panel"), kOnOff, 2, cap, profile_ && profile_->back_touch ? 1 : 0);
		{
			Row r;
			r.id = ROW_CTL_MAPPING; r.kind = K_LINK; r.label = tr("menu.button_mapping"); r.tag = PAGE_MAPPING; r.help = help_for(ROW_CTL_MAPPING);
			r.enabled = input_ok; if (!input_ok) r.reason = unsup;
			rows_.push_back(r);
		}
		{
			Row r;
			r.id = ROW_CTL_BTNTEST; r.kind = K_ACTION; r.label = tr("menu.button_test"); r.help = help_for(ROW_CTL_BTNTEST);
			rows_.push_back(r);
		}
		bool tA = model_ && model_->turbo_a >= 0, tB = model_ && model_->turbo_b >= 0;
		uint32_t tcap = cap ? cap : ((cfg_.caps & CAP_TURBO) ? 0u : (uint32_t) CAP_TURBO);
		if (tA) b.choice(ROW_CTL_TURBO_A, tr("menu.turbo_a_speed"), kRate, 3, tcap, profile_ ? 3 - profile_->turbo_half[0] : 1);
		if (tB) b.choice(ROW_CTL_TURBO_B, tr("menu.turbo_b_speed"), kRate, 3, tcap, profile_ ? 3 - profile_->turbo_half[1] : 1);
		if (tA || tB) b.choice(ROW_CTL_TURBO_MODE, tr("menu.turbo_mode"), kHold, 2, tcap, profile_ ? profile_->turbo_toggle[0] : 0);
		b.choice(ROW_CTL_APPLY, tr("menu.apply_to"), kApply, 2, cap, profile_ && profile_->own ? 1 : 0);
		{
			Row r;
			r.id = ROW_CTL_RESET; r.kind = K_ACTION; r.label = tr("menu.reset_to_defaults"); r.help = help_for(ROW_CTL_RESET);
			r.enabled = input_ok; if (!input_ok) r.reason = unsup;
			rows_.push_back(r);
		}
		break;
	}
	case PAGE_MAPPING: {
		if (!input_ok) break;
		int np = std::min(cfg_.players, model_->players);
		if (np < 1) np = 1;
		{
			Row r;
			r.id = ID_MAP_PLAYER; r.kind = K_CHOICE; r.label = tr("menu.player");
			for (int i = 0; i < np; i++) r.choices.push_back(strf(tr("menu.player_2"), i + 1));
			r.vmin = 0; r.vmax = np - 1; r.value = std::min(map_player_, np - 1); r.enabled = true;
			r.help = tr("menu.pick_the_player_whose_buttons_you");
			rows_.push_back(r);
		}
		for (int a = 0; a < model_->n; a++) {
			Row r;
			r.kind = K_ACTION; r.tag = a; r.enabled = true;
			map_row_text(r, a);
			r.help = trf("menu.map.press_ok_for", r.label.c_str());
			rows_.push_back(r);
		}
		if (map_player_ == 0) {
			static const int extra[] = { ACT_TURBO_A, ACT_TURBO_B, ACT_FF, ACT_QSAVE, ACT_QLOAD, ACT_REWIND };
			for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) {
				int a = extra[i];
				if (a == ACT_TURBO_A && model_->turbo_a < 0) continue;
				if (a == ACT_TURBO_B && model_->turbo_b < 0) continue;
				Row r;
				r.kind = K_ACTION; r.tag = a; r.enabled = true;
				map_row_text(r, a);
				r.help = action_is_shortcut(a) ? tr("menu.a_shortcut_can_be_one_button") : tr("menu.press_ok_then_the_vita_button");
				if (a == ACT_FF && !(cfg_.caps & CAP_FF)) continue;
				if (a == ACT_REWIND && !(cfg_.caps & CAP_REWIND)) continue;
				if ((a == ACT_QSAVE || a == ACT_QLOAD) && !(cfg_.caps & CAP_STATES)) continue;
				if ((a == ACT_TURBO_A || a == ACT_TURBO_B) && !(cfg_.caps & CAP_TURBO)) continue;
				rows_.push_back(r);
			}
		}
		Row r;
		r.kind = K_ACTION; r.tag = TAG_MAP_RESET; r.label = tr("menu.reset_this_player_s_buttons"); r.enabled = true;
		r.help = tr("menu.put_this_player_s_buttons_back");
		rows_.push_back(r);
		break;
	}
	case PAGE_SPEED:
		b.choice(ROW_SPD_FF, tr("menu.fast_forward"), kFF, FFSPD_COUNT, CAP_FF, vals_[ROW_SPD_FF]);
		/* Unlimited and Unlimited Smooth are silent (the main app folds the sound into the Fast-forward row): the sound choice only means something for 2x and 3x */
		if (vals_[ROW_SPD_FF] < FFSPD_UNLIMITED) {
			const char *const kFFSound[] = { tr("menu.off"), tr("menu.sped_up"), tr("menu.normal_pitch") };
			b.choice(ROW_SPD_SOUND, tr("menu.keep_sound"), kFFSound, 3, CAP_FF_SOUND, vals_[ROW_SPD_SOUND]);
		}
		{
			/* the main app's label: "FF button (Square)", naming the button it is bound to */
			std::string ffl = tr("menu.ff_button");
			if (profile_ && profile_->bind[0][ACT_FF]) ffl += " (" + phys_label(profile_->bind[0][ACT_FF], cfg_.enter_is_cross) + ")";
			b.choice(ROW_SPD_MODE, ffl.c_str(), kHold, 2, CAP_FF_MODE, vals_[ROW_SPD_MODE]);
		}
		b.choice(ROW_SPD_REWIND, tr("menu.rewind"), kOnOff, 2, CAP_REWIND, vals_[ROW_SPD_REWIND]);
		break;
	case PAGE_CHEATS: {
		cheats_.clear();
		if (host_ && (cfg_.caps & CAP_CHEATS)) host_->list_cheats(cheats_);
		if ((cfg_.caps & CAP_CHEATS) && (cfg_.caps & CAP_CHEAT_EDIT)) {
			Row r;
			r.id = ID_CHEAT_ADD; r.kind = K_ACTION; r.label = tr("menu.add_a_cheat"); r.enabled = true;
			r.value_text = cheats_.empty() ? tr("menu.no_cheats_yet") : strf(tr("menu.in_the_list"), (int) cheats_.size());
			r.help = tr("menu.type_a_game_genie_pro_action");
			rows_.push_back(r);
		}
		if ((cfg_.caps & CAP_CHEATS) && (cfg_.caps & CAP_CHEAT_SEARCH)) {
			Row r;
			r.id = ID_CHEAT_SEARCH; r.kind = K_ACTION; r.label = tr("menu.search_memory"); r.enabled = true;
			r.value_text = pk_cs_active(&cs_) ? strf(tr(cs_.count == 1 ? "menu.search.n_result" : "menu.search.n_results"), (int) cs_.count) : tr("menu.find_a_value_s_address");
			r.help = tr("menu.find_where_the_game_keeps_lives");
			rows_.push_back(r);
		}
		if (cheats_.empty() && !((cfg_.caps & CAP_CHEATS) && ((cfg_.caps & CAP_CHEAT_EDIT) || (cfg_.caps & CAP_CHEAT_SEARCH)))) {
			Row r;
			r.kind = K_INFO; r.enabled = false; r.label = tr("menu.no_cheats_for_this_game");
			r.reason = (cfg_.caps & CAP_CHEATS) ? "" : unsup;
			r.help = tr("menu.cheats_you_add_or_turn_on");
			rows_.push_back(r);
		}
		for (size_t i = 0; i < cheats_.size(); i++) {
			Row r;
			r.kind = K_CHOICE; r.label = cheats_[i].name; r.tag = (int) i;
			r.choices.push_back(tr("menu.off")); r.choices.push_back(tr("menu.on"));
			r.vmin = 0; r.vmax = 1; r.value = cheats_[i].on ? 1 : 0; r.enabled = cheats_[i].available;
			if (!r.enabled) r.reason = tr("menu.not_usable_here");
			r.help = (cfg_.caps & CAP_CHEAT_EDIT) ? tr("menu.turn_this_cheat_on_or_off_2") : tr("menu.turn_this_cheat_on_or_off");
			rows_.push_back(r);
		}
		break;
	}
	case PAGE_CHEAT_SEARCH: {
		const bool active = pk_cs_active(&cs_);
		Row r;
		r.id = ID_CS_SIZE; r.kind = K_CHOICE; r.label = tr("menu.size_of_the_number");
		const int nsz = pk_cs_size_count(&cs_);
		for (int i = 0; i < nsz; i++) r.choices.push_back(pk_cs_size_choice(i));
		r.vmin = 0; r.vmax = nsz - 1; r.value = cs_size_ < nsz ? cs_size_ : 0; r.enabled = true;
		r.help = nsz > 3 ? tr("menu.most_counters_are_1_byte_try")
		                 : tr("menu.most_counters_are_1_byte_try_2");
		rows_.push_back(r);
		Row n;
		n.id = ID_CS_NEW; n.kind = K_ACTION; n.label = tr("menu.new_search"); n.enabled = true;
		n.value_text = active ? tr("menu.start_again") : tr("menu.look_at_the_game_s_memory");
		n.help = tr("menu.remember_the_game_s_memory_as");
		rows_.push_back(n);
		if (active) {
			Row c;
			c.id = ID_CS_RESULTS; c.kind = K_INFO; c.enabled = false; c.label = tr("menu.results");
			c.value_text = strf("%d", (int) cs_.count);
			c.help = cs_.count == 0 ? tr("menu.nothing_matched_start_a_new_search")
			       : (int) cs_.count <= PK_CS_LIST_MAX ? tr("menu.pick_a_result_to_make_it")
			       : strf(tr("menu.narrow_it_down_to_or_fewer"), PK_CS_LIST_MAX);
			rows_.push_back(c);
			if (cs_.count > 0)
				for (int op = 0; op < PK_CS_OPS; op++) {
					Row o;
					o.id = ID_CS_OP + op; o.kind = K_ACTION; o.enabled = true;
					o.label = pk_cs_op_name(op);
					o.value_text = pk_cs_op_needs_value(op) ? tr("menu.a_number_you_type") : tr("menu.since_the_last_step");
					o.help = op == PK_CS_EQ ? tr("menu.keep_the_places_that_hold_the")
					       : op == PK_CS_NE ? tr("menu.keep_the_places_that_do_not")
					       : op == PK_CS_GT ? tr("menu.keep_the_places_that_hold_more")
					       : op == PK_CS_LT ? tr("menu.keep_the_places_that_hold_less")
					       : op == PK_CS_CHANGED ? tr("menu.keep_the_places_that_are_not")
					       : op == PK_CS_UNCHANGED ? tr("menu.keep_the_places_that_are_just")
					       : op == PK_CS_INC ? tr("menu.keep_the_places_that_went_up")
					       : tr("menu.keep_the_places_that_went_down");
					rows_.push_back(o);
				}
			for (int i = 0; i < cs_n_; i++) {
				Row h;
				char at[24];
				pk_cs_addr_text(&cs_, cs_addr_[i], at, sizeof at);
				h.kind = K_ACTION; h.tag = i; h.enabled = true; h.label = at;
				h.value_text = strf("%u (0x%X)", (unsigned) cs_val_[i], (unsigned) cs_val_[i]);
				h.help = tr("menu.make_this_a_cheat_that_keeps");
				rows_.push_back(h);
			}
		}
		break;
	}
	default: break;
	}
	/* the host's own rows */
	if (page_ == PAGE_PICTURE || page_ == PAGE_CONTROLS || page_ == PAGE_SPEED) {
		for (size_t k = 0; k < host_rows_.size(); k++) {
			const HostRow &h = host_rows_[k];
			if (h.page != (int) page_) continue;
			Row r;
			r.id = h.id; r.kind = K_CHOICE; r.label = tr(h.label.c_str()); r.help = tr(h.help.c_str());
			for (size_t c = 0; c < h.choices.size(); c++) r.choices.push_back(tr(h.choices[c].c_str()));      /* the host gives catalogue keys (or plain texts) */
			r.vmin = 0; r.vmax = (int) h.choices.size() - 1;
			r.value = h.value < 0 ? 0 : h.value > r.vmax ? r.vmax : h.value;
			r.enabled = true;
			size_t at = rows_.size();
			if (h.before >= 0)
				for (size_t i = 0; i < rows_.size(); i++) if (rows_[i].id == h.before) { at = i; break; }
			rows_.insert(rows_.begin() + (long) at, r);
		}
	}
	clamp_cursor();
}

std::string Menu::row_value_text(int i) const
{
	if (i < 0 || i >= (int) rows_.size()) return std::string();
	const Row &r = rows_[(size_t) i];
	if (r.kind == K_CHOICE && r.value >= 0 && r.value < (int) r.choices.size()) return r.choices[(size_t) r.value];
	if (r.kind == K_SLIDER) return r.value == 0 ? tr("menu.off") : strf("%d", r.value);
	return r.value_text;
}

/* ---- card enablement ----------------------------------------------------------------- */

bool Menu::card_visible(int c) const
{
	/* a console without cheats or without a link simply has no such card */
	if (c == CARD_TOGETHER) return (cfg_.caps & CAP_PLAY_TOGETHER) != 0;
	if (c == CARD_CHEATS) return (cfg_.caps & CAP_CHEATS) != 0;
	return true;
}

int Menu::step_card(int from, int dir) const
{
	int c = from;
	for (int i = 0; i < CARD_COUNT; i++) {
		c = (c + dir + CARD_COUNT) % CARD_COUNT;
		if (card_visible(c)) return c;
	}
	return from;
}

int Menu::visible_card_count() const
{
	int n = 0;
	for (int i = 0; i < CARD_COUNT; i++) n += card_visible(i) ? 1 : 0;
	return n;
}

bool Menu::card_enabled(int c, std::string *why) const
{
	switch (c) {
	case CARD_STATES:
		if (!(cfg_.caps & CAP_STATES) || !states_) { if (why) *why = tr(cfg_.unsupported_reason.c_str()); return false; }
		return true;
	case CARD_TOGETHER:
		if (!(cfg_.caps & CAP_PLAY_TOGETHER)) { if (why) *why = tr(cfg_.play_together_reason.c_str()); return false; }
		return true;
	case CARD_CHEATS:
		if (!(cfg_.caps & CAP_CHEATS)) { if (why) *why = tr(cfg_.unsupported_reason.c_str()); return false; }
		return true;
	case CARD_RESET:
		if (!(cfg_.caps & CAP_RESET)) { if (why) *why = tr(cfg_.unsupported_reason.c_str()); return false; }
		return true;
	default: return true;
	}
}

std::string Menu::card_help(int c) const
{
	std::string why;
	if (!card_enabled(c, &why)) return why;
	switch (c) {
	case CARD_RESUME: return tr("menu.back_to_the_game");
	case CARD_STATES: return strf(tr("menu.save_or_load_the_game_of"), used_slots_, (int) STATE_SLOTS);
	case CARD_TOGETHER: return tr("menu.play_with_a_friend");
	case CARD_CHEATS: return tr("menu.turn_cheats_on_and_off");
	case CARD_SETTINGS: return tr("menu.picture_controls_speed_and_text");
	case CARD_RESET: return tr("menu.start_the_game_again_from_the");
	case CARD_LIST: return tr("menu.leave_this_game_for_the_game");
	default: return cfg_.exit_help.empty() ? std::string(tr("menu.exit_help")) : std::string(tr(cfg_.exit_help.c_str()));
	}
}

void Menu::activate_card(int c)
{
	std::string why;
	if (!card_enabled(c, &why)) { notify(why, NOTICE_INFO); return; }
	switch (c) {
	case CARD_RESUME: pending_ = MENU_RESUME; break;
	case CARD_STATES: set_page(PAGE_STATES, true); break;
	case CARD_CHEATS: set_page(PAGE_CHEATS, true); break;
	case CARD_SETTINGS: set_page(PAGE_SETTINGS, true); break;
	case CARD_RESET: open_modal(M_RESET, 0); break;
	case CARD_LIST: open_modal(M_LIST, 0); break;
	case CARD_EXIT: open_modal(M_EXIT, 0); break;
	default: break;
	}
}

/* ---- changing values ---------------------------------------------------------------- */

void Menu::controls_changed()
{
	if (!profile_ || !store_ || !model_ || !mapper_) return;
	WriteResult rc = profile_->own ? store_->save_for(*model_, *profile_, true) : store_->save_global(*model_, *profile_);
	mapper_->set_profile(*profile_);
	if (host_) host_->on_controls_changed(rc);
	if (rc != W_OK) notify(trf("menu.controls_save_failed", write_result_text(rc)), NOTICE_ERROR);
}

void Menu::apply_value(Row &r, int v)
{
	r.value = v;
	if (!r.map.empty() && v >= 0 && v < (int) r.map.size()) v = r.map[(size_t) v];
	if (r.id >= ROW_HOST_BASE) {
		if (HostRow *h = host_row(r.id)) h->value = v;
		if (host_) host_->on_change(r.id, v);
		return;
	}
	if (page_ == PAGE_CHEAT_SEARCH) {
		if (r.id == ID_CS_SIZE) cs_size_ = v;
		return;
	}
	if (page_ == PAGE_CHEATS) {
		if (r.tag >= 0 && r.tag < (int) cheats_.size()) {
			if (v != 0 && host_ && host_->cheats_refused()) { notify(host_->cheats_refused(), NOTICE_ERROR); r.value = 0; cheats_[(size_t) r.tag].on = false; return; }
			cheats_[(size_t) r.tag].on = v != 0;
			if (host_) host_->set_cheat(r.tag, v != 0);
		}
		return;
	}
	if (r.id == ID_MAP_PLAYER) { map_player_ = v; build_rows(); return; }
	if (r.id < 0 || r.id >= ROW_COUNT) return;
	switch (r.id) {
	case ROW_CTL_STICK_L: if (profile_) { profile_->stick_left = v; controls_changed(); } return;
	case ROW_CTL_STICK_R: if (profile_) { profile_->stick_right = v; controls_changed(); } return;
	case ROW_CTL_REAR: if (profile_) { profile_->back_touch = v != 0; controls_changed(); } return;
	case ROW_CTL_TURBO_A: if (profile_) { profile_->turbo_half[0] = (uint8_t) (3 - v); controls_changed(); } return;
	case ROW_CTL_TURBO_B: if (profile_) { profile_->turbo_half[1] = (uint8_t) (3 - v); controls_changed(); } return;
	case ROW_CTL_TURBO_MODE: if (profile_) { profile_->turbo_toggle[0] = profile_->turbo_toggle[1] = (uint8_t) v; controls_changed(); } return;
	case ROW_CTL_APPLY:
		if (profile_ && store_ && model_) {
			if (v) { profile_->own = true; controls_changed(); }
			else {
				store_->save_for(*model_, *profile_, false);
				store_->load_effective(*model_, cfg_.enter_is_cross, *profile_);
				profile_->own = false;
				if (mapper_) mapper_->set_profile(*profile_);
				if (host_) host_->on_controls_changed(W_OK);
				build_rows();
			}
		}
		return;
	default: break;
	}
	vals_[r.id] = v;
	if (r.id == ROW_PIC_QUALITY) r.help = quality_help(v);
	const int id = r.id;
	if (id == ROW_UI_LANGUAGE) {         /* the texts change now; the host stores the choice (display.ini) */
		i18n_use(v, cfg_.system_lang);
		lang_serial_ = i18n_serial();
		if (host_) { host_->on_change(id, v); host_->on_language_changed(); }
		build_rows();
		return;                            /* r is not used after this */
	}
	if (host_) host_->on_change(id, v);
	if (id == ROW_SPD_FF) build_rows();        /* Keep sound is listed for 2x and 3x only (r is not used after this) */
}

void Menu::step_row(int i, int dir)
{
	if (i < 0 || i >= (int) rows_.size()) return;
	Row &r = rows_[(size_t) i];
	if (!r.enabled) { if (!r.reason.empty()) notify(r.reason, NOTICE_INFO); return; }
	if (r.kind == K_CHOICE) {
		int n = (int) r.choices.size();
		if (n < 2) return;
		apply_value(r, (r.value + dir + n) % n);
	} else if (r.kind == K_SLIDER) {
		int v = r.value + dir;
		if (v < r.vmin) v = r.vmin;
		if (v > r.vmax) v = r.vmax;
		if (v != r.value) apply_value(r, v);
	}
}

void Menu::activate_row(int i)
{
	if (i < 0 || i >= (int) rows_.size()) return;
	Row &r = rows_[(size_t) i];
	if (!r.enabled) { if (!r.reason.empty()) notify(r.reason, NOTICE_INFO); return; }
	switch (r.kind) {
	case K_CHOICE: step_row(i, 1); break;
	case K_SLIDER: apply_value(r, r.value >= r.vmax ? r.vmin : r.value + 1); break;
	case K_LINK: set_page((PageId) r.tag, true); break;
	case K_ACTION:
		if (r.id == ID_CHEAT_SEARCH) { if (search_open()) set_page(PAGE_CHEAT_SEARCH, true); }
		else if (page_ == PAGE_CHEAT_SEARCH) {
			const char *why = host_ ? host_->search_refused() : 0;
			if (why) { notify(why, NOTICE_ERROR); return; }
			if (r.id == ID_CS_NEW) search_new();
			else if (r.id >= ID_CS_OP && r.id < ID_CS_OP + PK_CS_OPS) search_ask(r.id - ID_CS_OP);
			else if (r.id == -1 && r.tag >= 0) search_add(r.tag);
		}
		else if (r.id == ID_CHEAT_ADD) { if (host_ && host_->cheats_refused()) notify(host_->cheats_refused(), NOTICE_ERROR); else if (host_) host_->add_cheat(); }
		else if (r.id == ROW_CTL_RESET) open_modal(M_CTL_RESET, 0);
		else if (r.id == ROW_CTL_BTNTEST) { bt_hold_ms_ = bt_touch_ms_ = 0; bt_pad_ = PadSample(); bt_touch_ = TouchPoint(); open_modal(M_BTNTEST, 0); }
		else if (page_ == PAGE_MAPPING && r.tag == TAG_MAP_RESET) {
			if (profile_ && model_) {
				Profile d = profile_defaults(*model_, cfg_.enter_is_cross);
				for (int a = 0; a < ACT_COUNT; a++) profile_->bind[map_player_][a] = (map_player_ == 0 || a < model_->n) ? d.bind[map_player_][a] : 0;
				controls_changed();
				build_rows();
				notify(strf(tr("menu.player_buttons_reset"), map_player_ + 1), NOTICE_OK);
			}
		} else if (page_ == PAGE_MAPPING) begin_capture(r.tag);
		break;
	default: break;
	}
}

/* ---- Search memory (pk_csearch.h) ------------------------------------------------------- */

bool Menu::search_open()
{
	const char *why = host_ ? host_->search_refused() : 0;
	if (why) { notify(why, NOTICE_ERROR); return false; }
	if (!cs_ready_) {
		pk_cs_free(&cs_);
		memset(&cs_, 0, sizeof cs_);
		if (!host_ || !host_->search_setup(&cs_) || cs_.total == 0) { notify(tr("menu.there_is_no_memory_to_search"), NOTICE_ERROR); return false; }
		cs_ready_ = true;
	}
	return true;
}

void Menu::search_new()
{
	cs_op_ = -1;
	cs_n_ = 0;
	int32_t n = pk_cs_start_ex(&cs_, pk_cs_size_width(cs_size_), pk_cs_size_any(cs_size_) && pk_cs_can_any(&cs_));
	if (n < 0) notify(tr("menu.the_search_could_not_start_not"), NOTICE_ERROR);
	else notify(strf(tr("menu.search_started_places_play_on_then"), (int) n), NOTICE_OK);
	build_rows();
	clamp_cursor();
	ensure_visible();
	layout();
}

void Menu::search_ask(int op)
{
	if (!pk_cs_active(&cs_)) return;
	if (!pk_cs_op_needs_value(op)) { search_apply(op, 0); return; }
	cs_op_ = op;
	if (host_) host_->ask_search_value(trf("menu.search.ask_number", pk_cs_op_name(op)));
}

void Menu::search_value_entered(bool ok, const std::string &text)
{
	int op = cs_op_;
	cs_op_ = -1;
	if (!ok || op < 0 || !pk_cs_active(&cs_)) return;
	uint32_t v = 0;
	if (!pk_cs_parse(trim(text).c_str(), cs_.width, &v)) {
		notify(strf(tr("menu.type_a_whole_number_from_0"), (unsigned) pk_cs_max_value(cs_.width)), NOTICE_ERROR);
		return;
	}
	search_apply(op, v);
}

void Menu::search_apply(int op, uint32_t v)
{
	int32_t n = pk_cs_step(&cs_, op, v);
	cs_n_ = 0;
	if (n >= 0 && n <= PK_CS_LIST_MAX) cs_n_ = pk_cs_list(&cs_, PK_CS_LIST_MAX, cs_addr_, cs_val_);
	if (n < 0) notify(tr("menu.there_is_no_search_to_narrow"), NOTICE_ERROR);
	else if (n == 0) notify(tr("menu.no_results_start_a_new_search"), NOTICE_INFO);
	else notify(strf(tr(n == 1 ? "menu.search.n_result" : "menu.search.n_results"), (int) n), NOTICE_OK);
	build_rows();
	clamp_cursor();
	ensure_visible();
	layout();
}

void Menu::search_add(int i)
{
	if (i < 0 || i >= cs_n_ || !host_) return;
	char code[160], at[24];
	if (!pk_cs_code(&cs_, cs_addr_[i], cs_val_[i], code, sizeof code)) { notify(tr("menu.this_place_cannot_be_made_into"), NOTICE_ERROR); return; }
	pk_cs_addr_text(&cs_, cs_addr_[i], at, sizeof at);
	std::string name = trf("menu.search.found_at", at);
	host_->add_cheat_code(code, name);        /* the host says what happened, as for a typed code */
}

/* ---- states -------------------------------------------------------------------------- */

void Menu::refresh_slots()
{
	for (int i = 0; i < STATE_SLOTS; i++) { slots_[i] = SlotInfo(); thumb_px_[i].clear(); }
	shot_px_.clear();
	shot_w_ = shot_h_ = 0;
	shot_slot_ = 0;
	undo_avail_ = false;
	used_slots_ = 0;
	if (states_ && (cfg_.caps & CAP_STATES)) {
		/* all from the States cache: nothing is read from the card here (but the big picture of one slot) */
		states_->scan(slots_);
		for (int i = 0; i < STATE_SLOTS; i++) {
			int w, h;
			if (pending_slot_[i].on) {
				slots_[i].present = true;
				slots_[i].saved = pending_slot_[i].saved;
				slots_[i].play_seconds = pending_slot_[i].play;
				slots_[i].has_thumb = !pending_slot_[i].thumb.empty();
				thumb_px_[i] = pending_slot_[i].thumb;
			} else if (slots_[i].present) states_->read_thumb(i + 1, thumb_px_[i], &w, &h);
			if (slots_[i].present) used_slots_++;
		}
		if (slot_ >= 1 && slot_ <= STATE_SLOTS && slots_[slot_ - 1].present && !pending_slot_[slot_ - 1].on) {
			if (states_->read_shot(slot_, shot_px_, &shot_w_, &shot_h_)) shot_slot_ = slot_;
		}
		undo_avail_ = (cfg_.caps & CAP_UNDO) && states_->undo_available();
	}
	tex_dirty_ = true;
}

void Menu::select_slot(int slot)
{
	if (slot < 1 || slot > STATE_SLOTS) return;
	slot_ = slot;
	cursor_ = slot - 1;
	shot_px_.clear();
	shot_w_ = shot_h_ = 0;
	shot_slot_ = 0;
	if (states_ && slots_[slot - 1].present && !pending_slot_[slot - 1].on && states_->read_shot(slot, shot_px_, &shot_w_, &shot_h_)) shot_slot_ = slot;
	tex_dirty_ = true;
	ensure_visible();
}

void Menu::request_save(int slot)
{
	if (slots_[slot - 1].present) open_modal(M_OVERWRITE, slot);
	else do_save(slot);
}

void Menu::do_save(int slot)
{
	if (!states_ || !host_) { notify(tr("menu.saving_states_is_not_available"), NOTICE_ERROR); return; }
	std::vector<uint8_t> data;
	if (!host_->capture_state(data) || data.empty()) { notify(tr("menu.could_not_save_the_state_the"), NOTICE_ERROR); return; }
	Frame f;
	host_->current_frame(&f);
	StateJob job = states_->prepare(slot, data.data(), data.size(), f, host_->play_seconds());
	std::vector<uint16_t> thumb;
	int tw, th;
	if (!job.thumb.empty()) parse_thumb(job.thumb, thumb, &tw, &th);
	int64_t saved = job.saved, play = job.play_seconds;
	if (host_->queue_state_write(job)) {
		/* written by the host's thread; shown at once from memory */
		Pending &p = pending_slot_[slot - 1];
		p.on = true; p.thumb.swap(thumb); p.saved = saved; p.play = play;
		notify(strf(tr("menu.saving_to_slot"), slot), NOTICE_INFO);
		refresh_slots();
		select_slot(slot);
		return;
	}
	SaveReport rep = states_->commit(job);
	if (rep.status != ST_OK) {
		notify(rep.text(), NOTICE_ERROR);       /* the previous state in the slot is untouched */
		refresh_slots();
		return;
	}
	host_->on_state_touched(slot);
	notify(strf(tr("menu.state_saved_to_slot"), slot), NOTICE_OK);
	refresh_slots();
	select_slot(slot);
}

void Menu::do_load(int slot)
{
	if (!states_ || !host_) { notify(tr("menu.loading_states_is_not_available"), NOTICE_ERROR); return; }
	if (const char *why = host_->state_load_refused()) { notify(why, NOTICE_ERROR); return; }
	host_->wait_state_writes();
	std::vector<uint8_t> data;
	StateStatus st = states_->load(slot, data);
	if (st != ST_OK) { notify(state_status_text(st), st == ST_EMPTY ? NOTICE_INFO : NOTICE_ERROR); return; }
	/* keep the present moment so the load can be undone */
	if (cfg_.caps & CAP_UNDO) {
		std::vector<uint8_t> now;
		if (host_->capture_state(now) && !now.empty()) states_->undo_store(now.data(), now.size());
	}
	if (!host_->restore_state(data.data(), data.size())) {
		notify(tr("menu.the_state_could_not_be_loaded"), NOTICE_ERROR);
		return;
	}
	host_->on_state_touched(slot);
	notify(strf(tr("menu.state_loaded_from_slot"), slot), NOTICE_OK);
	pending_ = MENU_RESUME;
}

void Menu::do_undo()
{
	if (!states_ || !host_) return;
	if (const char *why = host_->state_load_refused()) { notify(why, NOTICE_ERROR); return; }
	std::vector<uint8_t> data;
	if (states_->undo_fetch(data) != ST_OK) { notify(tr("menu.nothing_to_undo"), NOTICE_INFO); return; }
	if (!host_->restore_state(data.data(), data.size())) { notify(tr("menu.the_undo_could_not_be_applied"), NOTICE_ERROR); return; }
	states_->undo_clear();
	undo_avail_ = false;
	notify(tr("menu.load_undone"), NOTICE_OK);
	pending_ = MENU_RESUME;
}

void Menu::do_delete(int slot)
{
	if (!states_) return;
	if (host_) host_->wait_state_writes();
	if (states_->remove(slot)) {
		if (host_) host_->on_state_touched(slot);
		notify(strf(tr("menu.slot_deleted"), slot), NOTICE_OK);
	} else {
		notify(tr("menu.the_state_could_not_be_deleted"), NOTICE_ERROR);
	}
	refresh_slots();
	select_slot(slot);
}

/* ---- modals ------------------------------------------------------------------------- */

void Menu::open_modal(int kind, int arg)
{
	modal_ = kind;
	modal_arg_ = arg;
	if (kind == M_CAPTURE) cap_ms_ = 6000;
	layout();
}

void Menu::modal_answer(bool yes)
{
	int k = modal_, arg = modal_arg_;
	modal_ = M_NONE;
	switch (k) {
	case M_OVERWRITE: if (yes) do_save(arg); break;
	case M_DELETE: if (yes) do_delete(arg); break;
	case M_LIST: if (yes) pending_ = MENU_GAME_LIST; break;
	case M_EXIT: if (yes) pending_ = MENU_EXIT; break;
	case M_RESET:
		if (yes) {
			if (host_) host_->on_reset();
			pending_ = MENU_RESET_DONE;
		}
		break;
	case M_CTL_RESET:
		if (yes && profile_ && model_) {
			bool own = profile_->own;
			*profile_ = profile_defaults(*model_, cfg_.enter_is_cross);
			profile_->own = own;
			controls_changed();
			build_rows();
			notify(tr("menu.controls_reset_to_the_defaults"), NOTICE_OK);
		}
		break;
	case M_CAPTURE: break;
	case M_BTNTEST: break;
	case M_CHEAT_DELETE:
		if (yes && host_) {
			std::string name = arg >= 0 && arg < (int) cheats_.size() ? cheats_[(size_t) arg].name : std::string();
			if (host_->delete_cheat(arg)) notify(name.empty() ? std::string(tr("menu.cheat_deleted")) : trf("menu.cheat_deleted_name", name.c_str()), NOTICE_OK);
			else notify(tr("menu.the_cheat_could_not_be_deleted"), NOTICE_ERROR);
			build_rows();
			clamp_cursor();
			ensure_visible();
		}
		break;
	}
	layout();
}

void Menu::begin_capture(int action)
{
	if (!mapper_ || !profile_) return;
	cap_action_ = action;
	cap_bits_ = 0;
	cap_window_ms_ = 0;
	mapper_->capture_reset();
	open_modal(M_CAPTURE, action);
}

/* Settings > Controls > Button test.  Every input is shown as it is read; the page leaves by Start + Select held for half a
 * second (every other button is being tested) or, for a console whose Start or Select is the worn one, by a finger held on the
 * line that says so for a second. Nothing is sent to the game and no press reaches the menu. */
void Menu::bt_update(const MenuInput &in)
{
	const PadSample &ps = in.pads[nav_player_];
	bt_pad_ = ps;
	bt_touch_ = in.touch;
	if ((ps.buttons & PH_MENU_CHORD) == PH_MENU_CHORD) bt_hold_ms_ += in.dt_ms; else bt_hold_ms_ = 0;
	if (in.touch.down && in.touch.y >= 496) bt_touch_ms_ += in.dt_ms; else bt_touch_ms_ = 0;
	if (bt_hold_ms_ >= 500 || bt_touch_ms_ >= 1000) {
		modal_ = M_NONE;
		bt_hold_ms_ = bt_touch_ms_ = 0;
		t_down_ = in.touch.down; t_region_ = -1; t_scrolling_ = false; t_slider_ = false;   /* nor is the finger that left a tap there */
		ignore_mask_ = ps.buttons & PH_BUTTONS;      /* the two buttons that left are not a press on the page behind */
		prev_buttons_ = 0;
		first_update_ = false;
		layout();
	}
}

/* The actions player 1's profile puts on the physical input `bit` (a chord's member too, marked). */
std::string Menu::bt_actions(uint32_t bit) const
{
	std::string s;
	if (!profile_ || !model_) return s;
	for (int a = 0; a < ACT_COUNT; a++) {
		if (a >= model_->n && a < ACT_TURBO_A) continue;
		if (a == ACT_TURBO_A && model_->turbo_a < 0) continue;
		if (a == ACT_TURBO_B && model_->turbo_b < 0) continue;
		uint32_t b = profile_->bind[0][a];
		if (!(b & bit)) continue;
		if (!s.empty()) s += ", ";
		s += b != bit ? trf("menu.btntest.chord", action_label(*model_, a)) : std::string(action_label(*model_, a));
	}
	return s;
}

void Menu::capture_update(const MenuInput &in)
{
	int pl = map_player_;
	const PadSample &ps = in.pads[pl].connected ? in.pads[pl] : in.pads[nav_player_];
	cap_ms_ -= in.dt_ms;
	uint32_t got = mapper_->capture_next(ps);
	bool chord_ok = action_is_shortcut(cap_action_);
	if (got) {
		cap_bits_ |= got;
		if (chord_ok && cap_window_ms_ == 0) cap_window_ms_ = 450;     /* a shortcut may be a chord: wait for the rest of it */
	}
	if (cap_window_ms_ > 0) cap_window_ms_ -= in.dt_ms;
	if (cap_bits_ && (!chord_ok || cap_window_ms_ <= 0)) {
		int stolen = -1;
		AssignResult r = profile_assign(*profile_, *model_, pl, cap_action_, cap_bits_, &stolen);
		modal_ = M_NONE;
		if (r != ASSIGN_OK) notify(assign_result_text(r), NOTICE_ERROR);
		else {
			controls_changed();
			if (stolen >= 0) notify(strf(tr("menu.moved_here_from"), phys_label(cap_bits_, cfg_.enter_is_cross).c_str(), action_label(*model_, stolen)), NOTICE_INFO);
			else notify(strf(tr("menu.is_now"), action_label(*model_, cap_action_), phys_label(cap_bits_, cfg_.enter_is_cross).c_str()), NOTICE_OK);
		}
		build_rows();
		layout();
		return;
	}
	if (cap_ms_ <= 0) {
		modal_ = M_NONE;
		notify(tr("menu.nothing_pressed_the_button_was_not"), NOTICE_INFO);
		layout();
	}
}

/* ---- regions (touch targets) ---------------------------------------------------------- */

void Menu::add_region(float x, float y, float w, float h, int kind, int a, float radius, bool clip_to_view)
{
	Region r;
	r.kind = kind; r.a = a; r.radius = radius;
	r.vx = x; r.vy = y; r.vw = w; r.vh = h;
	float cx0 = x, cy0 = y, cx1 = x + w, cy1 = y + h;
	if (clip_to_view) {
		cy0 = std::max(cy0, VY);
		cy1 = std::min(cy1, VY + view_h());
	}
	if (cy1 - cy0 < 1 || cx1 - cx0 < 1) return;
	r.x = cx0; r.y = cy0; r.w = cx1 - cx0; r.h = cy1 - cy0;
	regions_.push_back(r);
}

float Menu::min_touch_height() const
{
	float m = 1e9f;
	for (size_t i = 0; i < regions_.size(); i++) {
		const Region &r = regions_[i];
		if (r.kind == REGION_OUTSIDE) continue;
		if (r.h < r.vh - 0.5f) continue;          /* a partly scrolled-out row */
		m = std::min(m, r.h);
	}
	return m > 1e8f ? 0 : m;
}

/* the regions of rows are drawn at 72 px but touched at the 80 px pitch: grow each by the gap below */
void Menu::grow_to_pitch()
{
	for (size_t i = 0; i < regions_.size(); i++) {
		Region &r = regions_[i];
		if (r.kind == REGION_ROW || r.kind == REGION_ARROW_L || r.kind == REGION_ARROW_R || r.kind == REGION_SLIDER || r.kind == REGION_SLOT || r.kind == REGION_DELETE) {
			float bottom = std::min(r.vy + PITCH, VY + view_h());
			if (r.vy + r.vh <= VY + view_h()) { r.h = bottom - r.y; }
		}
	}
}

void Menu::layout()
{
	regions_.clear();
	if (!open_) return;
	if (modal_ == M_BTNTEST) return;     /* a finger on the glass is only shown, never a press */
	if (modal_) {
		add_region(0, 0, 960, 544, REGION_OUTSIDE, 0, 0, false);
		float cx = 200, cy = modal_ == M_CAPTURE ? 150 : 170, cw = 560;
		float by = cy + (modal_ == M_CAPTURE ? 216 : 150);
		if (modal_ == M_CAPTURE) {
			add_region(cx + 24, by, 250, 80, REGION_BUTTON, BTN_CLEAR, 22, false);
			add_region(cx + cw - 24 - 250, by, 250, 80, REGION_BUTTON, BTN_CANCEL, 22, false);
		} else {
			add_region(cx + 24, by, 250, 80, REGION_BUTTON, BTN_PRIMARY, 22, false);
			add_region(cx + cw - 24 - 250, by, 250, 80, REGION_BUTTON, BTN_SECONDARY, 22, false);
		}
		return;
	}
	if (page_ == PAGE_TOP) {
		float y0 = PK_SCREEN_H - SHEET_H;
		add_region(0, 0, (float) PK_SCREEN_W, y0, REGION_OUTSIDE, 0, 0, false);     /* the game picture above the sheet */
		const int nvis = visible_card_count();
		float x = (PK_SCREEN_W - (nvis * CARD_W + (nvis - 1) * CARD_GAP)) * 0.5f;
		for (int i = 0; i < CARD_COUNT; i++) {
			if (!card_visible(i)) continue;
			add_region(x, y0 + 78, CARD_W, CARD_H, REGION_CARD, i, 20, false);
			x += CARD_W + CARD_GAP;
		}
		return;
	}
	if (page_ == PAGE_STATES) {
		for (int i = 0; i < STATE_SLOTS; i++) add_region(VX, VY + i * PITCH - scroll_, 420, RH, REGION_SLOT, i, 20, true);
		grow_to_pitch();
		float bx = 476, bw = 140, gap = 14;
		bool filled = slot_ >= 1 && slot_ <= STATE_SLOTS && slots_[slot_ - 1].present;
		add_region(bx, 348, bw, 80, REGION_BUTTON, BTN_LOAD, 20, false);
		add_region(bx + bw + gap, 348, bw, 80, REGION_BUTTON, BTN_SAVE, 20, false);
		add_region(bx + 2 * (bw + gap), 348, bw, 80, REGION_BUTTON, BTN_DELETE, 20, false);
		(void) filled;
		if (undo_avail_) add_region(bx, 436, 448, 80, REGION_BUTTON, BTN_UNDO, 20, false);
		return;
	}
	for (size_t i = 0; i < rows_.size(); i++) {
		float y = VY + (float) i * PITCH - scroll_;
		const Row &r = rows_[i];
		add_region(VX, y, VW, RH, REGION_ROW, (int) i, 20, true);
		if (page_ == PAGE_CHEATS && (cfg_.caps & CAP_CHEAT_EDIT) && r.kind == K_CHOICE)
			add_region(VX + VW - 380 - 132, y, 120, RH, REGION_DELETE, (int) i, 20, true);
		if (r.enabled && (r.kind == K_CHOICE || r.kind == K_SLIDER)) {
			float rx = VX + VW;
			add_region(rx - 380, y, 72, RH, REGION_ARROW_L, (int) i, 20, true);
			add_region(rx - 72, y, 72, RH, REGION_ARROW_R, (int) i, 20, true);
			if (r.kind == K_SLIDER) add_region(rx - 308 + 12, y, 160, RH, REGION_SLIDER, (int) i, 20, true);
		}
	}
	grow_to_pitch();
}

int Menu::hit(float x, float y) const
{
	for (int i = (int) regions_.size(); i-- > 0;) {
		const Region &r = regions_[(size_t) i];
		if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i;
	}
	return -1;
}

static int slider_value(const Menu::Region &r, float x, int vmin, int vmax)
{
	float t = clampf((x - r.vx) / r.vw, 0, 1);
	return vmin + (int) floorf(t * (vmax - vmin) + 0.5f);
}

void Menu::tap(const Region &r, float x)
{
	switch (r.kind) {
	case REGION_CARD: cursor_ = r.a; activate_card(r.a); break;
	case REGION_ROW: cursor_ = r.a; activate_row(r.a); break;
	case REGION_ARROW_L: cursor_ = r.a; step_row(r.a, -1); break;
	case REGION_ARROW_R: cursor_ = r.a; step_row(r.a, 1); break;
	case REGION_SLIDER: {
		cursor_ = r.a;
		Row &w = rows_[(size_t) r.a];
		if (w.enabled) { int v = slider_value(r, x, w.vmin, w.vmax); if (v != w.value) apply_value(w, v); }
		break;
	}
	case REGION_SLOT: select_slot(r.a + 1); break;
	case REGION_DELETE:
		cursor_ = r.a;
		if (r.a >= 0 && r.a < (int) rows_.size() && rows_[(size_t) r.a].kind == K_CHOICE) open_modal(M_CHEAT_DELETE, rows_[(size_t) r.a].tag);
		break;
	case REGION_BUTTON:
		switch (r.a) {
		case BTN_PRIMARY: modal_answer(true); break;
		case BTN_SECONDARY: modal_answer(false); break;
		case BTN_CLEAR:
			if (modal_ == M_CAPTURE && profile_) {
				profile_->bind[map_player_][cap_action_] = 0;
				controls_changed();
				modal_ = M_NONE;
				build_rows();
				layout();
				notify(strf(tr("menu.cleared"), action_label(*model_, cap_action_)), NOTICE_INFO);
			}
			break;
		case BTN_CANCEL: modal_ = M_NONE; layout(); break;
		case BTN_LOAD:
			if (slots_[slot_ - 1].present) do_load(slot_); else notify(tr("menu.no_saved_state_in_this_slot"), NOTICE_INFO);
			break;
		case BTN_SAVE: request_save(slot_); break;
		case BTN_DELETE:
			if (slots_[slot_ - 1].present) open_modal(M_DELETE, slot_); else notify(tr("menu.no_saved_state_in_this_slot"), NOTICE_INFO);
			break;
		case BTN_UNDO: do_undo(); break;
		}
		break;
	case REGION_OUTSIDE:
		if (modal_) modal_answer(false);
		else if (page_ == PAGE_TOP) pending_ = MENU_RESUME;      /* a tap on the dimmed game picture goes back to the game */
		break;
	}
}

void Menu::touch_update(const MenuInput &in)
{
	const TouchPoint &t = in.touch;
	if (first_update_ && t.down) { t_down_ = true; t_region_ = -1; t_scrolling_ = false; t_slider_ = false; }   /* the tap that opened the menu */
	if (flash_ms_ > 0) flash_ms_ -= in.dt_ms;
	if (t.down && !t_down_) {
		t_down_ = true;
		t_x0_ = t_x_ = t.x; t_y0_ = t_y_ = t.y;
		t_scrolling_ = false;
		t_slider_ = false;
		t_scroll0_ = scroll_;
		t_region_ = hit(t.x, t.y);
		if (t_region_ >= 0 && regions_[(size_t) t_region_].kind == REGION_SLIDER) {
			t_slider_ = true;
			const Region &r = regions_[(size_t) t_region_];
			Row &w = rows_[(size_t) r.a];
			cursor_ = r.a;
			int v = slider_value(r, t.x, w.vmin, w.vmax);
			if (v != w.value) apply_value(w, v);
		}
	} else if (t.down && t_down_) {
		t_x_ = t.x; t_y_ = t.y;
		if (t_slider_ && t_region_ >= 0 && t_region_ < (int) regions_.size()) {
			const Region &r = regions_[(size_t) t_region_];
			Row &w = rows_[(size_t) r.a];
			int v = slider_value(r, t.x, w.vmin, w.vmax);
			if (v != w.value) apply_value(w, v);
		} else if (t_region_ >= 0 && !modal_ && page_ != PAGE_TOP) {
			const Region &r = regions_[(size_t) t_region_];
			bool scrollable = r.kind == REGION_ROW || r.kind == REGION_ARROW_L || r.kind == REGION_ARROW_R || r.kind == REGION_SLOT || r.kind == REGION_DELETE;
			if (scrollable && fabsf(t.y - t_y0_) > 14) t_scrolling_ = true;
			if (t_scrolling_) {
				float maxs = std::max(0.0f, content_h() - view_h());
				scroll_ = scroll_target_ = clampf(t_scroll0_ - (t.y - t_y0_), 0, maxs);
				layout();
			}
		}
	} else if (!t.down && t_down_) {
		t_down_ = false;
		int ri = t_region_;
		t_region_ = -1;
		if (ri >= 0 && ri < (int) regions_.size() && !t_scrolling_ && !t_slider_) {
			Region r = regions_[(size_t) ri];
			const float slop = 10;
			if (t_x_ >= r.x - slop && t_x_ < r.x + r.w + slop && t_y_ >= r.y - slop && t_y_ < r.y + r.h + slop) {
				if (r.kind != REGION_OUTSIDE) { flash_ = r; have_flash_ = true; flash_ms_ = 170; }
				tap(r, t_x_);
				layout();
			}
		}
		t_scrolling_ = false;
		t_slider_ = false;
	}
}

/* ---- pad ------------------------------------------------------------------------------ */

void Menu::pad_update(const MenuInput &in)
{
	const PadSample &s = in.pads[nav_player_];
	uint32_t cur = s.buttons & PH_BUTTONS;
	/* the left stick as a second d-pad (a wide dead zone, so no state is needed) */
	{
		int dx = (int) s.lx - 128, dy = (int) s.ly - 128;
		if (dx <= -80) cur |= PH_LEFT;
		if (dx >= 80) cur |= PH_RIGHT;
		if (dy <= -80) cur |= PH_UP;
		if (dy >= 80) cur |= PH_DOWN;
	}
	if (first_update_) { ignore_mask_ = cur; first_update_ = false; }
	ignore_mask_ &= cur;
	cur &= ~ignore_mask_;
	uint32_t edge = cur & ~prev_buttons_;
	const uint32_t dirs[4] = { PH_UP, PH_DOWN, PH_LEFT, PH_RIGHT };
	uint32_t fire = edge & (PH_CROSS | PH_CIRCLE | PH_SQUARE | PH_TRIANGLE | PH_SELECT);
	for (int k = 0; k < 4; k++) {
		if (edge & dirs[k]) { fire |= dirs[k]; rep_ms_[k] = 0; }
		else if (cur & dirs[k]) {
			rep_ms_[k] += in.dt_ms;
			if (rep_ms_[k] >= 400) { fire |= dirs[k]; rep_ms_[k] = 400 - 90; }
		} else rep_ms_[k] = 0;
	}
	prev_buttons_ = cur;
	if (!fire) return;
	uint32_t enter = phys_confirm(cfg_.enter_is_cross), cancel = phys_cancel(cfg_.enter_is_cross);

	if (modal_ == M_CAPTURE) return;     /* handled by capture_update */
	if (modal_) {
		if (fire & enter) modal_answer(true);
		else if (fire & cancel) modal_answer(false);
		return;
	}

	if (page_ == PAGE_TOP) {
		if (fire & (PH_LEFT | PH_UP)) cursor_ = step_card(cursor_, -1);
		if (fire & (PH_RIGHT | PH_DOWN)) cursor_ = step_card(cursor_, 1);
		if (fire & enter) activate_card(cursor_);
		if (fire & cancel) pending_ = MENU_RESUME;
		return;
	}
	if (page_ == PAGE_STATES) {
		if (fire & PH_UP) select_slot((slot_ + STATE_SLOTS - 2) % STATE_SLOTS + 1);
		if (fire & PH_DOWN) select_slot(slot_ % STATE_SLOTS + 1);
		bool filled = slots_[slot_ - 1].present;
		if (fire & enter) { if (filled) do_load(slot_); else request_save(slot_); }
		if (fire & PH_SQUARE) request_save(slot_);
		if (fire & PH_TRIANGLE) { if (filled) open_modal(M_DELETE, slot_); }
		if ((fire & PH_SELECT) && undo_avail_) do_undo();
		if (fire & cancel) back();
		return;
	}
	if (page_ == PAGE_CHEATS && (fire & PH_TRIANGLE) && (cfg_.caps & CAP_CHEAT_EDIT) && cursor_ >= 0 && cursor_ < (int) rows_.size() &&
	    rows_[(size_t) cursor_].kind == K_CHOICE) {
		open_modal(M_CHEAT_DELETE, rows_[(size_t) cursor_].tag);
		return;
	}
	if (fire & PH_UP) { if (!rows_.empty()) cursor_ = (cursor_ + (int) rows_.size() - 1) % (int) rows_.size(); ensure_visible(); }
	if (fire & PH_DOWN) { if (!rows_.empty()) cursor_ = (cursor_ + 1) % (int) rows_.size(); ensure_visible(); }
	if (fire & PH_LEFT) step_row(cursor_, -1);
	if (fire & PH_RIGHT) step_row(cursor_, 1);
	if (fire & enter) activate_row(cursor_);
	if (fire & cancel) back();
}

MenuEvent Menu::update(const MenuInput &in)
{
	if (!open_) return MENU_NONE;
	if (lang_serial_ != i18n_serial()) { lang_serial_ = i18n_serial(); build_rows(); clamp_cursor(); }      /* the language changed under the menu */
	age_ms_ += in.dt_ms;
	notices_.update(in.dt_ms);
	layout();
	pending_ = MENU_NONE;
	if (modal_ == M_BTNTEST) {
		bt_update(in);
		layout();
		return MENU_NONE;
	}
	if (modal_ == M_CAPTURE && mapper_) {
		capture_update(in);
		/* whatever was pressed to answer must not also act as a menu press afterwards */
		ignore_mask_ = in.pads[nav_player_].buttons & PH_BUTTONS;
		prev_buttons_ = 0;
		first_update_ = false;
	} else {
		pad_update(in);
	}
	touch_update(in);
	first_update_ = false;
	/* ease the scroll toward its target */
	if (!t_scrolling_) {
		float d = scroll_target_ - scroll_;
		if (fabsf(d) < 0.5f) scroll_ = scroll_target_;
		else scroll_ += d * std::min(1.0f, in.dt_ms / 60.0f);
	}
	layout();
	if (pending_ != MENU_NONE) {
		if (pending_ == MENU_RESUME || pending_ == MENU_GAME_LIST || pending_ == MENU_EXIT || pending_ == MENU_RESET_DONE) open_ = false;
		return pending_;
	}
	return MENU_NONE;
}

/* ---- drawing ------------------------------------------------------------------------------ */

void Menu::upload_textures(Gfx *g)
{
	gfx_ = g;
	if (!tex_dirty_) return;
	tex_dirty_ = false;
	for (int i = 0; i < STATE_SLOTS; i++) {
		if (thumb_tex_[i]) { g->tex_free(thumb_tex_[i]); thumb_tex_[i] = 0; }
		if (!thumb_px_[i].empty()) {
			thumb_tex_[i] = g->tex_create(THUMB_W, THUMB_H, TEX_RGB565);
			if (thumb_tex_[i]) g->tex_update(thumb_tex_[i], &thumb_px_[i][0], THUMB_W * 2);
		}
	}
	if (shot_tex_) { g->tex_free(shot_tex_); shot_tex_ = 0; }
	if (!shot_px_.empty() && shot_w_ > 0) {
		shot_tex_ = g->tex_create(shot_w_, shot_h_, TEX_RGB565);
		if (shot_tex_) g->tex_update(shot_tex_, &shot_px_[0], shot_w_ * 2);
	}
}

void Menu::draw_backdrop(Gfx *g)
{
	float e = clampf((float) age_ms_ / SHEET_MS, 0, 1);
	if (page_ == PAGE_TOP) {
		g->fill_rect(0, 0, 960, 544, col(C_BG, (int) (110 * e)));
		g->gradient_v(0, 0, 960, 120, col(C_BG, (int) (200 * e)), col(C_BG, 0));
	} else {
		g->fill_rect(0, 0, 960, 544, col(C_BG, 190));
	}
}

/* a label centred in a box (top, h tall): on one line when it fits max_w, else on two (broken at a space; a line that still does not fit ends in "...") */
static void draw_box_label(Text *t, int face, float cx, float top, float h, float max_w, const std::string &s, uint32_t color)
{
	const float lh = (float) t->line_h(face);
	if (t->width(face, s.c_str()) <= max_w) { t->draw_center(face, cx, top + (h - lh) * 0.5f, s.c_str(), color); return; }
	/* the break nearest the middle that gives the shorter widest line */
	size_t best = std::string::npos;
	float best_w = 1e9f;
	for (size_t i = 1; i + 1 < s.size(); i++) {
		if (s[i] != ' ') continue;
		const float w = std::max(t->width(face, s.substr(0, i).c_str()), t->width(face, s.substr(i + 1).c_str()));
		if (w < best_w) { best_w = w; best = i; }
	}
	if (best == std::string::npos) { t->draw_center(face, cx, top + (h - lh) * 0.5f, t->fit(face, s, max_w).c_str(), color); return; }
	const float y0 = top + (h - 2 * lh) * 0.5f;
	t->draw_center(face, cx, y0, t->fit(face, s.substr(0, best), max_w).c_str(), color);
	t->draw_center(face, cx, y0 + lh, t->fit(face, s.substr(best + 1), max_w).c_str(), color);
}

static void hint(Gfx *g, Text *t, float *xr, int glyph, const char *label, uint32_t ink_unused)
{
	(void) ink_unused;
	const std::string lb = t->fit(F_BODY, label, 210);      /* a long word in another language ends in "..." */
	float w = t->width(F_BODY, lb.c_str());
	float x = *xr - w;
	t->draw(F_BODY, x, 47 - t->line_h(F_BODY) * 0.5f, lb.c_str(), col(C_TEXT2));
	draw_button_glyph(g, glyph, x - 20, 47, 28, col(C_INK), col(C_TEXT2));
	*xr = x - 20 - 14 - 22;
}

void Menu::draw_hints(Gfx *g, Text *t)
{
	float xr = 924;
	struct Left { float *dst, *src; ~Left() { *dst = *src - 20; } } left_guard = { &hints_left_, &xr };      /* where the hints end on the left (the title keeps clear of it) */
	hints_left_ = 960;
	int ge = cfg_.enter_is_cross ? GLYPH_CROSS : GLYPH_CIRCLE, gc = cfg_.enter_is_cross ? GLYPH_CIRCLE : GLYPH_CROSS;
	if (modal_ == M_CAPTURE || modal_ == M_BTNTEST) return;   /* any button is the answer / being tested: no hints */
	if (modal_ != M_NONE) {
		hint(g, t, &xr, gc, tr("menu.cancel"), 0);
		hint(g, t, &xr, ge, tr("menu.confirm"), 0);
		return;
	}
	if (page_ == PAGE_TOP) {
		hint(g, t, &xr, gc, tr("menu.resume"), 0);
		hint(g, t, &xr, ge, tr("menu.select"), 0);
		return;
	}
	if (page_ == PAGE_STATES) {
		hint(g, t, &xr, gc, tr("menu.back"), 0);
		hint(g, t, &xr, GLYPH_SQUARE, tr("menu.save_here"), 0);
		hint(g, t, &xr, GLYPH_TRIANGLE, tr("menu.delete"), 0);
		hint(g, t, &xr, ge, slots_[slot_ - 1].present ? tr("menu.load") : tr("menu.save"), 0);
		return;
	}
	hint(g, t, &xr, gc, tr("menu.back"), 0);
	if (page_ == PAGE_CHEATS && (cfg_.caps & CAP_CHEAT_EDIT) && cursor_ >= 0 && cursor_ < (int) rows_.size() && rows_[(size_t) cursor_].kind == K_CHOICE)
		hint(g, t, &xr, GLYPH_TRIANGLE, tr("menu.delete"), 0);
	bool act = cursor_ >= 0 && cursor_ < (int) rows_.size() && (rows_[(size_t) cursor_].kind == K_ACTION || rows_[(size_t) cursor_].kind == K_LINK);
	hint(g, t, &xr, ge, act ? tr("menu.select") : tr("menu.change"), 0);
}

void Menu::draw_header(Gfx *g, Text *t, const char *title, const char *sub)
{
	/* Large text grows the two lines; the header climbs by the extra so the subtitle stays above the panel. */
	const int pct = t->pct();
	const float up = pct > 100 ? (float) (t->line_h(F_TITLE) + t->line_h(F_CAPTION) - (t->line_h(F_TITLE) + t->line_h(F_CAPTION)) * 100 / pct) : 0.0f;
	const float ty = 22 - up;
	const float room = std::max(120.0f, hints_left_ - 44 - 16);      /* the title and the hints share the top line */
	t->draw(F_TITLE, 44, ty, t->fit(F_TITLE, title, room).c_str(), col(C_TEXT));
	std::string s = t->fit(F_CAPTION, sub, std::min(560.0f, room));
	t->draw(F_CAPTION, 46, ty + t->line_h(F_TITLE) + 0, s.c_str(), col(C_TEXT2));
	(void) g;
}

void Menu::draw_top(Gfx *g, Text *t)
{
	float e0 = clampf((float) age_ms_ / SHEET_MS, 0, 1);
	float e = 1.0f - (1.0f - e0) * (1.0f - e0) * (1.0f - e0);
	int a = (int) (255 * e);
	/* the game's name and console, top left */
	{
		std::string title = cfg_.game_name;
		size_t par = title.find(" (");
		if (par != std::string::npos && par > 0) title = title.substr(0, par);
		title = t->fit(F_SUB, title, std::min(640.0f, hints_left_ - 44 - 16));
		t->draw(F_SUB, 44, 22, title.c_str(), col(C_TEXT, a));
		t->draw(F_CAPTION, 46, 22 + t->line_h(F_SUB) + 2, cfg_.console_label.c_str(), col(C_TEXT2, a));
	}
	float y0 = PK_SCREEN_H - SHEET_H * e;
	g->fill_rrect(14, y0 + 6, PK_SCREEN_W - 28, SHEET_H + 40, 30, col(0x000000, 60));
	g->fill_rrect(20, y0, PK_SCREEN_W - 40, SHEET_H + 40, 30, col(C_SHEET, 236));
	g->fill_rect(40, y0 + 1, PK_SCREEN_W - 80, 1, col(0xFFFFFF, 26));

	std::string line = card_help(cursor_);
	const char *const labels[CARD_COUNT] = { tr("menu.resume"), tr("menu.save_states"), tr("menu.play_together"), tr("menu.cheats"), tr("menu.settings"), tr("menu.reset_game"), tr("menu.game_list"), tr("menu.exit") };
	std::string cl = cursor_ == CARD_EXIT ? exit_label() : std::string(labels[cursor_]);
	t->draw(F_LABEL, 52, y0 + 26, cl.c_str(), col(C_TEXT));
	std::string fit = t->fit(F_BODY, line, PK_SCREEN_W - 52 - 52 - t->width(F_LABEL, cl.c_str()) - 16);
	std::string why;
	t->draw(F_BODY, 52 + t->width(F_LABEL, cl.c_str()) + 16, y0 + 26, fit.c_str(), card_enabled(cursor_, &why) ? col(C_TEXT2) : col(C_TEXT3));

	static const int icons[CARD_COUNT] = { ICON_PLAY, ICON_LAYERS, ICON_PEOPLE, ICON_SPARK, ICON_GEAR, ICON_RESET, ICON_GRID, ICON_POWER };
	const int nvis = visible_card_count();
	float x = (PK_SCREEN_W - (nvis * CARD_W + (nvis - 1) * CARD_GAP)) * 0.5f;
	for (int i = 0; i < CARD_COUNT; i++) {
		if (!card_visible(i)) continue;
		bool on = i == cursor_, enabled = card_enabled(i, 0);
		float y = y0 + 78 - (on ? 6.0f : 0.0f);
		uint32_t ink = on ? col(C_INK) : enabled ? col(C_TEXT) : col(C_TEXT3);
		uint32_t lc = on ? col(C_INK) : enabled ? col(C_TEXT2) : col(C_TEXT3);
		if (on) {
			g->fill_rrect(x - 4, y - 4, CARD_W + 8, CARD_H + 8, 24, col(0xFFFFFF, 40));
			g->fill_rrect(x, y, CARD_W, CARD_H, 20, col(C_TEXT));
		} else {
			g->fill_rrect(x, y, CARD_W, CARD_H, 20, col(0xFFFFFF, 30));
		}
		std::string lb = (i == CARD_EXIT) ? exit_label() : std::string(labels[i]);
		size_t sp = lb.find(' ');
		if (t->width(F_CAPTION, lb.c_str()) > CARD_W - 12 && sp != std::string::npos) {
			draw_icon(g, icons[i], x + CARD_W * 0.5f, y + 36, 36, ink);
			t->draw_center(F_CAPTION, x + CARD_W * 0.5f, y + 62, t->fit(F_CAPTION, lb.substr(0, sp), CARD_W - 8).c_str(), lc);
			t->draw_center(F_CAPTION, x + CARD_W * 0.5f, y + 86, t->fit(F_CAPTION, lb.substr(sp + 1), CARD_W - 8).c_str(), lc);
		} else {
			draw_icon(g, icons[i], x + CARD_W * 0.5f, y + 42, 38, ink);
			t->draw_center(F_CAPTION, x + CARD_W * 0.5f, y + 76, t->fit(F_CAPTION, lb, CARD_W - 8).c_str(), lc);
		}
		/* a dot where something is going on */
		if (i == CARD_STATES && enabled && used_slots_ > 0) g->fill_circle(x + CARD_W - 16, y + 16, 5, on ? col(C_INK) : accent(255));
		if (i == CARD_CHEATS && enabled) {
			std::vector<CheatItem> ch;
			bool any = false;
			if (host_) { host_->list_cheats(ch); for (size_t k = 0; k < ch.size(); k++) any = any || ch[k].on; }
			if (any) g->fill_circle(x + CARD_W - 16, y + 16, 5, on ? col(C_INK) : accent(255));
		}
		x += CARD_W + CARD_GAP;
	}
	draw_hints(g, t);
}

void Menu::draw_rows(Gfx *g, Text *t)
{
	const char *title = page_ == PAGE_SETTINGS ? tr("menu.settings") : page_ == PAGE_PICTURE ? tr("menu.picture") : page_ == PAGE_CONTROLS ? tr("menu.controls") :
	                    page_ == PAGE_MAPPING ? tr("menu.button_mapping") : page_ == PAGE_SPEED ? tr("menu.speed") : page_ == PAGE_CHEAT_SEARCH ? tr("menu.search_memory") : page_ == PAGE_INTERFACE ? tr("menu.interface") : tr("menu.cheats");
	std::string sub = cfg_.game_name;
	draw_hints(g, t);
	draw_header(g, t, title, sub.c_str());
	g->fill_rrect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 30, col(C_SHEET, 236));
	g->clip(VX - 8, VY, VW + 16, view_h());
	for (size_t i = 0; i < rows_.size(); i++) {
		const Row &r = rows_[i];
		float y = VY + (float) i * PITCH - scroll_;
		if (y + RH < VY - 2 || y > VY + view_h() + 2) continue;
		bool sel = (int) i == cursor_;
		uint32_t ink = !r.enabled ? (sel ? col(C_INK, 200) : col(C_TEXT3)) : sel ? col(C_INK) : col(C_TEXT);
		if (sel) g->fill_rrect(VX, y, VW, RH, 20, col(C_TEXT, r.enabled ? 255 : 150));
		else g->fill_rrect(VX, y, VW, RH, 20, col(0xFFFFFF, r.enabled ? 30 : 16));
		float ly = y + (RH - t->line_h(F_LABEL)) * 0.5f;
		float rx = VX + VW;
		float lab_w = (r.kind == K_CHOICE || r.kind == K_SLIDER) ? VW - 380 - 40 : VW - 60;
		if (!r.enabled && !r.reason.empty()) lab_w = 330;
		bool del = page_ == PAGE_CHEATS && (cfg_.caps & CAP_CHEAT_EDIT) && r.kind == K_CHOICE;
		if (del) lab_w -= 140;
		std::string lab = t->fit(F_LABEL, r.label, lab_w);
		if (del) {
			float dx = VX + VW - 380 - 132;
			g->fill_rrect(dx, y + 12, 120, RH - 24, 16, sel ? col(C_INK, 30) : col(0xFFFFFF, 22));
			t->draw_center(F_BODY, dx + 60, y + (RH - t->line_h(F_BODY)) * 0.5f, t->fit(F_BODY, tr("menu.delete"), 108).c_str(), col(C_RED));
		}
		t->draw(F_LABEL, VX + 28, ly, lab.c_str(), ink);
		uint32_t vcol = !r.enabled ? col(C_TEXT3) : sel ? accent_deep() : accent_text();
		if (!r.enabled) {
			if (!r.reason.empty()) {
				std::string rs = t->fit(F_BODY, r.reason, 440);
				t->draw_right(F_BODY, rx - 28, y + (RH - t->line_h(F_BODY)) * 0.5f, rs.c_str(), sel ? col(C_INK, 200) : col(C_TEXT3));
			}
			continue;
		}
		if (r.kind == K_CHOICE || r.kind == K_SLIDER) {
			uint32_t arrow = sel ? col(C_INK, 200) : col(C_TEXT2);
			draw_icon(g, ICON_CHEVRON_L, rx - 380 + 36, y + RH * 0.5f, 24, arrow);
			draw_icon(g, ICON_CHEVRON_R, rx - 36, y + RH * 0.5f, 24, arrow);
			if (r.kind == K_CHOICE) {
				std::string v = t->fit(F_LABEL, row_value_text((int) i), 268);
				t->draw_center(F_LABEL, rx - 308 + 118, ly, v.c_str(), vcol);
			} else {
				float tx = rx - 308 + 12, tw = 160, ty = y + RH * 0.5f;
				g->fill_rrect(tx, ty - 3, tw, 6, 3, sel ? col(C_INK, 40) : col(0xFFFFFF, 40));
				float frac = (float) (r.value - r.vmin) / (float) (r.vmax - r.vmin);
				if (frac > 0) g->fill_rrect(tx, ty - 3, std::max(6.0f, tw * frac), 6, 3, vcol);
				g->fill_circle(tx + tw * frac, ty, 9, sel ? accent_deep() : col(C_TEXT));
				t->draw_right(F_LABEL, rx - 72 - 6, ly, row_value_text((int) i).c_str(), vcol);
			}
		} else if (r.kind == K_LINK || r.kind == K_ACTION) {
			if (!r.value_text.empty()) {
				std::string v = t->fit(F_BODY, r.value_text, 430);
				t->draw_right(F_BODY, rx - 60, y + (RH - t->line_h(F_BODY)) * 0.5f, v.c_str(), vcol);
			}
			if (r.kind == K_LINK) draw_icon(g, ICON_CHEVRON_R, rx - 32, y + RH * 0.5f, 24, sel ? col(C_INK, 200) : col(C_TEXT2));
		}
	}
	g->clip_off();
	/* scroll indicator */
	if (content_h() > view_h() + 1) {
		float th = view_h() * view_h() / content_h();
		float ty = VY + (view_h() - th) * (scroll_ / (content_h() - view_h()));
		g->fill_rrect(PANEL_X + PANEL_W - 14, ty, 4, th, 2, col(0xFFFFFF, 70));
	}
	/* the help line of the focused row */
	g->fill_rect(PANEL_X + 24, HELP_Y - 8, PANEL_W - 48, 1, col(0xFFFFFF, 26));
	if (cursor_ >= 0 && cursor_ < (int) rows_.size() && !notices_.active()) {     /* a notice takes the help line's place */
		const Row &r = rows_[(size_t) cursor_];
		std::string help = r.enabled || r.reason.empty() ? r.help : r.reason;
		t->wrap(F_BODY, 56, HELP_Y + 2, 830, 2, 4, help, col(C_TEXT2));
	}
}

void Menu::draw_states(Gfx *g, Text *t)
{
	draw_hints(g, t);
	draw_header(g, t, tr("menu.save_states"), cfg_.game_name.c_str());
	g->fill_rrect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, 30, col(C_SHEET, 236));
	g->clip(VX - 8, VY, 432, view_h());
	for (int i = 0; i < STATE_SLOTS; i++) {
		float y = VY + i * PITCH - scroll_;
		if (y + RH < VY - 2 || y > VY + view_h() + 2) continue;
		bool sel = i + 1 == slot_;
		if (sel) g->fill_rrect(VX, y, 420, RH, 20, col(C_TEXT));
		else g->fill_rrect(VX, y, 420, RH, 20, col(0xFFFFFF, 30));
		const float tw = 90, th = 59;
		float tx = VX + 12, ty = y + (RH - th) * 0.5f;
		if (thumb_tex_[i]) g->tex_draw(thumb_tex_[i], 0, 0, THUMB_W, THUMB_H, tx, ty, tw, th, col(0xFFFFFF), true);
		else g->fill_rrect(tx, ty, tw, th, 8, sel ? col(C_INK, 30) : col(0xFFFFFF, 26));
		uint32_t ink = sel ? col(C_INK) : col(C_TEXT);
		uint32_t dim = sel ? col(C_INK, 225) : col(C_TEXT2);
		t->draw(F_LABEL, tx + tw + 16, y + 10, strf(tr("menu.slot"), i + 1).c_str(), ink);
		std::string st = slots_[i].present ? stamp_text(slots_[i].saved) : tr("menu.states.empty");
		if (slots_[i].present && st.empty()) st = tr("menu.saved");
		t->draw(F_BODY, tx + tw + 16, y + 8 + t->line_h(F_LABEL), t->fit(F_BODY, st, 420 - 12 - tw - 16 - 12).c_str(), dim);
	}
	g->clip_off();
	if (content_h() > view_h() + 1) {
		float th = view_h() * view_h() / content_h();
		float ty = VY + (view_h() - th) * (scroll_ / (content_h() - view_h()));
		g->fill_rrect(VX + 428, ty, 4, th, 2, col(0xFFFFFF, 70));
	}
	/* the picture of the selected slot */
	const float px = 476, py = 108, pw = 448, ph = 170;
	g->fill_rrect(px, py, pw, ph, 18, col(0x000000, 110));
	bool filled = slots_[slot_ - 1].present;
	if (filled && shot_tex_ && shot_w_ > 0) {
		float s = std::min(pw / shot_w_, ph / shot_h_);
		float dw = shot_w_ * s, dh = shot_h_ * s;
		g->tex_draw(shot_tex_, 0, 0, (float) shot_w_, (float) shot_h_, px + (pw - dw) * 0.5f, py + (ph - dh) * 0.5f, dw, dh, col(0xFFFFFF), true);
	} else if (filled && thumb_tex_[slot_ - 1]) {
		float dw = THUMB_W * 3.0f, dh = THUMB_H * 3.0f;
		g->tex_draw(thumb_tex_[slot_ - 1], 0, 0, THUMB_W, THUMB_H, px + (pw - dw) * 0.5f, py + (ph - dh) * 0.5f, dw, dh, col(0xFFFFFF), true);
	} else {
		t->draw_center(F_SUB, px + pw * 0.5f, py + ph * 0.5f - t->line_h(F_SUB) * 0.5f, filled ? tr("menu.no_picture") : tr("menu.empty_slot"), col(C_TEXT3));
	}
	std::string cap = strf(tr("menu.slot_of"), slot_, (int) STATE_SLOTS), cap2;
	if (filled) {
		std::string st = stamp_text(slots_[slot_ - 1].saved);
		if (!st.empty()) cap += "  -  " + st;
		if (slots_[slot_ - 1].play_seconds >= 0) {
			int64_t s = slots_[slot_ - 1].play_seconds;
			cap2 = s >= 3600 ? strf(tr("menu.h_m_played"), (long long) (s / 3600), (long long) (s / 60 % 60)) : strf(tr("menu.min_played"), (long long) (s / 60));
		}
	} else cap += std::string("  -  ") + tr("menu.states.empty");
	t->draw(F_BODY, px + 6, py + ph + 2, t->fit(F_BODY, cap, 440).c_str(), col(C_TEXT2));
	if (!cap2.empty()) t->draw(F_BODY, px + 6, py + ph + 2 + t->line_h(F_BODY), t->fit(F_BODY, cap2, 440).c_str(), col(C_TEXT3));
	/* the buttons */
	struct { const char *label; int id; bool ok; bool primary; } bt[3] = {
		{ tr("menu.load"), BTN_LOAD, filled, true }, { tr("menu.save_here"), BTN_SAVE, true, false }, { tr("menu.delete"), BTN_DELETE, filled, false } };
	for (int i = 0; i < 3; i++) {
		float bx = 476 + i * 154.0f;
		bool pr = bt[i].primary && bt[i].ok;
		g->fill_rrect(bx, 348, 140, 80, 20, pr ? accent(255) : col(0xFFFFFF, bt[i].ok ? 36 : 16));
		uint32_t ink = !bt[i].ok ? col(C_TEXT3) : pr ? col(C_INK) : (bt[i].id == BTN_DELETE ? col(C_RED) : col(C_TEXT));
		draw_box_label(t, F_LABEL, bx + 70, 348, 80, 124, bt[i].label, ink);
	}
	if (undo_avail_) {
		g->fill_rrect(476, 436, 448, 80, 20, col(0xFFFFFF, 36));
		draw_icon(g, ICON_RESET, 476 + 56, 476, 30, col(C_TEXT));
		t->draw(F_LABEL, 476 + 92, 436 + (80 - t->line_h(F_LABEL)) * 0.5f, t->fit(F_LABEL, tr("menu.undo_last_load"), 448 - 92 - 20).c_str(), col(C_TEXT));
	}
}

void Menu::draw_btntest(Gfx *g, Text *t)
{
	g->fill_rect(0, 0, 960, 544, col(C_BG, 255));   /* (opaque: the page behind showed through the rows) */
	t->draw(F_TITLE, 44, 22, tr("menu.button_test"), col(C_TEXT));
	t->draw(F_CAPTION, 46, 22 + t->line_h(F_TITLE), cfg_.game_name.empty() ? cfg_.console_label.c_str() : cfg_.game_name.c_str(), col(C_TEXT2));
	const float RH2 = 30, PITCH2 = 34, W = 424, Y0 = 108;
	struct Line { std::string name, text; bool lit; };
	std::vector<Line> left, right;
	const bool ec = cfg_.enter_is_cross;
	for (int i = 0; i < 12; i++) left.push_back({ phys_label(1u << i, ec), bt_actions(1u << i), (bt_pad_.buttons & (1u << i)) != 0 });
	{
		int sl = profile_ ? profile_->stick_left : STICK_MOVE, sr = profile_ ? profile_->stick_right : STICK_OFF;
		int on = profile_ ? profile_->stick_on : 72;
		auto words = [&](int mode, uint32_t dirs) -> std::string {
			if (mode == STICK_OFF) return tr("menu.not_used");
			if (mode == STICK_MOVE) return tr("menu.moves_like_the_d_pad");
			std::string s;
			for (int k = 0; k < 4; k++) { std::string a = bt_actions(dirs << k); if (!a.empty()) s += (s.empty() ? "" : ", ") + a; }
			return s.empty() ? tr("menu.not_set") : s;
		};
		bool ll = abs(bt_pad_.lx - 128) >= on || abs(bt_pad_.ly - 128) >= on, rr = abs(bt_pad_.rx - 128) >= on || abs(bt_pad_.ry - 128) >= on;
		right.push_back({ strf(tr("menu.left_stick_2"), bt_pad_.lx, bt_pad_.ly), words(sl, PH_LS_UP), ll });
		right.push_back({ strf(tr("menu.right_stick_2"), bt_pad_.rx, bt_pad_.ry), words(sr, PH_RS_UP), rr });
		right.push_back({ bt_touch_.down ? strf(tr("menu.touch_screen_2"), (int) bt_touch_.x, (int) bt_touch_.y) : std::string(tr("menu.touch_screen")), bt_touch_.down ? tr("menu.touching") : "", bt_touch_.down });
		bool rear = profile_ && profile_->back_touch;
		for (int k = 0; k < 4; k++) {
			uint32_t bit = PH_BACK_TL << k;
			std::string a = rear ? bt_actions(bit) : std::string(tr("menu.off"));
			if (rear && a.empty()) a = tr("menu.not_set");
			right.push_back({ phys_label(bit, ec), a, (bt_pad_.back & (1u << k)) != 0 });
		}
	}
	for (int col_i = 0; col_i < 2; col_i++) {
		const std::vector<Line> &v = col_i ? right : left;
		float x = col_i ? 492 : 44;
		for (size_t i = 0; i < v.size(); i++) {
			float y = Y0 + (float) i * PITCH2;
			g->fill_rrect(x, y, W, RH2, 15, v[i].lit ? accent(255) : col(0xFFFFFF, 26));
			uint32_t ink = v[i].lit ? col(C_INK) : col(C_TEXT);
			float ty = y + (RH2 - t->line_h(F_BODY)) * 0.5f;
			std::string nm = t->fit(F_BODY, v[i].name, 300);   /* ("Rear touch: bottom right" whole) */
			t->draw(F_BODY, x + 14, ty, nm.c_str(), ink);
			std::string tx = t->fit(F_BODY, v[i].text, W - 14 - 14 - t->width(F_BODY, nm.c_str()) - 12);
			t->draw_right(F_BODY, x + W - 14, ty, tx.c_str(), v[i].lit ? col(C_INK, 200) : col(C_TEXT2));
		}
	}
	/* the leaving bar and the one line that says how to leave */
	if (bt_hold_ms_ > 0 || bt_touch_ms_ > 0) {
		float w = 300.0f * std::max((float) std::min(bt_hold_ms_, 500) / 500.0f, (float) std::min(bt_touch_ms_, 1000) / 1000.0f);
		g->fill_rrect(330, 500, 300, 6, 3, col(0xFFFFFF, 40));
		g->fill_rrect(330, 500, w, 6, 3, accent(255));
	}
	t->draw_center(F_BODY, 480, 512, tr("menu.to_leave_hold_start_select_or"), col(C_TEXT2));
}

void Menu::draw_modal(Gfx *g, Text *t)
{
	if (modal_ == M_BTNTEST) { draw_btntest(g, t); return; }
	g->fill_rect(0, 0, 960, 544, col(C_BG, 150));
	float cx = 200, cy = modal_ == M_CAPTURE ? 150 : 170, cw = 560, ch = modal_ == M_CAPTURE ? 336 : 254;
	g->fill_rrect(cx - 6, cy + 6, cw + 12, ch + 12, 34, col(0x000000, 90));
	g->fill_rrect(cx, cy, cw, ch, 30, col(C_PLATE, 250));
	std::string title, body, yes, no;
	switch (modal_) {
	case M_OVERWRITE: title = strf(tr("menu.overwrite_slot"), modal_arg_); body = tr("menu.the_state_saved_there_will_be"); yes = tr("menu.overwrite"); no = tr("menu.keep"); break;
	case M_DELETE: title = strf(tr("menu.delete_slot"), modal_arg_); body = tr("menu.the_saved_state_and_its_picture"); yes = tr("menu.delete"); no = tr("menu.keep"); break;
	case M_LIST: title = tr("menu.back_to_the_game_list"); body = tr("menu.anything_the_game_has_not_saved"); yes = tr("menu.quit"); no = tr("menu.stay"); break;
	case M_EXIT: title = cfg_.exit_help.empty() ? std::string(tr("menu.modal.exit_title")) : trf("menu.modal.exit_title_named", tr(cfg_.exit_help.c_str())); body = tr("menu.anything_the_game_has_not_saved"); yes = exit_label(); no = tr("menu.stay"); break;
	case M_RESET: title = tr("menu.reset_the_game"); body = tr("menu.the_game_starts_again_from_the"); yes = tr("menu.reset"); no = tr("menu.cancel"); break;
	case M_CTL_RESET: title = tr("menu.reset_the_controls"); body = tr("menu.every_button_stick_and_turbo_setting"); yes = tr("menu.reset"); no = tr("menu.cancel"); break;
	case M_CHEAT_DELETE:
		title = tr("menu.delete_this_cheat");
		body = modal_arg_ >= 0 && modal_arg_ < (int) cheats_.size() ? trf("menu.modal.cheat_delete_body_named", cheats_[(size_t) modal_arg_].name.c_str()) : std::string(tr("menu.modal.cheat_delete_body"));
		yes = tr("menu.delete"); no = tr("menu.keep");
		break;
	default: {
		const char *lab = model_ ? action_label(*model_, cap_action_) : "";
		title = strf(tr("menu.press_a_button_for"), lab);
		body = action_is_shortcut(cap_action_) ? tr("menu.press_one_button_or_hold_several") : tr("menu.press_the_vita_button_stick_direction");
		yes = tr("menu.clear"); no = tr("menu.cancel");
		break;
	}
	}
	t->draw(F_SUB, cx + 32, cy + 28, t->fit(F_SUB, title, cw - 64).c_str(), col(C_TEXT));
	t->wrap(F_BODY, cx + 32, cy + 28 + t->line_h(F_SUB) + 12, cw - 64, 2, 4, body, col(C_TEXT2));
	float by = cy + (modal_ == M_CAPTURE ? 216 : 150);
	if (modal_ == M_CAPTURE) {
		int secs = (cap_ms_ + 999) / 1000;
		if (secs < 0) secs = 0;
		std::string w = cap_bits_ ? phys_label(cap_bits_, cfg_.enter_is_cross) : strf(tr("menu.waiting"), secs);
		t->draw_center(F_SUB, cx + cw * 0.5f, cy + 150, w.c_str(), accent_text());
	}
	g->fill_rrect(cx + 24, by, 250, 80, 22, modal_ == M_CAPTURE ? col(0xFFFFFF, 40) : (modal_ == M_DELETE || modal_ == M_CHEAT_DELETE || modal_ == M_RESET || modal_ == M_LIST || modal_ == M_EXIT ? col(C_RED) : accent(255)));
	uint32_t yink = (modal_ == M_CAPTURE) ? col(C_TEXT) : col(C_INK);
	draw_box_label(t, F_LABEL, cx + 24 + 125, by, 80, 226, yes, yink);
	g->fill_rrect(cx + cw - 24 - 250, by, 250, 80, 22, col(0xFFFFFF, 40));
	draw_box_label(t, F_LABEL, cx + cw - 24 - 125, by, 80, 226, no, col(C_TEXT));
}

void Menu::draw_overlays(Gfx *g)
{
	/* the finger's pressed highlight */
	if (t_down_ && !t_scrolling_ && t_region_ >= 0 && t_region_ < (int) regions_.size()) {
		const Region &r = regions_[(size_t) t_region_];
		if (r.kind != REGION_OUTSIDE && r.kind != REGION_SLIDER && t_x_ >= r.x && t_x_ < r.x + r.w && t_y_ >= r.y && t_y_ < r.y + r.h) {
			g->clip(r.x, r.y, r.w, r.h + 0.0f);
			g->fill_rrect(r.vx, r.vy, r.vw, r.vh, r.radius, col(0xFFFFFF, 48));
			g->clip_off();
		}
	}
	/* the flash after a tap */
	if (have_flash_ && flash_ms_ > 0) {
		int a = 60 * flash_ms_ / 170;
		g->fill_rrect(flash_.vx, flash_.vy, flash_.vw, flash_.vh, flash_.radius, col(0xFFFFFF, a));
	}
}

void Menu::draw(Gfx *g, Text *t)
{
	if (!open_ || !g || !t) return;
	t->set_look(pk_text_pct(vals_[ROW_UI_TEXT] != 0), vals_[ROW_UI_CONTRAST] != 0);      /* Interface: Text size, High contrast */
	t->frame();      /* (the glyph atlases: a frame has passed) */
	upload_textures(g);
	draw_backdrop(g);
	if (page_ == PAGE_TOP) draw_top(g, t);
	else if (page_ == PAGE_STATES) draw_states(g, t);
	else draw_rows(g, t);
	draw_overlays(g);
	if (modal_) draw_modal(g, t);
	/* over the game picture on the control centre; on the pages in the place of the help line, where it
	 * covers no header, hint or row the player is looking at */
	if (page_ == PAGE_TOP) notices_.draw(g, t, PK_HEX(cfg_.accent, 255), 60, true);
	else if (page_ == PAGE_STATES && !modal_) notices_.draw(g, t, PK_HEX(cfg_.accent, 255), 116, true);   /* over the slot's picture, not its buttons */
	else notices_.draw(g, t, PK_HEX(cfg_.accent, 255), HELP_Y + 2, false);
}

}  // namespace pk
