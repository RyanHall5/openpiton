#!/usr/bin/env bash
# Checks rit/bin/nocrun without VCS: a fake `sims` writes the files a real run
# writes (sims.log, sim.log, fake_uart.log, trace.csv, diag.*, trace_hart_*.log),
# then this checks the run folder, the index, analysis at two windows, compare,
# import, and a killed run.
#   usage: rit/test/test_nocrun.sh        (needs a trace from rit/test/run_test.sh 2 1)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RIT=$(cd "$HERE/.." && pwd)
SRC=$HERE/build_2x1_win/roi.csv
[ -f "$SRC" ] || bash "$HERE/run_test.sh" 2 1 40 > /dev/null
T=${NOCRUN_TEST_DIR:-$(mktemp -d)}; [ -z "${NOCRUN_TEST_DIR:-}" ] && trap 'rm -rf "$T"' EXIT
export NOC_RUNS=$T/runs MODEL_DIR=$T/model PYTHONPATH=$RIT
mkdir -p "$MODEL_DIR/manycore/ariane_2x1" "$T/bin"
cat > "$MODEL_DIR/manycore/ariane_2x1/simv" <<SIMV
#!/usr/bin/env bash
# fake simv: only "-r CHECKPOINT [+plusargs]" (a restore) is used by nocrun
[ "\$1" = "-r" ] && [ -f "\$2" ] || { echo "fake simv: bad restore args \$*"; exit 3; }
for a in "\$@"; do case \$a in +noc_trace_file=*) TF=\${a#+noc_trace_file=};; esac; done
echo "rit_ckpt: running on from the checkpoint at cycle 50000 (time 50000000)"
[ -n "\${TF:-}" ] && cp "$SRC" "\$TF"
printf "hart 0: ROI cycles 2030996, instructions 910601\nNum Errors: 0\n" > fake_uart.log
printf "4001000: Simulation -> PASS (HIT GOOD TRAP)\nTime: 4001000 ps\nCPU Time:      1.000 seconds;\n"
SIMV
chmod +x "$MODEL_DIR/manycore/ariane_2x1/simv"
sleep 1   # the model must be older than the checkpoint below
cat > "$T/bin/sims" <<FAKE
#!/usr/bin/env bash
# fake sims: behaves like a finished run (or a killed one if FAKE_KILL is set)
for a in "\$@"; do case \$a in -sim_run_args=+noc_trace_file=*) TF=\${a#*=+noc_trace_file=};; esac; done
echo "sims: NoC trace window marker noc_roi_begin at 00000000800001ce" | tee sims.log
for i in \$(seq 1 3000); do echo "Control signals: 0101 \$i"; done >> sims.log
cp sims.log sim.log
echo "hello" > diag.c; echo x > diag.exe; echo x > mem.image; echo "good_trap 1 X 1" > symbol.tbl
echo "instruction" >> trace_hart_0.log
[ -n "\${TF:-}" ] && cp "$SRC" "\$TF"
case "\$*" in *+rit_save_at=*) echo "checkpoint bytes" > checkpoint.vcs;; esac
printf "PARSEC blackscholes (bare-metal port)\nhart 0: ROI cycles 2030996, instructions 910601\nhart 1: ROI cycles 2090539, instructions 910744\nNum Errors: 0\n" > fake_uart.log
if [ -n "\${FAKE_KILL:-}" ]; then echo "Received SIGHUP (signal 1), exiting." >> sims.log; exit 0; fi
printf "4001000: Simulation -> PASS (HIT GOOD TRAP)\n           V C S   S i m u l a t i o n   R e p o r t\nTime: 4001000 ps\nCPU Time:      2.000 seconds;\n" >> sims.log
FAKE
chmod +x "$T/bin/sims"; export PATH=$T/bin:$PATH
N="python3 $RIT/bin/nocrun"
ok=1; check() { if eval "$2"; then echo "PASS nocrun: $1"; else echo "FAIL nocrun: $1"; ok=0; fi; }

