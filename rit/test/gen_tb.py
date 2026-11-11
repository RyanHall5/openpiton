#!/usr/bin/env python3
"""Generate an Icarus testbench: an X x Y mesh of real OpenPiton routers
(dynamic_node_top_wrap, 3 NoCs per tile) with random credit-based traffic at
every P port and a chipset stub on tile 0's west port.

It exercises noc_trace_mon.v (the generated monitor, unmodified) and also prints
lines in the exact format of OpenPiton's built-in +enable_router_mon monitor, so
both parser adapters can be checked against ground truth (gt.csv).
"""
import argparse
import os

ap = argparse.ArgumentParser()
ap.add_argument("--x", type=int, default=3)
ap.add_argument("--y", type=int, default=2)
ap.add_argument("--npkt", type=int, default=60, help="packets per source per NoC")
ap.add_argument("--period", type=int, default=2500, help="clock period in ps")
ap.add_argument("--cycles", type=int, default=30000)
ap.add_argument("--out", default="build")
a = ap.parse_args()
X, Y = a.x, a.y
NT = X * Y
os.makedirs(os.path.join(a.out, "inc"), exist_ok=True)

DIRS = ["N", "E", "S", "W"]
OPP = {"N": "S", "S": "N", "E": "W", "W": "E"}


def nb(x, y, d):
    dx, dy = {"N": (0, -1), "S": (0, 1), "E": (1, 0), "W": (-1, 0)}[d]
    nx, ny = x + dx, y + dy
    return (nx, ny) if 0 <= nx < X and 0 <= ny < Y else None


# ---------------------------------------------------------------- includes
with open(os.path.join(a.out, "inc", "sys.h"), "w") as f:
    f.write("`timescale 1ps/1ps\n")
with open(os.path.join(a.out, "inc", "iop.h"), "w") as f:
    f.write("// stub\n")
with open(os.path.join(a.out, "inc", "cross_module.tmp.h"), "w") as f:
    for t in range(NT):
        f.write("`define TILE%d tb.tile%d\n`define RTL_TILE%d\n" % (t, t, t))

# ---------------------------------------------------------------- modules
lib = r"""
`timescale 1ps/1ps
`include "define.tmp.h"

// Credit-based packet source. NPKT random packets; header follows OpenPiton's
// layout: chipid[63:50] x[49:42] y[41:34] fbits[33:30] len[29:22] type[21:14]
// mshr[13:6] opt[5:0].
module traffic_src #(parameter SRC_ID=0, NOC=1, NT=6, XT=3, NPKT=40, SEED=1,
                     OFFCHIP_SRC=0, CREDITS=4)
  (input clk, input rst, output reg valid, output reg [63:0] data, input yummy);
  integer credits, remaining, sent, idx, gap, seed, r, dst, len, typ;
  reg [63:0] hdr;
  reg [13:0] chip; reg [7:0] dx, dy; reg [3:0] fb;
  initial seed = SEED;
  always @(posedge clk) begin
    if (rst) begin
      valid <= 0; data <= 0; credits = CREDITS; remaining = 0; sent = 0;
      gap = 0; idx = 0;
    end else begin
      credits = credits + yummy;
      valid <= 0;
      if (remaining == 0) begin
        if (sent < NPKT) begin
          if (gap > 0) gap = gap - 1;
          else if (credits > 0) begin
            r = $random(seed); if (r < 0) r = -r;
            dst = OFFCHIP_SRC ? (r % NT) : (r % (NT + 1)) - 1;  // -1 = off chip
            r = $random(seed); if (r < 0) r = -r;
            len = r % 9;
            r = $random(seed); if (r < 0) r = -r;
            typ = OFFCHIP_SRC ? 24 : ((r % 4) == 0 ? 31 : (r % 4) == 1 ? 2 :
                                      (r % 4) == 2 ? 29 : 19);
            if (dst < 0) begin chip = 14'h2000; dx = 0; dy = 0; fb = 4'b0010; end
            else begin chip = 0; dx = dst % XT; dy = dst / XT; fb = 4'b0000; end
            hdr = {chip, dx, dy, fb, len[7:0], typ[7:0], sent[7:0], SRC_ID[5:0]};
            valid <= 1; data <= hdr; credits = credits - 1;
            remaining = len; idx = 1;
            $fdisplay(tb.gt_fd, "inj,%0d,%0d,%0d,%0d,%0d,%0d,%016h",
                      $time, NOC, OFFCHIP_SRC ? -1 : SRC_ID, dst, len, typ, hdr);
            sent = sent + 1;
            r = $random(seed); if (r < 0) r = -r;
            gap = r % 40;
          end
        end
      end else if (credits > 0) begin
        valid <= 1;
        data <= {16'hB0D1, SRC_ID[7:0] ^ (OFFCHIP_SRC ? 8'hFF : 8'h00), NOC[7:0],
                 sent[15:0], idx[15:0]};
        credits = credits - 1; remaining = remaining - 1; idx = idx + 1;
      end
    end
  end
endmodule

// Always-ready sink: returns one credit per flit, logs packet tails.
module traffic_sink #(parameter DST_ID=0, NOC=1)
  (input clk, input rst, input valid, input [63:0] data, output reg yummy);
  integer remaining; reg [63:0] hdr;
  always @(posedge clk) begin
    if (rst) begin yummy <= 0; remaining = 0; end
    else begin
      yummy <= valid;
      if (valid) begin
        if (remaining == 0) begin
          hdr = data; remaining = data[29:22];
          if (remaining == 0)
            $fdisplay(tb.gt_fd, "ej,%0d,%0d,%0d,%016h", $time, NOC, DST_ID, hdr);
        end else begin
          remaining = remaining - 1;
          if (remaining == 0)
            $fdisplay(tb.gt_fd, "ej,%0d,%0d,%0d,%016h", $time, NOC, DST_ID, hdr);
        end
      end
    end
  end
endmodule
"""

