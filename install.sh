#!/bin/sh
# luna installer — Linux / macOS, one line:
#
#   curl -fsSL https://raw.githubusercontent.com/cuihairu/luna/main/install.sh | sh
#
# Installs the latest nightly build for this OS+arch into ~/.local/bin
# (override with --dir). Re-running is safe: it upgrades in place.
#
# Options (with `| sh -s --` when piped):
#   --token TOKEN   GitHub token for the Actions artifact API. Also read
#                   from $GITHUB_TOKEN / $GH_TOKEN, or auto-detected from
#                   a logged-in `gh` CLI.
#   --mirror URL    direct download URL of the artifact zip (bypasses the
#                   API entirely; the URL must be anonymously reachable).
#   --dir DIR       install directory (default: $HOME/.local/bin)
#   --repo O/N      source repository (default: cuihairu/luna)
#
# Artifacts need auth to download from GitHub Actions (anonymous API
# calls get 403), hence the token/mirror escapes above.

set -eu

REPO="cuihairu/luna"
DIR=""
TOKEN=""
MIRROR=""

die() {
    printf 'install.sh: %s\n' "$1" >&2
    exit 1
}

usage() {
    sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --token)  [ $# -ge 2 ] || die "--token needs a value";  TOKEN=$2;  shift 2 ;;
        --mirror) [ $# -ge 2 ] || die "--mirror needs a value"; MIRROR=$2; shift 2 ;;
        --dir)    [ $# -ge 2 ] || die "--dir needs a value";    DIR=$2;    shift 2 ;;
        --repo)   [ $# -ge 2 ] || die "--repo needs a value";   REPO=$2;   shift 2 ;;
        -h|--help) usage ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

# ---- platform detection ------------------------------------------------

case "$(uname -s)" in
    Linux)  OS=linux ;;
    Darwin) OS=macos ;;
    *) die "unsupported OS: $(uname -s) (this script covers Linux and macOS; Windows uses install.ps1)" ;;
esac

case "$(uname -m)" in
    x86_64|amd64)      ARCH=x86_64 ;;
    aarch64|arm64)     ARCH=aarch64 ;;
    *) die "unsupported architecture: $(uname -m)
supported: x86_64, aarch64 (nightly matrix: linux-x86_64, linux-aarch64, macos-aarch64; windows-x86_64 via install.ps1)" ;;
esac

PLATFORM="luna-nightly-${OS}-${ARCH}"

command -v unzip >/dev/null 2>&1 || die "unzip not found (artifact downloads are zip archives)"
command -v curl  >/dev/null 2>&1 || die "curl not found"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/luna-install.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM
ZIP="$WORK/artifact.zip"

# ---- obtain the artifact zip ------------------------------------------

api_get() { # path -> body on stdout
    curl -fsSL -H "Authorization: Bearer $1" \
              -H "Accept: application/vnd.github+json" \
              "https://api.github.com$2"
}

if [ -n "$MIRROR" ]; then
    echo "install.sh: downloading $PLATFORM from mirror"
    curl -fsSL "$MIRROR" -o "$ZIP" || die "mirror download failed: $MIRROR"
else
    if [ -z "$TOKEN" ]; then
        if [ -n "${GITHUB_TOKEN:-}" ]; then
            TOKEN=$GITHUB_TOKEN
        elif [ -n "${GH_TOKEN:-}" ]; then
            TOKEN=$GH_TOKEN
        elif command -v gh >/dev/null 2>&1 && gh auth token >/dev/null 2>&1; then
            TOKEN=$(gh auth token)
            echo "install.sh: using token from gh CLI"
        fi
    fi
    [ -n "$TOKEN" ] || die "nightly artifacts need a GitHub token to download.
  pass --token TOKEN, or set GITHUB_TOKEN / GH_TOKEN, or log in once with 'gh auth login' (auto-detected),
  or point LUNA_INSTALL_MIRROR / --mirror at an anonymously reachable zip URL"

    echo "install.sh: looking up latest $PLATFORM"
    JSON=$(api_get "$TOKEN" "/repos/$REPO/actions/artifacts?name=$PLATFORM&per_page=1") \
        || die "artifact lookup failed (network, or token lacks repo read)"
    TOTAL=$(printf '%s' "$JSON" | sed -n 's/.*"total_count"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p')
    [ "${TOTAL:-0}" -ge 1 ] || die "no nightly artifact for $PLATFORM yet
  check https://github.com/$REPO/actions/workflows/daily-build.yml for the latest run"
    ZIPURL=$(printf '%s' "$JSON" | sed -n 's/.*"archive_download_url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
    [ -n "$ZIPURL" ] || die "could not read archive_download_url from the API reply"

    echo "install.sh: downloading"
    curl -fsSL -H "Authorization: Bearer $TOKEN" \
              -H "Accept: application/vnd.github+json" \
              "$ZIPURL" -o "$ZIP" \
        || die "artifact download failed (expired artifact, thin token, or network)"
fi

# ---- unpack + install --------------------------------------------------

unzip -oq "$ZIP" -d "$WORK/unpacked" || die "artifact is not a valid zip"
BIN="$WORK/unpacked/luna"
[ -f "$BIN" ] || BIN="$WORK/unpacked/luna.exe"
[ -f "$BIN" ] || die "artifact does not contain a luna binary (unexpected payload)"
# the binary resolves its Lua layer (argparse and friends) from the
# luna_modules/ sidecar next to the executable — without it, --version
# dies with "module 'argparse' not found"
[ -d "$WORK/unpacked/luna_modules" ] || die "artifact does not contain the luna_modules sidecar
  (binaries from before 2026-10-02 were shipped without it and cannot run standalone)"

DEST="${DIR:-$HOME/.local/bin}"
mkdir -p "$DEST" || die "cannot create $DEST"
cp "$BIN" "$DEST/luna.new.$$" || die "cannot write to $DEST"
chmod 755 "$DEST/luna.new.$$"
mv -f "$DEST/luna.new.$$" "$DEST/luna"
rm -rf "$DEST/luna_modules"
cp -R "$WORK/unpacked/luna_modules" "$DEST/luna_modules" || die "cannot install the luna_modules sidecar to $DEST"

# ---- verify -------------------------------------------------------------

VER="$("$DEST/luna" --version 2>&1 | head -1)"
case "$VER" in
    "luna "*) echo "install.sh: installed $VER -> $DEST/luna" ;;
    *) die "installed binary did not answer --version with a version (got: $VER)" ;;
esac

case ":$PATH:" in
    *":$DEST:"*) ;;
    *) echo "install.sh: note: $DEST is not on PATH — add it, e.g.:
  export PATH=\"$DEST:\$PATH\"   (put this line in your shell profile)" ;;
esac
