# Run a TEST-ONLY packaged Builder on a disposable Windows player VM.
# No downloads, software installation, elevation or network changes. For an
# offline acceptance run, disconnect the VM network first (loopback still works).
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Builder,
    [string]$Rom,
    [string]$ResultsParent = (Get-Location).Path
)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This test must run on Windows.' }
foreach ($tool in @('go', 'cmake', 'zig', 'gcc', 'clang', 'cl')) {
    if (Get-Command $tool -ErrorAction SilentlyContinue) {
        throw "Clean-player test requires no system build tools; found $tool."
    }
}
$source = (Resolve-Path -LiteralPath $Builder).Path
if ($Rom) { $Rom = (Resolve-Path -LiteralPath $Rom).Path }
$root = Join-Path (Resolve-Path -LiteralPath $ResultsParent).Path ('builder-player-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
Write-Host "Retaining results and any private game data: $root"
$portable = Join-Path $root 'Portable Builder'
$unrelated = Join-Path $root 'Unrelated CWD'
New-Item -ItemType Directory -Path $portable, $unrelated | Out-Null
$exe = Join-Path $portable 'Renamed Builder.exe'
Copy-Item -LiteralPath $source -Destination $exe
$originalHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
Set-Content -LiteralPath "$exe.portable" -Value 'Builder Data' -Encoding ASCII
$savedEnv = @{}
foreach ($key in @('LOCALAPPDATA', 'AR_BUILDER_SMOKE_ROM', 'AR_USER_DATA_DIR')) {
    $savedEnv[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
function Invoke-Probe([string]$Label, [string]$Executable, [string]$Workspace) {
    $stdout = Join-Path $root "$Label.log"
    $stderr = Join-Path $root "$Label.stderr.log"
    # GUI-subsystem executables can still write into explicitly inherited pipes.
    $process = Start-Process -FilePath $Executable -ArgumentList '--jobs', '1' `
        -WorkingDirectory $unrelated -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -PassThru
    $deadline = (Get-Date).AddMinutes(10)
    if ($env:AR_BUILDER_SMOKE_ROM) { $deadline = (Get-Date).AddMinutes(30) }
    $runtimeSeen = $false
    $expectedRuntime = [IO.Path]::GetFullPath("$Workspace-runtime") + '\'
    $expectedProfile = "$Workspace-webview"
    if ($Workspace -eq (Join-Path $env:LOCALAPPDATA 'ActRaiserRecomp/installer/workspace')) {
        $expectedRuntime = [IO.Path]::GetFullPath((Join-Path (Split-Path $Workspace) 'runtime')) + '\'
        $expectedProfile = Join-Path (Split-Path $Workspace) 'webview'
    }
    try {
        while (-not $process.WaitForExit(250)) {
            # Confirm the renderer came from OUR extracted Fixed Runtime, not
            # an installed Evergreen runtime. Do not log process command lines.
            foreach ($renderer in @(Get-CimInstance Win32_Process -Filter "Name='msedgewebview2.exe'")) {
                if ($renderer.ExecutablePath -and $renderer.ExecutablePath.StartsWith($expectedRuntime, [StringComparison]::OrdinalIgnoreCase)) {
                    $runtimeSeen = $true
                }
            }
            if ((Get-Date) -gt $deadline) { throw "$Label timed out; inspect $stdout and $stderr" }
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$Label exited $($process.ExitCode); inspect $stderr" }
        $log = Get-Content -LiteralPath $stdout -Raw
        if ($log -notmatch '(?m)^BUILDER_RENDERER_SMOKE PASS ' -or $log -match '(?m)^BUILDER_RENDERER_SMOKE FAIL ') {
            throw "$Label did not explicitly PASS; use a smoketest package and inspect $stdout"
        }
        if ($env:AR_BUILDER_SMOKE_ROM -and $log -notmatch 'PASS .*full game build') { throw 'Full build did not PASS.' }
        if (-not $runtimeSeen) { throw 'Did not observe the bundled WebView2 process; runtime selection is unverified.' }
        if (-not (Test-Path -LiteralPath (Join-Path $Workspace 'utils/tools/snesbuild.exe'))) { throw 'Wrong workspace or missing tools.' }
        if (-not (Test-Path -LiteralPath $expectedProfile)) { throw 'Expected separate WebView2 profile was not created.' }
        if (-not (Test-Path -LiteralPath (Join-Path (Split-Path $Executable) 'ActRaiserRecomp/game-assets'))) { throw 'Game output is not beside the Builder.' }
        Write-Host "PASS: $Label renderer, bundled runtime and workspace"
    } finally {
        if (-not $process.HasExited) {
            # Only this test's process and descendants, never a name-wide kill.
            & "$env:SystemRoot\System32\taskkill.exe" /T /F /PID $process.Id | Out-Null
        }
        $process.Dispose()
    }
}
try {
    $env:LOCALAPPDATA = Join-Path $root 'Global App Data'
    $env:AR_BUILDER_SMOKE_ROM = $Rom
    [Environment]::SetEnvironmentVariable('AR_USER_DATA_DIR', $null, 'Process')
    $work = Join-Path $portable 'Builder Data'
    Invoke-Probe 'portable' $exe $work
    $global = Join-Path $env:LOCALAPPDATA 'ActRaiserRecomp/installer/workspace'
    if (Test-Path -LiteralPath $global) { throw 'Portable launch unexpectedly created a global workspace.' }
    Set-Content -LiteralPath (Join-Path $work 'user-edit.txt') -Value 'preserve me' -Encoding ASCII
    [Environment]::SetEnvironmentVariable('AR_BUILDER_SMOKE_ROM', $null, 'Process')
    $relocated = Join-Path $root 'Relocated Builder'
    Move-Item -LiteralPath $portable -Destination $relocated
    $exe = Join-Path $relocated 'Renamed Builder.exe'
    $work = Join-Path $relocated 'Builder Data'
    Invoke-Probe 'relocated' $exe $work
    if ((Get-Content -LiteralPath (Join-Path $work 'user-edit.txt') -Raw).Trim() -ne 'preserve me') { throw 'Workspace edit was lost.' }
    if (Test-Path -LiteralPath $global) { throw 'Relocated launch unexpectedly used global storage.' }
    Move-Item -LiteralPath "$exe.portable" -Destination "$exe.portable.saved"
    Invoke-Probe 'global' $exe $global
    if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne $originalHash) { throw 'Builder modified its own executable.' }
    Write-Host 'PASS: portable, relocation/edit preservation, global, Fixed WebView2, unchanged executable'
    Write-Host 'Still requires human checks: visual layout, dialogs, audio, console flashes, and generated game launch.'
} finally {
    foreach ($key in $savedEnv.Keys) { [Environment]::SetEnvironmentVariable($key, $savedEnv[$key], 'Process') }
}
