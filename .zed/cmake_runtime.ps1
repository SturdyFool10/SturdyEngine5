param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("debug", "relwithdebinfo", "dist")]
    [string]$Profile
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$profiles = @{
    debug = @{
        Configure = "ninja-debug"
        Build = "runtime-debug"
        Runtime = "build/ninja/debug/bin/Runtime.exe"
    }
    relwithdebinfo = @{
        Configure = "ninja-relwithdebinfo"
        Build = "runtime-relwithdebinfo"
        Runtime = "build/ninja/relwithdebinfo/bin/Runtime.exe"
    }
    dist = @{
        Configure = "ninja-dist"
        Build = "runtime-dist"
        Runtime = "build/ninja/dist/bin/Runtime.exe"
    }
}

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "cmake was not found in PATH. Install CMake or launch the editor from a shell where cmake is available."
}

$settings = $profiles[$Profile]

Push-Location $root
try {
    Invoke-Checked "cmake" @("--preset", $settings.Configure)
    Invoke-Checked "cmake" @("--build", "--preset", $settings.Build, "--parallel")

    $runtimePath = Join-Path $root $settings.Runtime
    if (-not (Test-Path -LiteralPath $runtimePath)) {
        Write-Error "Runtime executable not found: $runtimePath"
    }

    & $runtimePath
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
finally {
    Pop-Location
}
