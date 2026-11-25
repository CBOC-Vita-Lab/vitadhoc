/* Unit tests: pk_csearch (the "Search memory" engine). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "pk_csearch.h"
#include "harness.h"
#include <string.h>
#include <vector>
using namespace pk;

namespace {
struct Mem {
	std::vector<uint8_t> a, b;      /* two windows: addresses 0.. and 0x10000.. */
	uint32_t reads = 0;
};
uint32_t rd(void *ud, uint32_t addr, uint8_t *out, uint32_t n)
{
	Mem *m = (Mem *) ud;
	m->reads++;
	std::vector<uint8_t> *w = addr < 0x10000 ? &m->a : &m->b;
	uint32_t off = addr < 0x10000 ? addr : addr - 0x10000;
	if (off >= w->size()) return 0;
	uint32_t k = (uint32_t) w->size() - off < n ? (uint32_t) w->size() - off : n;
	memcpy(out, &(*w)[off], k);
	return k;
}
void setup(pk_cs &cs, Mem &m, int kind = PK_CS_GBA, int order = PK_CS_LE)
{
	m.a.assign(0x3000, 0);          /* not a multiple of the chunk */
	m.b.assign(0x81, 0);            /* an odd size */
	pk_cs_region r[2] = { { 0, 0x3000 }, { 0x10000, 0x81 } };
	pk_cs_init(&cs, rd, &m, kind, order, r, 2);
}
}

TEST(csearch_byte_search_narrows_to_the_changing_counter)
{
	pk_cs cs; Mem m; setup(cs, m);
	m.a[0x1234] = 3;               /* lives */
	m.a[0x2000] = 3;               /* a decoy that never changes */
	m.b[0x40] = 3;                 /* a decoy in the second window, which changes the wrong way */
	CHECK_EQ(pk_cs_start(&cs, 1), 0x3000 + 0x81);
	CHECK_EQ(pk_cs_step(&cs, PK_CS_EQ, 3), 3);
	m.a[0x1234] = 2; m.b[0x40] = 4;          /* a life lost */
	CHECK_EQ(pk_cs_step(&cs, PK_CS_DEC, 0), 1);
	uint32_t ad[8], v[8];
	CHECK_EQ(pk_cs_list(&cs, 8, ad, v), 1);
	CHECK_EQ(ad[0], 0x1234);
	CHECK_EQ(v[0], 2);
	m.a[0x1234] = 1;
	CHECK_EQ(pk_cs_step(&cs, PK_CS_UNCHANGED, 0), 0);
	pk_cs_free(&cs);
}

TEST(csearch_all_operations)
{
	pk_cs cs; Mem m; setup(cs, m);
	for (int i = 0; i < 16; i++) m.a[i] = (uint8_t) (i * 10);
	pk_cs_start(&cs, 1);
	CHECK_EQ(pk_cs_step(&cs, PK_CS_GT, 100), 5);          /* 110..150 */
	CHECK_EQ(pk_cs_step(&cs, PK_CS_LT, 140), 3);          /* 110, 120, 130 */
	CHECK_EQ(pk_cs_step(&cs, PK_CS_NE, 120), 2);
	m.a[11] = 111; m.a[13] = 129;
	CHECK_EQ(pk_cs_step(&cs, PK_CS_CHANGED, 0), 2);
	CHECK_EQ(pk_cs_step(&cs, PK_CS_INC, 0), 0);           /* the snapshot was taken at the last step */
	pk_cs_start(&cs, 1);
	m.a[5] = 51; m.a[6] = 59;
	CHECK_EQ(pk_cs_step(&cs, PK_CS_INC, 0), 1);
	pk_cs_free(&cs);
}

