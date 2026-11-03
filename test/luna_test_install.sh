#!/bin/sh
# Deterministic harness for install.sh's checksum branch, run under
# ctest as the `install_sh` group. No network: a fake curl serves the
# .sha256 sidecar (and a dummy zip), a fake sha256sum answers with a
# digest that disagrees with the sidecar, so install.sh must die on
# mismatch before unpacking. A second leg asserts the sidecar fetch
# carries the Authorization header when --token is passed (private-
# fork path).
set -eu

INSTALL_SH="${1:-}"
[ -n "$INSTALL_SH" ] || { echo "usage: $0 <install.sh>" >&2; exit 2; }
[ -f "$INSTALL_SH" ] || { echo "install.sh not found: $INSTALL_SH" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/luna-install-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT INT TERM
BIN="$WORK/bin"
mkdir -p "$BIN"

cat > "$BIN/uname" <<'EOF'
#!/bin/sh
case "$1" in -s) echo Linux;; -m) echo x86_64;; esac
EOF

# curl: log (auth, url) lines; serve the sidecar on stdout (or to -o)
# and a dummy zip to -o. The sidecar must also print to stdout because
# install.sh captures it via $(...).
cat > "$BIN/curl" <<'EOF'
#!/bin/sh
LOG="$FAKE_CURL_LOG"; SHA="$FAKE_CURL_SHA"; OUT=""; AUTH=""; URL=""
prev=""
for a in "$@"; do
    if [ "$prev" = "-o" ]; then OUT=$a; prev=""; continue; fi
    if [ "$prev" = "-H" ]; then AUTH=$a; prev=""; continue; fi
    case "$a" in
        -o) prev="-o" ;;
        -H) prev="-H" ;;
        -*) ;;
        *) URL=$a ;;
    esac
done
printf 'AUTH=%s URL=%s\n' "$AUTH" "$URL" >> "$LOG"
case "$URL" in
    *.sha256)
        [ -n "$SHA" ] || exit 22
        if [ -n "$OUT" ]; then printf '%s\n' "$SHA" > "$OUT"; else printf '%s\n' "$SHA"; fi
        ;;
    *)
        printf 'not-a-real-zip' > "$OUT"
        ;;
esac
exit 0
EOF

cat > "$BIN/sha256sum" <<'EOF'
#!/bin/sh
printf '%s  %s\n' "$FAKE_SHA256_SUM" "$1"
EOF

# unzip is only probed (command -v) before the checksum die; a stub
# suffices, the mismatch leg never reaches it.
cat > "$BIN/unzip" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod +x "$BIN"/*

fail() { echo "FAIL: $1" >&2; exit 1; }

# ---- leg 1: mismatch -> hard die ------------------------------------
LOG="$WORK/log-mismatch"
RC=0
FAKE_CURL_LOG="$LOG" \
FAKE_CURL_SHA="deadbeef  artifact.zip" \
FAKE_SHA256_SUM="11111111  artifact.zip" \
PATH="$BIN:$PATH" sh "$INSTALL_SH" --dir "$WORK/dest" >"$WORK/out1" 2>&1 \
    || RC=$?
[ "$RC" -ne 0 ] || fail "leg1: install.sh exited 0 on a checksum mismatch"
grep -q "sha256 mismatch" "$WORK/out1" \
    || fail "leg1: die message missing (got: $(cat "$WORK/out1"))"

# ---- leg 2: sidecar fetch carries the token (private-fork path) -----
LOG="$WORK/log-token"
RC=0
FAKE_CURL_LOG="$LOG" \
FAKE_CURL_SHA="11111111  artifact.zip" \
FAKE_SHA256_SUM="11111111  artifact.zip" \
PATH="$BIN:$PATH" sh "$INSTALL_SH" --token ghp_test --dir "$WORK/dest2" \
    >"$WORK/out2" 2>&1 || RC=$?
# matching digest -> install proceeds past checksum to unzip, which
# fails on the dummy zip; that is fine, we only assert the auth header.
SIDECAR_AUTH=$(grep '\.sha256$' "$LOG" | sed -n '1p' | sed 's/.*AUTH=//;s/ URL=.*//')
[ "$SIDECAR_AUTH" = "Authorization: Bearer ghp_test" ] \
    || fail "leg2: sidecar fetch auth = '$SIDECAR_AUTH' (want Bearer ghp_test)"

echo "PASS: install.sh checksum branch (mismatch dies; token attached to sidecar)"
exit 0
