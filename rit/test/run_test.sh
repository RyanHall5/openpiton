#!/usr/bin/env bash
# Self-test for the NoC trace flow, no VCS needed.
#
# Builds an X x Y mesh of OpenPiton's real router RTL (dynamic_node_top_wrap,
# 3 NoCs per tile) in Icarus Verilog, drives random credit-based traffic from
# every tile plus a chipset stub on tile 0 west, runs the generated
# noc_trace_mon unchanged, and also prints the built-in router monitor's line
# format. Then checks noctrace's reconstruction against the testbench's record
# of what it injected.
#
#   usage: rit/test/run_test.sh [X] [Y] [packets_per_source]     (default 3 2 60)
#   Finally checks that rit/docs/noctrace_reference.md documents every option,
#   plusarg, output file and column (check_docs.py).
#
#   needs: iverilog, python3 with pandas/numpy/matplotlib
set -euo pipefail
X=${1:-3}; Y=${2:-2}; N=${3:-60}; PERIOD=2500
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RIT=$(dirname "$HERE")
REPO=$(dirname "$RIT")
P=$REPO/piton
B=$HERE/build_${X}x${Y}; mkdir -p "$B/inc"
export DV_ROOT=$P PITON_X_TILES=$X PITON_Y_TILES=$Y PITON_NUM_TILES=$((X*Y))
D=$P/design/chip/tile/dynamic_node

command -v iverilog >/dev/null || { echo "iverilog not found"; exit 1; }
python3 "$HERE/gen_tb.py" --x "$X" --y "$Y" --npkt "$N" --period $PERIOD --out "$B"
python3 "$P/tools/bin/pyhp.py" "$P/design/include/define.h.pyv" > "$B/inc/define.tmp.h" 2>/dev/null
cp "$P/design/include/network_define.v" "$B/inc/"
python3 "$P/tools/bin/pyhp.py" "$D/components/rtl/one_of_n.v.pyv" > "$B/one_of_n.v" 2>/dev/null
python3 "$P/tools/bin/pyhp.py" "$D/components/rtl/one_of_n_plus_3.v.pyv" > "$B/one_of_n_plus_3.v" 2>/dev/null
python3 "$P/tools/bin/pyhp.py" "$P/verif/env/manycore/noc_trace_mon.v.pyv" > "$B/noc_trace_mon.v" 2>/dev/null

cd "$B"
iverilog -g2005 -o sim.vvp -I inc -I . tb_mesh.v noc_trace_mon.v one_of_n.v one_of_n_plus_3.v \
  $D/rtl/dynamic_node_top_wrap.v $D/rtl/dynamic_node_top.v $D/common/rtl/*.v \
  $D/components/rtl/{bus_compare_equal,flip_bus,one_of_eight,one_of_five,net_dff}.v \
  $D/dynamic/rtl/{dynamic_input_control,dynamic_input_route_request_calc,dynamic_input_top_16,dynamic_input_top_4,dynamic_output_control,dynamic_output_datapath,dynamic_output_top}.v
vvp -n sim.vvp +noc_trace +enable_router_mon > sims.log
cd "$RIT"
python3 "$HERE/check_against_gt.py" "$RIT" "$B" "$X" "$Y" $PERIOD
python3 "$HERE/check_docs.py" "$RIT" "$B/noc_trace.csv"
