#!/usr/bin/env bash
# ci-tier: fast
# Applet supervision (scripts/sde-supervise-applet.sh): an applet that dies is
# restarted, with a backoff; one that exits 0 is not. No compositor, no applet:
# the wrapper is exercised with a tiny script that fails a few times and then
# stays up, which is exactly the shape of a crashing applet.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
SUPERVISOR="$SDE_ROOT/scripts/sde-supervise-applet.sh"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-supervise.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== an applet that crashes is restarted ==="
# Fails twice, then runs (so the third start is the one that stays up).
cat > "$RUNTIME/flaky-applet" <<EOF
#!/usr/bin/env bash
echo start >> "$RUNTIME/starts"
n="\$(wc -l < "$RUNTIME/starts")"
if [[ "\$n" -lt 3 ]]; then
    exit 3
fi
sleep 300
EOF
chmod +x "$RUNTIME/flaky-applet"

"$SUPERVISOR" "$RUNTIME/flaky-applet" > "$RUNTIME/supervisor.log" 2>&1 &
SUPERVISOR_PID=$!
for _ in $(seq 1 100); do
    [[ -f "$RUNTIME/starts" ]] && [[ "$(wc -l < "$RUNTIME/starts")" -ge 3 ]] && break
    sleep 0.1
done
starts="$(wc -l < "$RUNTIME/starts" 2>/dev/null || echo 0)"
check "the applet was started three times (crashed twice)" "[[ '$starts' -ge 3 ]]"
check "each crash is logged" "grep -q 'exited with status 3' '$RUNTIME/supervisor.log'"
check "the restart is logged" "grep -q 'restarting in' '$RUNTIME/supervisor.log'"
# It is still up, so the supervisor is still waiting on it.
check "the supervisor is still running" "kill -0 '$SUPERVISOR_PID' 2>/dev/null"
kill "$SUPERVISOR_PID" 2>/dev/null || true
wait "$SUPERVISOR_PID" 2>/dev/null || true

echo "=== an applet that exits cleanly is not restarted ==="
cat > "$RUNTIME/tidy-applet" <<EOF
#!/usr/bin/env bash
echo start >> "$RUNTIME/tidy-starts"
exit 0
EOF
chmod +x "$RUNTIME/tidy-applet"
"$SUPERVISOR" "$RUNTIME/tidy-applet" > "$RUNTIME/tidy.log" 2>&1
status=$?
check "the supervisor exits 0 with it" "[[ '$status' -eq 0 ]]"
check "it ran exactly once" "[[ \"\$(wc -l < '$RUNTIME/tidy-starts')\" -eq 1 ]]"
check "it says why it stopped" "grep -q 'exited cleanly' '$RUNTIME/tidy.log'"

echo "=== one supervisor serves several applets ==="
# The session runs the whole applet set through one process: a crashed applet
# keeps restarting while a sibling that exited cleanly is left alone.
cat > "$RUNTIME/multi-flaky" <<EOF
#!/usr/bin/env bash
echo "start \$*" >> "$RUNTIME/multi-flaky-starts"
n="\$(wc -l < "$RUNTIME/multi-flaky-starts")"
if [[ "\$n" -lt 3 ]]; then
    exit 4
fi
sleep 300
EOF
chmod +x "$RUNTIME/multi-flaky"
cat > "$RUNTIME/multi-tidy" <<EOF
#!/usr/bin/env bash
echo start >> "$RUNTIME/multi-tidy-starts"
exit 0
EOF
chmod +x "$RUNTIME/multi-tidy"

flaky_spec="Flaky|$(printf '%q ' "$RUNTIME/multi-flaky" --marked 'two words')"
tidy_spec="Tidy|$(printf '%q ' "$RUNTIME/multi-tidy" --name=desktop)"
"$SUPERVISOR" --applet "$flaky_spec" --applet "$tidy_spec" > "$RUNTIME/multi.log" 2>&1 &
MULTI_PID=$!
for _ in $(seq 1 100); do
    [[ -f "$RUNTIME/multi-flaky-starts" ]] && [[ "$(wc -l < "$RUNTIME/multi-flaky-starts")" -ge 3 ]] && break
    sleep 0.1
done
flaky_starts="$(wc -l < "$RUNTIME/multi-flaky-starts" 2>/dev/null || echo 0)"
tidy_starts="$(wc -l < "$RUNTIME/multi-tidy-starts" 2>/dev/null || echo 0)"
check "the crashing applet was restarted alongside the tidy one" "[[ '$flaky_starts' -ge 3 ]]"
check "the tidy applet ran exactly once" "[[ '$tidy_starts' -eq 1 ]]"
check "the tidy applet is not restarted" "grep -q 'Tidy: exited cleanly' '$RUNTIME/multi.log'"
check "the crashing applet keeps the supervisor alive" "kill -0 '$MULTI_PID' 2>/dev/null"
check "the applet arguments survive the spec" \
    "grep -qF -- '--marked two words' '$RUNTIME/multi-flaky-starts' 2>/dev/null"
check "the supervisor logs the applet's name, not the command" \
    "grep -q 'sde-supervise-applet: Flaky: starting' '$RUNTIME/multi.log'"
kill "$MULTI_PID" 2>/dev/null || true
wait "$MULTI_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "--- supervisor log ---"; cat "$RUNTIME/supervisor.log" 2>/dev/null || true
    echo "==> applet supervision test FAILED"
    exit 1
fi
echo "==> applet supervision test PASSED"