TEST(csearch_wide_values_are_aligned_and_ordered)
{
	pk_cs cs; Mem m; setup(cs, m);
	m.a[0x100] = 0x34; m.a[0x101] = 0x12;      /* 0x1234 little endian at an aligned address */
	m.a[0x203] = 0x34; m.a[0x204] = 0x12;      /* the same bytes, but unaligned: never a candidate */
	pk_cs_start(&cs, 2);
	CHECK_EQ(pk_cs_step(&cs, PK_CS_EQ, 0x1234), 1);
	uint32_t ad[4], v[4];
	CHECK_EQ(pk_cs_list(&cs, 4, ad, v), 1);
	CHECK_EQ(ad[0], 0x100);
	CHECK_EQ(v[0], 0x1234);
	m.a[0x300] = 0x78; m.a[0x301] = 0x56; m.a[0x302] = 0x34; m.a[0x303] = 0x12;
	pk_cs_start(&cs, 4);
	CHECK_EQ(pk_cs_step(&cs, PK_CS_EQ, 0x12345678u), 1);
	pk_cs_free(&cs);
	/* big endian */
	pk_cs be; setup(be, m, PK_CS_NES, PK_CS_BE);
	m.a[0x100] = 0x34; m.a[0x101] = 0x12;
	pk_cs_start(&be, 2);
	CHECK_EQ(pk_cs_step(&be, PK_CS_EQ, 0x3412), 1);
	pk_cs_free(&be);
	/* Mega Drive: little endian words, the high word first */
	pk_cs md; setup(md, m, PK_CS_MDW, PK_CS_MD);
	m.a[0x400] = 0x34; m.a[0x401] = 0x12; m.a[0x402] = 0x78; m.a[0x403] = 0x56;   /* the 68000's long 0x12345678 */
	pk_cs_start(&md, 4);
	CHECK_EQ(pk_cs_step(&md, PK_CS_EQ, 0x12345678u), 1);
	pk_cs_free(&md);
}

TEST(csearch_any_address_finds_odd_places_and_values_across_a_chunk)
{
	pk_cs cs; Mem m; setup(cs, m, PK_CS_SNES);
	CHECK(pk_cs_can_any(&cs));
	m.a[0x203] = 0x34; m.a[0x204] = 0x12;      /* 0x1234 at an odd address */
	m.a[0xFFF] = 0x34; m.a[0x1000] = 0x12;     /* ... straddling the first 4 KB chunk */
	CHECK_EQ(pk_cs_start_ex(&cs, 2, 1), 0x3000 + 0x81 - 2);   /* every address but each window's last byte */
	CHECK_EQ(pk_cs_step(&cs, PK_CS_EQ, 0x1234), 2);
	uint32_t ad[4], v[4];
	CHECK_EQ(pk_cs_list(&cs, 4, ad, v), 2);
	CHECK_EQ(ad[0], 0x203); CHECK_EQ(ad[1], 0xFFF);
	CHECK_EQ(v[0], 0x1234); CHECK_EQ(v[1], 0x1234);
	/* the one that straddled the chunk changes: the snapshot across the chunk edge is the old one */
	m.a[0x1000] = 0x13;
	CHECK_EQ(pk_cs_step(&cs, PK_CS_CHANGED, 0), 1);
	CHECK_EQ(pk_cs_list(&cs, 4, ad, v), 1);
	CHECK_EQ(ad[0], 0xFFF);
	CHECK_EQ(v[0], 0x1334);
	char c[64];
	CHECK(pk_cs_code(&cs, 0xFFF, 0x1334, c, sizeof c) > 0); CHECK_STR(c, "7E0FFF:34+7E1000:13");
	/* a 4 byte value in the first window's last bytes stays inside it */
	pk_cs_start_ex(&cs, 4, 1);
	CHECK_EQ(cs.count, 0x3000 + 0x81 - 6);
	/* aligned search is what it was, and any is ignored for a byte */
	CHECK_EQ(pk_cs_start_ex(&cs, 1, 1), 0x3000 + 0x81);
	CHECK_EQ(cs.any, 0);
	pk_cs_start(&cs, 2);
	CHECK_EQ(cs.any, 0);
	pk_cs_free(&cs);
	pk_cs gba; Mem g; setup(gba, g, PK_CS_GBA);
	CHECK(!pk_cs_can_any(&gba));
	pk_cs md; Mem d; setup(md, d, PK_CS_MDW, PK_CS_MD);
	CHECK(!pk_cs_can_any(&md));
	CHECK_EQ(pk_cs_size_count(&gba), 3); CHECK_EQ(pk_cs_size_count(&cs), 5);
	CHECK_STR(pk_cs_size_choice(3), "2 bytes, any address");
	CHECK(pk_cs_size_width(4) == 4 && pk_cs_size_any(4) && !pk_cs_size_any(2));
}

