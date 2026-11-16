# tools/run-qa.ps1 — one local gate for the whole solution.
#
#   pwsh -File tools/run-qa.ps1                 # configure + build + test
#   pwsh -File tools/run-qa.ps1 -SkipConfigure  # incremental build + test
#   pwsh -File tools/run-qa.ps1 -SkipBuild      # tests against an existing build
#
# Runs the platform engine suite, every strategy-team suite, the desk suite,
# the benchmark (as a smoke test) and `platd --check`. Exits non-zero on the
# first failure so CI and a pre-commit hook can use it directly.

[CmdletBinding()]
param(
  [switch]$SkipConfigure,
  [switch]$SkipBuild,
  [string]$Preset = "vs2026"
)

$ErrorActionPreference = "Continue"  # native stderr must not abort the gate
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Find-CMake {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -version "[18.0,19.0)" -products * `
          -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) {
      $candidate = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
      if (Test-Path $candidate) {
        return @{ Cmake = $candidate; VsRoot = $vs }
      }
    }
  }
  $path = Get-Command cmake -ErrorAction SilentlyContinue
  if ($path) { return @{ Cmake = $path.Source; VsRoot = "" } }
  throw "cmake not found (install Visual Studio C++ tools or put cmake on PATH)"
}

$tool = Find-CMake
$cmake = $tool.Cmake
$bin = Join-Path $root ("build\" + $Preset)

if (-not $SkipConfigure) {
  $cmakeArgs = @("--preset", $Preset)
  if ($tool.VsRoot) { $cmakeArgs += "-DCMAKE_GENERATOR_INSTANCE=$($tool.VsRoot)" }
  & $cmake @cmakeArgs
  if ($LASTEXITCODE -ne 0) { throw "configure failed" }
}

if (-not $SkipBuild) {
  & $cmake --build $bin --config Release --parallel 4
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$tests = @(
  (Join-Path $bin "platform\Release\plat_tests.exe"),
  (Join-Path $bin "strategies\trend\Release\strat_trend_tests.exe"),
  (Join-Path $bin "strategies\fade\Release\strat_fade_tests.exe"),
  (Join-Path $bin "strategies\basis\Release\strat_basis_tests.exe"),
  (Join-Path $bin "strategies\cash\Release\strat_cash_tests.exe"),
  (Join-Path $bin "desk\Release\desk_tests.exe")
)
$failed = $false
foreach ($exe in $tests) {
  if (-not (Test-Path $exe)) { Write-Error "missing test binary: $exe"; $failed = $true; continue }
  Push-Location (Split-Path $exe)
  try {
    $out = & $exe 2>&1
    $code = $LASTEXITCODE
    $out | Select-Object -Last 1
    if ($code -ne 0) { $failed = $true; $out | Select-Object -Last 20 }
  } finally { Pop-Location }
}

# Smoke: the daemon must still load its CTP SDK and parse the example lineup
# offline (DLLs are copied beside the exe by the build).
$platd = Join-Path $bin "platform\Release\platd.exe"
if (Test-Path $platd) {
  Push-Location (Split-Path $platd)
  try {
    & $platd --check platd.example.cfg | Select-String -Pattern "config:|strategies" | Select-Object -Last 2
    $code = $LASTEXITCODE
    if ($code -ne 0) { $failed = $true }
  } finally { Pop-Location }
} else {
  Write-Error "missing platd.exe"; $failed = $true
}

if ($failed) { Write-Error "QA FAILED"; exit 1 }
Write-Host "QA PASSED" -ForegroundColor Green
exit 0
