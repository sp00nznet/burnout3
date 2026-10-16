# One-click setup for the Burnout 3: Takedown static recompilation. Run it by
# double-clicking Setup.cmd in the repo folder; the README's "Step by step" is
# the same thing by hand, command for command.
#
# It checks the tools, fetches the xboxrecomp toolkit beside this folder, takes
# your own disc image apart into "Burnout 3 Takedown\", regenerates the C from
# default.xbe (regen.sh), builds bin\burnout3.exe, and leaves "Play Burnout 3.cmd"
# in the repo folder. Each step is skipped when its output already exists
# (-Force redoes them), so a rerun after a failure resumes. Everything it does
# goes to setup.log. Nothing is downloaded but tools; the game comes from you.
param([switch]$Force, [string]$Iso = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = Join-Path (Split-Path -Parent $Root) 'xboxrecomp'
# The toolkit branch to clone.
$ToolkitRef = 'main'
$GameDir = Join-Path $Root 'Burnout 3 Takedown'
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
function Log($t) { Add-Content -Path $Log -Value $t -Encoding UTF8 }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/5] $t" 'Cyan' }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Read-Host "Press Enter to close" | Out-Null; exit 1
}
function Ask($q) { $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Winget-Install($name, $id, $size, [string[]]$extra = @()) {
  if (-not (Ask "  Install $name now (winget, $size)?")) { Fail "$name is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install $name yourself."
  }
  Exec (@('winget', 'install', '-e', '--id', $id, '--accept-package-agreements', '--accept-source-agreements') + $extra) | Out-Null
  Refresh-Path
}

Clear-Host
Say "Burnout 3: Takedown - static recompilation setup" 'White'
Say "You need your own Burnout 3: Takedown disc image (.iso) and about 12 GB free."
Say "The long steps are regenerating the C (a few minutes) and building it (20-40 minutes, once)."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools the build needs"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  Winget-Install 'Python 3.12 (for your user only)' 'Python.Python.3.12' 'about 30 MB' @('--scope', 'user')
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
# regen.sh calls "py -3"; the launcher comes with python.org installs.
if (-not (Get-Command py -ErrorAction SilentlyContinue)) {
  Fail "the Python launcher 'py' is missing. Reinstall Python from python.org with the launcher ticked."
}
Say "  Python: $($pyargs -join ' ')"

if ((Exec ($pyargs + @('-c', 'import capstone'))) -ne 0) {
  Say "  The Python package capstone (the disassembler, about 10 MB) is missing."
  if (-not (Ask "  Install it now (pip, for your user only)?")) { Fail "capstone is required." }
  Run "Installing capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'capstone'))
}

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  Say "  Git for Windows is missing. It fetches the toolkit, and its bash runs regen.sh."
  Winget-Install 'Git for Windows' 'Git.Git' 'about 60 MB'
}
# Git's own bash, not WSL's C:\Windows\System32\bash.exe. git.exe may be
# Git\cmd\git.exe or Git\mingw64\bin\git.exe, so walk up to the Git folder.
$Bash = $null
$d = Split-Path -Parent (Get-Command git).Source
while ($d -and -not $Bash) {
  if (Test-Path (Join-Path $d 'bin\bash.exe')) { $Bash = Join-Path $d 'bin\bash.exe' }
  $d = Split-Path -Parent $d
}
if (-not $Bash) { Fail "Git is installed but its bash.exe was not found beside it." }
Say "  Git bash: $Bash"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  Say "  CMake 3.20 or newer is missing."
  Winget-Install 'CMake' 'Kitware.CMake' 'about 35 MB'
  if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { Fail "CMake installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  CMake: $((& cmake --version | Select-Object -First 1))"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
function Has-MSVC {
  if (-not (Test-Path $vswhere)) { return $false }
  return [bool](& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
}
if (-not (Has-MSVC)) {
  Say "  The Visual Studio 2022 C++ compiler is missing."
  Winget-Install 'Visual Studio 2022 Build Tools with the C++ workload' 'Microsoft.VisualStudio.2022.BuildTools' 'about 6 GB' @('--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
  if (-not (Has-MSVC)) { Fail "the C++ build tools did not install. Install 'Desktop development with C++' from the Visual Studio Installer." }
}
Say "  MSVC: found"

if (-not (Test-Path (Join-Path $Toolkit 'tools\recomp'))) {
  Say "  The xboxrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Ask "  Download it now (git, about 50 MB)?")) { Fail "xboxrecomp is required at $Toolkit." }
  Run "Cloning xboxrecomp" @('git', 'clone', '--branch', $ToolkitRef, 'https://github.com/sp00nznet/xboxrecomp', $Toolkit)
}
Say "  xboxrecomp: $Toolkit"

# ---------------------------------------------------------------- the game
Step 2 "Unpacking your disc image into 'Burnout 3 Takedown\'"
function Is-Iso([string]$f) { return ($f -and (Test-Path -PathType Leaf $f)) }
if ((Test-Path (Join-Path $GameDir 'default.xbe')) -and -not $Force) {
  Say "  Already unpacked (skipping)."
} else {
  $Iso = $Iso.Trim('"', ' ')
  if (-not (Is-Iso $Iso)) {
    # Next to the repo, next to the toolkit, or in Downloads.
    $near = @((Split-Path -Parent $Root), $Root, (Join-Path $env:USERPROFILE 'Downloads')) |
      ForEach-Object { Get-ChildItem -Path $_ -Filter '*Burnout 3*.iso' -ErrorAction SilentlyContinue } |
      Select-Object -First 1
    if ($near -and (Ask "  Use $($near.FullName)?")) { $Iso = $near.FullName }
  }
  while (-not (Is-Iso $Iso)) {
    $Iso = (Read-Host "  Paste the path to your Burnout 3: Takedown .iso").Trim('"', ' ')
    if (-not (Is-Iso $Iso)) { Say "  No file there." 'Yellow' }
  }
  Push-Location $Toolkit
  Run "Unpacking $Iso (about 3 GB)" ($pyargs + @('-m', 'tools.xiso', 'unpack', $Iso, '-o', $GameDir))
  Pop-Location
  if (-not (Test-Path (Join-Path $GameDir 'default.xbe'))) { Fail "the image unpacked, but there is no default.xbe in it. Is it the right disc?" }
}

# ---------------------------------------------------------------- regen
Step 3 "Regenerating the C from default.xbe (a few minutes)"
if ((Test-Path 'src\game\recomp\gen\recomp_funcs.h') -and -not $Force) { Say "  Already done (skipping)." }
else {
  # Git Bash turns regen.sh's /c/... paths into C:\... for py; don't set
  # MSYS_NO_PATHCONV, which stops that.
  Run "Running regen.sh" @($Bash, '-lc', "cd '$($Root -replace '\\','/')' && ./regen.sh --disasm")
}

# ---------------------------------------------------------------- build
Step 4 "Building bin\burnout3.exe (20-40 minutes the first time)"
if ((Test-Path 'bin\burnout3.exe') -and -not $Force) { Say "  Already built (skipping)." }
else {
  Run "Configuring" @('cmake', '-S', '.', '-B', 'build-xr', '-G', 'Visual Studio 17 2022', '-A', 'x64')
  Run "Compiling" @('cmake', '--build', 'build-xr', '--config', 'Release', '--parallel', '2')
  if (-not (Test-Path 'bin\burnout3.exe')) { Fail "the build finished but bin\burnout3.exe is missing." }
}

# ---------------------------------------------------------------- launcher
# "start" so the launcher's console closes at once: burnout3.exe is a GUI
# program, and a batch file would otherwise keep an empty console open
# beside the game until it exits.
Step 5 "Making the launcher"
$launcher = Join-Path $Root 'Play Burnout 3.cmd'
Set-Content -Path $launcher -Encoding ASCII -Value @'
@echo off
cd /d "%~dp0"
start "" bin\burnout3.exe %*
'@
Say "  $launcher"

Write-Host ""
Say "Done. Double-click 'Play Burnout 3.cmd' to play (an Xbox or XInput pad, or two)." 'Green'
Say "It runs on a software GPU: menus are smooth, races run a few frames a second. See the README."
Read-Host "Press Enter to close" | Out-Null
