# Snes9x core translation units for the SNES program.
# (upstream libretro/Makefile.common list, minus libretro.cpp and the NTSC filter;
# the apu/bapu .cpp files not listed here are #included by smp.cpp / sdsp.cpp.)
CORE_SRCS := \
apu/apu.cpp \
apu/bapu/dsp/sdsp.cpp \
apu/bapu/smp/smp.cpp \
apu/bapu/smp/smp_state.cpp \
bml.cpp \
bsx.cpp \
c4.cpp \
c4emu.cpp \
cheats.cpp \
cheats2.cpp \
clip.cpp \
conffile.cpp \
controls.cpp \
cpu.cpp \
cpuexec.cpp \
cpuops.cpp \
crosshairs.cpp \
dma.cpp \
dsp.cpp \
dsp1.cpp \
dsp2.cpp \
dsp3.cpp \
dsp4.cpp \
fscompat.cpp \
fxemu.cpp \
fxinst.cpp \
gfx.cpp \
globals.cpp \
memmap.cpp \
movie.cpp \
msu1.cpp \
obc1.cpp \
ppu.cpp \
ppu_mt.cpp \
sa1.cpp \
sa1cpu.cpp \
screenshot.cpp \
sdd1.cpp \
sdd1emu.cpp \
seta.cpp \
seta010.cpp \
seta011.cpp \
seta018.cpp \
sha256.cpp \
snapshot.cpp \
snes9x.cpp \
spc7110.cpp \
srtc.cpp \
stream.cpp \
tile.cpp \
tileimpl-h2x1.cpp \
tileimpl-n1x1.cpp \
tileimpl-n2x1.cpp \

