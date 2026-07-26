# Local Windows **full** build+test of CSP on the Parallels VM
# (windows/arm64, MSVC). Driven by scripts/win-validate.sh.
# Cloud CI uses scripts/win-ci-smoke.ps1 (abbreviated); this script runs
# the complete suite. Checkout the commit, cmake -DCSP_TLS=OFF, build
# Release, run csp_tests.exe --duration (no -s: verbose SUCCESS lines
# thrash redirected I/O and falsely look like hangs) with a hard
# wall-clock timeout. WorkingDirectory is the repo root so relative
# paths (docs/papers/…) resolve.
# Prereqs: VS 18 Community with ARM64 MSVC + CMake, Git.
# Generator: "Visual Studio 18 2026" -A ARM64.
param(
  [string]$Sha = "origin/master",
  [int]$TestTimeoutSec = 600,
  [string]$Work = "C:\Users\marcelo\winci-csp"
)

$ErrorActionPreference = "Continue"
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

New-Item -ItemType Directory -Force -Path $Work | Out-Null
Set-Location $Work

# --- csp: the commit under test ---
if (-not (Test-Path csp\.git)) {
  git clone --quiet --recurse-submodules https://github.com/marcelocantos/csp.git csp
}
Set-Location csp
git fetch --quiet origin
git checkout --quiet -f $Sha
git submodule update --init --recursive --quiet
"csp_head=$(git rev-parse --short HEAD)" | Write-Host

if (-not (Test-Path $cmake)) {
  "win_validate_fail=cmake_missing" | Write-Host
  "csp_test_exit=10" | Write-Host
  exit 10
}

# Incremental configure/build (wipe only when CSP_WIN_CLEAN=1). Full wipes
# take 10+ minutes on the ARM64 VM and invite first-run AV stalls.
if ($env:CSP_WIN_CLEAN -eq "1") {
  Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
}

& $cmake -G "Visual Studio 18 2026" -A ARM64 -B build -DCSP_TLS=OFF
if ($LASTEXITCODE -ne 0) {
  "win_validate_fail=cmake_configure" | Write-Host
  "csp_test_exit=11" | Write-Host
  exit 11
}
"cmake_cfg_ok" | Write-Host

& $cmake --build build --config Release --parallel
if ($LASTEXITCODE -ne 0) {
  "win_validate_fail=cmake_build" | Write-Host
  "csp_test_exit=12" | Write-Host
  exit 12
}

$exe = Join-Path (Get-Location) "build\Release\csp_tests.exe"
if (-not (Test-Path $exe)) {
  $found = Get-ChildItem -Recurse -Filter csp_tests.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($found) { $exe = $found.FullName } else {
    "win_validate_fail=exe_missing" | Write-Host
    "csp_test_exit=13" | Write-Host
    exit 13
  }
}
"exe=$exe" | Write-Host

$outLog = Join-Path $Work "test.out"
$errLog = Join-Path $Work "test.err"
$repoRoot = Get-Location

# Warm freshly linked binaries (AV / page-in on Parallels can stall the
# first long redirected run for many minutes).
& $exe --list-test-cases 2>$null | Out-Null
"warmup_ok" | Write-Host

function Invoke-CspSuite {
  param([string]$Label)
  Remove-Item $outLog, $errLog -ErrorAction SilentlyContinue
  Get-Process csp_tests -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 500
  # --duration only: -s floods multi-MB assertion text through redirected
  # stdout and makes the suite look hung on slow VM disks (🎯T39).
  $proc = Start-Process -FilePath $exe -ArgumentList @("--duration") `
    -WorkingDirectory $repoRoot.Path -NoNewWindow -PassThru `
    -RedirectStandardOutput $outLog -RedirectStandardError $errLog
  $timeoutMs = [Math]::Max(1, $TestTimeoutSec) * 1000
  if (-not $proc.WaitForExit($timeoutMs)) {
    try { $proc.Kill() } catch {}
    "win_validate_fail=test_timeout label=$Label" | Write-Host
    Get-Content $outLog -Tail 40 -ErrorAction SilentlyContinue | Write-Host
    return 124
  }
  $code = $proc.ExitCode
  if ($null -eq $code) { $code = -1 }
  if (Test-Path $outLog) {
    Get-Content $outLog -Tail 50 -ErrorAction SilentlyContinue | Write-Host
    $raw = Get-Content $outLog -Raw -ErrorAction SilentlyContinue
    if ($raw -match 'Status:\s*SUCCESS') {
      $code = 0
    } elseif ($raw -match 'Status:\s*FAILURE') {
      if ($code -eq 0) { $code = 1 }
    }
  }
  return $code
}

# Start-Process ExitCode can be $null even after WaitForExit on some hosts;
# prefer doctest's Status line when present (authoritative oracle).
$code = Invoke-CspSuite -Label "attempt1"
# One retry after a timeout: first cold run after rebuild has flaked on
# the VM even when the same binary then passes (AV / residual process).
if ($code -eq 124) {
  "win_validate_retry=1" | Write-Host
  $code = Invoke-CspSuite -Label "attempt2"
}

if ($code -ne 0) {
  Get-Content $errLog -Tail 20 -ErrorAction SilentlyContinue | Write-Host
}
"csp_test_exit=$code" | Write-Host
if ($code -eq 0) {
  "win-validate: PASS (windows/arm64)" | Write-Host
} else {
  "win-validate: FAIL - Windows build/tests did not pass" | Write-Host
}
exit $code
