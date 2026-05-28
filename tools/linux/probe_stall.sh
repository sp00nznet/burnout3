#!/usr/bin/env bash
# probe_stall.sh -- launch burnout3, wait, dump per-thread wchan + comm.
# Linux only.
set -u
cd "$(dirname "$0")/../.."
rm -f /tmp/run.log
./bin/burnout3 > /tmp/run.log 2>&1 &
PID=$!
echo "started pid=$PID"
sleep 8
echo "---per-thread state---"
if [ -d "/proc/$PID/task" ]; then
    for tid in $(ls "/proc/$PID/task/" 2>/dev/null); do
        state=$(awk '{print $3}' "/proc/$PID/task/$tid/stat" 2>/dev/null)
        wchan=$(cat "/proc/$PID/task/$tid/wchan" 2>/dev/null)
        comm=$(cat "/proc/$PID/task/$tid/comm" 2>/dev/null)
        # Only print the burnout3 threads (skip llvmpipe noise) plus main
        case "$comm" in
            llvmpipe-*) continue ;;
        esac
        echo "tid=$tid state=$state wchan=$wchan comm=$comm"
    done
fi
echo "---main thread syscall (number arg0 arg1 ... arg5 sp pc)---"
cat "/proc/$PID/syscall" 2>/dev/null || echo "(unavailable)"
echo "---main thread kernel stack (requires CAP_SYS_PTRACE)---"
cat "/proc/$PID/stack" 2>/dev/null || echo "(no perms)"
echo "---last 20 lines of stderr---"
tail -20 /tmp/run.log
kill -KILL "$PID" 2>/dev/null
sleep 1
pgrep burnout3 2>/dev/null | xargs -r kill -KILL 2>/dev/null
echo "done"
