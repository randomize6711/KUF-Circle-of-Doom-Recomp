<#
.SYNOPSIS
  The Windows acceptance test of KUF: Circle of Doom (M17, docs/WINDOWS_TEST.md), on the player's own Windows PC.

.DESCRIPTION
  Everything happens in new folders under -WorkDir: the portable edition extracted there (in a path with non-English
  letters), a test installation of the setup with its data in -WorkDir too, and every game run with its own saves
  and settings (copies of test saves from the kit). Your game files (-GameDir) are only read. Your real saves,
  settings and installations are not used or changed: if the setup is already installed for real, the setup test
  runs without shortcuts and without the Settings > Apps entry, so the real ones stay as they are.

  While the game runs, the test presses keys and moves the mouse itself (Alt+Tab, a drag onto the launcher).
  Do not use the keyboard or mouse until it says so. It takes about 1.5 hours (plus about an hour with -Build).

  The results are written to <kit>\results\<time>\ and packed as <kit>\results-<time>.zip: bring that file back to
  the Linux PC, where tools/windows/verify_acceptance.sh checks the game runs with the same rules as the Linux tests.

.PARAMETER GameDir
  (Asked when not given.) Your game: the folder with default.xex (the retail dump as it is, or with an unencrypted default.xex), or an
  Xbox 360 disc image (.iso). Only read.
.PARAMETER WorkDir
  Where the test folders are made (default: <kit>\work). About 30 GB free are needed.
.PARAMETER Only
  Run only these parts: tests, portable, scenarios, restart, update, move, drop, setup, controller, build.
.PARAMETER Controller
  Also the controller part (C1-C5): you plug and press when told (an Xbox controller is needed).
.PARAMETER Build
  Also the native build (E1): Git for Windows, CMake 3.28+, Ninja and Python 3 must be installed.
.PARAMETER NoNetwork
  Skip the one online check (the updater asks GitHub for the newest release, as the game's Check for Updates does).
#>
param(
  [string]$GameDir = "",
  [string]$WorkDir = "",
  [string[]]$Only = @(),
  [switch]$Controller,
  [switch]$Build,
  [switch]$NoNetwork,
  [switch]$Unattended
)
Set-StrictMode -Version 2
$ErrorActionPreference = "Stop"

$Here = $PSScriptRoot
$Kit = Split-Path -Parent $Here
if (-not $WorkDir) { $WorkDir = Join-Path $Kit "work" }
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$Res = Join-Path $Kit "results\$Stamp"
New-Item -ItemType Directory -Force -Path $Res, (Join-Path $Res "runs"), (Join-Path $Res "logs"), (Join-Path $Res "shots") | Out-Null
$WorkDir = (New-Item -ItemType Directory -Force -Path $WorkDir).FullName
$Work = Join-Path $WorkDir "run-$Stamp"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
Start-Transcript -Path (Join-Path $Res "transcript.txt") | Out-Null

if (-not $GameDir) {
  Write-Host "Drag your game folder (with default.xex) or your disc image (.iso) into this window, then press Enter." -ForegroundColor Yellow
  $GameDir = (Read-Host "Game").Trim().Trim('"')
}
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Path (Join-Path $Here "KufAcceptance.cs") -ReferencedAssemblies System.Windows.Forms, System.Drawing

# ---- results ---------------------------------------------------------------------------------------------------
$Checks = New-Object System.Collections.ArrayList
function Check([string]$id, [string]$status, [string]$detail) {
  # status: pass, fail, skip (not run, with the reason), manual (a person must look), verify (checked on Linux)
  [void]$Checks.Add([ordered]@{ id = $id; status = $status; detail = $detail; time = (Get-Date -Format "HH:mm:ss") })
  $color = @{ pass = "Green"; fail = "Red"; skip = "DarkGray"; manual = "Yellow"; verify = "Cyan" }[$status]
  Write-Host ("[{0,-6}] {1}: {2}" -f $status, $id, $detail) -ForegroundColor $color
  Save-Results
}
function Save-Results {
  $Checks | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $Res "results.json")
}
function Want([string]$part) { return ($Only.Count -eq 0) -or ($Only -contains $part) }
function Say([string]$text) { Write-Host ""; Write-Host "== $text" -ForegroundColor White }

# ---- programs ------------------------------------------------------------------------------------------------
# Runs a program to its end; its output in logs\<name>.txt. Returns @{ code; out }.
function Invoke-Prog([string]$name, [string]$exe, [string[]]$arguments, [hashtable]$envs = @{}, [int]$timeout = 600, [string]$cwd = "") {
  $log = Join-Path $Res "logs\$name.txt"
  if (-not $cwd) { $cwd = Split-Path -Parent $exe }
  $h = New-Object System.Collections.Hashtable
  foreach ($k in $envs.Keys) { $h[$k] = [string]$envs[$k] }
  $code = [KufAcc]::Run($exe, $arguments, $cwd, $h, $log, $timeout)
  $out = ""
  if (Test-Path -LiteralPath $log) { $out = [IO.File]::ReadAllText($log) }
  return @{ code = $code; out = $out }
}

function Copy-Tree([string]$from, [string]$to) {
  # robocopy: large game folders, long paths; only reads $from. Exit codes below 8 are success.
  New-Item -ItemType Directory -Force -Path $to | Out-Null
  & robocopy $from $to /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "copying $from to $to failed (robocopy $LASTEXITCODE)" }
  # Writable copies (the game writes saves; a read-only attribute could have come with the kit).
  Get-ChildItem -LiteralPath $to -Recurse -File -Force | Where-Object { $_.IsReadOnly } | ForEach-Object { $_.IsReadOnly = $false }
}

function Hash-Tree([string]$dir) {
  if (-not (Test-Path -LiteralPath $dir)) { return "" }
  $lines = Get-ChildItem -LiteralPath $dir -Recurse -File | Sort-Object FullName | ForEach-Object {
    $_.FullName.Substring($dir.Length) + " " + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
  }
  return ($lines -join "`n")
}

