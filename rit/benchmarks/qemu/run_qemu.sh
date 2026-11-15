#!/usr/bin/env bash
# Quick functional check of a bare-metal OpenPiton+Ariane C test in QEMU, before
# spending hours in VCS. Not cycle-accurate and has no NoC: it only shows that the
# program compiles and links the way sims builds it, runs on N harts, and gets the
# right answer.
#
#   usage: rit/benchmarks/qemu/run_qemu.sh TEST.c [HARTS] [extra gcc args...]
#   e.g.   rit/benchmarks/qemu/run_qemu.sh parsec_blackscholes.c 2 -DNUM_OPTIONS=4096
#
# Builds TEST.c with the same flags and files as piton/tools/bin/rv64_cc, except
# for two changes to copies of the support files: the UART points at QEMU virt's
# 16550 (same registers as OpenPiton's fake UART) and pass()/fail() exit QEMU
# through its test device. Also prints the ROI marker lines rv64_img would add
# to symbol.tbl.
#
# QEMU_ICOUNT=1 makes mcycle/minstret count instructions (otherwise QEMU derives
# them from host time and the ROI counts the program prints mean nothing).
#
# Needs riscv64-unknown-elf-gcc (newlib) and qemu-system-riscv64 on PATH. The lab
# workstations don't have QEMU; run this anywhere that does.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
INC=$REPO/piton/verif/diag/assembly/include/riscv/ariane
SRC=$1; NH=${2:-2}; shift; [ $# -gt 0 ] && shift
[ -f "$SRC" ] || SRC=$(find "$REPO/piton/verif/diag" -name "$(basename "$SRC")" | head -1)
[ -f "$SRC" ] || { echo "can't find $1"; exit 1; }
CC=${RV64_TARGET_TRIPLE:-riscv64-unknown-elf}-gcc
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
mkdir -p "$W/inc"; cp -r "$INC"/. "$W/inc/"
sed -i 's/#define PITON_UART_ADDRESS  0xFFF0C2C000ULL/#define PITON_UART_ADDRESS  0x10000000ULL/' "$W/inc/util.h"
python3 - "$W/inc/syscalls.c" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace("void __attribute__((noreturn, noinline)) pass () {",
              "void __attribute__((noreturn, noinline)) pass () {\n  *(volatile unsigned int *)0x100000 = 0x5555;")
s = s.replace("void __attribute__((noreturn, noinline)) fail () {",
              "void __attribute__((noreturn, noinline)) fail () {\n  *(volatile unsigned int *)0x100000 = 0x13333;")
open(p, "w").write(s)
PY
# GCC >= 12 needs zicsr/zifencei spelled out; the lab's GCC 8.3 doesn't know them
MARCH=rv64imafdc
echo 'int main(){return 0;}' > "$W/t.c"
$CC -march=rv64imafdc_zicsr_zifencei -mabi=lp64d -c "$W/t.c" -o "$W/t.o" 2>/dev/null && MARCH=rv64imafdc_zicsr_zifencei
cd "$W"
$CC -I"$W/inc" -march=$MARCH -mabi=lp64d -DPREALLOCATE=1 -mcmodel=medany -static -std=gnu99 -O2 \
    -fno-common -fno-builtin-printf -DPITON_NUMTILES=$NH "$@" -o diag.exe "$SRC" \
    "$W/inc/syscalls.c" "$W/inc/crt.S" \
    -static -nostdlib -nostartfiles -lm -lgcc -T "$W/inc/link.ld"
echo "== built $(basename "$SRC") for $NH harts ($MARCH) $*"
${CC%gcc}nm diag.exe | awk '$3=="noc_roi_begin_mark"{printf("symbol.tbl: noc_roi_begin %s X %s\n", $1, $1);} $3=="noc_roi_end_mark"{printf("symbol.tbl: noc_roi_end %s X %s\n", $1, $1);}'
echo "== running in QEMU (virt, $NH harts)"
set +e
timeout ${QEMU_TIMEOUT:-600} qemu-system-riscv64 -machine virt -smp "$NH" -m 256M -bios none \
    -kernel diag.exe -nographic -monitor none -serial stdio ${QEMU_ICOUNT:+-icount shift=0}
rc=$?
set -e
case $rc in
  0)   echo "== PASS (program reached pass())";;
  1)   echo "== FAIL (program reached fail())";;
  124) echo "== TIMEOUT (hung?)";;
  *)   echo "== QEMU exit code $rc";;
esac
exit $rc
