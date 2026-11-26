#!/usr/bin/env bash
# Checks rit/bin/nocrun without VCS: a fake `sims` writes the files a real run
# writes (sims.log, sim.log, fake_uart.log, trace.csv, diag.*, trace_hart_*.log),
# then this checks the run folder, the index, analysis at two windows, compare,
# import, a killed run, checkpoints, a detached run and a batch.
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
printf "PARSEC blackscholes (bare-metal port)\nhart 0: ROI cycles 2030996, instructions 910601\nhart 1: ROI cycles 2090539, instructions 910744\n" > fake_uart.log
case "\$*" in *aibench_relu*) printf "Checksum: 0x9bb2151206d00020\nChecksum matches the original AIBench output\n" >> fake_uart.log;;
               *aibench_tanh*) printf "Checksum: 0xd9b11be50c7492d4\nChecksum matches the OpenPiton+Ariane result (= the original's output except ...)\n" >> fake_uart.log;; esac
printf "Num Errors: 0\n" >> fake_uart.log
sleep \${FAKE_SLEEP:-0}
if [ -n "\${FAKE_KILL:-}" ]; then echo "Received SIGHUP (signal 1), exiting." >> sims.log; exit 0; fi
printf "4001000: Simulation -> PASS (HIT GOOD TRAP)\n           V C S   S i m u l a t i o n   R e p o r t\nTime: 4001000 ps\nCPU Time:      2.000 seconds;\n" >> sims.log
FAKE
chmod +x "$T/bin/sims"; export PATH=$T/bin:$PATH
N="python3 $RIT/bin/nocrun"
P=$NOC_RUNS/baremetal/parsec; A=$NOC_RUNS/baremetal/aibench_micro
ok=1; check() { if eval "$2"; then echo "PASS nocrun: $1"; else echo "FAIL nocrun: $1"; ok=0; fi; }

