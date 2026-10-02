#requires -Version 5.1
<#
luna installer — Windows, one line (PowerShell):

  irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex

Installs the latest nightly build for x86_64 Windows into
%LOCALAPPDATA%\Programs\luna and adds it to the user PATH.
Re-running is safe: it upgrades in place.

Options — via parameters when saved and run as a file
  .\install.ps1 -Token ghp_xxx
or via environment variables when piped (param binding does not apply
through irm | iex):

  $env:GITHUB_TOKEN = "ghp_xxx"; irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex

  $env:LUNA_MIRROR  = "https://.../artifact.zip"   # anonymous direct zip
  $env:LUNA_DIR     = "D:\tools\luna"              # install directory

Artifacts need auth to download from GitHub Actions (anonymous API
calls get 403), hence the token/mirror escapes above. A logged-in gh
CLI is auto-detected as a last resort before going anonymous.
#>
param(
    [string]$Token,
    [string]$Mirror,
    [string]$Dir,
    [string]$Repo = "cuihairu/luna"
)

$ErrorActionPreference = "Stop"

function Die([string]$Message) {
    [Console]::Error.WriteLine("install.ps1: $Message")
    exit 1
}

if ($env:OS -ne "Windows_NT") {
    Die "this installer is for Windows (use install.sh on Linux/macOS)"
}

# ---- platform detection -------------------------------------------------

$archMap = @{ AMD64 = "x86_64"; ARM64 = "aarch64" }
$arch = $archMap[$env:PROCESSOR_ARCHITECTURE]
if (-not $arch) {
    Die "unsupported architecture: $env:PROCESSOR_ARCHITECTURE (supported: AMD64, ARM64; nightly matrix: windows-x86_64 first)"
}

$platform = "luna-nightly-windows-$arch"

if (-not $Mirror) { $Mirror = $env:LUNA_MIRROR }
if (-not $Dir)    { $Dir    = $env:LUNA_DIR }
if (-not $Dir)    { $Dir    = Join-Path $env:LOCALAPPDATA "Programs\luna" }
$exe = Join-Path $Dir "luna.exe"

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("luna-install-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work | Out-Null
try {

    # ---- obtain the artifact zip ----------------------------------------

    if ($Mirror) {
        Write-Host "install.ps1: downloading $platform from mirror"
        Invoke-WebRequest -Uri $Mirror -OutFile (Join-Path $work "artifact.zip")
    }
    else {
        if (-not $Token) { $Token = $env:GITHUB_TOKEN }
        if (-not $Token) { $Token = $env:GH_TOKEN }
        if (-not $Token) {
            $gh = Get-Command gh -ErrorAction SilentlyContinue
            if ($gh) {
                try { $Token = (& gh auth token) 2>$null } catch { }
                if ($Token) { Write-Host "install.ps1: using token from gh CLI" }
            }
        }
        if (-not $Token) {
            Die "nightly artifacts need a GitHub token to download.
  pass -Token, or set GITHUB_TOKEN / GH_TOKEN, or log in once with 'gh auth login' (auto-detected),
  or point LUNA_MIRROR at an anonymously reachable zip URL"
        }

        Write-Host "install.ps1: looking up latest $platform"
        $headers = @{ Authorization = "Bearer $Token"; Accept = "application/vnd.github+json" }
        $list = Invoke-RestMethod -Headers $headers `
            -Uri "https://api.github.com/repos/$Repo/actions/artifacts?name=$platform&per_page=1"
        if ($list.total_count -lt 1) {
            Die "no nightly artifact for $platform yet
  check https://github.com/$Repo/actions/workflows/daily-build.yml for the latest run"
        }

        Write-Host "install.ps1: downloading"
        Invoke-WebRequest -Headers $headers `
            -Uri $list.artifacts[0].archive_download_url `
            -OutFile (Join-Path $work "artifact.zip")
    }

    # ---- unpack + install -------------------------------------------------

    Expand-Archive -Path (Join-Path $work "artifact.zip") -DestinationPath $work\unpacked -Force
    $bin = Join-Path $work\unpacked "luna.exe"
    if (-not (Test-Path $bin)) { $bin = Join-Path $work\unpacked "luna" }
    if (-not (Test-Path $bin)) {
        Die "artifact does not contain a luna binary (unexpected payload)"
    }
    # the binary resolves its Lua layer (argparse and friends) from the
    # luna_modules/ sidecar next to the executable
    $mods = Join-Path $work\unpacked "luna_modules"
    if (-not (Test-Path $mods)) {
        Die "artifact does not contain the luna_modules sidecar
  (binaries from before 2026-10-02 were shipped without it and cannot run standalone)"
    }

    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    Copy-Item -Path $bin -Destination $exe -Force
    $destMods = Join-Path $Dir "luna_modules"
    if (Test-Path $destMods) { Remove-Item -Recurse -Force $destMods }
    Copy-Item -Path $mods -Destination $destMods -Recurse -Force

    # ---- verify + PATH -----------------------------------------------------

    $ver = (& $exe --version 2>&1 | Select-Object -First 1)
    if ($ver -notmatch '^luna ') {
        Die "installed binary did not answer --version with a version (got: $ver)"
    }
    Write-Host "install.ps1: installed $ver -> $exe"

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (($userPath -split ";") -notcontains $Dir) {
        $newPath = if ([string]::IsNullOrEmpty($userPath)) { $Dir } else { "$userPath;$Dir" }
        [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
        Write-Host "install.ps1: added $Dir to the user PATH (restart the shell to pick it up)"
    }

}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
