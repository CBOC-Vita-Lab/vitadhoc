#!/usr/bin/env python3
# Mock of api.cloudflare.com/client/v4 for test/scripts.test.mjs (zone psvita.dev). State is a JSON file named by $MOCK_STATE; every call is
# appended to $MOCK_DIR/calls.log as "<METHOD> <path>"; the token never reaches a log. Adapted from the Galaxy service's zone-script test.
import sys, os, json, re
args = sys.argv[1:]
cfg = sys.stdin.read()
url = ""
with open(os.environ["MOCK_DIR"] + "/seen/curl-args", "a") as f: f.write("curl " + " ".join(args) + "\n")
with open(os.environ["MOCK_DIR"] + "/seen/curl-env", "a") as f: f.write(repr(sorted(os.environ.items())) + "\n")
method = "GET"; body = None; auth = False
for line in cfg.splitlines():
    m = re.match(r'(\w[\w-]*) = "(.*)"$', line)
    if not m: continue
    k, v = m.groups()
    if k == "url": url = v
    elif k == "request": method = v
    elif k == "data-binary": body = json.loads(v.replace('\\"', '"').replace('\\\\', '\\'))
    elif k == "header" and v.startswith("Authorization: Bearer "):
        auth = True
        with open(os.environ["MOCK_DIR"] + "/seen/hdr-stdin", "a") as f: f.write(v + "\n")
path = url.split("/client/v4", 1)[1]
st = json.load(open(os.environ["MOCK_STATE"]))
def save(): json.dump(st, open(os.environ["MOCK_STATE"], "w"))
def log(m): open(os.environ["MOCK_DIR"] + "/calls.log", "a").write(m + "\n")
log(method + " " + path.split("?")[0])
def out(code, obj):
    sys.stdout.write(json.dumps(obj) + "\n" + str(code)); save(); sys.exit(0)
def refuse_host():
    if st.get("refuse_host_ratelimit") == "403": err(403, 20120, "The http.host field is not available on your plan")
    err(400, 20217, "invalid expression: field http.host is not allowed on this plan")
def err(code, c, msg): out(code, {"success": False, "errors": [{"code": c, "message": msg}], "result": None})
if not auth: err(401, 10000, "no token")
if path.startswith("/zones?"):
    if st.get("zones_fail"): err(403, 9109, "Unauthorized to access requested resource")
    z = st["zone"]
    res = [] if z is None else [{"id": z["id"], "name": "psvita.dev", "status": z["status"], "account": {"id": z["account"], "name": "Acme Hat Company"}, "owner": {"email": "owner@example.com"}}]
    out(200, {"success": True, "result": res})
m = re.match(r"/zones/(\w+)/settings/browser_check$", path)
if m:
    if st.get("bic") is None: err(403, 9109, "Unauthorized")
    out(200, {"success": True, "result": {"id": "browser_check", "value": st["bic"]}})
m = re.match(r"/zones/(\w+)/bot_management$", path)
if m and method == "PUT":
    if st.get("bot_write") == "no": err(403, 9109, "Unauthorized")
    st["bot"] = body["fight_mode"]; out(200, {"success": True, "result": {"fight_mode": st["bot"]}})
m = re.match(r"/zones/(\w+)/bot_management$", path)
if m:
    if st.get("bot") is None: err(403, 9109, "Unauthorized")
    out(200, {"success": True, "result": {"fight_mode": st["bot"]}})
if re.match(r"/zones/(\w+)/rulesets$", path) and method == "POST":
    if st.get("rulesets_perm") == "read": err(403, 9109, "Unauthorized to write")
    if st.get("rulesets_perm") == "plan": err(403, 20120, "This feature is not available on your plan")
    ph = body["phase"]
    if ph in st["phases"]: err(400, 20002, "a ruleset for this phase already exists")
    if st.get("refuse_host_ratelimit") and ph == "http_ratelimit" and any("http.host" in r.get("expression", "") for r in body["rules"]):
        refuse_host()
    for r in body["rules"]: r["id"] = "r" + str(st["n"]); st["n"] += 1
    st["phases"][ph] = {"id": "rs_" + ph, "rules": body["rules"]}
    out(200, {"success": True, "result": st["phases"][ph]})
m = re.match(r"/zones/(\w+)/rulesets/phases/(\w+)/entrypoint$", path)
if m:
    ph = m.group(2)
    if st.get("rulesets_perm") == "none": err(403, 9109, "Unauthorized to access requested resource owner@example.com")
    if method == "GET":
        p = st["phases"].get(ph)
        if p is None: err(404, 10003, "could not find entrypoint ruleset")
        out(200, {"success": True, "result": p})
    if method == "PUT":
        if st.get("rulesets_perm") == "read": err(403, 9109, "Unauthorized to write")
        for r in body["rules"]: r["id"] = "r" + str(st["n"]); st["n"] += 1
        if st.get("refuse_host_ratelimit") and ph == "http_ratelimit" and any("http.host" in r.get("expression", "") for r in body["rules"]):
            refuse_host()
        st["phases"][ph] = {"id": "rs_" + ph, "rules": body["rules"]}
        out(200, {"success": True, "result": st["phases"][ph]})
m = re.match(r"/zones/(\w+)/rulesets/(\w+)/rules(?:/(\w+))?$", path)
if m:
    if st.get("rulesets_perm") == "read": err(403, 9109, "Unauthorized to write")
    if st.get("rulesets_perm") == "plan": err(403, 20120, "This feature is not available on your plan")
    rs, rid = m.group(2), m.group(3)
    ph = rs[3:]; p = st["phases"][ph]
    if st.get("refuse_host_ratelimit") and ph == "http_ratelimit" and body and "http.host" in body.get("expression", ""):
        refuse_host()
    if method == "POST":
        body["id"] = "r" + str(st["n"]); st["n"] += 1; p["rules"].append(body); out(200, {"success": True, "result": p})
    if method == "PATCH":
        for i, r in enumerate(p["rules"]):
            if r["id"] == rid: body["id"] = rid; p["rules"][i] = body
        out(200, {"success": True, "result": p})
    if method == "DELETE":
        p["rules"] = [r for r in p["rules"] if r["id"] != rid]; out(200, {"success": True, "result": p})
err(404, 7003, "no route " + path)
