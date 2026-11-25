#!/bin/sh
# Guest instructions per frame of one scaler under qemu-arm: the difference of two runs (1 frame and N + 1 frames) of
# scale_neon_check's bench mode, counted as translation blocks of a single-stepped run (every block is one instruction).
# usage: scale_icount.sh BINARY KIND SIZE FRAMES NEON GEN   (KIND 0 MMPX2, 1 XBR2, 2 XBR3; SIZE 0..3, see scale_neon_check.cpp)
# The figure is qemu's count of ARM instructions (NEON instructions count one each), NOT time and NOT a Vita measurement.
B=$1; K=$2; S=$3; N=$4; U=$5; G=${6:-8}
cnt() { qemu-arm -singlestep -d nochain,exec -D /dev/stdout -L /usr/arm-linux-gnueabihf "$B" bench "$K" "$S" "$1" "$U" "$G" 2>&1 | grep -c '^Trace'; }
a=$(cnt 1); b=$(cnt $((N + 1)))
echo "scaler=$K size=$S neon=$U gen=$G guest_instr_per_frame=$(( (b - a) / N ))"