function Read-Shared([string]$path) {
  # A log the game is still writing.
  if (-not (Test-Path -LiteralPath $path)) { return "" }
  $fs = [IO.File]::Open($path, "Open", "Read", "ReadWrite, Delete")
  try { return (New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)).ReadToEnd() } finally { $fs.Dispose() }
}

function Wait-Exit($proc, [int]$seconds) {
  if (-not $proc.WaitForExit($seconds * 1000)) { try { $proc.Kill() } catch {}; [void]$proc.WaitForExit(10000); return 124 }
  $proc.WaitForExit()
  return $proc.ExitCode
}

function Game-Processes([string]$root) {
  return @(Get-Process -Name kuf_cod -ErrorAction SilentlyContinue | Where-Object {
      try { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) } catch { $false } })
}

# The game's command line and environment as the launcher of a portable folder or installation starts it.
function Get-Launch([string]$launcher, [hashtable]$envs = @{}) {
  $r = Invoke-Prog ("print-launch-" + [IO.Path]::GetFileName((Split-Path -Parent $launcher))) $launcher @("--print-launch") $envs 120
  if ($r.code -ne 0) { throw "--print-launch failed ($($r.code)): $($r.out)" }
  $argv = @(); $vars = @{}
  foreach ($line in ($r.out -split "`r?`n")) {
    if ($line.StartsWith("arg ")) { $argv += $line.Substring(4) }
    elseif ($line.StartsWith("env ")) { $kv = $line.Substring(4); $i = $kv.IndexOf("="); $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
  }
  return @{ exe = $argv[0]; args = @($argv | Select-Object -Skip 1); env = $vars }
}

# ---- one game scenario (plan.json; checked on Linux by tools/menu_test.py --verify-only) --------------------------
$Plan = Get-Content -Raw -Encoding UTF8 (Join-Path $Here "plan.json") | ConvertFrom-Json
Copy-Item -LiteralPath (Join-Path $Here "plan.json") (Join-Path $Res "plan.json")
$RunsDir = Join-Path $Res "runs"

function Invoke-Scenario($sc, $launch, [string]$runId = "") {
  if (-not $runId) { $runId = $sc.run_id }
  $run = Join-Path $RunsDir $runId
  New-Item -ItemType Directory -Force -Path (Join-Path $run "user"), (Join-Path $run "saves"), (Join-Path $run "cache") | Out-Null
  [IO.File]::WriteAllText((Join-Path $run "run-root.txt"), $run)
  if ($sc.saves) {
    if ($sc.saves.PSObject.Properties.Name -contains "fixture") { $src = Join-Path $Here $sc.saves.fixture }
    else { $src = Join-Path (Join-Path $RunsDir $sc.saves.from_run) "saves" }
    if (-not (Test-Path -LiteralPath $src)) { Check "game:$runId" "fail" "no saves to start with ($src): the scenario before it did not run"; return $false }
    Copy-Tree $src (Join-Path $run "saves")
    Copy-Tree $src (Join-Path $run "input-saves")
  }
  if ($sc.pc_settings) {
    if ($sc.pc_settings.PSObject.Properties.Name -contains "file") { $src = Join-Path $Here $sc.pc_settings.file }
    else { $src = Join-Path (Join-Path $RunsDir $sc.pc_settings.from_run) "user\pc_settings.toml" }
    Copy-Item -LiteralPath $src (Join-Path $run "user\pc_settings.toml")
  }
  $skip = "^--(user_data_root|content_root|cache_root|log_file|log_max_file_size_mb|log_max_files)="
  $argv = @($launch.args | Where-Object { $_ -notmatch $skip })
  $argv += @("--user_data_root=$run\user", "--content_root=$run\saves", "--cache_root=$run\cache",
    "--log_file=$run\kuf_cod.log", "--log_max_file_size_mb=64", "--log_max_files=2")
  if ($sc.script_file) {
    Copy-Item -LiteralPath (Join-Path $Here $sc.script_file) (Join-Path $run "input-script.txt")
    $argv += "--kuf_input_script_file=$run\input-script.txt"
  }
  $argv += @($sc.args)
  $envs = New-Object System.Collections.Hashtable
  foreach ($p in $sc.env.PSObject.Properties) { $envs[$p.Name] = [string]$p.Value }
  [IO.File]::WriteAllLines((Join-Path $run "command.txt"), [string[]](@($launch.exe) + $argv))
  Write-Host "   $runId ($($sc.seconds) s): $($sc.description)"
  $proc = [KufAcc]::Start($launch.exe, $argv, (Split-Path -Parent $launch.exe), $envs, (Join-Path $run "stdout.txt"))
  $win = [KufAcc]::WaitMainWindow($proc.Id, 60)
  if ($win -ne [IntPtr]::Zero) { [void][KufAcc]::Focus($win) }
  # Alt+Tab when a mark appears in the log: away, then back (the foreground window recorded each time).
  $events = @()
  foreach ($a in @($sc.alt_tab)) {
    if (-not $a) { continue }
    $deadline = (Get-Date).AddSeconds($sc.timeout)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline -and -not ((Read-Shared "$run\kuf_cod.log").Contains($a.after))) { Start-Sleep -Milliseconds 250 }
    if ($proc.HasExited) { break }
    Start-Sleep -Seconds 2
    $before = [KufAcc]::ForegroundPid()
    [KufAcc]::AltTab(); Start-Sleep -Seconds 1
    $away = [KufAcc]::ForegroundPid()
    Start-Sleep -Seconds ([int]$a.away_seconds)
    [KufAcc]::AltTab(); Start-Sleep -Seconds 2
    $back = [KufAcc]::ForegroundPid()
    if ($back -ne $proc.Id) { $w = [KufAcc]::MainWindow($proc.Id); if ($w -ne [IntPtr]::Zero) { [void][KufAcc]::Focus($w) } }
    $events += "after '$($a.after)': foreground before $before (game $($proc.Id)), away $away, back $back"
  }
  if ($events.Count) { [IO.File]::WriteAllLines((Join-Path $run "alt-tab.txt"), [string[]]$events) }
  $code = Wait-Exit $proc $sc.timeout
  [IO.File]::WriteAllText((Join-Path $run "exit_status"), "$code`n")
  $crash = Test-Path -LiteralPath (Join-Path $run "user\crash-report.txt")
  $ok = ($code -eq $sc.expect_exit) -and -not $crash
  foreach ($e in $events) { if ($e -match "away $($proc.Id),") { $ok = $false } }
  $detail = "exit $code" + $(if ($crash) { ", crash report" } else { "" }) + $(if ($events.Count) { "; " + ($events -join "; ") } else { "" }) + " (log, script, images: checked on Linux)"
  Check "game:$runId" $(if ($ok) { "verify" } else { "fail" }) $detail
  return $ok
}

