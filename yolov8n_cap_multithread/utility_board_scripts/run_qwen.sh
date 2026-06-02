#!/bin/sh
# run_qwen.sh — feed one snapshot to Qwen2.5-0.5B via the rkllm llm_demo REPL
# and write a clean reply next to the input. event_summarizer calls it as:
#   run_qwen.sh /tmp/yolo_summary_33.txt  →  /tmp/yolo_summary_33.reply.txt
#
# llm_demo is an interactive REPL (std::getline), so the multi-line snapshot is
# flattened to a single line and a trailing "exit" is appended to end the loop.

set -e

SNAPSHOT="$1"
if [ -z "$SNAPSHOT" ] || [ ! -f "$SNAPSHOT" ]; then
    echo "[run_qwen] missing or invalid snapshot: $SNAPSHOT" >&2
    exit 1
fi

QWEN_DIR="${QWEN_DIR:-$HOME/programs/demo_Linux_aarch64}"
MODEL="${QWEN_MODEL:-Qwen2.5-0.5B-Instruct_w8a8_RK3588.rkllm}"
# llm_demo args: <model> <max_new_tokens> <max_context_len> (order matters).
MAX_NEW="${QWEN_MAX_NEW:-256}"          # cap output so a 0.5B can't ramble
MAX_CTX="${QWEN_MAX_CTX:-2048}"         # must exceed prompt length (~320 tok)
EXIT_TOKEN="${QWEN_EXIT_TOKEN:-exit}"   # word that leaves the REPL
TIMEOUT_S="${QWEN_TIMEOUT_S:-90}"       # hard cap against a hung demo
CPUS="${QWEN_CPUS:-4-7}"                 # big A76 cores (idle while YOLO blacked out)
REPLY="${SNAPSHOT%.txt}.reply.txt"

cd "$QWEN_DIR"
export LD_LIBRARY_PATH="./lib"
export RKLLM_LOG_LEVEL="${QWEN_LOG_LEVEL:-0}"   # off by default; logs pollute output

# Flatten to one getline() prompt.
PROMPT=$(tr '\n\t' '  ' < "$SNAPSHOT" | tr -s ' ')

echo "[run_qwen] model=$MODEL max_new=$MAX_NEW ctx=$MAX_CTX cpus=$CPUS" >&2
echo "[run_qwen] prompt chars: $(printf '%s' "$PROMPT" | wc -c)" >&2
echo "[run_qwen] generating (live output below; also saved to $REPLY) ..." >&2

# Run the REPL once. Notes:
#  - taskset 4-7: re-pin to big cores; std::system() inherits the parent's A55
#    mask, which would crawl the host-side token loop.
#  - tee: stream tokens live to the terminal (mixed Mandarin/English banner) AND
#    to "$RAW" for distillation.
#  - POSIX sh lacks PIPESTATUS, so capture the real exit code via a temp file.
RAW="${REPLY}.raw"
RC_FILE="${SNAPSHOT}.rc"
{
  printf '%s\n%s\n' "$PROMPT" "$EXIT_TOKEN" \
    | timeout "$TIMEOUT_S" taskset -c "$CPUS" stdbuf -oL -eL ./llm_demo "./$MODEL" "$MAX_NEW" "$MAX_CTX" 2>&1
  echo $? > "$RC_FILE"
} | tee "$RAW"
rc=$(cat "$RC_FILE" 2>/dev/null || echo 1)
rm -f "$RC_FILE"

# Distill: keep only the answer after the last "robot:" marker (strip the
# trailing "user:" prompt and blanks). The Mandarin banner stays on screen only.
ANSWER=$(sed -n 's/.*robot:[[:space:]]*//p' "$RAW" \
         | sed 's/[[:space:]]*user:[[:space:]]*$//' \
         | sed '/^[[:space:]]*$/d' \
         | tail -n 1)
printf '%s\n' "$ANSWER" > "$REPLY"
rm -f "$RAW"

if [ "$rc" -eq 124 ]; then
    echo "[run_qwen] TIMED OUT after ${TIMEOUT_S}s (partial reply in $REPLY)" >&2
else
    echo "[run_qwen] done (rc=$rc); clean reply written: $REPLY" >&2
fi
exit "$rc"

