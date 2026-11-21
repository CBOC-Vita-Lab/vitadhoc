#!/usr/bin/env python3
"""Test fixtures for the Sega CD path of md.bin (md/twin/cd_test.cpp, selftest.sh section 12).

  mkscd.py standin OUT.bin     a 128 KB STAND-IN for the console's BIOS, for the test suite only
  mkscd.py discs DIR           synthetic disc images (cooked ISO, raw BIN + CUE, multi-file CUE, WAV and Ogg tracks, a CHD)

THIS IS NOT A BIOS. The Mega CD's boot ROM is Sega's; VitAdhoc never contains, ships or generates it for players. The stand-in is a few hundred
bytes of 68000 machine code written here from scratch so that the emulated hardware (both 68000s, the PCM chip, the CD drive, the VDP) can be
run, timed, saved and loaded without the real one: the main CPU starts the sub CPU, the sub CPU plays a PCM wave and a CD audio track and counts
in the communication registers, the main CPU draws a changing backdrop. It is generated into a scratch folder by the tests and never installed;
the app only accepts files that have the structure of a real BIOS (md_host_cd_bios_add; the tests force this one with force = 1).

The discs hold no game: sector n of the data track is a known pattern, the audio tracks are known waves, so a reader can be checked byte for byte.
Copyright CBOC-Vita. MIT licence.
"""
import os, struct, sys, zlib, subprocess, shutil

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

def raw_sector(n):
    return b'\x00' + b'\xff' * 10 + b'\x00' + msf(n) + b'\x01' + user_data(n) + bytes(288)

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
    # not discs: a text file called .cue, a Genesis-like file, an empty file
    w(d + '/notacue.cue', b'this is not a cue sheet\n')
    w(d + '/empty.iso', b'')
    w(d + '/randomdata.bin', bytes((i * 31 + 7) & 0xFF for i in range(100000)))

if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'standin': w(sys.argv[2], standin_bios())
    elif len(sys.argv) == 3 and sys.argv[1] == 'discs': make_discs(sys.argv[2])
    else: sys.exit(__doc__)
