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
#   Then repeats the run with trace windows (cycle limits, ROI marker PCs from
#   stand-in Ariane cores) and checks them with check_window.py.
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

# ---- trace window: cycle limits and ROI markers (stand-in Ariane cores) ----
# Same mesh, 200 packets per source so traffic runs ~5k cycles. The stand-in
# cores commit the begin marker at cycles 800 (core 1), 801 and 1200 (ignored:
# already open) and 3000, and the end marker at 500 (ignored: not open), 1800,
# 1801 (ignored) and 4000. Each lands in the trace one cycle later.
W=${B}_win; mkdir -p "$W/inc"
python3 "$HERE/gen_tb.py" --x "$X" --y "$Y" --npkt 200 --period $PERIOD --out "$W" \
    --roi-events "e:500:0,b:800:$((X*Y-1)),b:801:0,b:1200:0,e:1800:0,e:1801:$((X*Y-1)),b:3000:0,e:4000:$((X*Y-1))"
cp "$B/inc/define.tmp.h" "$B/inc/network_define.v" "$W/inc/"
python3 "$P/tools/bin/pyhp.py" "$P/verif/env/manycore/noc_trace_mon.v.pyv" > "$W/noc_trace_mon.v" 2>/dev/null
cd "$W"
iverilog -g2005 -o sim.vvp -I inc -I . tb_mesh.v noc_trace_mon.v "$B/one_of_n.v" "$B/one_of_n_plus_3.v" \
  $D/rtl/dynamic_node_top_wrap.v $D/rtl/dynamic_node_top.v $D/common/rtl/*.v \
  $D/components/rtl/{bus_compare_equal,flip_bus,one_of_eight,one_of_five,net_dff}.v \
  $D/dynamic/rtl/{dynamic_input_control,dynamic_input_route_request_calc,dynamic_input_top_16,dynamic_input_top_4,dynamic_output_control,dynamic_output_datapath,dynamic_output_top}.v
ROI="+noc_roi_begin_pc=0000000080001000 +noc_roi_end_pc=0000000080002000"
vvp -n sim.vvp +noc_trace +enable_router_mon > sims.log                                   # no window
vvp -n sim.vvp +noc_trace +noc_trace_file=full.csv $ROI +noc_trace_full > /dev/null       # markers ignored
vvp -n sim.vvp +noc_trace +noc_trace_file=cyc.csv +noc_trace_start=1000 +noc_trace_stop=2500 > /dev/null
vvp -n sim.vvp +noc_trace +noc_trace_file=roi.csv $ROI > roi.log
vvp -n sim.vvp +noc_trace +noc_trace_file=roi_cyc.csv $ROI +noc_trace_start=1500 > /dev/null
vvp -n sim.vvp +noc_trace +noc_trace_file=roi_open.csv +noc_roi_begin_pc=0000000080001000 > /dev/null
cd "$RIT"
python3 "$HERE/check_against_gt.py" "$RIT" "$W" "$X" "$Y" $PERIOD | sed 's/^/[window tb, no window] /'
cmp -s "$W/noc_trace.csv" "$W/full.csv" && echo "PASS +noc_trace_full: same trace as no window options" \
  || { echo "FAIL +noc_trace_full: trace differs from the run without window options"; exit 1; }
python3 "$HERE/check_window.py" "$RIT" "$W" "$X" "$Y" $PERIOD "$W/cyc.csv"      "1000:2500"
python3 "$HERE/check_window.py" "$RIT" "$W" "$X" "$Y" $PERIOD "$W/roi.csv"      "801:1801,3001:4001"
python3 "$HERE/check_window.py" "$RIT" "$W" "$X" "$Y" $PERIOD "$W/roi_cyc.csv"  "1500:1801,3001:4001"
python3 "$HERE/check_window.py" "$RIT" "$W" "$X" "$Y" $PERIOD "$W/roi_open.csv" "801:end"

python3 "$HERE/check_docs.py" "$RIT" "$B/noc_trace.csv"
