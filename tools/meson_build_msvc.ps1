Param(
  [string]$BuildDir = "build-cross",
  [switch]$RunTests
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$CrossFile = Join-Path $ProjectRoot "meson-cross-msvc.ini"

$crossConfig = @"
[binaries]
cpp = 'cl'
c = 'cl'
ar = 'lib'
exe_wrapper = ['cmd', '/c']

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
"@

[System.IO.File]::WriteAllText(
  $CrossFile,
  $crossConfig,
  (New-Object System.Text.UTF8Encoding($false))
)

Push-Location $ProjectRoot
try {
  $selectedBuildDir = $BuildDir

  if (Test-Path $selectedBuildDir) {
    Remove-Item -Path $selectedBuildDir -Recurse -Force -ErrorAction SilentlyContinue
  }

  meson setup $selectedBuildDir --buildtype=release --cross-file $CrossFile
  if ($LASTEXITCODE -ne 0) {
    $selectedBuildDir = "{0}-{1}" -f $BuildDir, (Get-Date -Format 'yyyyMMdd-HHmmss')
    Write-Host "[INFO] setup failed for '$BuildDir'. retry with '$selectedBuildDir'."

    if (Test-Path $selectedBuildDir) {
      Remove-Item -Path $selectedBuildDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    meson setup $selectedBuildDir --buildtype=release --cross-file $CrossFile
    if ($LASTEXITCODE -ne 0) {
      throw "Meson setup failed for '$BuildDir' and retry '$selectedBuildDir'."
    }
  }

  meson compile -C $selectedBuildDir

  if ($RunTests) {
    meson test -C $selectedBuildDir
  }
}
finally {
  Pop-Location
}
