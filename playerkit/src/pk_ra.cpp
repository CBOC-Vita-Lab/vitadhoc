/* See pk_ra.h. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include "pk_ra.h"

#include "pk_ra_plat.h"

#include "rc_client.h"
#include "rc_consoles.h"
extern "C" {
#include "ra_http.h"
#include "rhash/md5.h"
}

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>

namespace pk {

namespace {

const char *const RA_FILE = "retroachievements.txt";
const unsigned RETRY_PUMPS = 60u * 30u;      /* about half a minute of refreshes */
const size_t RA_MAX_ROM = 64u << 20;
const size_t MAX_TOASTS = 8;

struct Call {
	std::string url, post, type;
	char api[24];
	rc_client_server_callback_t cb;
	void *cb_data;
	Call *next;
};

/* the one client of the process (rcheevos' callbacks carry no user pointer of ours) */
Ra *g_ra = 0;

void wipe(std::string &s)
{
	volatile char *v = s.empty() ? 0 : &s[0];
	for (size_t i = 0; v && i < s.size(); i++) v[i] = 0;
	s.clear();
}

}  // namespace

struct Ra::Impl {
	Call *head = 0, *tail = 0;
	bool inflight = false;
	std::string path;
	int (*online)(void *) = 0;
	void *online_ud = 0;
};