# ---- 0. the PC ---------------------------------------------------------------------------------------------------
Say "This PC"
$os = Get-CimInstance Win32_OperatingSystem
$gpus = (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) (driver $($_.DriverVersion))" }) -join "; "
$screens = ([System.Windows.Forms.Screen]::AllScreens | ForEach-Object { "$($_.DeviceName) $($_.Bounds.Width)x$($_.Bounds.Height)$(if ($_.Primary) {' primary'})" }) -join "; "
$pads = [KufAcc]::Pads()
$padText = (0..3 | ForEach-Object { if ($pads[$_] -ge 0) { "slot $_ connected" } }) -join ", "
$me = [KufAcc]::Elevated($PID)
$drive = (Get-Item -LiteralPath $WorkDir).PSDrive
$free = [math]::Round($drive.Free / 1GB, 1)
$info = [ordered]@{ windows = "$($os.Caption) $($os.Version) build $($os.BuildNumber)"; gpus = $gpus; screens = $screens
  controllers = $(if ($padText) { $padText } else { "none (XInput)" }); harness_elevated = $me; work = $WorkDir; free_gb = $free
  game = $GameDir; kit = $Kit; powershell = $PSVersionTable.PSVersion.ToString() }
$info | ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $Res "pc.json")
$info.GetEnumerator() | ForEach-Object { Write-Host ("   {0}: {1}" -f $_.Key, $_.Value) }
if ($me -eq 1) { Write-Host "   This window runs as administrator: start the test from a normal window (D1 needs it)." -ForegroundColor Red }
if ($free -lt 30) { Write-Host "   Only $free GB free on $($drive.Name): about 30 GB are needed." -ForegroundColor Red }
if (-not (Test-Path -LiteralPath $GameDir)) { throw "the game files were not found: $GameDir" }
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$GameIsFile = Test-Path -LiteralPath $GameDir -PathType Leaf

$Zip = @(Get-ChildItem -LiteralPath (Join-Path $Kit "packages") -Filter "KUF-Circle-of-Doom-Portable-*-windows-x86_64.zip")[0].FullName
$SetupExe = @(Get-ChildItem -LiteralPath (Join-Path $Kit "packages") -Filter "KUF-Circle-of-Doom-Setup-*-windows-x86_64.exe")[0].FullName
Write-Host "   packages: $([IO.Path]::GetFileName($Zip)), $([IO.Path]::GetFileName($SetupExe))"

# A real installation of the setup on this PC (its shortcuts, its Settings > Apps entry): never touched.
$GameName = "Kingdom Under Fire - Circle of Doom"
$StartMenu = [Environment]::GetFolderPath("Programs")
$Desktop = [Environment]::GetFolderPath("Desktop")
$AppsKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\KUF-Circle-of-Doom-Recomp"
$RealInstall = (Test-Path -LiteralPath (Join-Path $StartMenu "$GameName.lnk")) -or (Test-Path -LiteralPath (Join-Path $Desktop "$GameName.lnk")) -or (Test-Path $AppsKey)
$RealData = Join-Path $env:LOCALAPPDATA "kuf-cod-recomp"
$RealDataHash = Hash-Tree (Join-Path $RealData "data\saves")
Write-Host "   a real installation of the setup: $(if ($RealInstall) { 'yes (left alone: the setup test runs without shortcuts)' } else { 'no' })"

if (-not $Unattended) {
  Write-Host ""
  Write-Host "The test now runs the game many times and presses keys and moves the mouse itself." -ForegroundColor Yellow
  Write-Host "Do not use the keyboard or the mouse until it says it is done (about 1.5 hours)." -ForegroundColor Yellow
  Read-Host "Press Enter to start"
}

