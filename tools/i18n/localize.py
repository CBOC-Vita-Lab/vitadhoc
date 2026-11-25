#!/usr/bin/env python3
"""localize.py FILE...: an indented (function-local) `static const char *[const] name[..] = { TR_KEY(..)... };`
becomes an automatic array filled with tr() at each call, so a language change shows at once."""
import re, sys
for f in sys.argv[1:]:
    s = open(f, encoding="utf-8", errors="surrogateescape").read()
    n = 0
    def fix(m):
        global n
        body = m.group(0)
        if "TR_KEY(" not in body:
            return body
        n += 1
        return body.replace("static const char", "const char", 1).replace("TR_KEY(", "tr(")
    s = re.sub(r"^[ \t]+static const char \*(?:const )?\w+\[[^\]]*\](?:\[[^\]]*\])? = \{.*?\};", fix, s, flags=re.S | re.M)
    open(f, "w", encoding="utf-8", errors="surrogateescape").write(s)
    print(f, n)