struct RaCb {
	static uint32_t RC_CCONV read_memory(uint32_t addr, uint8_t *buf, uint32_t n, rc_client_t *)
	{
		return g_ra && g_ra->host_.read ? g_ra->host_.read(g_ra->host_.ud, addr, buf, n) : 0;
	}
	static void RC_CCONV server_call(const rc_api_request_t *req, rc_client_server_callback_t cb, void *data, rc_client_t *)
	{
		Ra *r = g_ra;
		if (!r) return;
		Call *c = new (std::nothrow) Call();
		if (!c) {
			rc_api_server_response_t rs;
			memset(&rs, 0, sizeof rs);
			rs.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
			rs.body = "";
			cb(&rs, data);
			return;
		}
		c->url = req->url ? req->url : "";
		if (req->post_data) c->post = req->post_data;
		if (req->content_type) c->type = req->content_type;
		const char *p = c->post.empty() ? 0 : strstr(c->post.c_str(), "r=");
		size_t n = 0;
		c->api[0] = 0;
		if (p && (p == c->post.c_str() || p[-1] == '&'))
			for (p += 2; p[n] && p[n] != '&' && n + 1 < sizeof c->api; n++) c->api[n] = p[n];
		c->api[n] = 0;
		if (!n) snprintf(c->api, sizeof c->api, "%s", c->post.empty() ? "get" : "?");
		c->cb = cb;
		c->cb_data = data;
		c->next = 0;
		if (r->m_->tail) r->m_->tail->next = c; else r->m_->head = c;
		r->m_->tail = c;
	}
	static rc_clock_t RC_CCONV now_ms(const rc_client_t *)
	{
		return g_ra && g_ra->host_.now_us ? (rc_clock_t) (g_ra->host_.now_us(g_ra->host_.ud) / 1000u) : 0;
	}
	static void RC_CCONV on_log(const char *msg, const rc_client_t *)
	{
		if (g_ra) g_ra->log("rc %s", msg);
	}
	static int online_tramp(void)
	{
		return !g_ra || !g_ra->m_->online || g_ra->m_->online(g_ra->m_->online_ud);
	}
	static void RC_CCONV login_done(int result, const char *error, rc_client_t *client, void *)
	{
		Ra *r = g_ra;
		if (!r) return;
		const rc_client_user_t *u = rc_client_get_user_info(client);
		r->login_busy_ = false;
		r->log("ra_login ok=%d rc=%d user=%s", result == RC_OK, result, u ? u->username : r->user_.c_str());
		if (result == RC_OK && u) {
			r->retry_login_ = false;
			r->retry_told_ = false;
			if (r->game_on_ && r->want_load_) r->begin_load();
			return;
		}
		if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED) {
			/* The app's token is no longer good. The file is the app's: it is not touched. */
			r->retry_login_ = false;
			r->token_.clear();
			r->toast("RetroAchievements: please sign in again in VitAdhoc's Settings", NOTICE_ERROR);
			return;
		}
		/* Not reached (no Wi-Fi yet, a timeout): the token is still good; tried again every half minute and when a game starts. */
		r->retry_login_ = true;
		r->retry_wait_ = RETRY_PUMPS;
		if (!r->retry_told_) r->toast(std::string("RetroAchievements: ") + (error ? error : "could not sign in, will try again"));
		r->retry_told_ = true;
	}
	static void RC_CCONV game_done(int result, const char *error, rc_client_t *client, void *ud)
	{
		Ra *r = g_ra;
		if (!r) return;
		r->load_busy_ = false;
		if (!r->game_on_ || (unsigned) (uintptr_t) ud != r->game_gen_) return;      /* another game's answer */
		const rc_client_game_t *g = rc_client_get_game_info(client);
		rc_client_user_game_summary_t s;
		memset(&s, 0, sizeof s);
		if (result == RC_OK && g && g->id) {
			rc_client_get_user_game_summary(client, &s);
			r->title_ = g->title ? g->title : "";
			r->n_total_ = s.num_core_achievements;
			r->n_unlocked_ = s.num_unlocked_achievements;
			/* leaderboards are Hardcore's too: a score is sent as one */
			r->game_state_ = (s.num_core_achievements || rc_client_has_leaderboards(client)) ? 1 : -1;
			r->log("ra_game ok=1 id=%u core=%u unsup=%u unlocked=%u hardcore=%d title=\"%s\"", g->id, s.num_core_achievements, s.num_unsupported_achievements,
			       s.num_unlocked_achievements, r->game_hardcore_, r->title_.c_str());
			if (s.num_core_achievements) {
				char t[96];
				snprintf(t, sizeof t, "%u of %u achievements%s", s.num_unlocked_achievements, s.num_core_achievements,
				         r->game_hardcore_ ? " - Hardcore" : "");
				r->toast(t);
			} else r->toast("No achievements for this game yet");
		} else {
			r->game_state_ = -1;
			r->log("ra_game ok=0 rc=%d msg=\"%s\"", result, error ? error : "");
			if (result == RC_NO_GAME_LOADED) r->toast("RetroAchievements does not know this version");
			else if (result != RC_ABORTED && result != RC_LOGIN_REQUIRED)
				r->toast(std::string("Achievements unavailable: ") + (error ? error : "no answer"));
		}
		if (r->game_state_ < 0) rc_client_set_hardcore_enabled(client, 0);       /* nothing to protect */
	}
	static void RC_CCONV on_event(const rc_client_event_t *e, rc_client_t *)
	{
		Ra *r = g_ra;
		if (!r) return;
		char t[128];
		switch (e->type) {
		case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
			r->log("ra_unlock id=%u points=%u title=\"%s\"", e->achievement->id, e->achievement->points, e->achievement->title);
			snprintf(t, sizeof t, "Achievement: %s (%u)", e->achievement->title, e->achievement->points);
			r->toast(t, NOTICE_OK);
			if (r->n_unlocked_ < r->n_total_) r->n_unlocked_++;
			break;
		case RC_CLIENT_EVENT_GAME_COMPLETED:
			r->log("ra_completed hardcore=%d", r->game_hardcore_);
			r->toast(std::string(r->game_hardcore_ ? "Mastered: " : "Completed: ") + r->title_, NOTICE_OK);
			break;
		case RC_CLIENT_EVENT_SUBSET_COMPLETED:
			r->toast(std::string("Set completed: ") + (e->subset && e->subset->title ? e->subset->title : ""), NOTICE_OK);
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
			r->log("ra_lboard start id=%u", e->leaderboard->id);
			r->toast(std::string("Leaderboard attempt: ") + e->leaderboard->title);
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
			r->log("ra_lboard failed id=%u", e->leaderboard->id);
			r->toast(std::string("Leaderboard attempt failed: ") + e->leaderboard->title);
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
			r->log("ra_lboard submit id=%u", e->leaderboard->id);
			r->toast(std::string("Submitted: ") + e->leaderboard->title + " " + (e->leaderboard->tracker_value ? e->leaderboard->tracker_value : ""), NOTICE_OK);
			break;
		case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW: {
			r->log("ra_challenge show id=%u", e->achievement->id);
			bool have = false;
			for (size_t i = 0; i < r->challenges_.size(); i++) if (r->challenges_[i].first == e->achievement->id) have = true;
			if (!have) r->challenges_.push_back(std::make_pair((unsigned) e->achievement->id, std::string(e->achievement->title ? e->achievement->title : "")));
			break;
		}
		case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
			r->log("ra_challenge hide id=%u", e->achievement->id);
			for (size_t i = 0; i < r->challenges_.size(); i++)
				if (r->challenges_[i].first == e->achievement->id) { r->challenges_.erase(r->challenges_.begin() + (long) i); break; }
			break;
		case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
		case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
			/* a value that changes every frame is taken at most every 20th, and the first one always */
			if (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW || r->frame_ - r->prog_at_ >= 20) {
				r->prog_at_ = r->frame_;
				r->progress_ = std::string(e->achievement->title ? e->achievement->title : "") + ": " + e->achievement->measured_progress;
				if (e->type == RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW) r->log("ra_progress show id=%u", e->achievement->id);
			}
			break;
		case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
			r->progress_.clear();
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
			r->log("ra_tracker show id=%u", e->leaderboard_tracker->id);
			r->tracker_ = e->leaderboard_tracker->display;
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
			r->tracker_ = e->leaderboard_tracker->display;
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
			r->log("ra_tracker hide id=%u", e->leaderboard_tracker->id);
			r->tracker_.clear();
			break;
		case RC_CLIENT_EVENT_LEADERBOARD_SCOREBOARD:
			r->log("ra_scoreboard id=%u rank=%u of=%u", e->leaderboard_scoreboard->leaderboard_id, e->leaderboard_scoreboard->new_rank,
			       e->leaderboard_scoreboard->num_entries);
			snprintf(t, sizeof t, "Rank %u of %u", e->leaderboard_scoreboard->new_rank, e->leaderboard_scoreboard->num_entries);
			r->toast(t);
			break;
		case RC_CLIENT_EVENT_SERVER_ERROR:
			r->log("ra_server_error api=%s rc=%d", e->server_error->api ? e->server_error->api : "", e->server_error->result);
			r->toast(std::string("RetroAchievements: ") + (e->server_error->error_message ? e->server_error->error_message : "server error"), NOTICE_ERROR);
			break;
		case RC_CLIENT_EVENT_DISCONNECTED:
			r->log("ra_disconnected");
			r->toast("Achievements will be sent when back online");
			break;
		case RC_CLIENT_EVENT_RECONNECTED:
			r->log("ra_reconnected");
			r->toast("Achievements sent");
			break;
		default:
			break;
		}
	}
};