# ---- 1. the Windows test programs (no game) ------------------------------------------------------------------------
if (Want "tests") {
  Say "Windows test programs (networking)"
  $nt = Join-Path $Kit "native-tests"
  $r = Invoke-Prog "net-tests" (Join-Path $nt "kuf_net_tests.exe") @() @{} 900
  $sum = ([regex]::Matches($r.out, "(All tests passed.*|test cases:.*)") | ForEach-Object { $_.Value }) -join " "
  Check "net:unit" $(if ($r.code -eq 0) { "pass" } else { "fail" }) "kuf_net_tests.exe exit $($r.code): $sum"
  # A lobby on this PC: host and guest as two Windows programs over loopback and LAN discovery (E3's local part).
  $hostLog = Join-Path $Res "logs\lobby-host.txt"
  $hp = [KufAcc]::Start((Join-Path $nt "kuf_lobby_peer.exe"), @("host", "--port", "41100", "--discovery", "41101", "--password", "s3cret pass", "--name", "Host"), $nt, $null, $hostLog)
  $t0 = Get-Date
  while (-not (Read-Shared $hostLog).Contains("READY") -and ((Get-Date) - $t0).TotalSeconds -lt 20) { Start-Sleep -Milliseconds 200 }
  $g = Invoke-Prog "lobby-guest" (Join-Path $nt "kuf_lobby_peer.exe") @("join", "--port", "41100", "--discovery", "41101", "--password", "s3cret pass", "--name", "Guest") @{} 120
  $hc = Wait-Exit $hp 60
  Check "net:lobby-local" $(if ($g.code -eq 0 -and $hc -eq 0) { "pass" } else { "fail" }) "host $hc, guest $($g.code): $((($g.out -split "`n") | Where-Object { $_ -match 'PASS|FAIL' }) -join ' ')"
}

# ---- 2. the portable edition in a Unicode path (A1-A3) -------------------------------------------------------------
$PortParent = Join-Path $Work "Spiel Übung\Тест KUF"
$Port = Join-Path $PortParent "KUF-Circle-of-Doom-Portable"
$PortLauncher = Join-Path $Port "Play KUF.exe"
$Prepared = $false
if ((Want "portable") -or (Want "scenarios") -or (Want "restart") -or (Want "update") -or (Want "move")) {
  Say "Portable edition in '$PortParent' (A1-A3)"
  New-Item -ItemType Directory -Force -Path $PortParent | Out-Null
  Expand-Archive -LiteralPath $Zip -DestinationPath $PortParent
  if ($GameIsFile) { Copy-Item -LiteralPath $GameDir (Join-Path $Port "GameFiles") } else { Copy-Tree $GameDir (Join-Path $Port "GameFiles") }
  $st = Join-Path $Res "logs\self-test-1"; New-Item -ItemType Directory -Force -Path $st | Out-Null
  $r = Invoke-Prog "portable-self-test-1" $PortLauncher @("--self-test", $st) @{} 300
  $text = Get-Content -Raw -Encoding UTF8 (Join-Path $st "self-test.txt")
  Check "A1" $(if ($text -match "source=.* ok") { "pass" } else { "fail" }) ("game files detected: " + (($text -split "`n") -join " | "))
  $t0 = Get-Date
  $r = Invoke-Prog "portable-prepare" $PortLauncher @("--prepare") @{} 5400
  $mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
  $Prepared = $r.code -eq 0
  Check "A2" $(if ($Prepared) { "pass" } else { "fail" }) "preparation exit $($r.code) in $mins min$(if (-not $Prepared) { ': ' + ($r.out -split "`n" | Select-Object -Last 3) -join ' ' })"
  if (-not $Prepared) {
    $bl = Join-Path $Port "Data\build-cache\build.log"
    if (Test-Path -LiteralPath $bl) { Copy-Item -LiteralPath $bl (Join-Path $Res "logs\portable-build.log") }
  }
  $st = Join-Path $Res "logs\self-test-2"; New-Item -ItemType Directory -Force -Path $st | Out-Null
  [void](Invoke-Prog "portable-self-test-2" $PortLauncher @("--self-test", $st) @{} 300)
  $text = Get-Content -Raw -Encoding UTF8 (Join-Path $st "self-test.txt")
  Check "A3" $(if ($text -match "ready=1") { "pass" } else { "fail" }) "the next start needs no preparation: $((($text -split "`n") | Where-Object { $_ -match 'ready' }) -join ' ')"
}

# ---- 3. the game: Settings, saves, gameplay, display modes, Alt+Tab, no audio (B1-B5, E2) ------------------------------
if ((Want "scenarios") -and $Prepared) {
  Say "Game runs ($(@($Plan.scenarios).Count) scenarios; do not touch the keyboard or mouse)"
  $launch = Get-Launch $PortLauncher
  foreach ($k in @("KUF_UPDATER", "KUF_RESTART_FILE", "KUF_LAUNCHER")) { $launch.env.Remove($k) }  # as the Linux runs
  foreach ($sc in $Plan.scenarios) { [void](Invoke-Scenario $sc $launch) }
}

# ---- 4. starting and quitting from Play KUF.exe three times (B6) --------------------------------------------------
if ((Want "restart") -and $Prepared) {
  Say "Start and quit from Play KUF.exe, 3 times (B6)"
  # Test saves in the portable folder's own Data\saves (A4 checks they move with it).
  $fx = @($Plan.scenarios | Where-Object { $_.saves -and ($_.saves.PSObject.Properties.Name -contains "fixture") })[0]
  if ($fx) { Copy-Tree (Join-Path $Here $fx.saves.fixture) (Join-Path $Port "Data\saves") }
  $crashFile = Join-Path $Port "Data\user\crash-report.txt"
  $results = @()
  foreach ($n in 1..3) {
    $lp = [KufAcc]::Start($PortLauncher, @("--play"), $Port, $null, (Join-Path $Res "logs\play-$n.txt"))
    $t0 = Get-Date; $game = @()
    while ($game.Count -eq 0 -and ((Get-Date) - $t0).TotalSeconds -lt 60) { Start-Sleep -Milliseconds 500; $game = Game-Processes $Port }
    if ($game.Count -eq 0) { $results += "start ${n}: no game process"; continue }
    $gp = $game[0]
    $win = [KufAcc]::WaitMainWindow($gp.Id, 60)
    Start-Sleep -Seconds 40
    if ($win -ne [IntPtr]::Zero) { [void][KufAcc]::Screenshot($win, (Join-Path $Res "shots\play-$n.png")); [KufAcc]::Close($win) }
    $ok = $gp.WaitForExit(30000)
    if (-not $ok) { try { $gp.Kill() } catch {} }
    $code = if ($ok) { $gp.ExitCode } else { "hung (killed)" }
    $results += "start ${n}: window $(if ($win -ne [IntPtr]::Zero) {'yes'} else {'no'}), quit by closing the window: exit $code"
    Start-Sleep -Seconds 3
  }
  $bad = @($results | Where-Object { $_ -notmatch "window yes, quit by closing the window: exit 0$" })
  $crash = Test-Path -LiteralPath $crashFile
  Check "B6" $(if ($bad.Count -eq 0 -and -not $crash) { "pass" } else { "fail" }) (($results -join "; ") + $(if ($crash) { "; crash report written" } else { "" }))
  Copy-Item -LiteralPath (Join-Path $Port "Data\logs") (Join-Path $Res "logs\portable-data-logs") -Recurse -ErrorAction SilentlyContinue
}