$N start parsec_blackscholes.c -D NUM_OPTIONS=16 --fg > "$T/out1.txt"
R=$(ls -d "$P"/*_blackscholes_2x1_r1)
check "run folder baremetal/parsec/<date>_blackscholes_2x1_r1 (suite from the program's folder)" "[ -d '$R' ] && grep -q 'env / suite: *baremetal / parsec' '$R/run_info.txt'"
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
R2=$(ls -d "$P"/*_blackscholes_2x1_r2)
check "second run gets r2" "[ -d '$R2' ] && [ -d '$R' ]"

FAKE_KILL=1 $N start aibench_convolution.c --fg > /dev/null
R3=$(ls -d "$A"/*_convolution_2x1_r1)
check "killed run recorded as KILLED(SIGHUP), trace still analyzed" "grep -q 'status: *KILLED(SIGHUP)' '$R3/run_info.txt' && [ -f '$R3/analysis/w10000/summary.txt' ]"

$N compare blackscholes_2x1_r1 blackscholes_2x1_r2 -w 1000 -o "$T/cmp" > /dev/null
check "compare writes runs_summary.csv with 2 runs" "[ \$(wc -l < '$T/cmp/runs_summary.csv') -eq 3 ]"

cp sims.log 2>/dev/null || true
printf "x\n4001000: Simulation -> PASS (HIT GOOD TRAP)\nTime: 4001000 ps\n" > "$T/old_sims.log"
$N import --name bsold --trace "$SRC" --sims-log "$T/old_sims.log" --date 2026-11-15 --note "old run" > /dev/null
RI=$(ls -d "$NOC_RUNS"/baremetal/other/2026-11-15_bsold_2x1_r1)
check "import: folder, PASS, original log untouched" "grep -q 'status: *PASS' '$RI/run_info.txt' && [ -f '$T/old_sims.log' ] && [ -f '$RI/analysis/w10000/summary.txt' ]"
check "index has 4 runs" "[ \$(wc -l < '$NOC_RUNS/runs.csv') -eq 5 ]"
check "index keeps every run's name" "! grep -q '^,' '$NOC_RUNS/runs.csv' && [ \$(cut -d, -f1 '$NOC_RUNS/runs.csv' | grep -c _r) -eq 4 ]"
cp "$NOC_RUNS/runs.csv" "$T/before.csv"; rm "$NOC_RUNS/runs.csv"; $N reindex > /dev/null
check "reindex rebuilds the same index" "[ \$(wc -l < '$NOC_RUNS/runs.csv') -eq 5 ] && ! grep -q '^,' '$NOC_RUNS/runs.csv'"
$N list > /dev/null && $N status > /dev/null
check "list and status run" "true"

# checkpoint: save during a run, then restore it into a new run folder
$N start parsec_blackscholes.c --name ckpt --save-at 50000 --fg > /dev/null
RC=$(ls -d "$P"/*_ckpt_2x1_r1)
check "--save-at passes the plusargs and the checkpoint is kept" "grep -q -- '+rit_save_at=50000' '$RC/job.sh' && [ -f '$RC/checkpoint.vcs' ] && grep -q 'checkpoint: *checkpoint.vcs at cycle 50000' '$RC/run_info.txt'"
check "--save-at runs with address randomization off" "grep -q 'setarch .* -R sims' '$RC/job.sh' && grep -q 'MB with checkpoint.vcs.FILES' '$RC/run_info.txt'"
$N restore ckpt_2x1_r1 --fg > /dev/null
RR=$(ls -d "$P"/*_ckpt_2x1_r2)
check "restore runs with address randomization off" "grep -q 'setarch .* -R .*simv -r' '$RR/job.sh'"
check "restore: new folder, simv -r, PASS, trace analyzed, origin recorded" "grep -q 'simv -r .*checkpoint.vcs +noc_trace_file=trace.csv' '$RR/job.sh' && grep -q 'status: *PASS' '$RR/run_info.txt' && grep -q 'restored from: *[0-9-]*_ckpt_2x1_r1' '$RR/run_info.txt' && [ -f '$RR/analysis/w10000/summary.txt' ] && [ -f '$RR/program/diag.c' ]"

# detached start: returns at once, job finishes on its own
$N start parsec_blackscholes.c > /dev/null
for i in $(seq 1 60); do [ -z "$(ls "$P"/*_r3/job.pid 2>/dev/null)" ] && [ -f "$(ls -d "$P"/*_blackscholes_2x1_r3)/run.json" ] && grep -q '"status": "PASS"' "$P"/*_blackscholes_2x1_r3/run.json 2>/dev/null && break; sleep 1; done
check "detached run finishes by itself" "grep -q '\"status\": \"PASS\"' $P/*_blackscholes_2x1_r3/run.json"
# batch: three runs, two at a time, in the background
printf "# test batch\naibench_relu.c\nparsec_blackscholes.c --name bb -D NUM_OPTIONS=16   # comment\n\naibench_convolution.c --name cc\n" > "$T/list.txt"
FAKE_SLEEP=3 $N batch "$T/list.txt" -j 2 > "$T/batch_out.txt"
B=$(ls -d "$NOC_RUNS"/batches/*_list)
for i in $(seq 1 90); do grep -q "batch done" "$B/batch.log" 2>/dev/null && break; sleep 1; done
check "batch: all 3 runs finish, worker exits" "grep -q 'batch done' '$B/batch.log' && [ \$(grep -c ' finish .*PASS' '$B/batch.log') -eq 3 ] && [ ! -e '$B/batch.pid' ]"
check "batch: never more than 2 at once (3rd starts after a finish)" "awk '/ start /{n++; if(n-f>2) bad=1} / finish /{f++} END{exit bad}' '$B/batch.log'"
RB=$(ls -d "$A"/*_relu_2x1_r1)
check "batch: run records its batch, the machine load, and the reference checksum" "grep -q 'batch: *[0-9-]*_[0-9]*_list' '$RB/run_info.txt' && grep -q 'other sims running:' '$RB/run_info.txt' && grep -q 'checksum: *0x9bb2151206d00020 (matches original)' '$RB/run_info.txt' && grep -q 'matches original' '$NOC_RUNS/runs.csv'"
check "batch: a typo in the list is caught before anything starts" "printf 'no_such_program.c\n' > '$T/bad.txt'; ! $N batch '$T/bad.txt' > /dev/null 2>&1"

# batch cut off (as by a reboot) and resumed: finished runs are skipped
printf "aibench_tanh.c\naibench_sigmoid.c\naibench_softmax.c\n" > "$T/rb.txt"
FAKE_SLEEP=8 $N batch "$T/rb.txt" -j 1 > /dev/null
RBD=$(ls -d "$NOC_RUNS"/batches/*_rb)
for i in $(seq 1 60); do grep -q "finish run  1" "$RBD/batch.log" 2>/dev/null && break; sleep 1; done
sleep 4; kill -9 -- -$(cat "$RBD/batch.pid"); sleep 1
check "status shows a cut-off batch and how to resume it" "$N status | grep -c 'STOPPED: nocrun batch --resume' > /dev/null"
$N batch --resume rb > "$T/resume.txt"
for i in $(seq 1 90); do grep -q "batch done" "$RBD/batch.log" 2>/dev/null && break; sleep 1; done
check "resume: reruns only the unfinished runs, records the cut-off one as STOPPED" "grep -q 'resumed: runs 2 3' '$RBD/batch.log' && [ \$(grep -c 'tanh_2x1' '$RBD/batch.log') -eq 1 ] && grep -q 'STOPPED' \$(ls -d '$A'/*_sigmoid_2x1_r1)/run_info.txt && grep -q 'PASS' \$(ls -d '$A'/*_sigmoid_2x1_r2)/run_info.txt && grep -q 'batch done' '$RBD/batch.log'"
check "a run matching the Ariane hardware reference is recorded as such" "grep -q 'checksum: *0xd9b11be50c7492d4 (matches Ariane hw)' \$(ls -d '$A'/*_tanh_2x1_r1)/run_info.txt"
check "finish takes a run name, like the other commands" "cd '$A' && $N finish sigmoid_2x1_r1 > /dev/null"

# scratch: own folder and index, kept out of the main list
$N start aibench_relu.c --scratch -D IMG_H=16 --fg > /dev/null
check "--scratch: run under scratch/baremetal/aibench_micro with its own index" "ls -d '$NOC_RUNS'/scratch/baremetal/aibench_micro/*_relu_2x1_r1 > /dev/null && grep -q 'relu_2x1_r1' '$NOC_RUNS/scratch/runs.csv' && [ \$($N list | grep -c relu_2x1) -eq 1 ] && $N list --scratch | grep -c relu_2x1_r1 > /dev/null"
check "list --suite filters" "$N list --suite parsec | grep -c blackscholes > /dev/null && ! $N list --suite parsec | grep -c convolution > /dev/null"
check "analyze finds a run anywhere in the layout by part of its name" "$N analyze relu_2x1_r1 -w 700 > /dev/null && [ -d \$(ls -d '$A'/*_relu_2x1_r1)/analysis/w700 ]"

# reorganize: folders in the old flat layout, with the old program names
cp -r "$R" "$NOC_RUNS/2026-11-17_conv2d_2x1_r1"
python3 - "$NOC_RUNS/2026-11-17_conv2d_2x1_r1/run.json" <<'PY'
import json, sys
p = sys.argv[1]; i = json.load(open(p))
i.update(run="2026-11-17_conv2d_2x1_r1", name="conv2d", app="Conv2d", program="aibench_conv2d.c", defines=[],
         rerun="nocrun start aibench_conv2d.c --mesh 2x1")
for k in ("env", "suite", "runs_root", "checkpoint"): i.pop(k, None)
json.dump(i, open(p, "w"))
PY
cp -r "$R" "$NOC_RUNS/2026-11-17_bs16_2x1_r1"
python3 - "$NOC_RUNS/2026-11-17_bs16_2x1_r1/run.json" <<'PY'
import json, sys
p = sys.argv[1]; i = json.load(open(p))
i.update(run="2026-11-17_bs16_2x1_r1", name="bs16", defines=["NUM_OPTIONS=16"])
for k in ("env", "suite", "runs_root"): i.pop(k, None)
json.dump(i, open(p, "w"))
PY
$N reorganize --also "$T/none" > "$T/reorg_plan.txt"
check "reorganize: shows the plan and moves nothing without --yes" "grep -q 'convolution_2x1_r1' '$T/reorg_plan.txt' && [ -d '$NOC_RUNS/2026-11-17_conv2d_2x1_r1' ]"
$N reorganize --yes --also "$T/none" > /dev/null
RN=$NOC_RUNS/baremetal/aibench_micro/2026-11-17_convolution_2x1_r1
check "reorganize: full run moved and renamed (conv2d -> convolution), rerun uses the new file" "[ -d '$RN' ] && [ ! -e '$NOC_RUNS/2026-11-17_conv2d_2x1_r1' ] && grep -q 'renamed from: *2026-11-17_conv2d_2x1_r1' '$RN/run_info.txt' && grep -q 'nocrun start aibench_convolution.c' '$RN/run_info.txt'"
check "reorganize: a test run goes to scratch/" "[ -d '$NOC_RUNS/scratch/baremetal/parsec/2026-11-17_bs16_2x1_r1' ]"
check "reorganize: both indexes rebuilt" "grep -q '2026-11-17_convolution_2x1_r1' '$NOC_RUNS/runs.csv' && grep -q '2026-11-17_bs16_2x1_r1' '$NOC_RUNS/scratch/runs.csv' && ! grep -q 'conv2d_2x1' '$NOC_RUNS/runs.csv'"

[ $ok = 1 ] && echo "NOCRUN ALL PASS" || { echo "NOCRUN CHECKS FAILED"; exit 1; }
