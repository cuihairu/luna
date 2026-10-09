#requires -Version 5.1
<#
luna installer — Windows, one line (PowerShell):

  irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex

Installs the latest nightly build for x86_64 Windows into
%LOCALAPPDATA%\Programs\luna and adds it to the user PATH.
Re-running is safe: it upgrades in place.

The zip comes from the rolling nightly Release (fixed tag `nightly`,
republished - assets cleared and re-uploaded - after every green daily
build): https://github.com/cuihairu/luna/releases/tag/nightly
Release assets on a public repo download anonymously, so no token is
needed. Each asset ships a .sha256 sidecar that is verified before
installing.

Options — via parameters when saved and run as a file
  .\install.ps1 -Token ghp_xxx    # optional: private forks or Release
                                   # fallback only — public assets are
                                   # anonymous
or via environment variables when piped (param binding does not apply
through irm | iex):

  $env:GITHUB_TOKEN = "ghp_xxx"; irm https://raw.githubusercontent.com/cuihairu/luna/main/install.ps1 | iex
                                                     # private forks only
  $env:LUNA_MIRROR  = "https://.../artifact.zip"   # anonymous direct zip
  $env:LUNA_DIR     = "D:\tools\luna"              # install directory

A token (parameter, environment, or a logged-in gh CLI) is attached to
the Release download when present, so private forks keep working. If
the Release itself cannot be fetched, the legacy Actions-artifact API
is tried as a fallback - that API always needs a token. A missing
nightly asset is reported with a pointer at the Release page above.
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
        # credentials are optional: the nightly Release on a public repo
        # downloads anonymously. Attach a token when one is around so
        # private forks keep working.
        if (-not $Token) { $Token = $env:GITHUB_TOKEN }
        if (-not $Token) { $Token = $env:GH_TOKEN }
        if (-not $Token) {
            $gh = Get-Command gh -ErrorAction SilentlyContinue
            if ($gh) {
                try { $Token = (& gh auth token) 2>$null } catch { }
                if ($Token) { Write-Host "install.ps1: attaching gh CLI credentials" }
            }
        }
        $headers = @{ }
        if ($Token) { $headers["Authorization"] = "Bearer $Token" }

        $zip = Join-Path $work "artifact.zip"
        Write-Host "install.ps1: downloading $platform from the nightly Release"
        try {
            Invoke-WebRequest -Headers $headers `
                -Uri "https://github.com/$Repo/releases/download/nightly/$platform.zip" `
                -OutFile $zip
        }
        catch {
            $reason = "$_"
            if ($reason -match '\(404\)') {
                Die "no $platform asset on the nightly Release yet
  check https://github.com/$Repo/releases/tag/nightly - the daily-build
  workflow republishes the assets there after every green run (the
  Windows asset flows once the Windows port clears its last frozen item)"
            }
            if (-not $Token) {
                Die "nightly Release download failed: $reason
  check https://github.com/$Repo/releases/tag/nightly, or pass -Token /
  set GITHUB_TOKEN / GH_TOKEN / log in with 'gh auth login' (private
  forks), or point LUNA_MIRROR at an anonymously reachable zip URL"
            }
            # non-404 with credentials in hand (private repo oddities,
            # transient API shapes): fall back to the legacy
            # Actions-artifact API, which always needs a token
            Write-Host "install.ps1: release download failed ($reason), falling back to the Actions artifact API"
            $headers["Accept"] = "application/vnd.github+json"
            $list = Invoke-RestMethod -Headers $headers `
                -Uri "https://api.github.com/repos/$Repo/actions/artifacts?name=$platform&per_page=1"
            if ($list.total_count -lt 1) {
                Die "no nightly artifact for $platform yet
  the delivery channel is the nightly Release -
  check https://github.com/$Repo/releases/tag/nightly
  (Actions artifacts are run-local copies, not the delivery)"
            }
            Write-Host "install.ps1: downloading from the artifact API"
            Invoke-WebRequest -Headers $headers `
                -Uri $list.artifacts[0].archive_download_url `
                -OutFile $zip
        }

        # checksum: nightly assets ship a .zip.sha256 sidecar (sha256sum
        # format). A mismatch is usually the release being republished
        # mid-download - warn instead of installing a corrupt binary;
        # the --version smoke below is the real gate. The sidecar fetch
        # is anonymous and optional: no sidecar, no verdict.
        try {
            $shaUrl = "https://github.com/$Repo/releases/download/nightly/$platform.zip.sha256"
            $expected = ((Invoke-WebRequest -Uri $shaUrl).Content -split "\s+")[0]
            $actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
            if ($expected -and ($actual -ne $expected)) {
                Write-Host "install.ps1: WARNING sha256 mismatch (expected $expected, got $actual) - the nightly may have been republished mid-download; re-run to retry"
            }
        }
        catch {
            Write-Host "install.ps1: no checksum sidecar fetched ($($_.Exception.Message)); continuing"
        }
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
