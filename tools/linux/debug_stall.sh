#!/usr/bin/env bash
# debug_stall.sh - Run burnout3 under gdb, interrupt after a few seconds, dump
# backtraces of every thread. Reveals what the main thread is spinning on.
set -u
cd "$(dirname "$0")/../.."

cat > /tmp/dump_stall.gdb <<'EOF'
set pagination off
set print thread-events off
set print frame-arguments none
set print frame-info short-location
set confirm off

python
import threading, time, gdb
def stop_it():
    time.sleep(10)
    gdb.execute("interrupt")
threading.Thread(target=stop_it, daemon=True).start()
end

run
echo \n===== thread apply all bt 25 =====\n
thread apply all bt 25
echo \n===== main thread (#1) backtrace 60 =====\n
thread 1
bt 60
quit
EOF

gdb --batch -x /tmp/dump_stall.gdb ./bin/burnout3 2>&1
echo "=== exit ==="
pgrep burnout3 2>/dev/null | xargs -r kill -KILL 2>/dev/null
