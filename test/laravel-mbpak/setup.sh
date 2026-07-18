#!/usr/bin/env bash
# setup.sh - Reproduit de bout en bout l'integration mbpak/Laravel :
#            telecharge un Laravel neuf, empaquete app/ + config/ +
#            routes/web.php + resources/views/ dans une archive .mbpak
#            chiffree, retire ces dossiers du disque, et sert l'app
#            reelle depuis l'archive (page d'accueil + une route de
#            verification qui exerce Eloquent, l'heritage de classes,
#            la config et le rendu Blade).
#
# Auteur : CISSE Mohamed Bassirou
# Usage  : bash test/laravel-mbpak/setup.sh
#
# Necessite Docker (aucun droit root sur l'hote, meme logique que
# build/build-linux-docker.sh). Tout se passe dans un conteneur
# persistant nomme "mbpak-laravel-test" + un volume local
# test/laravel-mbpak/work/ (ignore par git -- ~80 Mo de vendor/ Composer,
# jamais commit).
#
# Lire README.md dans ce dossier pour le detail complet de la procedure
# et la liste exacte des fichiers Laravel a modifier, avec les pieges
# rencontres (et pourquoi) pendant la mise au point de ce script.
# ===========================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
WORK_DIR="$SCRIPT_DIR/work"
CONTAINER=mbpak-laravel-test
PHP_IMAGE="php:8.3-cli-bookworm"
ARCHIVE_PASSWORD="LaravelTest123!"
PORT=8000

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_step() { echo -e "\n${CYAN}==> $1${NC}"; }
log_ok()   { echo -e "    ${GREEN}[OK]${NC} $1"; }
log_fail() { echo -e "    ${RED}[KO]${NC} $1"; }
log_warn() { echo -e "    ${YELLOW}[!!]${NC} $1"; }

if ! command -v docker &>/dev/null; then
    echo -e "${RED}Docker introuvable.${NC} Voir build/GUIDE-COMPILATION-LINUX.md."
    exit 1
fi

mkdir -p "$WORK_DIR"

log_step "Conteneur Docker persistant ($CONTAINER)"
docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
docker run -d --name "$CONTAINER" \
    -v "$WORK_DIR":/work \
    -v "$PROJECT_DIR/mbcrypt":/src/mbcrypt:ro \
    -v "$PROJECT_DIR/mbpak":/src/mbpak:ro \
    "$PHP_IMAGE" sleep infinity >/dev/null
log_ok "Conteneur demarre"

log_step "Dependances systeme + Composer"
docker exec "$CONTAINER" bash -c '
    apt-get update -qq >/dev/null 2>&1
    apt-get install -y -qq --no-install-recommends \
        git unzip zlib1g-dev libssl-dev pkg-config gcc make curl procps sqlite3 >/dev/null 2>&1
    curl -sS https://getcomposer.org/installer | php -- --install-dir=/usr/local/bin --filename=composer >/dev/null 2>&1
' >/dev/null
log_ok "git/composer/build tools installes"

log_step "Build de mbcryptload.so + du CLI mbpak (depuis ../../mbcrypt et ../../mbpak)"
docker exec "$CONTAINER" bash -c '
    set -e
    mkdir -p /build/ext /build/cli
    cp /src/mbcrypt/mbcryptload.c /src/mbcrypt/mbcrypt_lib.c /src/mbcrypt/mbcrypt_lib.h \
       /src/mbcrypt/mbpak_lib.c /src/mbcrypt/mbpak_lib.h /src/mbcrypt/php_mbcryptload.h \
       /src/mbcrypt/config.m4 /build/ext/
    cd /build/ext
    phpize >/tmp/phpize.log 2>&1
    ./configure --enable-mbcryptload >/tmp/configure.log 2>&1
    make -j"$(nproc)" >/tmp/make.log 2>&1
    cp modules/mbcryptload.so /work/mbcryptload.so

    cp /src/mbpak/mbpak_lib.c /src/mbpak/mbpak_lib.h /src/mbpak/mbpak_cli.c \
       /src/mbcrypt/mbcrypt_lib.c /src/mbcrypt/mbcrypt_lib.h /build/cli/
    cd /build/cli
    gcc -Wall -Wextra -O2 -DMBCRYPT_HAS_OPENSSL -c mbpak_lib.c -o mbpak_lib.o
    gcc -Wall -Wextra -O2 -DMBCRYPT_HAS_OPENSSL -c mbpak_cli.c -o mbpak_cli.o
    gcc -Wall -Wextra -O2 -DMBCRYPT_HAS_OPENSSL -c mbcrypt_lib.c -o mbcrypt_lib.o
    gcc -o mbpak mbpak_lib.o mbpak_cli.o mbcrypt_lib.o -lz -lssl -lcrypto
    cp mbpak /work/mbpak
' 2>&1 | grep -iE "warning|error" && log_fail "warnings/erreurs de compilation -- voir ci-dessus" || log_ok "mbcryptload.so + mbpak CLI prets"

