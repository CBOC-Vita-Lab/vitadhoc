#!/bin/bash
# Builds the NAT / relay simulator service used by the online pair tests:
#   gpsp/netdrv/tests/natsim/build.sh <output file>
# (the RFC 4787 NAT world, the test TURN server, and the two small helpers
# they use; plain g++, no game headers).  Run it as `<out> --port 47000 --turn`.
set -e
cd "$(dirname "$0")"
OUT=${1:?output file}
mkdir -p "$(dirname "$OUT")"
exec g++ ${M} -O1 -std=gnu++11 -I. -fno-exceptions -fno-rtti \
  natsim.cpp turnsim.cpp natsim_proxy.cpp np_net.cpp np_hash.cpp -o "$OUT" -lpthread
