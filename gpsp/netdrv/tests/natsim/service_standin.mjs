// service_standin.mjs -- a stand-in for online/service/dev/server.mjs (package S), used by
// online_tools.py's FakeService ONLY while that file does not exist.  It speaks the dev server's
// process protocol (options, "READY <port>", one "REQ <method> <path> <status> <code|->" line per
// request, POST /_test/evict, GET /_test/stats) and answers GET /v1/info; every other /v1/ route
// gets 404 not_found.  It keeps no rooms, so scenarios that need rooms wait for the real core.
import http from "node:http";

const opts = { port: 0, env: {}, skew: 0 };
for (let i = 2; i < process.argv.length; i++) {
  const a = process.argv[i];
  if (a === "--port") opts.port = parseInt(process.argv[++i], 10);
  else if (a === "--env") { const kv = process.argv[++i]; const j = kv.indexOf("="); opts.env[kv.slice(0, j)] = kv.slice(j + 1); }
  else if (a === "--clock-skew-ms") opts.skew = parseInt(process.argv[++i], 10);
}
const st = { requests: 0, evictions: 0 };
const send = (res, status, obj) => {
  res.writeHead(status, { "content-type": "application/json", "cache-control": "no-store" });
  res.end(JSON.stringify(obj));
};
const srv = http.createServer((req, res) => {
  const url = new URL(req.url, "http://x");
  let code = "-", status = 200;
  req.resume();
  req.on("end", () => {
    if (url.pathname === "/_test/evict" && req.method === "POST") { st.evictions++; send(res, 200, { ok: true }); }
    else if (url.pathname === "/_test/stats") send(res, 200, { requests: st.requests, rooms: 0, budget: {}, turn: {}, evictions: st.evictions, standin: true });
    else {
      st.requests++;
      if (!(req.headers["user-agent"] || "")) { status = 400; code = "bad_request"; send(res, status, { error: { code, message: "user agent" } }); }
      else if (url.pathname === "/v1/info" && req.method === "GET") send(res, 200, { ok: true, now: Date.now() + opts.skew, minApp: opts.env.MIN_APP_VERSION || "2.0.0", latest: opts.env.LATEST_APP_VERSION || "2.0.0", standin: true });
      else { status = 404; code = "not_found"; send(res, status, { error: { code, message: "not found" } }); }
      console.log(`REQ ${req.method} ${req.url} ${status} ${code}`);
    }
  });
});
srv.listen(opts.port, "127.0.0.1", () => console.log(`READY ${srv.address().port}`));
process.on("SIGTERM", () => process.exit(0));
