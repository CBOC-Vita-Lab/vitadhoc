#!/usr/bin/env python3
"""Test fixtures for the Sega CD path of md.bin (md/twin/cd_test.cpp, selftest.sh section 12).

  mkscd.py standin OUT.bin     a 128 KB STAND-IN for the console's BIOS, for the test suite only
  mkscd.py discs DIR           synthetic disc images (cooked ISO, raw BIN + CUE, multi-file CUE, WAV and Ogg tracks, an uncompressed CHD), and the
                               awkward ones: a sheet with a byte order mark, tabs and CRLF, mixed case extensions, a long name and a long path, a
                               missing audio file, a Windows path and a device name ("ux0:") in the sheet, big endian (MOTOROLA) audio, INDEX 00 / PREGAP
                               in one file, MODE2/2352, 36 tracks in 36 files (WAV, Ogg: a decoder and descriptor budget), and CHDs compressed with each codec libchdr reads (LZMA, zlib, zstd, FLAC, a mix, 4 and 32 frame hunks, self references), written by this script without chdman

THIS IS NOT A BIOS. The Mega CD's boot ROM is Sega's; VitAdhoc never contains, ships or generates it for players. The stand-in is a few hundred
bytes of 68000 machine code written here from scratch so that the emulated hardware (both 68000s, the PCM chip, the CD drive, the VDP) can be
run, timed, saved and loaded without the real one: the main CPU starts the sub CPU, the sub CPU plays a PCM wave and a CD audio track and counts
in the communication registers, the main CPU draws a changing backdrop. It is generated into a scratch folder by the tests and never installed;
the app only accepts files that have the structure of a real BIOS (md_host_cd_bios_add; the tests force this one with force = 1).

The discs hold no game: sector n of the data track is a known pattern, the audio tracks are known waves, so a reader can be checked byte for byte.
Copyright CBOC-Vita. MIT licence.
"""
import os, struct, sys, zlib, subprocess, shutil, lzma, functools
try:
    if os.environ.get('MKSCD_PYPATH'): sys.path.insert(0, os.environ['MKSCD_PYPATH'])      # (python3 -I ignores PYTHONPATH: a folder with the zstandard module can be named here)
    import zstandard
except ImportError:
    zstandard = None

# ---------------------------------------------------------------------------------------------------------------- the stand-in BIOS

class Asm:
    """just enough 68000: words and longs, labels for short branches and dbf"""
    def __init__(self, base):
        self.base = base; self.b = bytearray(); self.labels = {}; self.fix = []
    def pc(self): return self.base + len(self.b)
    def w(self, *ws):
        for x in ws: self.b += struct.pack('>H', x & 0xFFFF)
    def l(self, x): self.b += struct.pack('>I', x & 0xFFFFFFFF)
    def label(self, n): self.labels[n] = len(self.b)
    def bra(self, op, n):          # 6xdd short branch
        self.fix.append((len(self.b), n, 's', op)); self.w(op << 8)
    def dbf(self, reg, n):         # 51C8+reg disp16
        self.w(0x51C8 + reg); self.fix.append((len(self.b), n, 'w', 0)); self.w(0)
    def done(self):
        for at, n, kind, op in self.fix:
            if kind == 's':
                d = self.labels[n] - (at + 2)             # relative to the opcode word + 2
                assert -128 <= d <= 126 and d != 0
                self.b[at] = op; self.b[at + 1] = d & 0xFF
            else:
                struct.pack_into('>h', self.b, at, self.labels[n] - at)       # dbf: relative to its own displacement word
        return bytes(self.b)

