#!/usr/bin/env python3
"""fake_igd.py -- a pretend home router for netdrv/tests/test_igd.c.

An SSDP responder (UDP, unicast on the loopback; --multicast also joins
239.255.255.250:1900) and the HTTP side of an Internet Gateway Device: the
device description and the WANIPConnection (or WANPPPConnection) control URL
with GetExternalIPAddress, AddPortMapping and DeletePortMapping.

Prints "PORTS <ssdp-udp-port> <http-port>" when it is listening, appends one
line per SOAP call to --log, and runs until killed.  The switches make it
misbehave the ways real routers do.
"""
import argparse
import re
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

p = argparse.ArgumentParser()
p.add_argument("--log", required=True)
p.add_argument("--extip", default="203.0.113.7")
p.add_argument("--taken", default="", help="external ports another device holds (718)")
p.add_argument("--permanent-only", action="store_true", help="725 for a leased mapping")
p.add_argument("--deny", action="store_true", help="606 for AddPortMapping")
p.add_argument("--chunked", action="store_true", help="description sent chunked")
p.add_argument("--no-length", action="store_true", help="responses end by close only")
p.add_argument("--urlbase", action="store_true", help="description has a URLBase")
p.add_argument("--relative", action="store_true", help="controlURL without a leading /")
p.add_argument("--absolute", action="store_true", help="controlURL is a full http:// URL")
p.add_argument("--ppp", action="store_true", help="WANPPPConnection instead of WANIPConnection")
p.add_argument("--no-service", action="store_true", help="no WAN connection service at all")
p.add_argument("--desc-404", action="store_true")
p.add_argument("--silent", action="store_true", help="never answer SSDP")
p.add_argument("--wrong-location", action="store_true", help="first SSDP answer points elsewhere")
p.add_argument("--other-device", action="store_true", help="a media server answers first")
p.add_argument("--slow", action="store_true", help="responses dribble out in pieces")
p.add_argument("--no-extip", action="store_true", help="GetExternalIPAddress fails")
p.add_argument("--multicast", action="store_true")
args = p.parse_args()

taken = {int(x) for x in args.taken.split(",") if x}
mappings = {}
lock = threading.Lock()
SERVICE = ("urn:schemas-upnp-org:service:WANPPPConnection:1" if args.ppp
           else "urn:schemas-upnp-org:service:WANIPConnection:1")


def log(line):
    with lock:
        with open(args.log, "a") as f:
            f.write(line + "\n")


def description(port):
    ctl = "/ctl/IPConn"
    if args.relative:
        ctl = "ctl/IPConn"
    if args.absolute:
        ctl = "http://127.0.0.1:%d/ctl/IPConn" % port
    base = "<URLBase>http://127.0.0.1:%d/</URLBase>" % port if args.urlbase else ""
    wan = "" if args.no_service else """
        <service>
         <serviceType>%s</serviceType>
         <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
         <SCPDURL>/WANIPCn.xml</SCPDURL>
         <controlURL>
           %s
         </controlURL>
         <eventSubURL>/evt/IPConn</eventSubURL>
        </service>""" % (SERVICE, ctl)
    # A decoy first: a service with a controlURL of its own that is NOT the
    # one to use, and a tag whose name merely starts like controlURL.
    return """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
 <specVersion><major>1</major><minor>0</minor></specVersion>
 %s
 <device>
  <deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
  <friendlyName>Fake Router</friendlyName>
  <serviceList>
   <service>
    <serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
    <controlURLish>/nope</controlURLish>
    <controlURL>/ctl/L3F</controlURL>
   </service>
  </serviceList>
  <deviceList>
   <device>
    <deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
    <serviceList>
     <service>
      <serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
      <controlURL>/ctl/CmnIfCfg</controlURL>
     </service>
    </serviceList>
    <deviceList>
     <device>
      <deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
      <serviceList>%s
      </serviceList>
     </device>
    </deviceList>
   </device>
  </deviceList>
 </device>
</root>
""" % (base, wan)


def soap_ok(action, inner=""):
    return ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
            '<u:%sResponse xmlns:u="%s">%s</u:%sResponse></s:Body></s:Envelope>'
            % (action, SERVICE, inner, action))


def soap_err(code, text):
    return ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
            '<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>'
            '<detail><UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>%d</errorCode>'
            '<errorDescription>%s</errorDescription></UPnPError></detail></s:Fault></s:Body>'
            '</s:Envelope>' % (code, text))


