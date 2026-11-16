# Online Play: The HTTPS Client And Its Roots

The room service (`https://vitadhoc.psvita.dev`) is only ever reached through
VitAdhoc's own HTTPS client in `gpsp/vita/nethttps/`. This page says why it
exists, which certificates it trusts and how to change that list. The player
side of online play is in [Online Play](ONLINE.md); building Mbed TLS is in
[Building](BUILDING.md).

## Why Not The System's HTTPS

Cloudflare's free edge certificates are ECDSA, and the Vita's own `sceHttp` /
`sceSsl` give no control over the trusted roots or the TLS versions. The client is the one Galaxy and Strikers use
(same owner), built on Mbed TLS 3.6.7: TLS 1.2 and 1.3, client side only,
certificate checks always on, no system store. The Linux twin builds the same
code without TLS (plain HTTP to the local test service) unless `MBEDTLS=` is
given.

## The Trusted Roots

Only these eight roots are compiled in (`np_https_roots.h`), the ones a
Cloudflare-hosted service can chain to:

| Root | Who serves chains to it |
|---|---|
| GTS Root R1, GTS Root R4 | Google Trust Services (WR* RSA, WE* ECDSA intermediates) |
| ISRG Root X1, ISRG Root X2 | Let's Encrypt (RSA, ECDSA) |
| SSL.com TLS ECC / RSA Root CA 2022 | SSL.com, behind Cloudflare's "TLS Issuing" CAs |
| SSL.com Root Certification Authority ECC / RSA | SSL.com's 2016 roots (Transit CA R2) |

Each root is embedded as DER with its SHA-256. The client checks every hash
again at start-up, so a damaged header fails closed (no connection) rather than
trusting something else. The header's comments give each root's fingerprint and
expiry; the earliest expiry is 2035.

## The Clock Floor

A Vita whose clock is wrong would see every certificate as "not yet valid" or
"expired" and the player would be told the service is broken. The header
records the day the list was generated (`NP_HTTPS_ROOTS_DAY`, now
2026-11-15). A console clock earlier than that day is reported as a clock
problem ("set the date and time"), not as a certificate fault.

## Changing The List

The header is generated, never edited by hand:

```sh
NP_HTTPS_ROOTS_DAY=YYYY-MM-DD python3 gpsp/vita/tools/gen_https_roots.py \
    [CERT_DIR ...] [--new-isrg-roots] [--pin STEM=SHA256 ...] \
    > gpsp/vita/nethttps/np_https_roots.h
```

- `CERT_DIR` defaults to `/etc/ssl/certs` (the `ca-certificates` package). With
  several, each root is taken from the first folder that has it.
- `--pin STEM=SHA256` refuses to write the header unless that root's DER has
  that SHA-256. Pin every root whose fingerprint you have checked against the
  CA's own published value.
- Set `NP_HTTPS_ROOTS_DAY` to the day you generate it; it becomes the clock
  floor.
- The script uses the Python standard library only.

### Adding ISRG Root YE / YR

Let's Encrypt's newer roots are served cross-signed by ISRG Root X2 / X1 for
now, so the current list keeps working. Before those cross-signs stop:

1. Download `ISRG_Root_YE.pem` and `ISRG_Root_YR.pem` from Let's Encrypt and
   check their SHA-256 fingerprints against the values Let's Encrypt
   publishes.
2. Put them in a folder you pass as a `CERT_DIR`.
3. Run the generator with `--new-isrg-roots` and a `--pin` for each.
4. Rebuild and release: older builds keep working only while the cross-signs
   are served.

## Tests

- `gpsp/vita/tests/onlinehttp` (gate step `online-http`): the room client
  against a local fake service, plain HTTP on the twin. With a host Mbed TLS
  prefix (`GATE_MBEDTLS_HOST`) it also runs the TLS cases: a chain to a test
  root succeeds, an unknown root is refused, and a clock before the floor is
  reported as a clock error.
