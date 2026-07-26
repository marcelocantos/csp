#!/usr/bin/env bash
# Local Windows CI gate (mnemo pattern): build + **full** test suite on the
# Parallels VM. Cloud `Windows x86_64` only runs MSVC + abbreviated smoke
# (scripts/win-ci-smoke.ps1); this script is the merge Windows signal.
#
# Validates the *pushed* current commit: push the branch first, then run
# this. scp's scripts/win-validate.ps1 to the VM and runs it (clone SHA,
# CMake MSVC ARM64, full csp_tests.exe). Exit 0 = Windows is green.
#
# Config: WINCI_VM (default hms-vm), WINCI_TEST_TIMEOUT_SEC (default 480).
set -uo pipefail

VM="${WINCI_VM:-hms-vm}"
TEST_TIMEOUT_SEC="${WINCI_TEST_TIMEOUT_SEC:-480}"
REMOTE_PS="C:/Users/marcelo/csp-win-validate.ps1"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

sha="$(git rev-parse HEAD)"

# The gate validates what will merge, so the commit must be on origin.
if [ -z "$(git branch -r --contains "$sha" 2>/dev/null)" ]; then
  echo "win-validate: HEAD ${sha:0:12} is not on any remote branch — push first." >&2
  exit 2
fi

echo "win-validate: $VM building + testing ${sha:0:12} (windows/arm64, MSVC, timeout ${TEST_TIMEOUT_SEC}s)…"

if ! scp -q "$HERE/win-validate.ps1" "$VM:$REMOTE_PS"; then
  echo "win-validate: scp to $VM failed (is the VM up / ssh $VM reachable?)" >&2
  exit 3
fi

out="$(ssh -o ConnectTimeout=15 "$VM" \
  "powershell -NoProfile -ExecutionPolicy Bypass -File $REMOTE_PS -Sha $sha -TestTimeoutSec $TEST_TIMEOUT_SEC" 2>&1)"
echo "$out" | grep -vE '^[[:space:]]*$'

# Authoritative pass signal is the script's own csp_test_exit marker —
# robust against any ssh exit-code propagation quirks.
if printf '%s\n' "$out" | grep -q "csp_test_exit=0"; then
  echo "win-validate: PASS (windows/arm64)"
  exit 0
fi
echo "win-validate: FAIL — Windows build/tests did not pass on $VM" >&2
exit 1
