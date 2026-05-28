#!/usr/bin/env bash
# catch_crash.sh - Run burnout3 under gdb; dump bt at first SIGSEGV.
set -u
cd "$(dirname "$0")/../.."
cat > /tmp/catch.gdb <<'EOF'
set pagination off
set print thread-events off
set confirm off
handle SIGPIPE nostop noprint pass
run
echo \n===== thread apply all bt 30 =====\n
thread apply all bt 30
echo \n===== current frame =====\n
frame
info registers
quit
EOF
gdb --batch -x /tmp/catch.gdb ./bin/burnout3 2>&1
pgrep burnout3 2>/dev/null | xargs -r kill -KILL 2>/dev/null