def arg(body, name):
    m = re.search(r"<%s>(.*?)</%s>" % (name, name), body, re.S)
    return m.group(1) if m else None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, status, body, chunked=False):
        data = body.encode()
        head = "HTTP/1.1 %d %s\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nConnection: close\r\n" % (
            status, "OK" if status == 200 else "Error")
        if chunked:
            head += "Transfer-Encoding: chunked\r\n\r\n"
            out = b""
            step = 700
            for i in range(0, len(data), step):
                piece = data[i:i + step]
                out += b"%x\r\n" % len(piece) + piece + b"\r\n"
            out += b"0\r\n\r\n"
            data = out
        elif args.no_length:
            head += "\r\n"
        else:
            head += "Content-Length: %d\r\n\r\n" % len(data)
        raw = head.encode() + data
        if args.slow:
            for i in range(0, len(raw), 97):
                self.wfile.write(raw[i:i + 97])
                self.wfile.flush()
                time.sleep(0.004)
        else:
            self.wfile.write(raw)
        self.wfile.flush()
        self.close_connection = True

    def do_GET(self):
        if self.path == "/rootDesc.xml" and not args.desc_404:
            self.reply(200, description(self.server.server_port), args.chunked)
        else:
            self.reply(404, "<html>no</html>")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode(errors="replace")
        action = (self.headers.get("SOAPAction") or "").strip('"')
        if self.path != "/ctl/IPConn" or not action.startswith(SERVICE + "#"):
            log("BAD path=%s action=%s" % (self.path, action))
            self.reply(500, soap_err(401, "Invalid Action"))
            return
        name = action.split("#", 1)[1]
        if ("<u:%s " % name) not in body or SERVICE not in body:
            log("BAD body for %s" % name)
            self.reply(500, soap_err(402, "Invalid Args"))
            return
        if name == "GetExternalIPAddress":
            log("GetExternalIPAddress")
            if args.no_extip:
                self.reply(500, soap_err(501, "Action Failed"))
            else:
                self.reply(200, soap_ok(name, "<NewExternalIPAddress>%s</NewExternalIPAddress>" % args.extip))
        elif name == "AddPortMapping":
            ext = int(arg(body, "NewExternalPort"))
            lease = int(arg(body, "NewLeaseDuration"))
            entry = "proto=%s ext=%d int=%s client=%s lease=%d desc=%s enabled=%s remote=[%s]" % (
                arg(body, "NewProtocol"), ext, arg(body, "NewInternalPort"),
                arg(body, "NewInternalClient"), lease,
                arg(body, "NewPortMappingDescription"), arg(body, "NewEnabled"),
                arg(body, "NewRemoteHost"))
            if args.deny:
                log("AddPortMapping %s -> 606" % entry)
                self.reply(500, soap_err(606, "Action not authorized"))
            elif args.permanent_only and lease != 0:
                log("AddPortMapping %s -> 725" % entry)
                self.reply(500, soap_err(725, "OnlyPermanentLeasesSupported"))
            elif ext in taken:
                log("AddPortMapping %s -> 718" % entry)
                self.reply(500, soap_err(718, "ConflictInMappingEntry"))
            else:
                mappings[ext] = entry
                log("AddPortMapping %s -> ok" % entry)
                self.reply(200, soap_ok(name))
        elif name == "DeletePortMapping":
            ext = int(arg(body, "NewExternalPort"))
            if ext in mappings and arg(body, "NewProtocol") == "UDP":
                del mappings[ext]
                log("DeletePortMapping ext=%d -> ok" % ext)
                self.reply(200, soap_ok(name))
            else:
                log("DeletePortMapping ext=%d -> 714" % ext)
                self.reply(500, soap_err(714, "NoSuchEntryInArray"))
        else:
            self.reply(500, soap_err(401, "Invalid Action"))


httpd = HTTPServer(("0.0.0.0" if args.multicast else "127.0.0.1", 0), Handler)
http_port = httpd.server_port

udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
if args.multicast:
    udp.bind(("0.0.0.0", 1900))
    for ifaddr in ("0.0.0.0", "127.0.0.1"):
        try:
            udp.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                           struct.pack("4s4s", socket.inet_aton("239.255.255.250"),
                                       socket.inet_aton(ifaddr)))
        except OSError:
            pass
else:
    udp.bind(("127.0.0.1", 0))
ssdp_port = udp.getsockname()[1]


def ssdp():
    first = True
    while True:
        data, src = udp.recvfrom(2048)
        text = data.decode(errors="replace")
        if not text.startswith("M-SEARCH") or '"ssdp:discover"' not in text:
            continue
        m = re.search(r"\r\nST:\s*(\S+)", text)
        st = m.group(1) if m else ""
        log("M-SEARCH st=%s" % st)
        if args.silent:
            continue
        host = "127.0.0.1"
        if args.multicast:
            # answer from, and point at, the address the searcher can reach
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.connect(src)
            host = probe.getsockname()[0]
            probe.close()

        def answer(location, st_value, usn):
            msg = ("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: %s\r\nUSN: %s\r\n"
                   "EXT:\r\nSERVER: Fake/1.0 UPnP/1.0 fake_igd/1.0\r\nLocation: %s\r\n\r\n"
                   % (st_value, usn, location))
            udp.sendto(msg.encode(), src)

        if first and args.other_device:
            answer("http://%s:%d/dms.xml" % (host, http_port),
                   "urn:schemas-upnp-org:device:MediaServer:1", "uuid:dms::x")
        if first and args.wrong_location:
            answer("http://198.51.100.9:%d/rootDesc.xml" % http_port,
                   "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "uuid:evil::x")
            first = False
            continue
        first = False
        if "InternetGatewayDevice:1" in st or "WANIPConnection" in st or st == "ssdp:all":
            answer("http://%s:%d/rootDesc.xml" % (host, http_port), st,
                   "uuid:fake-igd::" + st)


threading.Thread(target=ssdp, daemon=True).start()
print("PORTS %d %d" % (ssdp_port, http_port), flush=True)
try:
    httpd.serve_forever()
except KeyboardInterrupt:
    sys.exit(0)
