#!/usr/bin/env bash
# End-to-end functional test for mbpak. Run via `make test`.
# Exercises: pack, list, extract (+ content integrity), put (update),
# get, rm, compact, wrong-password rejection, tamper detection, and the
# refuse-to-overwrite guard. Exits non-zero on the first failure.
set -euo pipefail

cd "$(dirname "$0")/.."
BIN=./mbpak
PW="S3cret!"
TMP=test/tmp

rm -rf "$TMP"
mkdir -p "$TMP/src/lib"

cat > "$TMP/src/index.php" <<'EOF'
<?php
echo "index\n";
require __DIR__ . "/lib/auth.php";
EOF
cat > "$TMP/src/lib/auth.php" <<'EOF'
<?php
function auth() { return true; }
EOF
cat > "$TMP/src/config.php" <<'EOF'
<?php
return ["db" => "prod"];
EOF

echo "[1/9] pack"
$BIN pack -p "$PW" -o "$TMP/app.mbpak" "$TMP/src" >/dev/null

echo "[2/9] list"
[ "$($BIN list -p "$PW" "$TMP/app.mbpak" | grep -c '^--')" = "1" ]

echo "[3/9] extract + integrity"
$BIN extract -p "$PW" -o "$TMP/out" "$TMP/app.mbpak" >/dev/null
diff -r "$TMP/src" "$TMP/out"

echo "[4/9] put (update, append-only)"
SZ1=$(stat -c%s "$TMP/app.mbpak")
echo "<?php return ['db'=>'staging'];" > "$TMP/config2.php"
$BIN put -p "$PW" "$TMP/app.mbpak" config.php "$TMP/config2.php" >/dev/null
SZ2=$(stat -c%s "$TMP/app.mbpak")
[ "$SZ2" -gt "$SZ1" ]
[ "$($BIN get -p "$PW" "$TMP/app.mbpak" config.php)" = "<?php return ['db'=>'staging'];" ]

echo "[5/9] rm + compact reclaims space"
$BIN rm -p "$PW" "$TMP/app.mbpak" lib/auth.php >/dev/null
SZ3=$(stat -c%s "$TMP/app.mbpak")
$BIN compact -p "$PW" "$TMP/app.mbpak" >/dev/null
SZ4=$(stat -c%s "$TMP/app.mbpak")
[ "$SZ4" -lt "$SZ3" ]
$BIN get -p "$PW" "$TMP/app.mbpak" lib/auth.php >/dev/null 2>&1 && exit 1 # must be gone

echo "[6/9] wrong password rejected"
$BIN list -p "wrong-password" "$TMP/app.mbpak" >/dev/null 2>&1 && exit 1

echo "[7/9] tamper detection (GCM auth)"
cp "$TMP/app.mbpak" "$TMP/app_t.mbpak"
python3 -c "
d = bytearray(open('$TMP/app_t.mbpak','rb').read())
d[len(d)-10] ^= 0xFF
open('$TMP/app_t.mbpak','wb').write(d)
"
$BIN list -p "$PW" "$TMP/app_t.mbpak" >/dev/null 2>&1 && exit 1

echo "[8/9] empty archive"
mkdir -p "$TMP/empty_src"
$BIN pack -p "$PW" -o "$TMP/empty.mbpak" "$TMP/empty_src" >/dev/null
[ "$($BIN list -p "$PW" "$TMP/empty.mbpak" | grep -c '^--')" = "1" ]

echo "[9/9] refuse to overwrite an existing archive"
$BIN pack -p "$PW" -o "$TMP/app.mbpak" "$TMP/src" >/dev/null 2>&1 && exit 1

echo "ALL TESTS PASSED"