# ---- 5. an update and the restart into it, then back (M17G; the launcher renames itself) ---------------------------
if ((Want "update") -and $Prepared) {
  Say "Update to a local test release, restart into it, roll back (M17G)"
  $upd = Join-Path $Port "Runtime\bin\kuf-update.exe"
  if (-not $NoNetwork) {
    $r = Invoke-Prog "update-github-check" $upd @("--json", "check") @{} 120
    $last = ($r.out -split "`n" | Where-Object { $_ -match '"exit"' } | Select-Object -Last 1)
    Check "G:github-check" $(if ($r.code -eq 3 -or $r.code -eq 0) { "pass" } else { "fail" }) "Check for Updates against GitHub (WinHTTP, Windows' certificates): exit $($r.code): $last"
  } else { Check "G:github-check" "skip" "-NoNetwork" }
  $signers = Join-Path $Port "Runtime\allowed_signers"
  Copy-Item -LiteralPath $signers "$signers.real"
  Copy-Item -LiteralPath (Join-Path $Kit "update-test\allowed_signers") $signers -Force
  $before = (Get-FileHash -LiteralPath (Join-Path $Port "Runtime\bin\kuf_cod.exe")).Hash
  $v0 = (Get-Content -Raw (Join-Path $Port "Runtime\VERSION")).Trim()
  $r = Invoke-Prog "update-stage" $upd @("--json", "--source-dir", (Join-Path $Kit "update-test\releases"), "update") @{} 900
  $staged = ($r.code -eq 0) -and (Test-Path -LiteralPath (Join-Path $Port "Runtime.next"))
  Check "G:stage" $(if ($staged) { "pass" } else { "fail" }) "download, verify, stage: exit $($r.code): $(($r.out -split "`n" | Select-Object -Last 1))"
  if ($staged) {
    # As the game's Restart: the launcher started with --after-restart <the game's process>, here one that ended.
    $dummy = Start-Process -PassThru -WindowStyle Hidden cmd.exe -ArgumentList "/c exit"; $dummy.WaitForExit()
    $lp = [KufAcc]::Start($PortLauncher, @("--after-restart", "$($dummy.Id)"), $Port, $null, (Join-Path $Res "logs\after-restart.txt"))
    $t0 = Get-Date; $game = @()
    while ($game.Count -eq 0 -and ((Get-Date) - $t0).TotalSeconds -lt 90) { Start-Sleep -Milliseconds 500; $game = Game-Processes $Port }
    $v1 = (Get-Content -Raw (Join-Path $Port "Runtime\VERSION")).Trim()
    $after = (Get-FileHash -LiteralPath (Join-Path $Port "Runtime\bin\kuf_cod.exe")).Hash
    $prevLauncher = Test-Path -LiteralPath (Join-Path $Port "Play KUF.exe.previous")
    $launchLog = Read-Shared (Join-Path $Port "Data\logs\launcher.log")
    if ($game.Count) {
      $win = [KufAcc]::WaitMainWindow($game[0].Id, 60); Start-Sleep -Seconds 20
      if ($win -ne [IntPtr]::Zero) { [KufAcc]::Close($win) }; [void]$game[0].WaitForExit(30000)
    }
    $ok = $game.Count -gt 0 -and $v1 -ne $v0 -and $after -eq $before -and $prevLauncher
    Check "G:restart" $(if ($ok) { "pass" } else { "fail" }) "version $v0 -> $v1; game started: $($game.Count -gt 0); prepared game carried over: $($after -eq $before); old launcher kept as .previous: $prevLauncher; launcher.log: $((($launchLog -split "`n") | Where-Object { $_ -match '^update' }) -join ' ')"
    $r = Invoke-Prog "update-rollback" $PortLauncher @("--rollback-update") @{ KUF_NO_DIALOG = "1" } 300
    $v2 = (Get-Content -Raw (Join-Path $Port "Runtime\VERSION")).Trim()
    Check "G:rollback" $(if ($r.code -eq 0 -and $v2 -eq $v0) { "pass" } else { "fail" }) "Play KUF.exe --rollback-update: exit $($r.code), now ${v2}: $($r.out.Trim())"
  }
  Copy-Item -LiteralPath "$signers.real" $signers -Force
}