log_step "composer create-project laravel/laravel"
docker exec "$CONTAINER" bash -c '
    cd /work
    rm -rf laravel-app
    composer create-project laravel/laravel laravel-app --no-interaction -q
' >/dev/null 2>&1
docker exec "$CONTAINER" bash -c "cd /work/laravel-app && php artisan --version"

# ---------------------------------------------------------------------------
# Code applicatif de verification -- exerce Eloquent, l'heritage de classes
# (TestController extends Controller, les deux depuis mbpak://), la config
# (via bootstrap/cache/config.php genere a partir de mbpak://) et le rendu
# Blade (compile depuis mbpak://resources/views).
# ---------------------------------------------------------------------------
log_step "Ajout du code de verification (controller/vue/config/route)"
docker exec "$CONTAINER" bash -c 'cat > /work/laravel-app/app/Http/Controllers/TestController.php <<"EOF"
<?php

namespace App\Http\Controllers;

use App\Models\User;

class TestController extends Controller
{
    public function index()
    {
        return response()->json([
            "source"           => "mbpak://",
            "user_count"       => User::count(),
            "controller_class" => static::class,
            "parent_class"     => get_parent_class($this),
            "config_test"      => config("mbpaktest.value"),
            "view_render"      => (string) view("mbpak-test", ["name" => "Laravel"]),
        ]);
    }
}
EOF
cat > /work/laravel-app/resources/views/mbpak-test.blade.php <<"EOF"
<p>Bonjour {{ $name }}, cette vue Blade a ete compilee depuis mbpak://</p>
EOF
cat > /work/laravel-app/config/mbpaktest.php <<"EOF"
<?php
return ["value" => "config chargee depuis le cache genere a partir de mbpak://"];
EOF
cat > /work/laravel-app/routes/web.php <<"EOF"
<?php

use Illuminate\Support\Facades\Route;
use App\Http\Controllers\TestController;

Route::get("/", function () {
    return view("welcome");
});

Route::get("/mbpak-test", [TestController::class, "index"]);
EOF
'
log_ok "Code de verification en place"

# ---------------------------------------------------------------------------
# Publier config/view.php et repointer ses "paths" vers l'archive.
# ---------------------------------------------------------------------------
log_step "Publication de config/view.php + repointage vers mbpak://resources/views"
docker exec "$CONTAINER" bash -c "
    cd /work/laravel-app
    php artisan config:publish view >/dev/null 2>&1
    sed -i \"s|resource_path('views'),|'mbpak://resources/views',|\" config/view.php
"
log_ok "config/view.php pointe vers mbpak://resources/views"

# ---------------------------------------------------------------------------
# IMPORTANT (voir README.md "Ordre des operations") : composer dump-autoload
# et php artisan config:cache doivent tourner ICI, PENDANT que app/, config/,
# routes/, resources/ sont encore de VRAIS fichiers sur disque et que
# bootstrap/app.php pointe encore vers de vrais chemins. Une fois ces
# fichiers repointes vers mbpak://, N'IMPORTE QUEL boot de l'application
# (HTTP ou artisan) exige que l'extension soit chargee ET l'archive montee --
# composer/artisan tournent ici avec un PHP nu, sans l'extension.
# ---------------------------------------------------------------------------
log_step "composer dump-autoload + php artisan config:cache (fichiers encore reels)"
docker exec "$CONTAINER" bash -c "cd /work/laravel-app && composer dump-autoload -q" >/dev/null 2>&1 || true
docker exec "$CONTAINER" bash -c "cd /work/laravel-app && php artisan config:cache" 2>&1 | tail -3
log_ok "vendor/composer/autoload_static.php et bootstrap/cache/config.php generes"

# ---------------------------------------------------------------------------
# Empaquetage : app/, config/, resources/views/, routes/web.php dans UNE
# archive, via un dossier de mise en scene qui reproduit la structure de
# chemins attendue par les patches ci-dessous. routes/console.php N'EST PAS
# empaquete (voir README.md -- realpath() de Laravel le rejette).
# ---------------------------------------------------------------------------
log_step "mbpak pack : app/ + config/ + resources/views/ + routes/web.php"
docker exec "$CONTAINER" bash -c "
    set -e
    rm -rf /work/pack_staging /work/app.mbpak
    mkdir -p /work/pack_staging/routes /work/pack_staging/resources
    cp -a /work/laravel-app/app /work/pack_staging/app
    cp -a /work/laravel-app/config /work/pack_staging/config
    cp -a /work/laravel-app/resources/views /work/pack_staging/resources/views
    cp /work/laravel-app/routes/web.php /work/pack_staging/routes/web.php
    /work/mbpak pack -p '$ARCHIVE_PASSWORD' -o /work/app.mbpak /work/pack_staging
"
docker exec "$CONTAINER" /work/mbpak list -p "$ARCHIVE_PASSWORD" /work/app.mbpak
log_ok "Archive /work/app.mbpak creee"