def sub_program():
    """the sub CPU's program, loaded at PRG-RAM $000000 (vectors) / $000100 (code)"""
    a = Asm(0x100)
    # PCM: a 64 byte ramp in wave RAM bank 0 ($FF2001, odd bytes), loop marker, then channel 0 on
    a.w(0x41F9); a.l(0xFF2001)                 # lea $FF2001,a0
    a.w(0x7000)                                # moveq #0,d0
    a.w(0x7200)                                # moveq #0,d1
    a.w(0x13FC, 0x0000); a.l(0xFF000F)         # move.b #$00,$FF000F  (CTRL: RAM bank 0, chip off)
    a.label('fill')
    a.w(0x1081)                                # move.b d1,(a0)
    a.w(0x5488)                                # addq.l #2,a0
    a.w(0x0601, 0x0003)                        # addi.b #3,d1
    a.w(0x5200)                                # addq.b #1,d0
    a.w(0x0C00, 0x0040)                        # cmpi.b #$40,d0
    a.bra(0x65, 'fill')                        # blo
    a.w(0x10FC, 0x00FF)                        # move.b #$FF,(a0)+   loop end marker
    a.w(0x13FC, 0x00FF); a.l(0xFF0001)         # ENV = $FF
    a.w(0x13FC, 0x00FF); a.l(0xFF0003)         # PAN = $FF
    a.w(0x13FC, 0x0000); a.l(0xFF0007)         # FD high = 0
    a.w(0x13FC, 0x0008); a.l(0xFF0005)         # FD low = 8 (a quarter of the original rate)
    a.w(0x13FC, 0x0000); a.l(0xFF000D)         # ST = 0
    a.w(0x13FC, 0x00C0); a.l(0xFF000F)         # CTRL: channel 0 selected, chip on
    a.w(0x13FC, 0x00FE); a.l(0xFF0011)         # ON/OFF: channel 0 on
    # CD drive: enable the drive's communication, then PLAY from absolute time 00:06:00 (track 2 of the test discs)
    a.w(0x13FC, 0x0004); a.l(0xFF8037)         # CDD communication on
    a.w(0x33FC, 0x0000); a.l(0xFF8044)         # minutes 00
    a.w(0x33FC, 0x0006); a.l(0xFF8046)         # seconds 06
    a.w(0x33FC, 0x0000); a.l(0xFF8048)         # frames 00
    a.w(0x33FC, 0x0300); a.l(0xFF8042)         # command 3: play
    a.w(0x33FC, 0x0000); a.l(0xFF804A)         # run it
    # the work: a busy loop that reads and writes memory and multiplies, like a game's engine; the count goes to the communication registers
    a.w(0x7600)                                # moveq #0,d3
    a.w(0x7800)                                # moveq #0,d4
    a.w(0x41F9); a.l(0x00040000)               # lea $40000,a0   (PRG-RAM work area)
    a.label('work')
    a.w(0x5283)                                # addq.l #1,d3
    a.w(0x7A3F)                                # moveq #63,d5
    a.label('inner')
    a.w(0xC8C3)                                # mulu d3,d4
    a.w(0xD883)                                # add.l d3,d4
    a.w(0x2184, 0x5000)                        # move.l d4,0(a0,d5.w)   (index by d5)
    a.w(0x2030, 0x5000)                        # move.l 0(a0,d5.w),d0
    a.w(0xD880)                                # add.l d0,d4
    a.dbf(5, 'inner')
    a.w(0x33C3); a.l(0xFF8020)                 # move.w d3,$FF8020   (comm word 0 = the loop count)
    a.bra(0x60, 'work')
    code = a.done()
    img = bytearray(0x100) + code
    struct.pack_into('>II', img, 0, 0x0007F000, 0x00000100)
    return bytes(img)