TEST(csearch_code_at_address_zero_is_made_on_every_console)
{
	pk_cs cs; Mem m; setup(cs, m, PK_CS_SNES);
	char c[64];
	cs.width = 1;
	CHECK(pk_cs_code(&cs, 0, 0x56, c, sizeof c) > 0); CHECK_STR(c, "7E0000:56");
	cs.kind = PK_CS_GBA; CHECK(pk_cs_code(&cs, 0, 0x56, c, sizeof c) > 0); CHECK_STR(c, "03000000 56");
	cs.kind = PK_CS_NES; CHECK(pk_cs_code(&cs, 0, 0x56, c, sizeof c) > 0); CHECK_STR(c, "0000:56");
	cs.kind = PK_CS_WS; CHECK(pk_cs_code(&cs, 0, 0x56, c, sizeof c) > 0); CHECK_STR(c, "00000:56");
	pk_cs_free(&cs);
}

TEST(csearch_unreadable_memory_is_never_a_candidate)
{
	pk_cs cs; Mem m; setup(cs, m);
	m.b.assign(0x10, 7);                       /* the second window shrank */
	CHECK_EQ(pk_cs_start(&cs, 1), 0x3000 + 0x10);
	m.b.clear();
	CHECK_EQ(pk_cs_step(&cs, PK_CS_UNCHANGED, 0), 0x3000);
	pk_cs_free(&cs);
	CHECK_EQ(pk_cs_step(&cs, PK_CS_EQ, 0), -1);   /* no search is active */
	CHECK_EQ(pk_cs_start(&cs, 3), -1);
}

TEST(csearch_typed_values)
{
	uint32_t v = 0;
	CHECK(pk_cs_parse("100", 1, &v) && v == 100);
	CHECK(!pk_cs_parse("256", 1, &v));
	CHECK(pk_cs_parse("0xFF", 1, &v) && v == 255);
	CHECK(pk_cs_parse(" 65535 ", 2, &v) && v == 65535);
	CHECK(!pk_cs_parse("65536", 2, &v));
	CHECK(pk_cs_parse("4294967295", 4, &v) && v == 0xFFFFFFFFu);
	CHECK(!pk_cs_parse("4294967296", 4, &v));
	CHECK(!pk_cs_parse("", 1, &v));
	CHECK(!pk_cs_parse("12a", 1, &v));
	CHECK(!pk_cs_parse("-1", 1, &v));
	CHECK(!pk_cs_parse("0x", 1, &v));
}

