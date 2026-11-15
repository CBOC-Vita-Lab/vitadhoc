# tests/onlinehttp

Tests of `online_http.{h,cpp}` (the room client) and of the vendored HTTPS client under it (`nethttps/`).

    python3 -I gpsp/vita/tests/onlinehttp/test_onlinehttp.py [-v]

- `runner.cpp` is one binary with one mode per run (`json`, `flow`, `errors`, `three`, `down`, `blackhole`, `nohttps`, `tls`,
  `selftest`); it is built twice with the app's C++ flags, without and with the host64 Mbed TLS prefix (`$MBEDTLS_HOST`, default
  `/tls/mbedtls/host64`; the TLS cases are SKIPPED without it).
- `fake_service.py` is a fake of the room service on 127.0.0.1: plain HTTP, and TLS (ECDSA and RSA leaves, TLS 1.2 and 1.3, wrong
  host name, untrusted chain) behind a private test CA that reaches the client only through `tls::SetExtraTestRoots`. It refuses a
  request the real service would refuse (missing `v`, empty User-Agent, unknown body keys) and records what it saw (`/__stats`).
- The run against the real service code (`online/service/dev/server.mjs`, package S) is made when that file exists.
- Nothing here leaves the loopback.
