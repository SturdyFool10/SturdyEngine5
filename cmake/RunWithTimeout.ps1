param(
    [int]$TimeoutSeconds
)

if ($TimeoutSeconds -lt 1 -or $TimeoutSeconds -gt 3600) {
    Write-Error "RunWithTimeout.ps1 requires TimeoutSeconds between 1 and 3600."
    exit 2
}

$Command = $args
if ($Command.Count -eq 0) {
    Write-Error "RunWithTimeout.ps1 requires a command to run."
    exit 2
}

$commandArguments = @()
if ($Command.Count -gt 1) {
    $commandArguments = $Command[1..($Command.Count - 1)]
}

$commandName = Split-Path -Leaf $Command[0]
Write-Host "SturdyEngine archive step '${commandName}' (timeout ${TimeoutSeconds}s)"
$process = Start-Process -FilePath $Command[0] -ArgumentList $commandArguments -PassThru -NoNewWindow
if ($process.WaitForExit($TimeoutSeconds * 1000)) {
    exit $process.ExitCode
}

& "$env:SystemRoot\System32\taskkill.exe" /PID $process.Id /T /F | Out-Null
if (-not $process.WaitForExit(5000)) {
    Write-Error "SturdyEngine archive step exceeded the ${TimeoutSeconds}-second timeout and could not be terminated. The build will stop; inspect the remaining process and locks or antivirus activity on the build directory."
    exit 124
}

Write-Error "SturdyEngine archive step '${commandName}' exceeded the ${TimeoutSeconds}-second timeout and was terminated. Check for file locks or antivirus activity on the build directory."
exit 124
