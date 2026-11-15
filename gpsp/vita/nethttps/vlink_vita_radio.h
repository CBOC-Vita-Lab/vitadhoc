// vlink_vita_radio.h -- the one constant np_https_sys.cpp takes from Galaxy's radio header.
//
// W0 scaffold: Galaxy's file is its whole Wi-Fi layer; here only the size of the sceNet pool the HTTPS client
// hands to sceNetInit when nothing else initialised it first.  Package H decides the real value together
// with net_vita.c (the app initialises sceNet there) and may delete this file.
#ifndef VLINK_VITA_RADIO_H
#define VLINK_VITA_RADIO_H

#ifndef NP_NET_POOL_BYTES
#define NP_NET_POOL_BYTES (2u * 1024u * 1024u)
#endif

#endif
