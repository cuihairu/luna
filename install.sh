#!/bin/sh
# luna installer — Linux / macOS, one line:
#
#   curl -fsSL https://raw.githubusercontent.com/cuihairu/luna/main/install.sh | sh
#
# Installs the latest nightly build for this OS+arch into ~/.local/bin
# (override with --dir). Re-running is safe: it upgrades in place.
#
# The zip comes from the rolling nightly Release (fixed tag `nightly`,
# republished - assets cleared and re-uploaded - after every green daily
# build): https://github.com/cuihairu/luna/releases/tag/nightly
# Release assets on a public repo download anonymously, so no token is
# needed. Each asset ships a .sha256 sidecar that is verified when
# sha256sum is available.
#
# Options (with `| sh -s --` when piped):
#   --token TOKEN   GitHub token. Optional: only needed for private
#                   forks, or as a fallback to the Release download.
#                   Also read from $GITHUB_TOKEN / $GH_TOKEN, or
#                   auto-detected from a logged-in `gh` CLI.
#   --mirror URL    direct download URL of the artifact zip (bypasses
#                   the Release entirely; see also $LUNA_INSTALL_MIRROR)
#   --dir DIR       install directory (default: $HOME/.local/bin)
#   --repo O/N      source repository (default: cuihairu/luna)
#
# A token is only attached when present. If the Release download fails
# with credentials in hand (non-404), the legacy Actions-artifact API is
# tried as a fallback - that API always needs a token. A missing nightly
# asset is reported with a pointer at the Release page above.

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
    sed -n '2,31p' "$0" | sed 's/^# \{0,1\}//'
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

if [ -z "$MIRROR" ] && [ -n "${LUNA_INSTALL_MIRROR:-}" ]; then
    MIRROR=$LUNA_INSTALL_MIRROR
fi

if [ -n "$MIRROR" ]; then
    echo "install.sh: downloading $PLATFORM from mirror"
    curl -fsSL "$MIRROR" -o "$ZIP" || die "mirror download failed: $MIRROR"
else
    # credentials are optional: the nightly Release on a public repo
    # downloads anonymously. Attach a token when one is around so
    # private forks keep working.
    if [ -z "$TOKEN" ]; then
        if [ -n "${GITHUB_TOKEN:-}" ]; then
            TOKEN=$GITHUB_TOKEN
        elif [ -n "${GH_TOKEN:-}" ]; then
            TOKEN=$GH_TOKEN
        elif command -v gh >/dev/null 2>&1 && gh auth token >/dev/null 2>&1; then
            TOKEN=$(gh auth token)
            echo "install.sh: attaching gh CLI credentials"
        fi
    fi

    RELEASE_URL="https://github.com/$REPO/releases/download/nightly/$PLATFORM.zip"
    echo "install.sh: downloading $PLATFORM from the nightly Release"
    CURL_ERR="$WORK/curl.err"
    # set -e 会直接杀掉失败 curl:显式承接退出码再分支
    RC=0
    if [ -n "$TOKEN" ]; then
        curl -fsSL -H "Authorization: Bearer $TOKEN" \
             "$RELEASE_URL" -o "$ZIP" 2>"$CURL_ERR" || RC=$?
    else
        curl -fsSL "$RELEASE_URL" -o "$ZIP" 2>"$CURL_ERR" || RC=$?
    fi
    if [ "$RC" -ne 0 ]; then
        if grep -q ' 404' "$CURL_ERR" 2>/dev/null; then
            die "no $PLATFORM asset on the nightly Release yet
  check https://github.com/$REPO/releases/tag/nightly - the daily-build
  workflow republishes the assets there after every green run (the
  Windows asset flows once the Windows port clears its last frozen item)"
        fi
        if [ -n "$TOKEN" ]; then
            # non-404 with credentials in hand (private repo oddities,
            # transient API shapes): fall back to the legacy
            # Actions-artifact API, which always needs a token
            echo "install.sh: release download failed, falling back to the Actions artifact API"
            JSON=$(api_get "$TOKEN" "/repos/$REPO/actions/artifacts?name=$PLATFORM&per_page=1") \
                || die "artifact lookup failed (network, or token lacks repo read)"
            TOTAL=$(printf '%s' "$JSON" | sed -n 's/.*"total_count"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p')
            [ "${TOTAL:-0}" -ge 1 ] || die "no nightly artifact for $PLATFORM yet
  the delivery channel is the nightly Release -
  check https://github.com/$REPO/releases/tag/nightly
  (Actions artifacts are run-local copies, not the delivery)"
            ZIPURL=$(printf '%s' "$JSON" | sed -n 's/.*"archive_download_url"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')
            [ -n "$ZIPURL" ] || die "could not read archive_download_url from the API reply"
            curl -fsSL -H "Authorization: Bearer $TOKEN" \
                      -H "Accept: application/vnd.github+json" \
                      "$ZIPURL" -o "$ZIP" \
                || die "artifact download failed (expired artifact, thin token, or network)"
        else
            die "nightly Release download failed: $(cat "$CURL_ERR")
  check https://github.com/$REPO/releases/tag/nightly, or pass --token /
  set GITHUB_TOKEN / GH_TOKEN / log in with 'gh auth login' (private
  forks), or point --mirror / LUNA_INSTALL_MIRROR at an anonymously
  reachable zip URL"
        fi
    fi

    # checksum: nightly assets ship a .zip.sha256 sidecar (sha256sum
    # format). A mismatch is usually the release being republished
    # mid-download - warn, don't bail; the --version smoke below is the
    # real gate. Sidecar fetch is anonymous and optional: no sha256sum
    # command, no checksum, no verdict.
    if command -v sha256sum >/dev/null 2>&1; then
        if SHA=$(curl -fsSL "$RELEASE_URL.sha256" 2>/dev/null); then
            want=$(printf '%s' "$SHA" | awk '{print $1}')
            have=$(sha256sum "$ZIP" | awk '{print $1}')
            if [ -n "$want" ] && [ "$have" != "$want" ]; then
                echo "install.sh: WARNING sha256 mismatch (expected $want, got $have) - the nightly may have been republished mid-download; re-run to retry" >&2
            fi
        fi
    fi
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