TEST(csearch_codes_in_each_consoles_format)
{
	char c[128];
	Mem m; pk_cs cs;
	m.a.assign(0x10000, 0); m.b.clear();
	pk_cs_region r[1] = { { 0, 0x10000 } };
	pk_cs_init(&cs, rd, &m, PK_CS_GBA, PK_CS_LE, r, 1);
	cs.width = 1;
	CHECK(pk_cs_code(&cs, 0x0010, 0x63, c, sizeof c) > 0); CHECK_STR(c, "03000010 63");
	CHECK(pk_cs_code(&cs, 0x8010, 0x63, c, sizeof c) > 0); CHECK_STR(c, "02000010 63");
	cs.width = 2; CHECK(pk_cs_code(&cs, 0x0010, 0x1234, c, sizeof c) > 0); CHECK_STR(c, "03000010 1234");
	cs.width = 4; CHECK(pk_cs_code(&cs, 0x0010, 0x12345678, c, sizeof c) > 0); CHECK_STR(c, "03000010:12345678");
	CHECK(pk_cs_code(&cs, 0x8010, 0x2A, c, sizeof c) > 0); CHECK_STR(c, "02000010:0000002A");   /* 4 bytes whatever the value */
	cs.kind = PK_CS_GB; cs.width = 1;
	CHECK(pk_cs_code(&cs, 0xC123, 0x09, c, sizeof c) > 0); CHECK_STR(c, "010923C1");
	cs.width = 2; CHECK(pk_cs_code(&cs, 0xC123, 0x1234, c, sizeof c) > 0); CHECK_STR(c, "013423C1+011224C1");
	/* D000-DFFF: work RAM bank 1 by name, the bank the search read */
	cs.width = 1; CHECK(pk_cs_code(&cs, 0xD123, 0x09, c, sizeof c) > 0); CHECK_STR(c, "910923D1");
	CHECK(pk_cs_code(&cs, 0xFF80, 0x09, c, sizeof c) > 0); CHECK_STR(c, "010980FF");
	cs.width = 2; CHECK(pk_cs_code(&cs, 0xCFFF, 0x1234, c, sizeof c) > 0); CHECK_STR(c, "0134FFCF+911200D0");
	cs.kind = PK_CS_NES; cs.width = 1;
	CHECK(pk_cs_code(&cs, 0x0075, 0x05, c, sizeof c) > 0); CHECK_STR(c, "0075:05");
	cs.kind = PK_CS_WS;
	CHECK(pk_cs_code(&cs, 0x0075, 0x05, c, sizeof c) > 0); CHECK_STR(c, "00075:05");
	cs.kind = PK_CS_SNES;
	CHECK(pk_cs_code(&cs, 0x1234, 0x56, c, sizeof c) > 0); CHECK_STR(c, "7E1234:56");
	CHECK(pk_cs_code(&cs, 0xFFFF, 0x56, c, sizeof c) > 0); CHECK_STR(c, "7EFFFF:56");
	cs.kind = PK_CS_Z80;
	CHECK(pk_cs_code(&cs, 0x0123, 0x56, c, sizeof c) > 0); CHECK_STR(c, "C123:56");
	/* Mega Drive: the word as it is now, at the even 68000 address */
	cs.kind = PK_CS_MDW; cs.order = PK_CS_MD;
	m.a[0x20] = 0x34; m.a[0x21] = 0x12;
	/* a byte: the byte at its 68000 address (held at 0x21 = the 68000's FF0020), nothing beside it, also when it is 0 */
	cs.width = 1; CHECK(pk_cs_code(&cs, 0x21, 0x12, c, sizeof c) > 0); CHECK_STR(c, "FF0020:12");
	CHECK(pk_cs_code(&cs, 0x21, 0, c, sizeof c) > 0); CHECK_STR(c, "FF0020:00");
	CHECK(pk_cs_code(&cs, 0x20, 0x34, c, sizeof c) > 0); CHECK_STR(c, "FF0021:34");
	cs.width = 2; CHECK(pk_cs_code(&cs, 0x20, 0x1234, c, sizeof c) > 0); CHECK_STR(c, "FF0020:1234");
	/* a word under 0100 is stored by the player as its low byte alone: its high byte is written too */
	CHECK(pk_cs_code(&cs, 0x20, 0x0003, c, sizeof c) > 0); CHECK_STR(c, "FF0020:0003+FF0020:00");
	m.a[0x22] = 0x78; m.a[0x23] = 0x56;
	cs.width = 4; CHECK(pk_cs_code(&cs, 0x20, 0x12345678, c, sizeof c) > 0); CHECK_STR(c, "FF0020:1234+FF0022:5678");
	CHECK(pk_cs_code(&cs, 0x20, 0x00000064, c, sizeof c) > 0); CHECK_STR(c, "FF0020:0000+FF0020:00+FF0022:0064+FF0022:00");
	CHECK_EQ(pk_cs_code(&cs, 0x20, 1, c, 4), 0);   /* too small a buffer */
}

TEST(csearch_words_are_the_same_everywhere)
{
	CHECK_STR(pk_cs_size_name(1), "1 byte");
	CHECK_STR(pk_cs_size_name(4), "4 bytes");
	CHECK_STR(pk_cs_op_name(PK_CS_NE), "Not equal to");
	CHECK_STR(pk_cs_op_name(PK_CS_DEC), "Decreased");
	CHECK(pk_cs_op_needs_value(PK_CS_LT) && !pk_cs_op_needs_value(PK_CS_CHANGED));
}

TEST(csearch_gba_sized_search_stays_small_and_fast_enough)
{
	/* the GBA's work RAM: 32 KB + 256 KB */
	std::vector<uint8_t> ram(0x48000, 0);
	pk_cs cs; pk_cs_region r[1] = { { 0, 0x48000 } };
	pk_cs_init(&cs, [](void *ud, uint32_t a, uint8_t *o, uint32_t n) -> uint32_t {
		std::vector<uint8_t> *v = (std::vector<uint8_t> *) ud;
		if (a >= v->size()) return 0;
		if (n > v->size() - a) n = (uint32_t) v->size() - a;
		memcpy(o, &(*v)[a], n);
		return n;
	}, &ram, PK_CS_GBA, PK_CS_LE, r, 1);
	ram[0x1000] = 9;
	CHECK_EQ(pk_cs_start(&cs, 1), 0x48000);
	CHECK_EQ(pk_cs_step(&cs, PK_CS_EQ, 9), 1);
	CHECK_EQ(pk_cs_start(&cs, 4), 0x48000 / 4);
	/* one search holds a snapshot and a bitmap, nothing else */
	CHECK_EQ(cs.total + (cs.total + 7) / 8, 0x48000 + 0x9000);
	pk_cs_free(&cs);
	CHECK(cs.snap == 0 && cs.bits == 0);
}

