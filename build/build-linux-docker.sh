#!/usr/bin/env bash
# build-linux-docker.sh - Compile mbcryptload.so pour PHP 8.0 a 8.5 (NTS + ZTS)
#                          + le CLI mbcrypt statique, via Docker.
# Auteur : CISSE Mohamed Bassirou
# Usage  : bash build/build-linux-docker.sh
#
# Pourquoi Docker plutot que le systeme hote :
#   - Aucun droit root necessaire sur la machine de build (pas de sudo,
#     pas d'ajout de depot tiers) : seul l'appartenance au groupe "docker"
#     est requise.
#   - Reproductible : chaque version PHP est compilee contre les vraies
#     images officielles "php" (Debian bookworm/bullseye), avec le meme
#     phpize/php-config que la distribution cible utiliserait.
#   - Portabilite maximale : bookworm/bullseye embarquent une glibc plus
#     ancienne que la plupart des distributions recentes, donc les .so
#     produits fonctionnent aussi sur des systemes plus recents (voir
#     GUIDE-COMPILATION-LINUX.md, section "Portabilite").
#
# Resultat dans build/dist-linux/ :
#   mbcryptload_php80_nts.so   mbcryptload_php80_zts.so
#   mbcryptload_php81_nts.so   mbcryptload_php81_zts.so
#   mbcryptload_php82_nts.so   mbcryptload_php82_zts.so
#   mbcryptload_php83_nts.so   mbcryptload_php83_zts.so
#   mbcryptload_php84_nts.so   mbcryptload_php84_zts.so
#   mbcryptload_php85_nts.so   mbcryptload_php85_zts.so
#   mbcrypt                    (CLI encodeur, lie statiquement)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$PROJECT_DIR/mbcrypt"
DIST_DIR="$SCRIPT_DIR/dist-linux"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

mkdir -p "$DIST_DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'
YELLOW='\033[1;33m'; NC='\033[0m'
log_step() { echo -e "\n${CYAN}==> $1${NC}"; }
log_ok()   { echo -e "    ${GREEN}[OK]${NC} $1"; }
log_fail() { echo -e "    ${RED}[KO]${NC} $1"; }

if ! command -v docker &>/dev/null; then
    echo -e "${RED}Docker introuvable.${NC} Installer Docker Engine et ajouter"
    echo "l'utilisateur au groupe docker (sudo usermod -aG docker \$USER),"
    echo "puis se reconnecter."
    exit 1
fi
if ! docker info &>/dev/null; then
    echo -e "${RED}Docker installe mais inaccessible${NC} (droits/groupe docker, ou daemon arrete)."
    exit 1
fi

# ---------------------------------------------------------------------------
# Matrice PHP 8.0 - 8.5 x (NTS, ZTS)
# Format : "version|image_nts|image_zts"
# ---------------------------------------------------------------------------
MATRIX=(
    "80|php:8.0-cli-bullseye|php:8.0-zts-bullseye"
    "81|php:8.1-cli-bookworm|php:8.1-zts-bookworm"
    "82|php:8.2-cli-bookworm|php:8.2-zts-bookworm"
    "83|php:8.3-cli-bookworm|php:8.3-zts-bookworm"
    "84|php:8.4-cli-bookworm|php:8.4-zts-bookworm"
    "85|php:8.5-cli-bookworm|php:8.5-zts-bookworm"
)

build_one() {
    local tag="$1" image="$2" variant="$3"
    local out_name="mbcryptload_php${tag}_${variant}.so"

    docker run --rm \
        -v "$SRC_DIR":/src:ro \
        -v "$DIST_DIR":/out \
        -e HOST_UID="$HOST_UID" -e HOST_GID="$HOST_GID" \
        "$image" \
        bash -c '
            set -e
            apt-get update -qq >/tmp/apt.log 2>&1
            apt-get install -y -qq --no-install-recommends zlib1g-dev libssl-dev >>/tmp/apt.log 2>&1
            mkdir -p /build
            cp /src/mbcryptload.c /src/mbcrypt_lib.c /src/mbcrypt_lib.h /src/mbpak_lib.c /src/mbpak_lib.h /src/php_mbcryptload.h /src/config.m4 /build/
            cd /build
            phpize >/tmp/phpize.log 2>&1
            ./configure --enable-mbcryptload >/tmp/configure.log 2>&1
            make -j"$(nproc)" >/tmp/make.log 2>&1
            SO=$(find modules -name "mbcryptload.so" | head -1)
            cp "$SO" "/out/'"$out_name"'"
            strip "/out/'"$out_name"'"
            chown "$HOST_UID:$HOST_GID" "/out/'"$out_name"'"
        ' > "/tmp/mbcrypt-build-${tag}-${variant}.log" 2>&1

    if [ -f "$DIST_DIR/$out_name" ]; then
        local size; size=$(du -h "$DIST_DIR/$out_name" | cut -f1)
        log_ok "PHP $tag ($variant, $image) -> $out_name ($size)"
        return 0
    else
        log_fail "PHP $tag ($variant, $image) -- voir /tmp/mbcrypt-build-${tag}-${variant}.log"
        tail -30 "/tmp/mbcrypt-build-${tag}-${variant}.log" | sed 's/^/      /'
        return 1
    fi
}

log_step "Compilation de la matrice mbcryptload.so (PHP 8.0-8.5, NTS+ZTS)"
FAILED=0
for entry in "${MATRIX[@]}"; do
    IFS='|' read -r tag img_nts img_zts <<< "$entry"
    build_one "$tag" "$img_nts" "nts" || FAILED=1
    build_one "$tag" "$img_zts" "zts" || FAILED=1
done

# ---------------------------------------------------------------------------
# CLI mbcrypt (statique, pour portabilite maximale entre distributions)
# ---------------------------------------------------------------------------
log_step "Compilation du CLI mbcrypt (statique)"

docker run --rm \
    -v "$SRC_DIR":/src:ro \
    -v "$DIST_DIR":/out \
    -e HOST_UID="$HOST_UID" -e HOST_GID="$HOST_GID" \
    debian:bookworm \
    bash -c '
        set -e
        apt-get update -qq >/tmp/apt.log 2>&1
        apt-get install -y -qq --no-install-recommends gcc make zlib1g-dev libssl-dev pkg-config >>/tmp/apt.log 2>&1
        mkdir -p /build
        cp /src/mbcrypt.c /src/mbcrypt_lib.c /src/mbcrypt_lib.h /src/Makefile /build/
        cd /build
        make static >/tmp/make.log 2>&1
        cp mbcrypt /out/mbcrypt
        strip /out/mbcrypt
        chown "$HOST_UID:$HOST_GID" /out/mbcrypt
    ' > /tmp/mbcrypt-cli-build.log 2>&1

if [ -f "$DIST_DIR/mbcrypt" ]; then
    log_ok "mbcrypt CLI (statique) -> $DIST_DIR/mbcrypt"
else
    log_fail "CLI -- voir /tmp/mbcrypt-cli-build.log"
    tail -30 /tmp/mbcrypt-cli-build.log | sed 's/^/      /'
    FAILED=1
fi

log_step "Resume"
ls -lh "$DIST_DIR" | tail -n +2 | awk '{printf "    %-35s %s\n", $NF, $5}'

if [ "$FAILED" -eq 1 ]; then
    echo -e "\n${YELLOW}Au moins une cible a echoue -- voir les logs /tmp/mbcrypt-build-*.log${NC}"
    exit 1
fi

echo -e "\n${GREEN}Matrice complete.${NC} Voir build/GUIDE-COMPILATION-LINUX.md pour l'installation."
