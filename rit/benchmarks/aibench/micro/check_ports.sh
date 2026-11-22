#!/usr/bin/env bash
# Check the bare-metal AIBench ports against the ORIGINAL AIBench kernels.
#
# For each kernel, size and thread count: build and run the original AIBench
# C++ code on x86 (aib_host.cpp, with the kernel's own header/.cpp unchanged)
# and the port in QEMU (rit/benchmarks/qemu/run_qemu.sh), both on the same
# input, and compare the output checksums. A match means every output value
# is bit-for-bit the same as the original's.
#
#   usage: AIBENCH=<DC_AIBench_Micro clone> rit/benchmarks/aibench/micro/check_ports.sh [kernel ...]
# Kernels are named as in AIBench's source (relu, max_pool, matmul, ...); the
# port of each is the file in PORT below.
#
# Needs g++, riscv64-unknown-elf-gcc and qemu-system-riscv64 (not on the lab
# machines; any Linux box or WSL). Get AIBench with
#   git clone https://github.com/BenchCouncil/DC_AIBench_Micro
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
SRC=${AIBENCH:?set AIBENCH to a DC_AIBench_Micro clone}/Pthread/Pthread
[ -f "$SRC/relu.h" ] || { echo "no AIBench Pthread sources in $SRC"; exit 1; }
KERNELS=${*:-relu sigmoid tanh softmax dropout add multiply matmul batch_norm max_pool avg_pool}
declare -A PORT=( [relu]=aibench_relu.c [sigmoid]=aibench_sigmoid.c [tanh]=aibench_tanh.c
                  [softmax]=aibench_softmax.c [dropout]=aibench_dropout.c [add]=aibench_element_add.c
                  [multiply]=aibench_element_multiply.c [matmul]=aibench_fully_connected.c
                  [batch_norm]=aibench_batch_norm.c [max_pool]=aibench_max_pooling.c
                  [avg_pool]=aibench_avg_pooling.c )

W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
cp "$SRC"/*.h "$SRC"/*.cpp "$W/"
cp "$HERE/host/loadImage.h" "$W/"          # replaces the OpenCV loader only

pass=0; fail=0
printf "%-11s %-14s %-3s  %-18s %-18s %s\n" kernel "size" thr original port result
for k in $KERNELS; do
    g++ -std=c++11 -O0 -pthread -w -DK_$k -I"$W" "$HERE/aib_host.cpp" -o "$W/host_$k" || { echo "$k: host build failed"; fail=$((fail+1)); continue; }
    case $k in
        matmul)                    sizes="1:16:16 1:64:64 2:12:20" ;;
        dropout)                   sizes="1:8:8 1:32:32" ;;
        batch_norm|max_pool|avg_pool) sizes="2:17:20 2:227:227 3:9:9" ;;
        *)                         sizes="1:17:20 1:227:227 2:9:9" ;;
    esac
    for sz in $sizes; do
        IFS=: read -r b h w <<< "$sz"
        for t in 1 2 3 4; do
            want=$("$W/host_$k" $b $h $w $t 2>/dev/null | grep -o 'Checksum: 0x[0-9a-f]*' | tail -1)
            out=$(QEMU_TIMEOUT=600 "$REPO/rit/benchmarks/qemu/run_qemu.sh" ${PORT[$k]} $t -DBATCH=$b -DIMG_H=$h -DIMG_W=$w 2>&1)
            got=$(grep -o 'Checksum: 0x[0-9a-f]*' <<< "$out" | tail -1)
            errs=$(grep -o 'Num Errors: [0-9]*' <<< "$out" | tail -1)
            if [ "$k" = dropout ] && [ $t -gt 1 ]; then
                # which elements are kept depends on thread timing (see the port), so
                # only the port's own check applies
                [ "$errs" = "Num Errors: 0" ] && r="ok (self-check; varies with timing)" || r="FAIL ($errs)"
            elif [ -n "$want" ] && [ "$want" = "$got" ] && [ "$errs" = "Num Errors: 0" ]; then
                r="MATCH"
            else
                r="FAIL ($errs)"
            fi
            [[ $r == FAIL* ]] && fail=$((fail+1)) || pass=$((pass+1))
            printf "%-11s %-14s %-3s  %-18s %-18s %s\n" $k "${b}x${h}x${w}" $t "${want#Checksum: }" "${got#Checksum: }" "$r"
        done
    done
done
echo "== $pass passed, $fail failed"
[ $fail -eq 0 ]
