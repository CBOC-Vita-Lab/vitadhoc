# online/service

The VitAdhoc room service: one Cloudflare Worker and one Durable Object (room codes, the
punch handshake, relay credentials).  Package S fills this folder; the protocol is section 1
of the 2.0 plan.  Tests run on node with `node --test`; nothing here is deployed by a test.