/* ---- the menu's pages ---------------------------------------------------------------------------------------- */

namespace {
struct SFix {
	pkh::TempDir td;
	pkh::MockHost host;
	pk::States states;
	pk::Menu menu;
	pk::MenuConfig cfg;
	static pk::StateLayout mk(const std::string &root)
	{
		pk::StateLayout l; l.root = root; l.console = "snes"; l.name = "Search Test (USA)";
		return l;
	}
	SFix(bool caps = true) : states(mk(td.path))
	{
		cfg.game_name = "Search Test";
		cfg.console_label = "Super Nintendo";
		cfg.caps = pk::CAP_ALL_BASIC | pk::CAP_CHEATS | pk::CAP_CHEAT_EDIT | (caps ? pk::CAP_CHEAT_SEARCH : 0u);
		host.mem.assign(0x1000, 0);
		host.search_ok = true;
		menu.configure(cfg, &host, &states);
		menu.open(0);
		menu.update(pkh::idle());
	}
	~SFix() { menu.release_textures(); }
	bool to_row(const char *label)
	{
		for (int k = 0; k < 40; k++) {
			if (menu.cursor() < menu.row_count() && menu.row_label(menu.cursor()) == label) return true;
			pkh::press(menu, PH_DOWN);
		}
		return menu.cursor() < menu.row_count() && menu.row_label(menu.cursor()) == label;
	}
	void open_search()
	{
		menu.goto_page(pk::Menu::PAGE_CHEATS);
		CHECK(to_row("Search memory"));
		pkh::press(menu, PH_CROSS);
	}
};
}  // namespace

TEST(csearch_menu_row_exists_only_with_the_capability)
{
	SFix with;
	with.menu.goto_page(pk::Menu::PAGE_CHEATS);
	CHECK(with.to_row("Search memory"));
	SFix without(false);
	without.menu.goto_page(pk::Menu::PAGE_CHEATS);
	CHECK(!without.to_row("Search memory"));
}