# test_tile: 3 routers named like OpenPiton's tile (user_dynamic_networkN)
tp = ["input clk", "input rst", "input [7:0] myx", "input [7:0] myy"]
for n in range(3):
    for d in DIRS:
        tp += ["input [63:0] din_%s%d" % (d, n), "input vin_%s%d" % (d, n),
               "input yin_%s%d" % (d, n), "output [63:0] dout_%s%d" % (d, n),
               "output vout_%s%d" % (d, n), "output yout_%s%d" % (d, n)]
tile = ["module test_tile #(parameter ID=0, NT=6, XT=3, NPKT=40)(\n  " +
        ",\n  ".join(tp) + ");"]
for n in range(3):
    tile.append("""
  wire [63:0] p_in_d{n}, p_out_d{n}; wire p_in_v{n}, p_in_y{n}, p_out_v{n}, p_out_y{n};
  traffic_src #(.SRC_ID(ID), .NOC({noc}), .NT(NT), .XT(XT), .NPKT(NPKT), .SEED(ID*7+{noc}*1000+3))
    src{n} (.clk(clk), .rst(rst), .valid(p_in_v{n}), .data(p_in_d{n}), .yummy(p_in_y{n}));
  traffic_sink #(.DST_ID(ID), .NOC({noc}))
    snk{n} (.clk(clk), .rst(rst), .valid(p_out_v{n}), .data(p_out_d{n}), .yummy(p_out_y{n}));
  dynamic_node_top_wrap user_dynamic_network{n} (
    .clk(clk), .reset_in(rst),
    .dataIn_N(din_N{n}), .dataIn_E(din_E{n}), .dataIn_S(din_S{n}), .dataIn_W(din_W{n}), .dataIn_P(p_in_d{n}),
    .validIn_N(vin_N{n}), .validIn_E(vin_E{n}), .validIn_S(vin_S{n}), .validIn_W(vin_W{n}), .validIn_P(p_in_v{n}),
    .yummyIn_N(yin_N{n}), .yummyIn_E(yin_E{n}), .yummyIn_S(yin_S{n}), .yummyIn_W(yin_W{n}), .yummyIn_P(p_out_y{n}),
    .myLocX(myx), .myLocY(myy), .myChipID(14'd0),
    .dataOut_N(dout_N{n}), .dataOut_E(dout_E{n}), .dataOut_S(dout_S{n}), .dataOut_W(dout_W{n}), .dataOut_P(p_out_d{n}),
    .validOut_N(vout_N{n}), .validOut_E(vout_E{n}), .validOut_S(vout_S{n}), .validOut_W(vout_W{n}), .validOut_P(p_out_v{n}),
    .yummyOut_N(yout_N{n}), .yummyOut_E(yout_E{n}), .yummyOut_S(yout_S{n}), .yummyOut_W(yout_W{n}), .yummyOut_P(p_in_y{n}),
    .thanksIn_P());""".format(n=n, noc=n + 1))
