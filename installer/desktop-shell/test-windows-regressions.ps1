# Runs precompiled native tests; no Go, C compiler, ROM, installation or CI.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$TestsDirectory,
    [ValidateSet('amd64', 'arm64')][string]$Architecture = 'arm64',
    [string]$OtherDriveRoot
)
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This test must run on Windows.' }
$tests = (Resolve-Path -LiteralPath $TestsDirectory).Path
$oldOther = $env:AR_WINDOWS_TEST_OTHER_DRIVE
$patterns = @{
    desktop = '^(TestPathWithin|TestWindowsContainmentAcrossVolumes|TestWindowsOtherDriveOutputAndImport|TestLocalDataPathsAcceptNativeSeparatorsAndRejectEscapes|TestNativeDataPathsSeedDefaultsAssetsAndArchiveHelper|TestNestedPortableMarkerUsesNativeContainmentAndPortableContents)$'
    builder = '^(TestStoreROMReplacesExistingCopy|TestStoreROMLockedDestinationPreservesPreviousCopy)$'
    launcher = '^(TestGameStartupFailureHasShareableLog|TestSnesbuildCancellationAfterStdoutCloses)$'
    host = '^(TestWindowsJobReapsCompilersButPreservesDetachedGame|TestBackendShutdownWaitsForOwnedHelperCleanup|TestBackendStartupCancellationReapsProcess|TestBackendAlreadyCancelledDoesNotStart|TestWorkspaceLock)$'
}
try {
    [Environment]::SetEnvironmentVariable('AR_WINDOWS_TEST_OTHER_DRIVE', $null, 'Process')
    if ($OtherDriveRoot) { $env:AR_WINDOWS_TEST_OTHER_DRIVE = (Resolve-Path -LiteralPath $OtherDriveRoot).Path }
    foreach ($package in @('desktop', 'builder', 'launcher', 'host')) {
        $test = Join-Path $tests "actraiser-$package-windows-hardening-$Architecture.test.exe"
        if (-not (Test-Path -LiteralPath $test)) { throw "Missing test executable: $test" }
        # These fixtures are self-contained. Other package tests intentionally
        # read repository testdata and are not portable test executables.
        & $test -test.v -test.timeout=3m ('-test.run=' + $patterns[$package])
        if ($LASTEXITCODE -ne 0) { throw "$package regression tests failed ($LASTEXITCODE)." }
    }
    $fsTest = Join-Path $tests "actraiser-utf8-fs-$Architecture.exe"
    if (-not (Test-Path -LiteralPath $fsTest)) { throw "Missing test executable: $fsTest" }
    & $fsTest
    if ($LASTEXITCODE -ne 0) { throw "UTF-8 filesystem tests failed ($LASTEXITCODE)." }
    Write-Host 'PASS: native Windows Builder hardening regressions'
    if (-not $OtherDriveRoot) { Write-Host 'SKIP: real cross-drive filesystem test (supply -OtherDriveRoot); lexical drive tests still ran.' }
} finally {
    [Environment]::SetEnvironmentVariable('AR_WINDOWS_TEST_OTHER_DRIVE', $oldOther, 'Process')
}