TEST(csearch_menu_flow_new_search_refine_and_add)
{
	SFix f;
	f.host.mem[0x123] = 7;
	f.host.mem[0x200] = 7;
	f.open_search();
	CHECK_EQ(f.menu.page(), pk::Menu::PAGE_CHEAT_SEARCH);
	CHECK(!f.menu.search_active());
	CHECK(f.to_row("New search"));
	pkh::press(f.menu, PH_CROSS);
	CHECK(f.menu.search_active());
	CHECK_EQ(f.menu.search_count(), 0x1000);
	/* Equal to: the menu asks for a number, the host answers */
	CHECK(f.to_row("Equal to"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.host.asked.size(), 1);
	CHECK_STR(f.host.asked[0], "Equal to (a number)");
	f.menu.search_value_entered(true, "300");               /* does not fit a byte */
	CHECK_EQ(f.menu.search_count(), 0x1000);
	CHECK(f.menu.notices().active());
	pkh::press(f.menu, PH_CROSS);
	f.menu.search_value_entered(true, "7");
	CHECK_EQ(f.menu.search_count(), 2);
	/* both are listed with their value; one changes, "Increased" keeps it */
	CHECK(f.to_row("7E0123") && f.to_row("7E0200"));
	f.host.mem[0x123] = 9;
	CHECK(f.to_row("Increased"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.search_count(), 1);
	CHECK(f.to_row("7E0123"));
	CHECK_STR(f.menu.row_value_text(f.menu.cursor()), "9 (0x9)");
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.host.added.size(), 1);
	CHECK_STR(f.host.added[0].first, "7E0123:09");
	CHECK_STR(f.host.added[0].second, "Found at 7E0123");
	/* Changed / Unchanged need no number */
	CHECK(f.to_row("Unchanged"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.search_count(), 1);
	CHECK_EQ(f.host.asked.size(), 2);                        /* no new question for a step without a number */
	/* the search outlives the page and the menu */
	pkh::press(f.menu, PH_CIRCLE);
	CHECK(f.menu.search_active());
}

TEST(csearch_menu_size_row_sets_the_width_of_the_next_search)
{
	SFix f;
	f.host.mem[0x10] = 0x34; f.host.mem[0x11] = 0x12;
	f.open_search();
	CHECK(f.to_row("Size of the number"));
	pkh::press(f.menu, PH_RIGHT);                            /* 2 bytes */
	CHECK_STR(f.menu.row_value_text(f.menu.cursor()), "2 bytes");
	CHECK(f.to_row("New search"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.search_count(), 0x800);
	CHECK(f.to_row("Equal to"));
	pkh::press(f.menu, PH_CROSS);
	f.menu.search_value_entered(true, "0x1234");
	CHECK_EQ(f.menu.search_count(), 1);
	CHECK(f.to_row("7E0010"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.host.added.size(), 1);
	CHECK_STR(f.host.added[0].first, "7E0010:34+7E0011:12");
}

TEST(csearch_menu_any_address_choice_on_a_byte_console)
{
	SFix f;
	f.host.mem[0x11] = 0x34; f.host.mem[0x12] = 0x12;       /* 0x1234 at an odd address */
	f.open_search();
	CHECK(f.to_row("Size of the number"));
	pkh::press(f.menu, PH_RIGHT); pkh::press(f.menu, PH_RIGHT); pkh::press(f.menu, PH_RIGHT);
	CHECK_STR(f.menu.row_value_text(f.menu.cursor()), "2 bytes, any address");
	pkh::press(f.menu, PH_RIGHT); pkh::press(f.menu, PH_RIGHT);
	CHECK_STR(f.menu.row_value_text(f.menu.cursor()), "1 byte");          /* five choices, then round */
	pkh::press(f.menu, PH_LEFT);
	CHECK_STR(f.menu.row_value_text(f.menu.cursor()), "4 bytes, any address");
	pkh::press(f.menu, PH_LEFT);
	CHECK(f.to_row("New search"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.search_count(), 0x1000 - 1);
	CHECK(f.to_row("Equal to"));
	pkh::press(f.menu, PH_CROSS);
	f.menu.search_value_entered(true, "0x1234");
	CHECK_EQ(f.menu.search_count(), 1);
	CHECK(f.to_row("7E0011"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_STR(f.host.added[0].first, "7E0011:34+7E0012:12");
}

TEST(csearch_menu_is_refused_like_cheats)
{
	SFix f;
	f.host.refused = "Hardcore mode: cheats are off";
	f.menu.goto_page(pk::Menu::PAGE_CHEATS);
	CHECK(f.to_row("Search memory"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.page(), pk::Menu::PAGE_CHEATS);          /* did not open */
	CHECK(f.menu.notices().active());
	CHECK_STR(f.menu.notices().text(), "Hardcore mode: cheats are off");
}

TEST(csearch_menu_machine_without_memory_says_so)
{
	SFix f;
	f.host.search_ok = false;
	f.menu.goto_page(pk::Menu::PAGE_CHEATS);
	CHECK(f.to_row("Search memory"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.page(), pk::Menu::PAGE_CHEATS);
	CHECK(f.menu.notices().active());
}

TEST(csearch_menu_lists_results_only_when_few)
{
	SFix f;
	f.open_search();
	CHECK(f.to_row("New search"));
	pkh::press(f.menu, PH_CROSS);
	const int base_rows = f.menu.row_count();
	for (int i = 0; i < 10; i++) f.host.mem[(size_t) i * 3] = 5;
	CHECK(f.to_row("Increased"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.search_count(), 10);
	CHECK_EQ(f.menu.row_count(), base_rows + 10);             /* the results are rows */
	CHECK(f.to_row("7E0000") && f.to_row("7E001B"));
	/* sixty places changed: a count, no list */
	CHECK(f.to_row("New search"));
	pkh::press(f.menu, PH_CROSS);
	for (int i = 0; i < 60; i++) f.host.mem[(size_t) i] = 9;
	CHECK(f.to_row("Changed"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.search_count(), 60);
	CHECK_EQ(f.menu.row_count(), base_rows);
	/* fifty is the most listed */
	CHECK(f.to_row("New search"));
	pkh::press(f.menu, PH_CROSS);
	for (int i = 0; i < 50; i++) f.host.mem[(size_t) i] = 1;
	CHECK(f.to_row("Changed"));
	pkh::press(f.menu, PH_CROSS);
	CHECK_EQ(f.menu.search_count(), 50);
	CHECK_EQ(f.menu.row_count(), base_rows + 50);
}