tile.append("endmodule\n")

# top
tb = ["`timescale 1ps/1ps", "module tb;", "  reg clk = 0; reg rst = 1; integer gt_fd;",
      "  always #%d clk = ~clk;" % (a.period // 2)]
for t in range(NT):
    for n in range(3):
        for d in DIRS:
            tb.append("  wire [63:0] t%d_dout_%s%d; wire t%d_vout_%s%d, t%d_yout_%s%d;"
                      % (t, d, n, t, d, n, t, d, n))
# chipset stub on tile0 W
for n in range(3):
    tb.append("""  wire [63:0] cs_d{n}; wire cs_v{n}, cs_y{n}, cs_sink_y{n};
  traffic_src #(.SRC_ID(63), .NOC({noc}), .NT({NT}), .XT({X}), .NPKT({np}), .SEED({seed}), .OFFCHIP_SRC(1))
    cs_src{n} (.clk(clk), .rst(rst), .valid(cs_v{n}), .data(cs_d{n}), .yummy(t0_yout_W{n}));
  traffic_sink #(.DST_ID(-1), .NOC({noc}))
    cs_snk{n} (.clk(clk), .rst(rst), .valid(t0_vout_W{n}), .data(t0_dout_W{n}), .yummy(cs_sink_y{n}));"""
              .format(n=n, noc=n + 1, NT=NT, X=X, np=a.npkt // 2, seed=999 + n))
for t in range(NT):
    x, y = t % X, t // X
    conns = [".clk(clk)", ".rst(rst)", ".myx(8'd%d)" % x, ".myy(8'd%d)" % y]
    for n in range(3):
        for d in DIRS:
            o = nb(x, y, d)
            if o is not None:
                u = o[0] + o[1] * X
                od = OPP[d]
                conns += [".din_%s%d(t%d_dout_%s%d)" % (d, n, u, od, n),
                          ".vin_%s%d(t%d_vout_%s%d)" % (d, n, u, od, n),
                          ".yin_%s%d(t%d_yout_%s%d)" % (d, n, u, od, n)]
            elif t == 0 and d == "W":
                conns += [".din_W%d(cs_d%d)" % (n, n), ".vin_W%d(cs_v%d)" % (n, n),
                          ".yin_W%d(cs_sink_y%d)" % (n, n)]
            else:
                conns += [".din_%s%d(64'd0)" % (d, n), ".vin_%s%d(1'b0)" % (d, n),
                          ".yin_%s%d(1'b0)" % (d, n)]
            conns += [".dout_%s%d(t%d_dout_%s%d)" % (d, n, t, d, n),
                      ".vout_%s%d(t%d_vout_%s%d)" % (d, n, t, d, n),
                      ".yout_%s%d(t%d_yout_%s%d)" % (d, n, t, d, n)]
    tb.append("  test_tile #(.ID(%d), .NT(%d), .XT(%d), .NPKT(%d)) tile%d (\n    %s);"
              % (t, NT, X, a.npkt, t, ",\n    ".join(conns)))

# emulate OpenPiton's built-in router monitor (manycore_network_mon, +enable_router_mon)
tb.append("  always @(negedge clk) if ($test$plusargs(\"enable_router_mon\")) begin")
for t in range(NT):
    for n in range(3):
        for d in ["N", "S", "E", "W"]:
            tb.append('    if (t%d_vout_%s%d) $display("%%d: TILE%d noc%d router out %s data: 0x%%x ", $time, t%d_dout_%s%d);'
                      % (t, d, n, t, n + 1, d, t, d, n))
tb.append("  end")
tb.append("""  noc_trace_mon mon (.clk(clk));
  initial begin
    gt_fd = $fopen("gt.csv", "w");
    $fdisplay(gt_fd, "# ground truth: inj,time,noc,src,dst,len,type,hdr | ej,time,noc,dst,hdr");
    $display("sims: x_tiles=%d y_tiles=%d (test harness)");
    repeat (10) @(posedge clk); rst = 0;
    repeat (%d) @(posedge clk);
    $display("Simulation -> PASS (test harness)");
    $fclose(gt_fd);
    $finish;
  end
endmodule""" % (X, Y, a.cycles))

with open(os.path.join(a.out, "tb_mesh.v"), "w") as f:
    f.write(lib + "\n" + "\n".join(tile) + "\n" + "\n".join(tb) + "\n")
print("wrote", os.path.join(a.out, "tb_mesh.v"))