# ---- 6. moving the folder (A4) ---------------------------------------------------------------------------------------
if ((Want "move") -and $Prepared) {
  Say "Moving the portable folder (A4)"
  $savesBefore = Hash-Tree (Join-Path $Port "Data\saves")
  $MovedParent = Join-Path $Work "Перенос Ü"
  New-Item -ItemType Directory -Force -Path $MovedParent | Out-Null
  $Moved = Join-Path $MovedParent "KUF moved"
  Move-Item -LiteralPath $Port $Moved
  $st = Join-Path $Res "logs\self-test-moved"; New-Item -ItemType Directory -Force -Path $st | Out-Null
  [void](Invoke-Prog "portable-self-test-moved" (Join-Path $Moved "Play KUF.exe") @("--self-test", $st) @{} 300)
  $text = Get-Content -Raw -Encoding UTF8 (Join-Path $st "self-test.txt")
  $savesAfter = Hash-Tree (Join-Path $Moved "Data\saves")
  $ok = ($text -match "ready=1") -and ($savesAfter -eq $savesBefore)
  Check "A4" $(if ($ok) { "pass" } else { "fail" }) "after the move: $((($text -split "`n") | Where-Object { $_ -match 'ready' }) -join ' '); saves the same: $($savesAfter -eq $savesBefore)"
  $short = @($Plan.scenarios | Where-Object { $_.name -eq "quit_popup" })[0]
  if ($short) { [void](Invoke-Scenario $short (Get-Launch (Join-Path $Moved "Play KUF.exe")) "win-moved-quit_popup") }
  $Port = $Moved; $PortLauncher = Join-Path $Moved "Play KUF.exe"
}

# ---- 7. drag and drop onto the launcher and the setup (A5, D2; D1) --------------------------------------------------
function Test-Drop([string]$id, [string]$exe, [string]$logFile, [string]$dropped, [string]$want, [hashtable]$envs) {
  foreach ($k in $envs.Keys) { Set-Item -LiteralPath "env:$k" -Value $envs[$k] }
  try { $p = Start-Process -FilePath $exe -PassThru } finally { foreach ($k in $envs.Keys) { Remove-Item -LiteralPath "env:$k" -ErrorAction SilentlyContinue } }
  $win = [KufAcc]::WaitMainWindow($p.Id, 30)
  if ($win -eq [IntPtr]::Zero) { Check $id "fail" "no window within 30 s"; try { $p.Kill() } catch {}; return $null }
  $elev = [KufAcc]::Elevated($p.Id)
  [void][KufAcc]::Focus($win); Start-Sleep -Seconds 2
  $r = [KufAcc]::Rect($win)
  $before = (Read-Shared $logFile)
  $effect = [KufAcc]::DragFiles(@($dropped), [int](($r[0] + $r[2]) / 2), [int](($r[1] + $r[3]) / 2))
  Start-Sleep -Seconds 4
  $new = (Read-Shared $logFile).Substring($before.Length)
  [void][KufAcc]::Screenshot($win, (Join-Path $Res "shots\$($id -replace '[:\\/]', '-').png"))
  $line = (($new -split "`n") | Where-Object { $_.StartsWith("drop: ") } | Select-Object -Last 1)
  $ok = $line -and $line.Contains($dropped) -and $line.Contains($want)
  Check $id $(if ($ok) { "pass" } else { "fail" }) "drag result $effect; launcher.log: $(if ($line) { $line } else { 'no drop line' }) (a drag by the test program, not Explorer: drag once yourself to be sure)"
  [KufAcc]::Close($win); [void]$p.WaitForExit(15000); if (-not $p.HasExited) { try { $p.Kill() } catch {} }
  return $elev
}

if (Want "drop") {
  Say "Drag and drop (A5, D2) and the setup without an administrator (D1)"
  $dp = Join-Path $Work "Drop Ü"
  New-Item -ItemType Directory -Force -Path $dp | Out-Null
  Expand-Archive -LiteralPath $Zip -DestinationPath $dp
  $dPort = Join-Path $dp "KUF-Circle-of-Doom-Portable"
  [void](Test-Drop "A5" (Join-Path $dPort "Play KUF.exe") (Join-Path $dPort "Data\logs\launcher.log") $GameDir "(usable), portable" @{})
  $copied = @(Get-ChildItem -LiteralPath (Join-Path $dPort "GameFiles") -Recurse -File | Where-Object { $_.Name -notlike "*.txt" }).Count
  $gameData = Test-Path -LiteralPath (Join-Path $dPort "Data\game")
  Check "A5:nothing-copied" $(if ($copied -eq 0 -and -not $gameData) { "pass" } else { "fail" }) "after the drop: $copied files in GameFiles, Data\game exists: $gameData (nothing until Play)"
  $sd = Join-Path $Work "setup-data-drop"
  $defaultDest = Join-Path ([Environment]::GetFolderPath("LocalApplicationData")) "Programs\$GameName"
  $destBefore = Test-Path -LiteralPath $defaultDest
  $elev = Test-Drop "D2" $SetupExe (Join-Path $sd "logs\launcher.log") $GameDir "(usable), setup" @{ KUF_SETUP_DATA_DIR = $sd }
  if (-not $GameIsFile) {
    $xex = Join-Path $GameDir "default.xex"
    if (Test-Path -LiteralPath $xex) { [void](Test-Drop "D2:default.xex" $SetupExe (Join-Path $sd "logs\launcher.log") $xex "setup" @{ KUF_SETUP_DATA_DIR = $sd }) }
  }
  Check "D2:nothing-installed" $(if ((Test-Path -LiteralPath $defaultDest) -eq $destBefore) { "pass" } else { "fail" }) "the default destination unchanged by the drops (nothing before Install)"
  $elevText = if ($null -eq $elev) { "no window (an administrator prompt, or it did not start)" } elseif ($elev -eq 0) { "no" } elseif ($elev -eq 1) { "yes" } else { "unknown" }
  Check "D1" $(if ($null -ne $elev -and $elev -eq 0) { "pass" } else { "fail" }) "the setup started like a double-click (ShellExecute): window shown, administrator: $elevText"
}