def main_code(sub):
    """the boot ROM's code at $000200: VDP set-up, copy the sub program to PRG-RAM, release the sub CPU, then per frame a backdrop colour"""
    a = Asm(0x200)
    a.w(0x46FC, 0x2700)                        # move #$2700,sr
    a.w(0x41F9); a.l(0x00C00004)               # lea $C00004,a0
    for r in (0x8004, 0x8144, 0x8230, 0x8328, 0x8407, 0x8578, 0x8700, 0x8A00, 0x8B00, 0x8C00, 0x8D3F, 0x8F02, 0x9001, 0x9100, 0x9200):
        a.w(0x30BC, r)                         # move.w #reg,(a0)
    # copy the sub program (at ROM $000400) to PRG-RAM $020000 (the sub CPU is held in reset with the bus requested after power on)
    a.w(0x43F9); a.l(0x00000400)               # lea $400,a1
    a.w(0x45F9); a.l(0x00020000)               # lea $20000,a2
    a.w(0x343C, (len(sub) + 1) // 2 - 1)       # move.w #n,d2
    a.label('cp')
    a.w(0x34D9)                                # move.w (a1)+,(a2)+
    a.dbf(2, 'cp')
    a.w(0x13FC, 0x0001); a.l(0x00A12001)       # release the sub CPU (reset off, bus released)
    a.w(0x7200)                                # moveq #0,d1
    a.label('frame')
    a.label('wv0'); a.w(0x3010); a.w(0x0800, 0x0003); a.bra(0x67, 'wv0')   # wait for vertical blank
    a.label('wv1'); a.w(0x3010); a.w(0x0800, 0x0003); a.bra(0x66, 'wv1')   # ... to end
    a.w(0x5241)                                # addq.w #1,d1
    a.w(0x20BC); a.l(0xC0000000)               # CRAM write address 0
    a.w(0x3141, 0xFFFC)                        # move.w d1,-4(a0)   (backdrop colour)
    a.w(0x33C1); a.l(0x00FFFF00)               # move.w d1,$FFFF00  (frame count to work RAM)
    a.w(0x3839); a.l(0x00A12020)               # move.w $A12020,d4  (the sub CPU's count)
    a.w(0x33C4); a.l(0x00FFFF02)               # move.w d4,$FFFF02
    a.bra(0x60, 'frame')
    return a.done()

def standin_bios():
    sub = sub_program()
    img = bytearray(0x20000)
    struct.pack_into('>II', img, 0, 0x00FFF000, 0x00000200)
    for v in range(2, 64):                     # every other vector: its own `bra.s *` at $1000 + 2 v (an exception stops the machine there, and which one shows in the PC)
        struct.pack_into('>I', img, v * 4, 0x00001000 + 2 * v)
        img[0x1000 + 2 * v:0x1002 + 2 * v] = struct.pack('>H', 0x60FE)
    hdr = b'SEGA MEGA DRIVE ' + b'(C)VITADHOC 2026 ' + (b'TEST STAND-IN, NOT A BIOS'.ljust(48)) + (b'TEST STAND-IN, NOT A BIOS'.ljust(48)) + b'BR 00000000-00'.ljust(14)
    img[0x100:0x100 + len(hdr)] = hdr
    img[0x1F0:0x200] = b'U'.ljust(16)         # the header's region field
    code = main_code(sub)
    assert len(code) < 0x200
    img[0x200:0x200 + len(code)] = code
    img[0x400:0x400 + len(sub)] = sub
    return bytes(img)

# ---------------------------------------------------------------------------------------------------------------- discs

DATA_SECTORS = 300           # the data track: 4 s, so track 2 starts at absolute time 00:06:00 (150 sectors of lead-in are not in the image)
def user_data(n):
    """2048 bytes of sector n of the data track"""
    b = bytearray(((n * 7 + i * 13 + (i >> 8) * 3) & 0xFF) for i in range(2048))
    if n == 0:
        b[0:16] = b'SEGADISCSYSTEM  '
        b[16:32] = b'SEGADISCSYSTEM  '
        b[0x100:0x110] = b'SEGA MEGA DRIVE '
        b[0x110:0x120] = b'(C)VITADHOC 2026 '[:16]
        b[0x120:0x150] = b'VITADHOC TEST DISC'.ljust(48)
        b[0x150:0x180] = b'VITADHOC TEST DISC'.ljust(48)
        b[0x180:0x18E] = b'GM T-000000-00'
        b[0x1F0:0x1F1] = b'U'
        b[0x1F1:0x200] = b' ' * 15
    return bytes(b)

def msf(lba):
    f = lba + 150
    bcd = lambda x: ((x // 10) << 4) | (x % 10)
    return bytes([bcd(f // 75 // 60), bcd(f // 75 % 60), bcd(f % 75)])

# a MODE 1 sector with its error detection and correction codes as a drive reads them (the layout of ECMA-130): sync, header, 2048 bytes of data,
# EDC (CRC-32 of the first 2064 bytes), 8 zero bytes, then P parity (172 bytes) and Q parity (104 bytes). The CHD codecs of libchdr drop the sync and the
# parity of a sector whose codes are right and rebuild them when it is read, so a fixture with the right codes is what runs that rebuild.
def _gf():
    low = [0] * 256; high = [0] * 256
    for i in range(256):
        j = (i << 1) ^ (0x11d if i & 0x80 else 0)
        low[i] = j & 0xFF; high[i ^ (j & 0xFF)] = i
    return low, high
_ECC_LOW, _ECC_HIGH = _gf()
_P_OFFS = [[(c * 43 + (b >> 1)) * 2 + (b & 1) for c in range(24)] for b in range(86)]
_Q_OFFS = [[(((b >> 1) * 43 + i * 44) % 1118) * 2 + (b & 1) for i in range(43)] for b in range(52)]
_EDC_TAB = []
for _i in range(256):
    _e = _i
    for _k in range(8): _e = (_e >> 1) ^ (0xD8018001 if _e & 1 else 0)
    _EDC_TAB.append(_e)

def edc(b):
    c = 0
    for x in b: c = (c >> 8) ^ _EDC_TAB[(c ^ x) & 0xFF]
    return c

def _ecc_pair(sec, offs):
    v1 = v2 = 0
    for o in offs:
        x = sec[12 + o]
        v1 ^= x; v2 ^= x
        v1 = _ECC_LOW[v1]
    v1 = _ECC_HIGH[_ECC_LOW[v1] ^ v2]
    return v1, v1 ^ v2

def ecc_fill(sec):
    """writes the P and Q parity into the bytearray sec (a raw 2352 byte sector); returns it"""
    for b in range(86): sec[2076 + b], sec[2076 + 86 + b] = _ecc_pair(sec, _P_OFFS[b])
    for b in range(52): sec[2248 + b], sec[2248 + 52 + b] = _ecc_pair(sec, _Q_OFFS[b])
    return sec

def ecc_ok(sec):
    t = ecc_fill(bytearray(sec))
    return bytes(t[2076:]) == bytes(sec[2076:])

@functools.lru_cache(maxsize=None)
def raw_sector(n):
    s = bytearray(b'\x00' + b'\xff' * 10 + b'\x00' + msf(n) + b'\x01' + user_data(n) + bytes(288))
    s[2064:2068] = struct.pack('<I', edc(s[:2064]))
    return bytes(ecc_fill(s))

def audio_sample(track, i):
    l = ((i * 37 + track * 101) % 20000) - 10000
    return l, -l

def audio_sectors(track, nsec, first=0):
    out = bytearray()
    for s in range(first, first + nsec):
        for k in range(588):
            l, r = audio_sample(track, s * 588 + k)
            out += struct.pack('<hh', l, r)
    return bytes(out)

def w(path, data):
    with open(path, 'wb') as f: f.write(data)

def wav(pcm):
    return b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 44100 * 4, 4, 16) + b'data' + struct.pack('<I', len(pcm)) + pcm

def chd_v5(path, tracks, hunk_frames=8):
    """an uncompressed CHD v5 of a CD: tracks = [(type, [frame bytes of 2352], pregap_frames)]; frames are stored as 2352 + 96 subcode"""
    CF = 2448
    frames = []
    meta = []
    for tn, (ttype, fr) in enumerate(tracks, 1):
        meta.append(('TRACK:%d TYPE:%s SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0' % (tn, ttype, len(fr))).encode() + b'\0')
        for f in fr: frames.append(f + bytes(96))
        while len(frames) % 4: frames.append(bytes(CF))           # tracks are padded to 4 frames
    while len(frames) % hunk_frames: frames.append(bytes(CF))
    hunkbytes = CF * hunk_frames
    nh = len(frames) // hunk_frames
    hdr = 124
    mapoff = hdr
    mapbytes = nh * 4
    metaoff = mapoff + mapbytes
    metablob = bytearray()
    off = metaoff
    for i, m in enumerate(meta):
        nxt = off + 16 + len(m) if i + 1 < len(meta) else 0
        metablob += b'CHT2' + bytes([1]) + len(m).to_bytes(3, 'big') + struct.pack('>Q', nxt) + m
        off = nxt
    dataoff = (metaoff + len(metablob) + hunkbytes - 1) // hunkbytes * hunkbytes
    h = bytearray(b'MComprHD' + struct.pack('>II', hdr, 5) + bytes(16) + struct.pack('>QQQ', len(frames) * CF, mapoff, metaoff) + struct.pack('>II', hunkbytes, CF) + bytes(60))
    mp = b''.join(struct.pack('>I', dataoff // hunkbytes + i) for i in range(nh))
    body = bytearray(h) + mp + metablob
    body += bytes(dataoff - len(body))
    body += b''.join(frames)
    w(path, bytes(body))

# ---------------------------------------------------------------------------------------------------------------- compressed CHD (v5)
# A pure Python writer for the CD codecs libchdr reads, so that the decoders (LZMA, zlib, zstd, FLAC, the ECC rebuild, the Huffman coded map with its run
# lengths, the self references and the uncompressed fallback) run on a real file without chdman. Every hunk is verified by the reader against the CRC-16
# stored here, so a wrong byte anywhere fails the test. What it is not: chdman. The choices (which codec for which hunk) are the fixture's own.
#   chd_v5_compressed(path, tracks, codecs, pick, hunk_frames)   codecs: up to 4 of 'cdlz' 'cdzl' 'cdzs' 'cdfl'; pick(hunk number, is_audio) -> index into
#   codecs, or None for an uncompressed hunk

@functools.lru_cache(maxsize=None)
def _crc16_table():
    t = []
    for i in range(256):
        c = i << 8
        for _ in range(8): c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        t.append(c)
    return t

def crc16(data):
    t = _crc16_table(); c = 0xFFFF
    for x in data: c = ((c << 8) & 0xFFFF) ^ t[(c >> 8) ^ x]
    return c

class BitW:
    def __init__(self): self.b = bytearray(); self.acc = 0; self.n = 0
    def put(self, v, bits):
        for i in range(bits - 1, -1, -1):
            self.acc = (self.acc << 1) | ((v >> i) & 1); self.n += 1
            if self.n == 8: self.b.append(self.acc); self.acc = 0; self.n = 0
    def align(self):
        while self.n: self.put(0, 1)
    def data(self): self.align(); return bytes(self.b)

def _crc8(data):
    c = 0
    for x in data:
        c ^= x
        for _ in range(8): c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

def _crc16_flac(data):
    c = 0
    for x in data:
        c ^= x << 8
        for _ in range(8): c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def _flac_frame(chans, frame_no, order=2):
    """one FLAC frame of two channels of 16 bit samples (lists), fixed predictors of the given order with one Rice partition per channel"""
    n = len(chans[0])
    hdr = bytearray(b'\xff\xf8')                                   # sync, fixed block size
    hdr += bytes([0x70 | 0x09, 0x18])                               # block size: 16 bit field follows; 44.1 kHz; two independent channels; 16 bit samples
    assert frame_no < 128
    hdr.append(frame_no)
    hdr += struct.pack('>H', n - 1)
    hdr.append(_crc8(hdr))
    bw = BitW()
    for ch in chans:
        bw.put(0, 1); bw.put(0b001000 | order, 6); bw.put(0, 1)     # subframe header: FIXED, order, no wasted bits
        for i in range(order): bw.put(ch[i] & 0xFFFF, 16)           # warm up samples
        res = []
        for i in range(order, n):
            if order == 0: p = 0
            elif order == 1: p = ch[i - 1]
            elif order == 2: p = 2 * ch[i - 1] - ch[i - 2]
            else: p = 3 * ch[i - 1] - 3 * ch[i - 2] + ch[i - 3]
            res.append(ch[i] - p)
        u = [(r << 1) if r >= 0 else ((-r << 1) - 1) for r in res]
        best = min(range(15), key=lambda k: sum((x >> k) + 1 + k for x in u))
        bw.put(0, 2); bw.put(0, 4); bw.put(best, 4)                 # Rice coding, partition order 0, the parameter
        for x in u:
            q = x >> best
            bw.put(0, q) if q else None
            bw.put(1, 1); bw.put(x & ((1 << best) - 1), best) if best else None
    body = bw.data()
    frame = bytes(hdr) + body
    return frame + struct.pack('>H', _crc16_flac(frame))

def _flac_frames(pcm_be_pairs, block):
    """pcm: bytes of 16 bit stereo samples (little endian numbers); returns the frames (blocks of 2352 samples) as one stream"""
    n = len(pcm_be_pairs) // 4
    smp = struct.unpack('<%dh' % (2 * n), pcm_be_pairs)
    L, R = list(smp[0::2]), list(smp[1::2])
    out = b''; k = 0
    for at in range(0, n, block):
        out += _flac_frame((L[at:at + block], R[at:at + block]), k); k += 1
    return out

def _zstd_frame(data):
    """a zstd frame; the zstandard module when it is installed, else stored (raw / run length) blocks: a valid frame the decoder reads, with no
    Huffman or FSE coded block in it"""
    if zstandard is not None:
        return zstandard.ZstdCompressor(level=19).compress(data)
    n = len(data)
    fcs = struct.pack('<H', n - 256) if n >= 256 else bytes([n])
    out = b'\x28\xb5\x2f\xfd' + bytes([0x60 if n >= 256 else 0x20]) + fcs
    if n and data.count(data[:1]) == n: out += struct.pack('<I', 1 | (1 << 1) | (n << 3))[:3] + data[:1]
    else: out += struct.pack('<I', 1 | (0 << 1) | (n << 3))[:3] + data
    return out

def _lzma_dict(reduce):
    """the dictionary size LzmaEncProps_Normalize picks for level 9 and this reduceSize (libchdr rebuilds the decoder's properties the same way)"""
    for i in range(11, 31):
        if reduce <= (2 << i): return 2 << i
        if reduce <= (3 << i): return 3 << i
    return 1 << 26

def _raw_deflate(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()

def _cd_hunk(frames, codec, audio_swapped):
    """frames: list of 2448 byte frames (2352 + 96 subcode). Returns the compressed hunk of the codec ('cdlz' 'cdzl' 'cdzs' 'cdfl')."""
    nf = len(frames)
    bitmap = bytearray((nf + 7) // 8)
    base = bytearray(); sub = bytearray()
    for i, f in enumerate(frames):
        sec = bytearray(f[:2352])
        if bytes(sec[:12]) == b'\x00' + b'\xff' * 10 + b'\x00' and sec[15] == 1 and ecc_ok(sec):
            bitmap[i // 8] |= 1 << (i % 8)
            sec[:12] = bytes(12); sec[2076:] = bytes(276)
        base += sec; sub += f[2352:]
    if codec == 'cdfl':
        # audio only: FLAC of the samples (the frames hold them big endian), the subcode deflated after it; no ECC bitmap, no length field
        sw = bytearray()
        for f in frames:
            d = f[:2352]
            sw += bytes(x for i in range(0, 2352, 2) for x in (d[i + 1], d[i]))        # big endian -> the numbers' little endian bytes
        blk = nf * 588
        while blk > 2048: blk //= 2                                        # the block size libchdr's FLAC reader expects for this hunk size
        return _flac_frames(bytes(sw), blk) + _raw_deflate(bytes(sub))
    lenb = 2 if nf * 2448 < 65536 else 3
    if codec == 'cdlz':
        flt = [{'id': lzma.FILTER_LZMA1, 'dict_size': _lzma_dict(nf * 2352), 'lc': 3, 'lp': 0, 'pb': 2, 'preset': 9}]
        cb = lzma.compress(bytes(base), format=lzma.FORMAT_RAW, filters=flt)
        cs = _raw_deflate(bytes(sub))
    elif codec == 'cdzl':
        cb = _raw_deflate(bytes(base)); cs = _raw_deflate(bytes(sub))
    elif codec == 'cdzs':
        cb = _zstd_frame(bytes(base)); cs = _zstd_frame(bytes(sub))
    else: raise ValueError(codec)
    return bytes(bitmap) + len(cb).to_bytes(lenb, 'big') + cb + cs

def chd_v5_compressed(path, tracks, codecs, pick, hunk_frames=8):
    """tracks = [(type, [raw sector bytes (2352; audio already big endian, as chdman stores it)])]"""
    CF = 2448
    frames = []; isaudio = []; meta = []
    for tn, (ttype, fr) in enumerate(tracks, 1):
        meta.append(('TRACK:%d TYPE:%s SUBTYPE:NONE FRAMES:%d PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0' % (tn, ttype, len(fr))).encode() + b'\0')
        for f in fr:
            frames.append(f + bytes(96)); isaudio.append(ttype == 'AUDIO')
        while len(frames) % 4: frames.append(bytes(CF)); isaudio.append(ttype == 'AUDIO')
    while len(frames) % hunk_frames: frames.append(bytes(CF)); isaudio.append(isaudio[-1])
    hunkbytes = CF * hunk_frames
    nh = len(frames) // hunk_frames
    # the hunks: what each is stored as
    ents = []                      # (type symbol, length, offset or self hunk, crc, payload)
    seen = {}
    last_self = 0
    out = bytearray()
    for h in range(nh):
        fr = frames[h * hunk_frames:(h + 1) * hunk_frames]
        raw = b''.join(fr)
        aud = isaudio[h * hunk_frames]
        key = raw
        if key in seen:
            tgt = seen[key]
            if tgt == last_self: ents.append((9, 0, tgt, 0, b''))            # COMPRESSION_SELF_0
            elif tgt == last_self + 1: ents.append((10, 0, tgt, 0, b'')); last_self = tgt   # SELF_1
            else: ents.append((5, 0, tgt, 0, b'')); last_self = tgt           # SELF
            continue
        seen[key] = h
        sel = pick(h, aud)
        if sel is None:
            ents.append((4, hunkbytes, None, crc16(raw), raw))
        else:
            payload = _cd_hunk(fr, codecs[sel], aud)
            ents.append((sel, len(payload), None, crc16(raw), payload))
    maxlen = max([e[1] for e in ents if e[0] <= 3] + [1])
    lengthbits = max(1, maxlen.bit_length())
    selfbits = max(1, (nh - 1).bit_length())
    # the metadata
    hdr_len = 124
    mapoff = hdr_len
    def build(dataoff):
        # the map's bit stream: the 16 symbol tree (every code 4 bits: valid), the types with run lengths, then the lengths and CRCs
        bw = BitW()
        for sym in range(16): bw.put(4, 4)
        types = [e[0] for e in ents]
        i = 0
        while i < len(types):
            t = types[i]
            run = 1
            while i + run < len(types) and types[i + run] == t: run += 1
            i += run
            bw.put(t, 4); run -= 1                                         # the type once; then the symbols that repeat it: a run of 3..18 or 19..274 hunks
            while run >= 3:                                                # (the symbol's own hunk and the repeats that follow it)
                n = min(run, 274)
                if n >= 19: v = n - 19; bw.put(8, 4); bw.put(v >> 4, 4); bw.put(v & 15, 4)
                else: bw.put(7, 4); bw.put(n - 3, 4)
                run -= n
            for _ in range(run): bw.put(t, 4)
        off = dataoff
        rawmap = bytearray()
        for (t, ln, tgt, crc, payload) in ents:
            if t <= 3:
                bw.put(ln, lengthbits); bw.put(crc, 16); rawmap += bytes([t]) + ln.to_bytes(3, 'big') + off.to_bytes(6, 'big') + struct.pack('>H', crc); off += ln
            elif t == 4:
                bw.put(crc, 16); rawmap += bytes([4]) + ln.to_bytes(3, 'big') + off.to_bytes(6, 'big') + struct.pack('>H', crc); off += ln
            else:
                if t == 5: bw.put(tgt, selfbits)
                rawmap += bytes([5]) + (0).to_bytes(3, 'big') + tgt.to_bytes(6, 'big') + b'\0\0'
        return bw.data(), bytes(rawmap)
    stream, rawmap = build(0)                                               # (the length of the stream does not depend on the data offset)
    mapblob_len = 16 + len(stream)
    metablob = bytearray(); off = mapoff + mapblob_len
    metaoff = off
    for i, m in enumerate(meta):
        nxt = off + 16 + len(m) if i + 1 < len(meta) else 0
        metablob += b'CHT2' + bytes([1]) + len(m).to_bytes(3, 'big') + struct.pack('>Q', nxt) + m
        off = nxt
    dataoff = metaoff + len(metablob)
    stream, rawmap = build(dataoff)
    mapblob = struct.pack('>I', len(stream)) + dataoff.to_bytes(6, 'big') + struct.pack('>H', crc16(rawmap)) + bytes([lengthbits, selfbits, 0, 0]) + stream
    tags = b''.join(c.encode() for c in codecs) + bytes(4 * (4 - len(codecs)))
    head = b'MComprHD' + struct.pack('>II', hdr_len, 5) + tags + struct.pack('>QQQ', len(frames) * CF, mapoff, metaoff) + struct.pack('>II', hunkbytes, CF) + bytes(60)
    body = bytearray(head) + mapblob + metablob
    assert len(body) == dataoff
    for (t, ln, tgt, crc, payload) in ents: body += payload
    w(path, bytes(body))

def raw_sector_mode2(n):
    """a raw sector flagged MODE 2 (byte 15) with the 2048 bytes of user data where a Sega CD image has them (offset 16)"""
    return raw_sector(n)[:15] + b'\x02' + raw_sector(n)[16:]

def make_extra(d, data_raw, data_cooked, a2, a3, sp, swap):
    A2, A3 = 200, 150
    mmss = lambda lba: '%02d:%02d:%02d' % (lba // 75 // 60, lba // 75 % 60, lba % 75)
    single = open(d + '/single.cue', 'rb').read()
    # sheets as editors write them: a UTF-8 byte order mark, TAB indentation, CRLF
    sheet = b'\xef\xbb\xbf' + (b'FILE "single.bin" BINARY\r\n\tTRACK 01 MODE1/2352\r\n\t\tINDEX 01 00:00:00\r\n\tTRACK 02 AUDIO\r\n\t\tINDEX 01 ' + mmss(DATA_SECTORS).encode() +
                                  b'\r\n\tTRACK 03 AUDIO\r\n\t\tINDEX 01 ' + mmss(DATA_SECTORS + A2).encode() + b'\r\n')
    w(d + '/bomtab.cue', sheet)
    # extensions in mixed case: Game.Cue + Game.bin, GAME.CHD
    w(d + '/Mixed.Cue', single.replace(b'single.bin', b'Mixed.bin')); w(d + '/Mixed.bin', open(d + '/single.bin', 'rb').read())
    shutil.copy(d + '/disc.chd', d + '/UPPER.CHD')
    # a long file name (a FILE line of more than 127 characters) and a long path (the sheet's and the track's, over 255 characters)
    long = 'A very long track name for a disc image ' * 4 + 'Game'              # 164 characters
    w(d + '/' + long + '.bin', open(d + '/single.bin', 'rb').read())
    w(d + '/longname.cue', single.replace(b'single.bin', (long + '.bin').encode()))
    deep = d + '/' + '/'.join(('deep%d_' % i) + 'x' * 70 for i in range(4))
    os.makedirs(deep, exist_ok=True)
    w(deep + '/longpath.bin', open(d + '/single.bin', 'rb').read()); w(deep + '/longpath.cue', single.replace(b'single.bin', b'longpath.bin'))
    # a sheet that names a file with a Windows path, and one named like a Vita path ("ux0:Game.cue", the track beside it)
    w(d + '/winpath.cue', single.replace(b'"single.bin"', b'"C:\\Music\\Rips\\single.bin"'))
    w(d + '/ux0:Dev.cue', single.replace(b'single.bin', b'Dev.bin')); w(d + '/ux0:Dev.bin', open(d + '/single.bin', 'rb').read())
    # an audio file the sheet names is missing (the data file is there)
    w(d + '/missing.cue', b'FILE "multi (Track 1).bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n'
                          b'FILE "multi (Track 2).bin" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n'
                          b'FILE "gone (Track 3).bin" BINARY\r\n  TRACK 03 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n')
    # big endian raw audio (FILE ... MOTOROLA)
    w(d + '/motorola.bin', b''.join(raw_sector(n) for n in range(DATA_SECTORS)) + swap(a2) + swap(a3))
    w(d + '/motorola.cue', single.replace(b'single.bin" BINARY', b'motorola.bin" MOTOROLA'))
    # one file with a 2 second pause before each audio track: INDEX 00 inside the file, and a PREGAP that is not in the file
    pre2 = DATA_SECTORS + 150                                 # track 2's INDEX 01 (its INDEX 00 is at DATA_SECTORS)
    w(d + '/pregap.bin', data_raw + bytes(150 * 2352) + a2 + a3)
    w(d + '/pregap.cue', (('FILE "pregap.bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    INDEX 00 %s\r\n    INDEX 01 %s\r\n'
                          '  TRACK 03 AUDIO\r\n    PREGAP 00:02:00\r\n    INDEX 01 %s\r\n') % (mmss(DATA_SECTORS), mmss(pre2), mmss(pre2 + A2))).encode())
    # a MODE2/2352 data track
    w(d + '/mode2.bin', b''.join(raw_sector_mode2(n) for n in range(DATA_SECTORS)))
    w(d + '/mode2.cue', b'FILE "mode2.bin" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n')
    # 36 tracks in 36 files: more than the descriptors the layer keeps open at once
    NT, NS = 36, 30
    cue = b'FILE "many (Track 01).bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n'
    w(d + '/many (Track 01).bin', data_raw)
    for t in range(2, NT + 1):
        w(d + '/many (Track %02d).bin' % t, bytes(150 * 2352) + audio_sectors(t, NS))
        cue += ('FILE "many (Track %02d).bin" BINARY\r\n  TRACK %02d AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n' % (t, t)).encode()
    w(d + '/many.cue', cue)
    ff = shutil.which('ffmpeg')
    if ff:
        cue = b'FILE "manyogg.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'
        w(d + '/manyogg.bin', data_cooked)
        ok = True
        for t in range(2, NT + 1):
            w(d + '/_t.wav', wav(audio_sectors(t, NS)))
            r = subprocess.run([ff, '-v', 'error', '-y', '-i', d + '/_t.wav', '-c:a', 'libvorbis', '-q:a', '3', d + '/manyogg (Track %02d).ogg' % t], capture_output=True)
            ok = ok and r.returncode == 0
            cue += ('FILE "manyogg (Track %02d).ogg" WAVE\n  TRACK %02d AUDIO\n    INDEX 01 00:00:00\n' % (t, t)).encode()
        if os.path.exists(d + '/_t.wav'): os.remove(d + '/_t.wav')
        if ok: w(d + '/manyogg.cue', cue)
    # compressed CHDs written by chd_v5_compressed above: every codec libchdr reads (the uncompressed one never runs a decoder)
    disc = [('MODE1_RAW', sp(data_raw)), ('AUDIO', sp(swap(a2))), ('AUDIO', sp(swap(a3)))]
    chd_v5_compressed(d + '/chd_lzma.chd', disc, ['cdlz'], lambda h, a: 0)
    chd_v5_compressed(d + '/chd_zlib.chd', disc, ['cdzl'], lambda h, a: 0)
    chd_v5_compressed(d + '/chd_zstd.chd', disc, ['cdzs'], lambda h, a: 0)
    chd_v5_compressed(d + '/chd_flac.chd', disc, ['cdfl', 'cdzl'], lambda h, a: 0 if a else 1)
    # the default mix: data hunks LZMA, audio FLAC, every ninth hunk stored uncompressed, zstd for one in seven
    chd_v5_compressed(d + '/chd_mixed.chd', disc, ['cdlz', 'cdzl', 'cdfl', 'cdzs'],
                      lambda h, a: None if h % 9 == 4 else (2 if a else (3 if h % 7 == 3 else 0)))
    # a 4 frame hunk (3 byte lengths are not needed) and a 32 frame hunk (a 3 byte length field in front of the data), and a disc with repeated hunks (self references)
    chd_v5_compressed(d + '/chd_multi.chd', disc, ['cdlz', 'cdzl', 'cdfl'], lambda h, a: 2 if a else 0, 4)
    chd_v5_compressed(d + '/chd_big.chd', disc, ['cdlz', 'cdzl', 'cdfl'], lambda h, a: 2 if a else 1, 32)
    sil = [('MODE1_RAW', sp(data_raw)), ('AUDIO', sp(swap(bytes(2352 * 16)))), ('AUDIO', sp(swap(a2[:2352 * 40])))]
    chd_v5_compressed(d + '/chd_self.chd', sil, ['cdlz', 'cdzl'], lambda h, a: 0)

def make_discs(d):
    os.makedirs(d, exist_ok=True)
    data_cooked = b''.join(user_data(n) for n in range(DATA_SECTORS))
    data_raw = b''.join(raw_sector(n) for n in range(DATA_SECTORS))
    a2, a3 = audio_sectors(2, 200), audio_sectors(3, 150)
    w(d + '/cooked.iso', data_cooked)
    w(d + '/cooked.bin', data_cooked)                                   # a lone .bin of 2048 byte sectors (no cue beside it)
    # one raw file with the three tracks
    w(d + '/single.bin', data_raw + a2 + a3)
    w(d + '/single.cue', ('FILE "single.bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n  TRACK 02 AUDIO\r\n    INDEX 01 %s\r\n  TRACK 03 AUDIO\r\n    INDEX 01 %s\r\n'
                          % ('%02d:%02d:%02d' % (DATA_SECTORS // 75 // 60, DATA_SECTORS // 75 % 60, DATA_SECTORS % 75),
                             '%02d:%02d:%02d' % ((DATA_SECTORS + 200) // 75 // 60, (DATA_SECTORS + 200) // 75 % 60, (DATA_SECTORS + 200) % 75))).encode())
    # one file per track, with the usual 2 second pause at the start of each audio file (INDEX 00 / INDEX 01)
    w(d + '/multi (Track 1).bin', data_raw)
    w(d + '/multi (Track 2).bin', bytes(150 * 2352) + a2)
    w(d + '/multi (Track 3).bin', bytes(150 * 2352) + a3)
    w(d + '/multi.cue', b'FILE "multi (Track 1).bin" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n'
                        b'FILE "multi (Track 2).bin" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n'
                        b'FILE "multi (Track 3).bin" BINARY\r\n  TRACK 03 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n')
    # cooked data track with WAV audio files
    w(d + '/wavs.bin', data_cooked)
    w(d + '/wavs (Track 2).wav', wav(a2)); w(d + '/wavs (Track 3).wav', wav(a3))
    w(d + '/wavs.cue', b'FILE "wavs.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'
                       b'FILE "wavs (Track 2).wav" WAVE\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n'
                       b'FILE "wavs (Track 3).wav" WAVE\n  TRACK 03 AUDIO\n    INDEX 01 00:00:00\n')
    # Ogg Vorbis audio (ffmpeg's libvorbis; lossy, the test compares the shape, not the samples)
    ff = shutil.which('ffmpeg')
    if ff:
        ok = True
        for t, pcm in ((2, a2), (3, a3)):
            w(d + '/_t%d.wav' % t, wav(pcm))
            r = subprocess.run([ff, '-v', 'error', '-y', '-i', d + '/_t%d.wav' % t, '-c:a', 'libvorbis', '-q:a', '6', d + '/oggs (Track %d).ogg' % t], capture_output=True)
            ok = ok and r.returncode == 0
            os.remove(d + '/_t%d.wav' % t)
        if ok:
            w(d + '/oggs.bin', data_cooked)
            w(d + '/oggs.cue', b'FILE "oggs.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n'
                               b'FILE "oggs (Track 2).ogg" WAVE\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n'
                               b'FILE "oggs (Track 3).ogg" WAVE\n  TRACK 03 AUDIO\n    INDEX 01 00:00:00\n')
    # a CHD of the raw disc
    sp = lambda b: [b[i:i + 2352] for i in range(0, len(b), 2352)]
    swap = lambda b: bytes(x for i in range(0, len(b), 2) for x in (b[i + 1], b[i]))      # chdman stores CD audio as big endian samples
    chd_v5(d + '/disc.chd', [('MODE1_RAW', sp(data_raw)), ('AUDIO', sp(swap(a2))), ('AUDIO', sp(swap(a3)))])
    make_extra(d, data_raw, data_cooked, a2, a3, sp, swap)
    # not discs: a text file called .cue, a Genesis-like file, an empty file
    w(d + '/notacue.cue', b'this is not a cue sheet\n')
    w(d + '/empty.iso', b'')
    w(d + '/randomdata.bin', bytes((i * 31 + 7) & 0xFF for i in range(100000)))

if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'standin': w(sys.argv[2], standin_bios())
    elif len(sys.argv) == 3 and sys.argv[1] == 'discs': make_discs(sys.argv[2])
    else: sys.exit(__doc__)
