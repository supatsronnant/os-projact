#!/bin/bash
# ทดลองอัตโนมัติ: Client 5 ตัวส่ง "RESERVE 10" พร้อมกัน
#
# Usage: ./experiment.sh <1|2|3>
#   1 = Sequential Baseline    (1 worker,  sync ON)
#   2 = Concurrent NO sync     (3 workers, sync OFF)  -> เกิด Race Condition
#   3 = Concurrent WITH sync   (3 workers, sync ON)
#
# ปรับจำนวน Client / หมายเลขที่นั่งได้ด้วยตัวแปร เช่น
#   CLIENTS=5 SEAT=10 ./experiment.sh 2
#
# สคริปต์นี้เปิด Server ของตัวเอง จึงต้องปิด Server ที่เปิดค้างไว้ก่อน
# ผลลัพธ์เก็บไว้ในโฟลเดอร์ results/

cd "$(dirname "$0")" || exit 1        # ทำงานที่โฟลเดอร์ของสคริปต์เสมอ

case "${1:-}" in
  1) WORKERS=1; SYNC=on  ;;
  2) WORKERS=3; SYNC=off ;;
  3) WORKERS=3; SYNC=on  ;;
  *) echo "Usage: $0 <1|2|3>"; exit 1 ;;
esac

EXP=$1
CLIENTS=${CLIENTS:-5}
SEAT=${SEAT:-10}
OUT=results
SERVER_LOG="$OUT/exp${EXP}_server.log"

# ---- กันเปิดซ้อน: ถ้ามี server รันอยู่แล้ว การทดลองจะเพี้ยน ----
if command -v pgrep >/dev/null 2>&1 && pgrep -x server >/dev/null; then
  echo "A server is already running. Stop it first (Ctrl+C in its terminal), then retry."
  exit 1
fi

# ---- คอมไพล์ใหม่ทุกครั้ง เพื่อให้แน่ใจว่าใช้โค้ดล่าสุดใน src/ ----
echo "Compiling ..."
gcc -Wall -Wextra -O2 -pthread src/server.c -o server -lrt || { echo "Compile failed: server.c"; exit 1; }
gcc -Wall -Wextra -O2 src/client.c -o client -lrt         || { echo "Compile failed: client.c"; exit 1; }

mkdir -p "$OUT"
rm -f "$OUT"/exp${EXP}_*

# ---- เปิด Server ----
./server --workers $WORKERS --sync $SYNC > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

cleanup() { kill -INT "$SERVER_PID" 2>/dev/null; }
trap cleanup EXIT                      # ปิด Server ให้เสมอ แม้กด Ctrl+C กลางทาง

sleep 1
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "Server failed to start:"
  cat "$SERVER_LOG"
  exit 1
fi

# ---- เปิด Client พร้อมกัน ----
pids=()
for i in $(seq 1 "$CLIENTS"); do
  echo "RESERVE $SEAT" | ./client "$i" > "$OUT/exp${EXP}_client${i}.out" 2>&1 &
  pids+=($!)
done
wait "${pids[@]}"

# ---- ปิด Server และรอให้จบ ----
kill -INT "$SERVER_PID" 2>/dev/null
wait "$SERVER_PID" 2>/dev/null

# ---- สรุปผล ----
echo
echo "=== Experiment $EXP: workers=$WORKERS, sync=$SYNC ==="
success=0
for i in $(seq 1 "$CLIENTS"); do
  res=$(grep -Eo "SUCCESS|FAILED" "$OUT/exp${EXP}_client${i}.out" | head -1)
  printf "Client %d : %s\n" "$i" "${res:-NO REPLY}"
  [ "$res" = "SUCCESS" ] && success=$((success + 1))
done
echo "Clients that reserved Seat $SEAT successfully: $success"

if [ "$success" -gt 1 ]; then
  echo ">>> RACE CONDITION: more than one client got the same seat!"
  races=$(grep -c "RACE CONDITION DETECTED" "$SERVER_LOG")
  echo "    (server logged $races race detection line(s))"
elif [ "$success" -eq 1 ]; then
  echo ">>> CORRECT: exactly one client got the seat."
else
  echo ">>> PROBLEM: no client got the seat. Check $SERVER_LOG"
fi
echo "Server log: $SERVER_LOG"