# Abbreviated Windows test filter for GitHub Actions (MSVC x86_64).
#
# Full `csp_tests.exe -s --duration` is the local VM gate
# (scripts/win-validate.ps1). Cloud CI runs this smoke instead so the
# job finishes in seconds of test time and does not hit the 🎯T39 hang
# (mid-suite / Random---UniformInt under full load).
#
# Coverage intent: MSVC build + core channels/runtime units that
# exercise rendezvous, alt, cancel, timers, and basic invariants —
# not net/http/ws, MN stress, Random, or T38 microbenches.
#
# Usage (from repo root after a Release build):
#   powershell -File scripts/win-ci-smoke.ps1 -Exe build\Release\csp_tests.exe
param(
  [Parameter(Mandatory = $true)]
  [string]$Exe
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
  Write-Error "win-ci-smoke: exe not found: $Exe"
  exit 13
}

# doctest -sf takes a comma-separated list of wildcards matched against
# the source path recorded at compile time (usually test/foo.test.cc).
$sourceFilter = @(
  '*channel.test.cc*',
  '*chanutil*',
  '*chanmain*',
  '*ringbuffer*',
  '*timer*',
  '*cancel*',
  '*clock*',
  '*dynamic*',
  '*invariants*',
  '*buffer.test*',
  '*closer*',
  '*protocol*',
  '*bugs*',
  '*imp_exit*',
  '*main_context*',
  '*context.test*'
) -join ','

Write-Host "win-ci-smoke: $Exe -s --duration -sf=$sourceFilter"
& $Exe -s --duration "-sf=$sourceFilter"
exit $LASTEXITCODE
