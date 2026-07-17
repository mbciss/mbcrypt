#!/usr/bin/env bash
# build-ubuntu.sh - Compile mbcryptload.so + mbcrypt pour PHP 8.1 / 8.2 / 8.3
# Auteur : CISSE Mohamed Bassirou
# Usage  : bash build/build-ubuntu.sh

set -euo pipefail

# ---------------------------------------------------------------------------
# Chemins
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_DIR/mbcrypt"
DIST_DIR="$SCRIPT_DIR/dist"

mkdir -p "$DIST_DIR"

# Versions PHP a compiler (on ne compile que celles presentes sur le systeme)
PHP_VERSIONS=(8.1 8.2 8.3)

# ---------------------------------------------------------------------------
# Couleurs
# ---------------------------------------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'
YELLOW='\033[1;33m'; GRAY='\033[0;37m'; NC='\033[0m'

log_step() { echo -e "\n${CYAN}==> $1${NC}"; }
log_ok()   { echo -e "    ${GREEN}[OK]${NC} $1"; }
log_warn() { echo -e "    ${YELLOW}[!!]${NC} $1"; }
log_fail() { echo -e "    ${RED}[KO]${NC} $1"; }

# ---------------------------------------------------------------------------
# Verification des dependances systeme
# ---------------------------------------------------------------------------
log_step "Verification des dependances"

check_cmd() {
    if command -v "$1" &>/dev/null; then
        log_ok "$1 : $(command -v "$1")"
        return 0
    else
        log_fail "$1 introuvable"
        return 1
    fi
}

MISSING=0

check_cmd gcc    || MISSING=1
check_cmd make   || MISSING=1
check_cmd phpize || true   # non bloquant, on verifie par version ensuite

# zlib
if pkg-config --exists zlib 2>/dev/null; then
    log_ok "zlib : $(pkg-config --modversion zlib)"
else
    log_fail "zlib-dev manquant -- installer : sudo apt install zlib1g-dev"
    MISSING=1
fi

# OpenSSL
if pkg-config --exists openssl 2>/dev/null; then
    HAS_SSL=1
    log_ok "OpenSSL : $(pkg-config --modversion openssl)"
else
    HAS_SSL=0
    log_warn "OpenSSL absent -- AES-256-GCM desactive (XOR uniquement)"
    log_warn "Pour activer : sudo apt install libssl-dev"
fi

if [ "$MISSING" -eq 1 ]; then
    echo ""
    log_fail "Dependances manquantes. Installer avec :"
    echo -e "    ${YELLOW}sudo apt update && sudo apt install -y \\"
    echo -e "        build-essential zlib1g-dev libssl-dev \\"
    echo -e "        php8.3-dev php8.2-dev php8.1-dev${NC}"
    exit 1
fi

# ---------------------------------------------------------------------------
# Compilation de l'extension PHP (une par version)
# ---------------------------------------------------------------------------
log_step "Compilation de l'extension PHP"

RESULTS=()

for VER in "${PHP_VERSIONS[@]}"; do

    PHPIZE_BIN=""
    PHP_CONFIG_BIN=""

    # Cherche phpize specifique a la version (ondrej/php PPA) puis generique
    for candidate in "phpize$VER" "phpize${VER/./}" "phpize"; do
        if command -v "$candidate" &>/dev/null; then
            PHPIZE_BIN="$candidate"
            break
        fi
    done

    for candidate in "php-config$VER" "php-config${VER/./}" "php-config"; do
        if command -v "$candidate" &>/dev/null; then
            PHP_CONFIG_BIN="$candidate"
            break
        fi
    done

    if [ -z "$PHPIZE_BIN" ] || [ -z "$PHP_CONFIG_BIN" ]; then
        log_warn "PHP $VER : php-dev non installe -- ignore"
        log_warn "  Installer : sudo apt install php$VER-dev"
        RESULTS+=("PHP $VER : SKIP (php-dev absent)")
        continue
    fi

    # Verifie que phpize correspond bien a la bonne version majeure
    DETECTED_VER=$("$PHP_CONFIG_BIN" --version 2>/dev/null | cut -d. -f1,2)
    if [ "$DETECTED_VER" != "$VER" ]; then
        log_warn "PHP $VER : php-config retourne $DETECTED_VER -- ignore"
        RESULTS+=("PHP $VER : SKIP (version incorrecte)")
        continue
    fi

    echo -e "    ${GRAY}--- PHP $VER ($PHPIZE_BIN / $PHP_CONFIG_BIN) ---${NC}"

    # Dossier de build temporaire isole par version
    BUILD_TMP="$SCRIPT_DIR/.build_php$VER"
    rm -rf "$BUILD_TMP"
    mkdir -p "$BUILD_TMP"

    # Copie des sources dans le dossier de build temporaire
    cp "$SRC_DIR/mbcryptload.c"   "$BUILD_TMP/"
    cp "$SRC_DIR/mbcrypt_lib.c"   "$BUILD_TMP/"
    cp "$SRC_DIR/mbcrypt_lib.h"   "$BUILD_TMP/"
    cp "$SRC_DIR/php_mbcryptload.h" "$BUILD_TMP/"
    cp "$SRC_DIR/config.m4"       "$BUILD_TMP/"

    cd "$BUILD_TMP"

    # phpize
    "$PHPIZE_BIN" > /dev/null 2>&1 || {
        log_fail "PHP $VER : phpize a echoue"
        RESULTS+=("PHP $VER : FAIL (phpize)")
        cd "$SCRIPT_DIR"
        continue
    }

    # configure
    CONFIGURE_OPTS="--with-php-config=$PHP_CONFIG_BIN --enable-mbcryptload"
    ./configure $CONFIGURE_OPTS > /dev/null 2>&1 || {
        log_fail "PHP $VER : configure a echoue"
        RESULTS+=("PHP $VER : FAIL (configure)")
        cd "$SCRIPT_DIR"
        continue
    }

    # make
    if make -j"$(nproc)" > /dev/null 2>&1; then
        SO_FILE=$(find "$BUILD_TMP/modules" -name "mbcryptload.so" 2>/dev/null | head -1)
        if [ -n "$SO_FILE" ]; then
            DEST="$DIST_DIR/mbcryptload_php${VER/./}.so"
            cp "$SO_FILE" "$DEST"
            strip "$DEST" 2>/dev/null || true
            SIZE=$(du -sh "$DEST" | cut -f1)
            log_ok "PHP $VER : $DEST ($SIZE)"
            RESULTS+=("PHP $VER : OK")
        else
            log_fail "PHP $VER : .so introuvable apres make"
            RESULTS+=("PHP $VER : FAIL (so manquant)")
        fi
    else
        log_fail "PHP $VER : make a echoue"
        make 2>&1 | tail -20 | sed 's/^/      /'
        RESULTS+=("PHP $VER : FAIL (make)")
    fi

    cd "$SCRIPT_DIR"
    rm -rf "$BUILD_TMP"