# ---- 8. the setup: install, play, shortcuts, Settings > Apps, repair, uninstall (D3-D6) ---------------------------------
if (Want "setup") {
  Say "The setup: install, play, repair, uninstall (D3-D6)"
  $sd = Join-Path $Work "setup-data"
  $dest = Join-Path $Work "Setup Prüfung\KUF"
  $envs = @{ KUF_SETUP_DATA_DIR = $sd; KUF_NO_DIALOG = "1" }
  $sargs = @("--install-to", $dest)
  if ($RealInstall) { $sargs += "--no-shortcuts" }
  $sargs += $GameDir
  $t0 = Get-Date
  $r = Invoke-Prog "setup-install" $SetupExe $sargs $envs 5400 $Work
  $mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
  $installed = $r.code -eq 0 -and (Test-Path -LiteralPath (Join-Path $dest "Play KUF.exe"))
  Check "D3:install" $(if ($installed) { "pass" } else { "fail" }) "install exit $($r.code) in $mins min: $(($r.out.Trim() -split "`n" | Select-Object -Last 1))"
  if ($installed) {
    $short = @($Plan.scenarios | Where-Object { $_.name -eq "quit_popup" })[0]
    if ($short) {
      $ok = Invoke-Scenario $short (Get-Launch (Join-Path $dest "Play KUF.exe") $envs) "win-setup-quit_popup"
      Check "D3:play" $(if ($ok) { "verify" } else { "fail" }) "the installed game runs (win-setup-quit_popup)"
    }
    if (-not $RealInstall) {
      $sh = New-Object -ComObject WScript.Shell
      $links = @((Join-Path $StartMenu "$GameName.lnk"), (Join-Path $StartMenu "Uninstall $GameName.lnk"))
      $found = @($links | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object { $l = $sh.CreateShortcut($_); "$([IO.Path]::GetFileName($_)) -> $($l.TargetPath) $($l.Arguments)" })
      $targetsOk = @($found | Where-Object { $_ -like "*-> $dest\Play KUF.exe*" }).Count -eq 2
      Check "D4:shortcuts" $(if ($targetsOk) { "pass" } else { "fail" }) ("Start menu: " + ($found -join "; "))
      $app = Get-ItemProperty -Path $AppsKey -ErrorAction SilentlyContinue
      $appOk = $app -and $app.UninstallString -like "*$dest\Play KUF.exe*--uninstall*"
      Check "D6:apps-entry" $(if ($appOk) { "pass" } else { "fail" }) $(if ($app) { "Settings > Apps: $($app.DisplayName) $($app.DisplayVersion), $([math]::Round($app.EstimatedSize / 1024)) MB, uninstall: $($app.UninstallString)" } else { "no Settings > Apps entry" })
      # The Start menu entry starts the launcher.
      $p = Start-Process -FilePath $links[0] -PassThru -ErrorAction SilentlyContinue
      Start-Sleep -Seconds 8
      $lw = @(Get-Process -Name "Play KUF" -ErrorAction SilentlyContinue | Where-Object { try { $_.Path -eq (Join-Path $dest "Play KUF.exe") } catch { $false } })
      if ($lw.Count) { $w = [KufAcc]::MainWindow($lw[0].Id); if ($w -ne [IntPtr]::Zero) { [void][KufAcc]::Screenshot($w, (Join-Path $Res "shots\D4-start-menu.png")); [KufAcc]::Close($w) }; [void]$lw[0].WaitForExit(15000) }
      Check "D4:start-menu" $(if ($lw.Count) { "pass" } else { "fail" }) "the Start menu entry opened the installed launcher: $($lw.Count -gt 0) (Play from there: by hand)"
    } else {
      Check "D4:shortcuts" "skip" "a real installation exists on this PC: the test installs without shortcuts and Settings > Apps entry"
      Check "D6:apps-entry" "skip" "a real installation exists on this PC"
    }
    # Repair: the setup over the installation; a save kept.
    New-Item -ItemType Directory -Force -Path (Join-Path $sd "saves") | Out-Null
    [IO.File]::WriteAllText((Join-Path $sd "saves\acceptance-marker.txt"), "keep me")
    $exeHash = (Get-FileHash -LiteralPath (Join-Path $dest "Runtime\bin\kuf_cod.exe") -ErrorAction SilentlyContinue).Hash
    $t0 = Get-Date
    $r = Invoke-Prog "setup-repair" $SetupExe $sargs $envs 5400 $Work
    $mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
    $kept = (Test-Path -LiteralPath (Join-Path $sd "saves\acceptance-marker.txt"))
    Check "D5" $(if ($r.code -eq 0 -and $kept) { "pass" } else { "fail" }) "repair exit $($r.code) in $mins min, save kept: ${kept}: $(($r.out.Trim() -split "`n" | Select-Object -Last 1))"
    # Uninstall, keeping the saves (as "Remove (keep saves)" does: a copy of the launcher runs from the temporary folder).
    $tmp = Join-Path $Work "uninstall-copy"; New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Copy-Item -LiteralPath (Join-Path $dest "Play KUF.exe") $tmp
    $r = Invoke-Prog "setup-uninstall" (Join-Path $tmp "Play KUF.exe") @("--uninstall-from", $dest) $envs 300
    $gone = -not (Test-Path -LiteralPath $dest)
    $kept = Test-Path -LiteralPath (Join-Path $sd "saves\acceptance-marker.txt")
    $linksGone = -not ((Test-Path -LiteralPath (Join-Path $StartMenu "$GameName.lnk")) -or (Test-Path -LiteralPath (Join-Path $StartMenu "Uninstall $GameName.lnk")))
    $appGone = -not (Test-Path $AppsKey)
    if ($RealInstall) { $linksGone = $true; $appGone = $true }
    Check "D6" $(if ($r.code -eq 0 -and $gone -and $kept -and $linksGone -and $appGone) { "pass" } else { "fail" }) "uninstall exit $($r.code): folder removed $gone, saves kept $kept, shortcuts removed $linksGone, Settings > Apps entry removed ${appGone}: $($r.out.Trim())"
  }
  $after = Hash-Tree (Join-Path $RealData "data\saves")
  Check "real-saves" $(if ($after -eq $RealDataHash) { "pass" } else { "fail" }) "the saves of a real installation (%LOCALAPPDATA%\kuf-cod-recomp\data\saves) unchanged by the test"
}

