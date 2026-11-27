/* Unit tests: pk_m3u (.m3u disc lists). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_test.h"
#include "harness.h"
#include "pk_m3u.h"
#include "pk_util.h"
#include <string.h>

using namespace pk;

static int parse(const char *text, const char *where, std::vector<std::string> &out, int *skipped = 0)
{
	static char l[PK_M3U_MAX_DISCS][PK_M3U_PATH];
	int sk = 0;
	int n = pk_m3u_parse(text, strlen(text), where, l, PK_M3U_MAX_DISCS, &sk);
	out.clear();
	for (int i = 0; i < n; i++) out.push_back(l[i]);
	if (skipped) *skipped = sk;
	return n;
}

TEST(m3u_relative_paths_and_folding)
{
	std::vector<std::string> d;
	CHECK_EQ(parse("Disc 1.cue\nsub/Disc 2.chd\n../other/D3.iso\n./a/./b/../c.cue\n", "ux0:data/G/Game.m3u", d), 4);
	CHECK_STR(d[0], "ux0:data/G/Disc 1.cue");
	CHECK_STR(d[1], "ux0:data/G/sub/Disc 2.chd");
	CHECK_STR(d[2], "ux0:data/other/D3.iso");
	CHECK_STR(d[3], "ux0:data/G/a/c.cue");
	/* the .m3u in a folder with no device, and in the current folder */
	CHECK_EQ(parse("a.cue", "/r/s/Game.m3u", d), 1);
	CHECK_STR(d[0], "/r/s/a.cue");
	CHECK_EQ(parse("a.cue", "Game.m3u", d), 1);
	CHECK_STR(d[0], "a.cue");
	/* absolute paths stay as they are */
	CHECK_EQ(parse("ux0:abs/x.iso\n/abs2/y.cue\n", "ux0:data/G/Game.m3u", d), 2);
	CHECK_STR(d[0], "ux0:abs/x.iso");
	CHECK_STR(d[1], "/abs2/y.cue");
}

TEST(m3u_line_ends_comments_bom_backslashes)
{
	std::vector<std::string> d;
	int sk = 0;
	CHECK_EQ(parse("\xEF\xBB\xBF#EXTM3U\r\n#EXTINF:-1,One\r\nOne.cue\r\n\r\n  sub\\Two.CHD \t\r\nthree.iso", "x/G.m3u", d, &sk), 3);
	CHECK_STR(d[0], "x/One.cue");
	CHECK_STR(d[1], "x/sub/Two.CHD");        /* the backslash is a slash; the case is kept */
	CHECK_STR(d[2], "x/three.iso");
	CHECK_EQ(sk, 0);
	/* a lone CR ends a line too (an old Mac editor) */
	CHECK_EQ(parse("a.cue\rb.cue\r", "G.m3u", d), 2);
	/* lines that are not discs are skipped and counted: a text file, another list, no extension */
	CHECK_EQ(parse("a.cue\nreadme.txt\nother.m3u\nnoext\nb.iso\n", "G.m3u", d, &sk), 2);
	CHECK_EQ(sk, 3);
	/* nothing in it */
	CHECK_EQ(parse("", "G.m3u", d), 0);
	CHECK_EQ(parse("# just a comment\n\n   \n", "G.m3u", d), 0);
}

TEST(m3u_limits)
{
	std::vector<std::string> d;
	int sk = 0;
	std::string many;
	for (int i = 0; i < 12; i++) many += "d" + std::to_string(i) + ".cue\n";
	CHECK_EQ(parse(many.c_str(), "G.m3u", d, &sk), PK_M3U_MAX_DISCS);
	CHECK_EQ(sk, 12 - PK_M3U_MAX_DISCS);
	/* a line that is too long for a path is skipped, the next one is read */
	std::string longl(900, 'x');
	CHECK_EQ(parse((longl + ".cue\nok.cue\n").c_str(), "G.m3u", d, &sk), 1);
	CHECK_STR(d[0], "ok.cue");
	CHECK_EQ(sk, 1);
	/* a path made too long by the folder in front of it is skipped as well */
	std::string a(400, 'a'), b(300, 'b');
	CHECK_EQ(parse((b + ".cue\n").c_str(), (a + "/G.m3u").c_str(), d, &sk), 0);
	CHECK_EQ(sk, 1);
}

TEST(m3u_load_reports_missing_discs)
{
	pkh::TempDir t;
	make_dirs(t.path + "/sub");
	{
		FILE *f = fopen((t.path + "/One.cue").c_str(), "wb"); fputs("x", f); fclose(f);
		f = fopen((t.path + "/sub/Two.iso").c_str(), "wb"); fputs("x", f); fclose(f);
		f = fopen((t.path + "/Game.m3u").c_str(), "wb");
		fputs("One.cue\r\nsub\\Two.iso\r\nThree.chd\r\n", f);
		fclose(f);
	}
	static char l[PK_M3U_MAX_DISCS][PK_M3U_PATH];
	int missing = -1, skipped = -1;
	CHECK_EQ(pk_m3u_load((t.path + "/Game.m3u").c_str(), l, PK_M3U_MAX_DISCS, &missing, &skipped), 3);
	CHECK_EQ(missing, 1);                     /* Three.chd is not there; it stays in the list so a disc number keeps its meaning */
	CHECK_EQ(skipped, 0);
	CHECK_STR(std::string(l[2]), t.path + "/Three.chd");
	/* no such list, and a list larger than the limit is not read */
	CHECK_EQ(pk_m3u_load((t.path + "/none.m3u").c_str(), l, PK_M3U_MAX_DISCS, &missing, &skipped), -1);
	{
		FILE *f = fopen((t.path + "/Big.m3u").c_str(), "wb");
		std::string line(1000, '#');
		for (int i = 0; i < 80; i++) { fputs(line.c_str(), f); fputc('\n', f); }
		fputs("a.cue\n", f);
		fclose(f);
	}
	CHECK_EQ(pk_m3u_load((t.path + "/Big.m3u").c_str(), l, PK_M3U_MAX_DISCS, &missing, &skipped), -1);
}

TEST(m3u_names_discs_only_for_a_disc_list)
{
	pkh::TempDir t;
	auto put = [&](const char *name, const char *text) {
		FILE *f = fopen((t.path + "/" + name).c_str(), "wb"); fputs(text, f); fclose(f);
	};
	put("Game.m3u", "Disc 1.cue\nDisc 2.cue\n");
	put("Missing.m3u", "Gone.chd\n");                          /* the disc need not be there: the player says so */
	put("Music.m3u", "#EXTM3U\n#EXTINF:1,x\nsong.mp3\nother/track.flac\n");
	put("Other.m3u", "game.pbp\nrom.bin\nhack.gba\n");
	put("Empty.m3u", "");
	put("Comments.m3u", "# nothing here\n\n");
	CHECK(pk_m3u_names_discs((t.path + "/Game.m3u").c_str()));
	CHECK(pk_m3u_names_discs((t.path + "/Missing.m3u").c_str()));
	CHECK(!pk_m3u_names_discs((t.path + "/Music.m3u").c_str()));
	CHECK(!pk_m3u_names_discs((t.path + "/Other.m3u").c_str()));
	CHECK(!pk_m3u_names_discs((t.path + "/Empty.m3u").c_str()));
	CHECK(!pk_m3u_names_discs((t.path + "/Comments.m3u").c_str()));
	CHECK(!pk_m3u_names_discs((t.path + "/nothing.m3u").c_str()));
}
