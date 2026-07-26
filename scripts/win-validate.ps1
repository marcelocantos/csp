# Local Windows **full** build+test of CSP on the Parallels VM
# (windows/arm64, MSVC). Driven by scripts/win-validate.sh.
# Cloud CI uses scripts/win-ci-smoke.ps1 (abbreviated); this script runs
# the complete suite. Checkout the commit, cmake -DCSP_TLS=OFF, build
# Release, run csp_tests.exe -s --duration with a wall-clock timeout.
# Prereqs: VS 18 Community with ARM64 MSVC + CMake, Git.
# Generator: "Visual Studio 18 2026" -A ARM64.
param(
  [string]$Sha = "origin/master",
  [int]$TestTimeoutSec = 480,
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

Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue

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
$p = Start-Process -FilePath $exe -ArgumentList @("-s", "--duration") -NoNewWindow -PassThru `
  -RedirectStandardOutput $outLog -RedirectStandardError $errLog
$timeoutMs = [Math]::Max(1, $TestTimeoutSec) * 1000
if (-not $p.WaitForExit($timeoutMs)) {
  try { $p.Kill() } catch {}
  "win_validate_fail=test_timeout" | Write-Host
  Get-Content $outLog -Tail 40 -ErrorAction SilentlyContinue | Write-Host
  "csp_test_exit=124" | Write-Host
  exit 14
}

$code = $p.ExitCode
Get-Content $outLog -Tail 50 -ErrorAction SilentlyContinue | Write-Host
if ($code -ne 0) {
  Get-Content $errLog -Tail 20 -ErrorAction SilentlyContinue | Write-Host
}
"csp_test_exit=$code" | Write-Host
exit $code