Ra::Ra() {}
Ra::~Ra() { shutdown(); }

void Ra::log(const char *fmt, ...)
{
	if (!host_.log) return;
	char b[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	host_.log(host_.ud, b);
}

void Ra::toast(const std::string &t, NoticeKind k)
{
	if (toasts_.size() >= MAX_TOASTS) toasts_.erase(toasts_.begin());
	toasts_.push_back(std::make_pair(t, k));
}

bool Ra::take_toast(std::string *text, NoticeKind *kind)
{
	if (toasts_.empty()) return false;
	if (text) *text = toasts_.front().first;
	if (kind) *kind = toasts_.front().second;
	toasts_.erase(toasts_.begin());
	return true;
}

std::string Ra::indicator() const
{
	if (!indic_ || !rc_) return "";
	if (!tracker_.empty()) return tracker_;
	if (!progress_.empty()) return progress_;
	if (challenges_.size() == 1) return "Challenge: " + challenges_[0].second;
	if (challenges_.size() > 1) { char b[32]; snprintf(b, sizeof b, "%u challenges active", (unsigned) challenges_.size()); return b; }
	return "";
}

bool Ra::logged_in() const { return rc_ && rc_client_get_user_info(rc_) != 0; }

/* the account file: read only, never written here */
bool Ra::read_account(const char *data, std::string *user, std::string *token, bool *hardcore, bool *indic, std::string *path_out)
{
	user->clear();
	token->clear();
	*hardcore = false;
	*indic = true;
	std::string path = std::string(data ? data : "") + "/" + RA_FILE;
	if (path_out) *path_out = path;
	FILE *f = fopen(path.c_str(), "r");
	if (!f) f = fopen((path + ".old").c_str(), "r");      /* the app's save was cut short between its two renames */
	if (f) {
		char line[256];
		while (fgets(line, sizeof line, f)) {
			char *v = strchr(line, '=');
			if (!v) continue;
			*v++ = 0;
			for (char *e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r');) *--e = 0;
			if (!strcmp(line, "user")) *user = v;
			else if (!strcmp(line, "token")) *token = v;
			else if (!strcmp(line, "hardcore")) *hardcore = atoi(v) != 0;
			else if (!strcmp(line, "indicators")) *indic = atoi(v) != 0;
		}
		fclose(f);
	}
	return !user->empty() && !token->empty();
}

bool Ra::has_account(const char *data)
{
	std::string u, t, p;
	bool h, i;
	bool ok = read_account(data, &u, &t, &h, &i, &p);
	wipe(t);
	return ok;
}

bool Ra::boot(const char *data, const char *server, const char *version, const RaHost &host)
{
	shutdown();
	host_ = host;
	std::string path;
	read_account(data, &user_, &token_, &hardcore_, &indic_, &path);
	if (user_.empty() || token_.empty()) { wipe(token_); return false; }
	m_ = new (std::nothrow) Impl();
	if (!m_) { wipe(token_); return false; }
	m_->path = path;
	m_->online = host.online;
	m_->online_ud = host.ud;
	g_ra = this;
	rc_ = rc_client_create(RaCb::read_memory, RaCb::server_call);
	if (!rc_) { g_ra = 0; delete m_; m_ = 0; wipe(token_); return false; }
	rc_client_set_get_time_millisecs_function(rc_, RaCb::now_ms);
	rc_client_enable_logging(rc_, RC_CLIENT_LOG_LEVEL_WARN, RaCb::on_log);
	rc_client_set_event_handler(rc_, RaCb::on_event);
	if (server && server[0]) rc_client_set_host(rc_, server);
	char clause[64] = "", ua[192];
	rc_client_get_user_agent_clause(rc_, clause, sizeof clause);
	snprintf(ua, sizeof ua, "VitAdhoc/%s (PlayStation Vita) %s", version ? version : "2.0", clause);
	ra_http_config(ua, RaCb::online_tramp);
	pk_ra_net_up();
	log("ra_boot user=%s hardcore=%d indicators=%d host=%s", user_.c_str(), hardcore_, indic_, server && server[0] ? server : "ra");
	token_login();
	return true;
}

void Ra::token_login()
{
	if (!rc_ || login_busy_ || token_.empty()) return;
	login_busy_ = true;
	rc_client_begin_login_with_token(rc_, user_.c_str(), token_.c_str(), RaCb::login_done, 0);
}

void Ra::shutdown()
{
	if (!rc_) { if (m_) { delete m_; m_ = 0; } return; }
	game_end();
	/* what is queued (an unlock earned a moment ago) gets three seconds to reach the server before the client goes */
	for (int i = 0; (m_->head || m_->inflight) && i < 300; i++) { pump(); plat_thread_sleep_us(10000); }
	if (m_->head || m_->inflight) log("ra_shutdown unsent=1");
	rc_client_destroy(rc_);
	rc_ = 0;
	while (m_->head) {
		Call *c = m_->head;
		m_->head = c->next;
		wipe(c->post);
		delete c;
	}
	m_->tail = 0;
	ra_http_stop(2000000);
	delete m_;
	m_ = 0;
	wipe(token_);
	if (g_ra == this) g_ra = 0;
}

void Ra::pump()
{
	if (!rc_) return;
	ra_http_res res;
	if (m_->inflight && ra_http_take(&res)) {
		if (m_->head) {
			Call *c = m_->head;
			rc_api_server_response_t r;
			m_->head = c->next;
			if (!m_->head) m_->tail = 0;
			log("ra_http api=%s status=%d len=%u note=%s", c->api, res.status, (unsigned) res.len, res.note);
			r.body = res.body ? res.body : "";
			r.body_length = res.len;
			r.http_status_code = res.status;
			c->cb(&r, c->cb_data);      /* may queue the next call */
			wipe(c->post);
			delete c;
		}
		free(res.body);
		m_->inflight = false;
	}
	if (!m_->inflight && m_->head && ra_http_idle() && ra_http_submit(m_->head->url.c_str(), m_->head->post.empty() ? 0 : m_->head->post.c_str(),
	                                                                   m_->head->type.empty() ? 0 : m_->head->type.c_str()) == 0)
		m_->inflight = true;
	if (retry_login_ && !login_busy_ && !rc_client_get_user_info(rc_) && !token_.empty() && (retry_wait_ == 0 || --retry_wait_ == 0)) {
		retry_wait_ = RETRY_PUMPS;
		if (!m_->online || m_->online(m_->online_ud)) token_login();
	}
	if (!framed_) rc_client_idle(rc_);
	framed_ = false;
}

bool Ra::hash_rom(const uint8_t *rom, size_t n, int console, char out[33])
{
	if (!rom || !n || n > RA_MAX_ROM) return false;
	size_t skip = 0;
	if (console == RA_SNES && n - (n / 0x2000) * 0x2000 == 512) skip = 512;
	md5_state_t st;
	md5_byte_t d[16];
	md5_init(&st);
	for (size_t o = skip; o < n;) {
		size_t k = n - o < (1u << 20) ? n - o : (1u << 20);
		md5_append(&st, rom + o, (int) k);
		o += k;
	}
	md5_finish(&st, d);
	for (int i = 0; i < 16; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
	return true;
}

void Ra::game_begin(const uint8_t *rom, size_t n, int console)
{
	if (!rc_) return;
	game_end();
	game_state_ = 0;
	title_.clear();
	n_total_ = n_unlocked_ = 0;
	stale_ = false;
	was_paused_ = false;
	game_gen_++;
	char h[33];
	if (!hash_rom(rom, n, console, h)) {
		log("ra_game hash FAILED size=%u", (unsigned) n);
		game_state_ = -1;
		return;
	}
	hash_ = h;
	game_on_ = true;
	game_hardcore_ = hardcore_;
	rc_client_set_hardcore_enabled(rc_, game_hardcore_ ? 1 : 0);
	log("ra_game_begin hash=%s hardcore=%d", h, game_hardcore_);
	want_load_ = true;
	if (!logged_in() && !login_busy_) token_login();      /* a sign-in that did not get through */
	if (logged_in()) begin_load();
	/* else: login_done() starts the load when it gets through */
}

void Ra::begin_load()
{
	if (!rc_ || !game_on_ || !want_load_ || load_busy_) return;
	want_load_ = false;
	load_busy_ = true;
	rc_client_begin_load_game(rc_, hash_.c_str(), RaCb::game_done, (void *) (uintptr_t) game_gen_);
}

void Ra::game_end()
{
	if (!rc_ || !game_on_) return;
	game_on_ = false;
	want_load_ = false;
	game_state_ = 0;
	game_gen_++;
	challenges_.clear();
	progress_.clear();
	tracker_.clear();
	rc_client_unload_game(rc_);
	log("ra_game_end");
}

void Ra::frame(bool paused)
{
	if (!rc_ || !game_on_) return;
	if (paused) { was_paused_ = true; return; }      /* pump() runs the idle work */
	if (was_paused_) { was_paused_ = false; stale_ = true; }
	if (stale_) { stale_ = false; rc_client_deserialize_progress_sized(rc_, NULL, 0); }
	frame_++;
	rc_client_do_frame(rc_);
	framed_ = true;
}

void Ra::state_loaded()
{
	challenges_.clear();
	progress_.clear();
	tracker_.clear();
	if (game_on_) stale_ = true;
}

void Ra::reset()
{
	if (game_on_ && rc_) rc_client_reset(rc_);
}

bool Ra::hardcore_active() const { return game_on_ && game_hardcore_ && game_state_ >= 0 && game_state_ == 1; }
bool Ra::hardcore_pending() const { return game_on_ && game_hardcore_ && game_state_ == 0; }

}  // namespace pk