# ---- 9. the controller (C1-C5; a person plugs and presses) -----------------------------------------------------------
function Ask([string]$q) { if ($Unattended) { return "not asked" }; return (Read-Host "$q [y/n]") }
if ($Controller -and $Prepared -and (Want "controller")) {
  Say "The controller (C1-C5): follow the instructions"
  Read-Host "Plug in the controller, then press Enter"
  $t0 = Get-Date; $seen = $false
  Write-Host "Press A on the controller (20 s)" -ForegroundColor Yellow
  while (((Get-Date) - $t0).TotalSeconds -lt 20 -and -not $seen) { foreach ($b in [KufAcc]::Pads()) { if ($b -ge 0 -and ($b -band 0x1000)) { $seen = $true } }; Start-Sleep -Milliseconds 50 }
  Check "C:xinput" $(if ($seen) { "pass" } else { "fail" }) "Windows sees the controller and its A button (XInput): $seen"
  $launch = Get-Launch $PortLauncher
  $run = Join-Path $RunsDir "win-controller"
  New-Item -ItemType Directory -Force -Path (Join-Path $run "user"), (Join-Path $run "saves"), (Join-Path $run "cache") | Out-Null
  $fx = @($Plan.scenarios | Where-Object { $_.saves -and ($_.saves.PSObject.Properties.Name -contains "fixture") })[0]
  if ($fx) { Copy-Tree (Join-Path $Here $fx.saves.fixture) (Join-Path $run "saves") }
  $skip = "^--(user_data_root|content_root|cache_root|log_file|log_max_file_size_mb|log_max_files)="
  $argv = @($launch.args | Where-Object { $_ -notmatch $skip }) + @("--user_data_root=$run\user", "--content_root=$run\saves",
    "--cache_root=$run\cache", "--log_file=$run\kuf_cod.log", "--kuf_exit_after_seconds=420", "--kuf_display_size=1280x720", "--fullscreen=false")
  $proc = [KufAcc]::Start($launch.exe, $argv, (Split-Path -Parent $launch.exe), $null, (Join-Path $run "stdout.txt"))
  $steps = @(
    @{ t = 0; say = "The game starts in a window. Use ONLY the controller now: in the menus A, B, the sticks and the D-pad; load your test save (CONTINUE) and play: move, attack, the triggers, bumpers, X, Y, Start and Back. 120 s." },
    @{ t = 120; say = "Unplug the controller now (in gameplay). 10 s." },
    @{ t = 130; say = "Plug it in again and keep playing with it (does it work again without restarting?). 40 s." },
    @{ t = 170; say = "Now use the keyboard and mouse in a menu (Escape opens the menu), then the controller again: do the button pictures follow the device you used? 60 s." },
    @{ t = 230; say = "Get into a fight: does the controller vibrate? 60 s. Then close the game window." })
  $start = Get-Date
  foreach ($s in $steps) {
    while (((Get-Date) - $start).TotalSeconds -lt $s.t -and -not $proc.HasExited) { Start-Sleep -Milliseconds 500 }
    if ($proc.HasExited) { break }
    Write-Host ""; Write-Host ">> $($s.say)" -ForegroundColor Yellow
    [Console]::Beep(880, 200)
  }
  [void](Wait-Exit $proc 400)
  $log = Read-Shared (Join-Path $run "kuf_cod.log")
  $added = ([regex]::Matches($log, "SDL OnControllerDeviceAdded")).Count
  $removed = ([regex]::Matches($log, "SDL OnControllerDeviceRemoved")).Count
  Check "C2" $(if ($added -ge 2 -and $removed -ge 1) { "pass" } else { "fail" }) "the game saw the controller connected $added time(s), disconnected $removed time(s)"
  Check "C1" "manual" "menus and gameplay with the controller: $(Ask 'Did everything on the controller work in the menus and in gameplay?')"
  Check "C2:works-again" "manual" "after plugging in again: $(Ask 'Did the controller work again after plugging it in (no restart)?')"
  Check "C4" "manual" "vibration: $(Ask 'Did it vibrate in the fight?')"
  Check "C5" "manual" "prompts follow the device: $(Ask 'Did the button pictures follow the device you used?')"
  Check "C3" "manual" "plugged in only after the start: not run here (start the game without it, plug it in at the title, check it works)"
} elseif (Want "controller") {
  Check "C1-C5" "skip" "not asked for (-Controller)"
}

# ---- 10. the native build (E1) --------------------------------------------------------------------------------------
if ($Build -and (Want "build")) {
  Say "The native Windows build (E1)"
  $gitExe = (Get-Command git -ErrorAction SilentlyContinue)
  if (-not $gitExe) { Check "E1" "fail" "git not found (Git for Windows)" }
  else {
    $bash = Join-Path (Split-Path -Parent (Split-Path -Parent $gitExe.Source)) "bin\bash.exe"
    $src = Join-Path $Work "build Ü\kuf-cod-recomp"
    & git clone --quiet (Join-Path $Kit "source\kuf-cod-recomp") $src 2>&1 | Out-File -Encoding UTF8 (Join-Path $Res "logs\build-clone.txt")
    $xex = Join-Path $Port "Data\game\default.xex"
    $t0 = Get-Date
    $r = Invoke-Prog "build" $bash @("-lc", "cd `"$($src -replace '\\', '/')`" && tools/windows/build.sh --xex `"$($xex -replace '\\', '/')`"") @{} 7200 $src
    $mins = [math]::Round(((Get-Date) - $t0).TotalMinutes, 1)
    Check "E1" $(if ($r.code -eq 0 -and $r.out -match "BUILD DONE") { "pass" } else { "fail" }) "tools/windows/build.sh in a fresh clone: exit $($r.code) in $mins min: $(($r.out.Trim() -split "`n" | Select-Object -Last 1))"
  }
} elseif (Want "build") {
  Check "E1" "skip" "not asked for (-Build)"
}

Check "E3" "skip" "co-op between two PCs: needs the Linux PC running at the same time (docs/MULTIPLAYER.md); the local lobby is net:lobby-local"

# ---- the end ------------------------------------------------------------------------------------------------------
Say "Done"
$counts = $Checks | Group-Object { $_.status } | ForEach-Object { "$($_.Count) $($_.Name)" }
$summary = @("KUF Windows acceptance $Stamp", ($counts -join ", "), "") + ($Checks | ForEach-Object { "[{0}] {1}: {2}" -f $_.status, $_.id, $_.detail })
[IO.File]::WriteAllLines((Join-Path $Res "summary.txt"), [string[]]$summary)
Stop-Transcript | Out-Null
$zipOut = Join-Path $Kit "results-$Stamp.zip"
Compress-Archive -Path (Join-Path $Res "*") -DestinationPath $zipOut
Write-Host ($counts -join ", ")
Write-Host "Results: $zipOut  (bring it to the Linux PC). The test folders are in $Work (delete them when done)." -ForegroundColor Green