$N start parsec_blackscholes.c -D NUM_OPTIONS=16 --fg > "$T/out1.txt"
R=$(ls -d "$NOC_RUNS"/*_blackscholes_2x1_r1)
check "run folder named <date>_blackscholes_2x1_r1" "[ -d '$R' ]"
check "status PASS, cycles (4001000 ps / 2500 ps clock in the test trace), ROI and errors parsed" "grep -q 'status: *PASS' '$R/run_info.txt' && grep -q 'sim cycles: *1600' '$R/run_info.txt' && grep -q 'hart 1 ROI: *2090539' '$R/run_info.txt' && grep -q 'num errors: *0' '$R/run_info.txt'"
check "define passed to sims" "grep -q -- '-gcc_args=-DNUM_OPTIONS=16' '$R/job.sh'"
check "trace gzipped, csv gone" "[ -f '$R/trace.csv.gz' ] && [ ! -f '$R/trace.csv' ]"
check "sims.log and sim.log dropped, summary kept" "[ ! -e '$R/sims.log' ] && [ ! -e '$R/sim.log' ] && grep -q 'Simulation -> PASS' '$R/sims_summary.log'"
check "instruction trace went to /dev/null and is gone" "[ -z \"\$(ls '$R'/trace_hart_* 2>/dev/null)\" ]"
check "program kept, mem.image removed" "[ -f '$R/program/diag.c' ] && [ -f '$R/program/symbol.tbl' ] && [ ! -e '$R/mem.image' ]"
check "uart.log kept" "grep -q 'Num Errors: 0' '$R/uart.log'"
check "analysis at the default window" "[ -f '$R/analysis/w10000/summary.txt' ]"
check "index row" "grep -q 'blackscholes_2x1_r1,.*,PASS,' '$NOC_RUNS/runs.csv'"

$N analyze blackscholes_2x1_r1 -w 500 > /dev/null
check "second window in its own folder, first untouched" "[ -f '$R/analysis/w500/signatures.csv' ] && [ -f '$R/analysis/w10000/summary.txt' ]"
check "window shift: ROI-relative cycles in the analysis" "grep -q 'cycle 0 = cycle' '$R/analysis/w500/summary.txt'"

$N start parsec_blackscholes.c --fg > /dev/null
R2=$(ls -d "$NOC_RUNS"/*_blackscholes_2x1_r2)
check "second run gets r2" "[ -d '$R2' ] && [ -d '$R' ]"

FAKE_KILL=1 $N start aibench_conv2d.c --fg > /dev/null
R3=$(ls -d "$NOC_RUNS"/*_conv2d_2x1_r1)
check "killed run recorded as KILLED(SIGHUP), trace still analyzed" "grep -q 'status: *KILLED(SIGHUP)' '$R3/run_info.txt' && [ -f '$R3/analysis/w10000/summary.txt' ]"

$N compare blackscholes_2x1_r1 blackscholes_2x1_r2 -w 1000 -o "$T/cmp" > /dev/null
check "compare writes runs_summary.csv with 2 runs" "[ \$(wc -l < '$T/cmp/runs_summary.csv') -eq 3 ]"

cp sims.log 2>/dev/null || true
printf "x\n4001000: Simulation -> PASS (HIT GOOD TRAP)\nTime: 4001000 ps\n" > "$T/old_sims.log"
$N import --name bsold --trace "$SRC" --sims-log "$T/old_sims.log" --date 2026-11-15 --note "old run" > /dev/null
RI=$(ls -d "$NOC_RUNS"/2026-11-15_bsold_2x1_r1)
check "import: folder, PASS, original log untouched" "grep -q 'status: *PASS' '$RI/run_info.txt' && [ -f '$T/old_sims.log' ] && [ -f '$RI/analysis/w10000/summary.txt' ]"
check "index has 4 runs" "[ \$(wc -l < '$NOC_RUNS/runs.csv') -eq 5 ]"
$N list > /dev/null && $N status > /dev/null
check "list and status run" "true"

# checkpoint: save during a run, then restore it into a new run folder
$N start parsec_blackscholes.c --name ckpt --save-at 50000 --fg > /dev/null
RC=$(ls -d "$NOC_RUNS"/*_ckpt_2x1_r1)
check "--save-at passes the plusargs and the checkpoint is kept" "grep -q -- '+rit_save_at=50000' '$RC/job.sh' && [ -f '$RC/checkpoint.vcs' ] && grep -q 'checkpoint: *checkpoint.vcs at cycle 50000' '$RC/run_info.txt'"
$N restore ckpt_2x1_r1 --fg > /dev/null
RR=$(ls -d "$NOC_RUNS"/*_ckpt_2x1_r2)
check "restore: new folder, simv -r, PASS, trace analyzed, origin recorded" "grep -q 'simv -r .*checkpoint.vcs +noc_trace_file=trace.csv' '$RR/job.sh' && grep -q 'status: *PASS' '$RR/run_info.txt' && grep -q 'restored from: *[0-9-]*_ckpt_2x1_r1' '$RR/run_info.txt' && [ -f '$RR/analysis/w10000/summary.txt' ] && [ -f '$RR/program/diag.c' ]"

# detached start: returns at once, job finishes on its own
$N start parsec_blackscholes.c > /dev/null
for i in $(seq 1 60); do [ -z "$(ls "$NOC_RUNS"/*_r3/job.pid 2>/dev/null)" ] && [ -f "$(ls -d "$NOC_RUNS"/*_blackscholes_2x1_r3)/run.json" ] && grep -q '"status": "PASS"' "$NOC_RUNS"/*_blackscholes_2x1_r3/run.json 2>/dev/null && break; sleep 1; done
check "detached run finishes by itself" "grep -q '\"status\": \"PASS\"' $NOC_RUNS/*_blackscholes_2x1_r3/run.json"
[ $ok = 1 ] && echo "NOCRUN ALL PASS" || { echo "NOCRUN CHECKS FAILED"; exit 1; }
