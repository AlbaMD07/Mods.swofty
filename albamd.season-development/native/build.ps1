# Builds native/season_development.dll with zig c++, the same command as Swofty's addon template (build.sh).
# The first run downloads Swofty's MIT addon SDK headers into native/include, pinned to one commit of
# Swofty-Developments/SwoftyLauncher. Set $env:ZIG to zig.exe, or put zig on PATH.
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$SdkCommit = '2d66119fa4aacb9b9122b503feffc22e5bc734aa'

if (-not (Test-Path include\swofty_api.h)) {
    $headers = @{ 'User-Agent' = 'albamd-mods-build' }
    $tree = Invoke-RestMethod -Headers $headers "https://api.github.com/repos/Swofty-Developments/SwoftyLauncher/git/trees/$($SdkCommit)?recursive=1"
    $prefix = 'templates/addon/native/include/'
    $files = $tree.tree | Where-Object { $_.type -eq 'blob' -and $_.path.StartsWith($prefix) }
    foreach ($f in $files) {
        $dest = Join-Path include ($f.path.Substring($prefix.Length))
        New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
        Invoke-WebRequest -UseBasicParsing -Headers $headers -OutFile $dest `
            "https://raw.githubusercontent.com/Swofty-Developments/SwoftyLauncher/$SdkCommit/$($f.path)"
    }
    Invoke-WebRequest -UseBasicParsing -Headers $headers -OutFile include\LICENSE-swofty-sdk `
        "https://raw.githubusercontent.com/Swofty-Developments/SwoftyLauncher/$SdkCommit/templates/addon/LICENSE"
    Write-Host "fetched $($files.Count) Swofty SDK headers at $SdkCommit"
}

$zig = if ($env:ZIG) { $env:ZIG } elseif (Get-Command zig -ErrorAction SilentlyContinue) { 'zig' } else { throw 'zig not found: put zig on PATH or set $env:ZIG to zig.exe' }
$env:ZIG_GLOBAL_CACHE_DIR = "$PSScriptRoot\build\zig-cache"
$env:ZIG_LOCAL_CACHE_DIR = "$PSScriptRoot\build\zig-cache"
New-Item -ItemType Directory -Force build | Out-Null
& $zig c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Iinclude -shared -s -o season_development.dll src/season_development.cpp
if ($LASTEXITCODE -ne 0) { throw "zig c++ failed ($LASTEXITCODE)" }
Remove-Item -ErrorAction SilentlyContinue season_development.lib, season_development.pdb
Write-Host "built $PSScriptRoot\season_development.dll"