# ---------------------------------------------------------------------------
# Derniere passe : repointer composer, bootstrap/app.php, public/index.php
# et artisan vers mbpak:// -- APRES que toutes les commandes composer/artisan
# aient tourne normalement (voir avertissement plus haut).
# ---------------------------------------------------------------------------
log_step "Repointage final (composer classmap, bootstrap/app.php, index.php, artisan)"
docker exec "$CONTAINER" bash -c "
    cd /work/laravel-app
    sed -i \"s|__DIR__ \. '/\.\./\.\.' \. '/app/\(.*\)\.php'|'mbpak://app/\1.php'|\" vendor/composer/autoload_static.php
    sed -i \"s|web: __DIR__.'/../routes/web.php',|web: 'mbpak://routes/web.php',|\" bootstrap/app.php
    cp public/index.php public/index.php.orig
    sed -i \"s|define('LARAVEL_START', microtime(true));|define('LARAVEL_START', microtime(true));\n\nmbpak_mount('/work/app.mbpak', '$ARCHIVE_PASSWORD') or die('mbpak_mount failed');|\" public/index.php
    cp artisan artisan.orig
    sed -i \"s|define('LARAVEL_START', microtime(true));|define('LARAVEL_START', microtime(true));\n\nmbpak_mount('/work/app.mbpak', '$ARCHIVE_PASSWORD') or fwrite(STDERR, 'mbpak_mount failed'.PHP_EOL);|\" artisan
"
log_ok "public/index.php, artisan, bootstrap/app.php, composer classmap repointes"

# ---------------------------------------------------------------------------
# Retrait des dossiers reels du disque -- aucun repli possible desormais.
# routes/console.php reste (jamais empaquete, voir README.md).
# ---------------------------------------------------------------------------
log_step "Retrait de app/, config/, resources/views/, routes/web.php du disque"
docker exec "$CONTAINER" bash -c "
    cd /work/laravel-app
    rm -rf app.REMOVED config.REMOVED resources/views.REMOVED routes/web.php.REMOVED
    mv app app.REMOVED
    mv config config.REMOVED
    mv resources/views resources/views.REMOVED
    mv routes/web.php routes/web.php.REMOVED
"
log_ok "Fichiers reels deplaces (suffixe .REMOVED, non lus par l'application)"

# ---------------------------------------------------------------------------
# Serveur + verification HTTP + CLI
# ---------------------------------------------------------------------------
log_step "Lancement du serveur PHP integre (port $PORT) et verification"
docker exec "$CONTAINER" bash -c "
    for p in /proc/[0-9]*; do
        cm=\$(tr '\0' ' ' < \$p/cmdline 2>/dev/null)
        case \"\$cm\" in *${PORT}*) kill -9 \$(basename \$p) 2>/dev/null ;; esac
    done
    true
" >/dev/null 2>&1 || true
docker exec -d "$CONTAINER" bash -c "cd /work/laravel-app && exec php -d extension=/work/mbcryptload.so -S 0.0.0.0:$PORT -t public > /work/serve.log 2>&1"
sleep 2

HOME_CODE=$(docker exec "$CONTAINER" curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/")
TEST_BODY=$(docker exec "$CONTAINER" curl -s "http://127.0.0.1:$PORT/mbpak-test")
TEST_CODE=$(docker exec "$CONTAINER" curl -s -o /dev/null -w "%{http_code}" "http://127.0.0.1:$PORT/mbpak-test")

echo "    GET /            -> HTTP $HOME_CODE"
echo "    GET /mbpak-test  -> HTTP $TEST_CODE"
echo "    $TEST_BODY"

FAILED=0
[ "$HOME_CODE" = "200" ] || { log_fail "page d'accueil: HTTP $HOME_CODE attendu 200"; FAILED=1; }
[ "$TEST_CODE" = "200" ] || { log_fail "route de test: HTTP $TEST_CODE attendu 200"; FAILED=1; }
echo "$TEST_BODY" | grep -q '"source":"mbpak' || { log_fail "reponse JSON inattendue"; FAILED=1; }

log_step "Verification CLI (php artisan route:list)"
ROUTE_LIST=$(docker exec "$CONTAINER" bash -c "cd /work/laravel-app && php -d extension=/work/mbcryptload.so artisan route:list" 2>&1)
echo "$ROUTE_LIST" | head -5
# NB: "docker exec ... | head -N" directly (without capturing to a variable
# first) is unsafe under `set -o pipefail` above -- head closing the pipe
# early sends docker exec a SIGPIPE, which pipefail then reports as this
# line failing (exit 141) and aborts the whole script via `set -e`.

if [ "$FAILED" -eq 0 ]; then
    log_ok "Laravel tourne integralement depuis .mbpak (app/config/routes/views absents du disque)"
else
    log_fail "Au moins une verification a echoue -- voir /work/serve.log dans le conteneur"
    exit 1
fi

echo ""
echo -e "${GREEN}Termine.${NC} Conteneur '$CONTAINER' laisse actif pour inspection :"
echo "    docker exec -it $CONTAINER bash"
echo "    docker exec $CONTAINER curl -s http://127.0.0.1:$PORT/mbpak-test"
echo "Nettoyage : docker rm -f $CONTAINER && sudo rm -rf \"$WORK_DIR\""
