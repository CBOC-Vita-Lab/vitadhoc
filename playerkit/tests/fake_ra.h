/* SPDX-License-Identifier: MIT
 * Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE).
 *
 * A stand-in for retroachievements.org on the loopback, for the tests of pk_ra (and the twins' player tests): one user, one token, one
 * game known by its MD5, achievements defined by the test, a record of every request. It answers what the rcheevos client asks for:
 * login2, achievementsets, startsession, awardachievement, ping. One request at a time, like the player's own web-request thread. */
#ifndef PK_FAKE_RA_H
#define PK_FAKE_RA_H

#include <arpa/inet.h>
#include <atomic>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace pk_test {

struct FakeAch {
	int id;
	std::string title, desc;
	int points;
	std::string mem;      /* rcheevos trigger text, e.g. "0xH000010=7" */
};

class FakeRa {
public:
	std::string user = "ra_tester", token = "tok-1";
	std::string md5;                     /* the game's hash as the site knows it */
	std::vector<FakeAch> achs;

	~FakeRa() { stop(); }
	bool start()
	{
		fd_ = socket(AF_INET, SOCK_STREAM, 0);
		if (fd_ < 0) return false;
		int one = 1;
		setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
		sockaddr_in a = {};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = 0;
		if (bind(fd_, (sockaddr *) &a, sizeof a) != 0 || listen(fd_, 4) != 0) return false;
		socklen_t l = sizeof a;
		getsockname(fd_, (sockaddr *) &a, &l);
		port_ = ntohs(a.sin_port);
		run_ = true;
		th_ = std::thread([this] { loop(); });
		return true;
	}
	void stop()
	{
		if (run_.exchange(false) && th_.joinable()) th_.join();
		if (fd_ >= 0) { close(fd_); fd_ = -1; }
	}
	std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
	int count(const std::string &r)
	{
		std::lock_guard<std::mutex> g(mu_);
		int n = 0;
		for (size_t i = 0; i < calls_.size(); i++) if (calls_[i]["r"] == r) n++;
		return n;
	}
	std::vector<std::map<std::string, std::string> > calls()
	{
		std::lock_guard<std::mutex> g(mu_);
		return calls_;
	}
	void clear_calls() { std::lock_guard<std::mutex> g(mu_); calls_.clear(); }

private:
	static std::string urldec(const std::string &s)
	{
		std::string o;
		for (size_t i = 0; i < s.size(); i++) {
			if (s[i] == '+') o += ' ';
			else if (s[i] == '%' && i + 2 < s.size()) { o += (char) strtol(s.substr(i + 1, 2).c_str(), 0, 16); i += 2; }
			else o += s[i];
		}
		return o;
	}
	static std::string esc(const std::string &s)
	{
		std::string o;
		for (size_t i = 0; i < s.size(); i++) { if (s[i] == '"' || s[i] == '\\') o += '\\'; o += s[i]; }
		return o;
	}
	void loop()
	{
		while (run_) {
			fd_set rs;
			FD_ZERO(&rs);
			FD_SET(fd_, &rs);
			timeval tv = { 0, 50000 };
			if (select(fd_ + 1, &rs, 0, 0, &tv) <= 0) continue;
			int c = accept(fd_, 0, 0);
			if (c < 0) continue;
			serve(c);
			close(c);
		}
	}
	void serve(int c)
	{
		std::string req;
		char b[4096];
		size_t hdr = std::string::npos, want = 0;
		timeval tv = { 3, 0 };
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		for (;;) {
			ssize_t n = recv(c, b, sizeof b, 0);
			if (n <= 0) break;
			req.append(b, (size_t) n);
			if (hdr == std::string::npos) {
				hdr = req.find("\r\n\r\n");
				if (hdr != std::string::npos) {
					hdr += 4;
					size_t cl = req.find("Content-Length:");
					if (cl == std::string::npos) cl = req.find("content-length:");
					want = cl == std::string::npos ? 0 : (size_t) atoi(req.c_str() + cl + 15);
				}
			}
			if (hdr != std::string::npos && req.size() >= hdr + want) break;
		}
		std::map<std::string, std::string> q;
		std::string body = hdr == std::string::npos ? "" : req.substr(hdr);
		size_t q0 = req.find('?');
		if (q0 != std::string::npos && q0 < req.find("\r\n")) { size_t e = req.find(' ', q0); body += (body.empty() ? "" : "&") + req.substr(q0 + 1, e - q0 - 1); }
		for (size_t i = 0; i < body.size();) {
			size_t e = body.find('&', i);
			if (e == std::string::npos) e = body.size();
			std::string kv = body.substr(i, e - i);
			size_t eq = kv.find('=');
			if (eq != std::string::npos) q[kv.substr(0, eq)] = urldec(kv.substr(eq + 1));
			i = e + 1;
		}
		int code = 200;
		std::string js = answer(q, &code);
		{
			std::lock_guard<std::mutex> g(mu_);
			q.erase("t");        /* the record keeps no token */
			calls_.push_back(q);
		}
		std::string out = std::string("HTTP/1.1 ") + (code == 200 ? "200 OK" : code == 401 ? "401 Unauthorized" : "400 Bad Request") +
		                  "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(js.size()) + "\r\nConnection: close\r\n\r\n" + js;
		size_t o = 0;
		while (o < out.size()) { ssize_t n = send(c, out.data() + o, out.size() - o, MSG_NOSIGNAL); if (n <= 0) break; o += (size_t) n; }
	}
	std::string answer(std::map<std::string, std::string> &q, int *code)
	{
		const std::string &r = q["r"];
		bool ok = q["u"] == user;
		if (r == "login2") {
			if (!ok || q["t"] != token) {
				*code = 401;
				return "{\"Success\":false,\"Code\":\"invalid_credentials\",\"Error\":\"Invalid User/Password combination.\"}";
			}
			return "{\"Success\":true,\"User\":\"" + user + "\",\"Token\":\"" + token + "\",\"Score\":5,\"SoftcoreScore\":0,\"Messages\":0}";
		}
		if (!ok || q["t"] != token) {
			*code = 401;
			return "{\"Success\":false,\"Code\":\"invalid_token\",\"Error\":\"Invalid token\"}";
		}
		if (r == "achievementsets") {
			if (q["m"] != md5)
				return "{\"Success\":true,\"GameId\":0,\"Title\":\"\",\"ConsoleId\":5,\"ImageIconUrl\":\"\",\"Sets\":[]}";
			std::string a;
			for (size_t i = 0; i < achs.size(); i++) {
				const FakeAch &x = achs[i];
				if (i) a += ",";
				a += "{\"ID\":" + std::to_string(x.id) + ",\"Title\":\"" + esc(x.title) + "\",\"Description\":\"" + esc(x.desc) + "\",\"Flags\":3,\"Points\":" +
				     std::to_string(x.points) + ",\"MemAddr\":\"" + esc(x.mem) + "\",\"Author\":\"test\",\"BadgeName\":\"00001\",\"Created\":1700000000,"
				     "\"Modified\":1700000000,\"Type\":\"\"}";
			}
			const std::string img = "http://127.0.0.1/Images/000001.png";
			return "{\"Success\":true,\"GameId\":77,\"Title\":\"Playerkit Test\",\"ConsoleId\":5,\"ImageIconUrl\":\"" + img + "\",\"RichPresencePatch\":\"\","
			       "\"Sets\":[{\"AchievementSetId\":7,\"GameId\":77,\"Title\":\"Playerkit Test\",\"Type\":\"core\",\"ImageIconUrl\":\"" + img +
			       "\",\"Leaderboards\":[],\"Achievements\":[" + a + "]}]}";
		}
		if (r == "startsession") return "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[],\"ServerNow\":1700000000}";
		if (r == "awardachievement")
			return "{\"Success\":true,\"Score\":10,\"SoftcoreScore\":5,\"AchievementID\":" + std::to_string(atoi(q["a"].c_str())) + ",\"AchievementsRemaining\":1}";
		if (r == "ping") return "{\"Success\":true}";
		*code = 400;
		return "{\"Success\":false,\"Error\":\"unknown request " + esc(r) + "\"}";
	}

	int fd_ = -1;
	int port_ = 0;
	std::atomic<bool> run_{false};
	std::thread th_;
	std::mutex mu_;
	std::vector<std::map<std::string, std::string> > calls_;
};

}  // namespace pk_test

#endif
