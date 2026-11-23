#!/bin/zsh
# Runs every backend on the 5-min excerpts, records wall-clock time, converts and analyzes.
# usage: run_all.sh [cw-large|cw-turbo|turbo|turbo-patched|all]
set -u
SP="${0:A:h}/.."; W="$SP/work"; S="$SP/scripts"; TP="$SP/third_party"
M="$HOME/Library/Application Support/TTSDatasetCreator/models"
CW="$TP/CrisperWhisper.cpp/build/bin/crisper-whisper"
WCLI="$TP/whisper.cpp/build/bin/whisper-cli"
WCLI_P="$TP/build-patched-whisper/bin/whisper-cli"
what=${1:-all}
cd "$W"
t() { local s=$(date +%s.%N 2>/dev/null || python3 -c 'import time;print(time.time())'); "$@"; local rc=$?; local e=$(python3 -c 'import time;print(time.time())'); echo "$rc $(python3 -c "print(round($e-$s,1))")"; }
now() { python3 -c 'import time;print(time.time())'; }

run_cw() { # name model
  for i in 1 2; do
    s=$(now); "$CW" -m "$M/$2" -f ex$i.wav --mode verbatim -l en --word-timestamps --json > $1_ex$i.json 2> $1_ex$i.log; rc=$?
    e=$(now); echo "$1 ex$i rc=$rc wall_s=$(python3 -c "print(round($e-$s,1))")" | tee -a times.txt
    python3 -I "$S/convert.py" crisper $1_ex$i.json $1_ex$i.words.json >/dev/null
  done
}
run_wcli() { # name binary
  for i in 1 2; do
    s=$(now); "$2" -m "$M/ggml-large-v3-turbo.bin" -f ex$i.wav -l en -ml 1 -sow -ojf -of $1_ex$i -dtw large.v3.turbo -np > /dev/null 2> $1_ex$i.log; rc=$?
    e=$(now); echo "$1 ex$i rc=$rc wall_s=$(python3 -c "print(round($e-$s,1))")" | tee -a times.txt
    python3 -I "$S/convert.py" wcli $1_ex$i.json $1_ex$i.words.json >/dev/null
    python3 -I "$S/convert.py" wcli-dtw $1_ex$i.json $1-dtw_ex$i.words.json >/dev/null
  done
}
case $what in
  cw-large) run_cw cwlarge ggml-crisperwhisper-large-f16.bin ;;
  cw-turbo) run_cw cwturbo ggml-crisperwhisper-turbo-f16.bin ;;
  turbo) run_wcli turbo "$WCLI" ;;
  turbo-patched) run_wcli turbop "$WCLI_P" ;;
  all) $0 cw-large; $0 cw-turbo; $0 turbo; $0 turbo-patched ;;
esac
