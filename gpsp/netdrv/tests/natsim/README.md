# natsim: the NAT and TURN simulator of the online tests

A copy of Galaxy's test world for hole punching and relays, adopted as this
project's own code (GPL-2.0-or-later here, like the rest of the tree).

| file | what it is |
|---|---|
| `natsim.{h,cpp}` | the RFC 4787 NAT world: LANs, NAT mapping and filtering behaviours, impairment (loss, delay, jitter, reorder, duplicates) |
| `turnsim.{h,cpp}` | a TURN server (RFC 5766, UDP, REST credentials), relay-to-relay |
| `natsim_proxy.cpp` | the world as a UDP service on loopback: a client wraps every datagram `0xFE | id | dst ip[4] | dst port[2] | payload` and gets `src ip[4] | src port[2] | payload` back |
| `np_net.{h,cpp}`, `np_hash.{h,cpp}`, `np_json.h`, `np_log.h` | the few helpers the simulator needs (addresses, MD5 / SHA-1 / HMAC-SHA1, a tiny JSON reader, logging) |

Where they came from: the Galaxy 3.0 multiplayer tree (`net/tests/natsim*`,
`net/tests/turnsim*`, `net/src/online/np_net.*`, `np_hash.*`, `np_json.h`,
`net/src/np_log.h`).  Includes were flattened to this folder and the header
comments kept.  One change: the clients' private port inside the simulated LAN
is the option `--lan-port N` (default 19019, VitAdhoc's own port) in place of
Galaxy's `NpNet::kDefaultPort` (41100).

Build and run:

    gpsp/netdrv/tests/natsim/build.sh /tmp/natsim
    /tmp/natsim --port 47000 --turn          # prints "ready"

Options are listed at the top of `natsim_proxy.cpp` (`--nat0 port --nat1 sym`,
`--same-lan`, `--loss`, `--delay`, `--turn-secret`, ...).