done

# ---------------------------------------------------------------------------
# Compilation du CLI encoder (mbcrypt)
# ---------------------------------------------------------------------------
log_step "Compilation de mbcrypt (CLI encoder)"

CLI_SRC1="$SRC_DIR/mbcrypt.c"
CLI_SRC2="$SRC_DIR/mbcrypt_lib.c"
CLI_OUT="$DIST_DIR/mbcrypt"

CFLAGS="-O2 -Wall -std=c99 -D_POSIX_C_SOURCE=200809L"
LDFLAGS="-lz"

if [ "$HAS_SSL" -eq 1 ]; then
    CFLAGS="$CFLAGS -DMBCRYPT_HAS_OPENSSL $(pkg-config --cflags openssl)"
    LDFLAGS="$LDFLAGS $(pkg-config --libs openssl)"
fi

echo -e "    ${GRAY}Compilation...${NC}"

if gcc $CFLAGS -o "$CLI_OUT" "$CLI_SRC1" "$CLI_SRC2" $LDFLAGS; then
    strip "$CLI_OUT" 2>/dev/null || true
    SIZE=$(du -sh "$CLI_OUT" | cut -f1)
    log_ok "mbcrypt : $CLI_OUT ($SIZE)"
    CLI_OK=1
else
    log_fail "Compilation du CLI echouee"
    CLI_OK=0
fi

# ---------------------------------------------------------------------------
# Resume
# ---------------------------------------------------------------------------
log_step "Resume"

for r in "${RESULTS[@]}"; do
    if [[ "$r" == *": OK" ]]; then
        echo -e "    ${GREEN}$r${NC}"
    elif [[ "$r" == *": SKIP"* ]]; then
        echo -e "    ${YELLOW}$r${NC}"
    else
        echo -e "    ${RED}$r${NC}"
    fi
done

if [ "$CLI_OK" -eq 1 ]; then
    echo -e "    ${GREEN}mbcrypt CLI : OK${NC}"
else
    echo -e "    ${RED}mbcrypt CLI : FAIL${NC}"
fi

echo ""
echo "Fichiers produits dans : $DIST_DIR"
ls -lh "$DIST_DIR" 2>/dev/null | tail -n +2 | awk '{printf "    %-45s %s\n", $NF, $5}'

# ---------------------------------------------------------------------------
# Instructions d'installation
# ---------------------------------------------------------------------------
echo ""
echo "=== Installation de l'extension ==="
echo ""
echo "1. Trouver le dossier extensions de PHP :"
echo "   php -i | grep extension_dir"
echo ""
echo "2. Copier le .so correspondant a votre version :"
echo "   sudo cp dist/mbcryptload_php83.so \$(php -i | grep ^extension_dir | awk '{print \$3}')/mbcryptload.so"
echo ""
echo "3. Activer dans php.ini :"
echo "   echo 'extension=mbcryptload' | sudo tee /etc/php/8.3/cli/conf.d/20-mbcryptload.ini"
echo ""
echo "4. Pour Apache/FPM, repeter pour le bon php.ini :"
echo "   echo 'extension=mbcryptload' | sudo tee /etc/php/8.3/fpm/conf.d/20-mbcryptload.ini"
echo "   sudo systemctl restart php8.3-fpm"
echo ""
echo "5. Verifier :"
echo "   php -m | grep mbcryptload"
echo "   php --ri mbcryptload"
echo ""
echo "=== Appel direct d'un fichier chiffre (sans require) ==="
echo ""
echo "   export MBCRYPTLOAD_PASSWORD='mon-mot-de-passe'"
echo "   php fichier_chiffre.php"
echo ""
echo "   # Apache (.htaccess ou VirtualHost) :"
echo "   # SetEnv MBCRYPTLOAD_PASSWORD 'mon-mot-de-passe'"
echo ""
echo "   # PHP-FPM (www.conf) :"
echo "   # env[MBCRYPTLOAD_PASSWORD] = 'mon-mot-de-passe'"
