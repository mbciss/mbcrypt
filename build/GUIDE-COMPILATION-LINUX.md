# MBCRYPT - Guide de compilation Linux

Auteur : CISSE Mohamed Bassirou
Date   : 2026-07-17
Cible  : PHP 8.0 a 8.5, NTS + ZTS, x86_64

---

## Reponse directe a la question de portabilite

**Non**, un `.so` compile sur une distribution Linux ne fonctionne pas
forcement sur toutes les autres, mais **ce n'est pas non plus "une
compilation par distribution"**. Ce qui determine la compatibilite d'un
`mbcryptload.so`, ce sont quatre choses independantes de la distribution
elle-meme :

| Facteur                         | Doit correspondre entre build et cible |
|----------------------------------|------------------------------------------|
| **Architecture CPU**             | x86_64 / aarch64 / etc. -- pas de mélange |
| **Version mineure de PHP**       | l'ABI Zend (`Zend Module API No`) change a chaque version mineure (8.1, 8.2, 8.3...) |
| **NTS vs ZTS**                   | thread-safety choisie a la compilation de PHP lui-meme |
| **Version de glibc**             | la cible doit avoir une glibc **egale ou plus recente** que celle utilisee pour compiler |

La "distribution" (Ubuntu, Debian, CentOS, Alpine...) n'apparait dans
aucune de ces quatre lignes en tant que telle -- elle n'intervient que
parce qu'elle fixe indirectement la version de glibc (et, pour Alpine,
remplace glibc par musl, une ABI totalement differente).

**En pratique** :

- Un `.so` compile pour "PHP 8.3 NTS x86_64" sur **Debian 12 (bookworm)**
  fonctionnera tel quel sur Ubuntu 22.04/24.04/26.04, Debian 11/12/13,
  et toute distribution avec une glibc >= celle de bookworm (2.36) --
  **a condition que le PHP cible soit lui aussi une build 8.3 NTS
  x86_64** (peu importe qui l'a compile : Ondrej, le paquet officiel
  Ubuntu, une build maison...).
- Ce meme `.so` **ne fonctionnera pas** sur PHP 8.2 ou 8.4 (ABI Zend
  differente -- PHP refuse de charger l'extension avec une erreur
  explicite `PHP api version X.X.X does not match Y.Y.Y`), ni sur PHP
  8.3 ZTS s'il a ete compile en NTS (crash au chargement ou refus),
  ni sur Alpine (musl != glibc), ni sur une distribution avec une glibc
  **plus ancienne** que bookworm (erreur `GLIBC_2.36 not found`).
- Un `.so` compile sur une base **plus ancienne** (ex: Debian 11
  bullseye, glibc 2.31) tourne en revanche sans probleme sur des
  distributions plus recentes (bookworm, Ubuntu 24.04/26.04...) : la
  compatibilite glibc est **ascendante uniquement**, jamais
  retrograde.

**Conclusion pratique** : il ne faut pas compiler "une fois par
distribution", mais **une fois par combinaison (version PHP mineure x
NTS/ZTS x architecture)**, en choisissant comme machine/image de build
la base glibc **la plus ancienne encore supportee** parmi vos cibles.
C'est exactement la logique derriere les roues "manylinux" de
l'ecosysteme Python, et c'est la strategie retenue ici (voir "Pourquoi
Docker" plus bas) : chaque `.so` de ce depot est compile dans une
image Debian bullseye (PHP 8.0) ou bookworm (PHP 8.1-8.5), pas sur la
machine de developpement elle-meme, precisement pour garantir cette
portabilite ascendante vers toute distribution plus recente.

Le seul cas ou une compilation par distribution est réellement
necessaire : une **distribution avec une glibc plus ancienne** que la
base de build (RHEL 7 / CentOS 7 par exemple, glibc 2.17), ou une
distribution **musl** (Alpine). Voir section "Cas particuliers"
plus bas.

---

## Deux methodes de compilation

| Methode | Script | Avantage | Inconvenient |
|---------|--------|----------|---------------|
| **Docker (recommandee)** | `build-linux-docker.sh` | Aucun droit root sur l'hote, matrice complete 8.0-8.5 x NTS+ZTS reproductible, glibc la plus ancienne possible (portabilite max) | Necessite Docker |
| **Native (systeme hote)** | `build-ubuntu.sh` | Pas de Docker requis | Compile seulement contre les PHP deja installes sur la machine ; necessite le PPA `ondrej/php` pour couvrir 8.0-8.5 en parallele sur Ubuntu/Debian |

---

## Methode recommandee : `build-linux-docker.sh`

### Prerequis

- Docker Engine installe, utilisateur dans le groupe `docker` (pas de
  `sudo` necessaire pour lancer le script lui-meme) :
  ```bash
  sudo usermod -aG docker "$USER"   # une seule fois, necessite un droit root ponctuel
  # puis se reconnecter (ou : newgrp docker)
  ```
- Acces reseau (le script telecharge les images officielles `php:*` et
  `debian:bookworm` depuis Docker Hub, puis `zlib1g-dev`/`libssl-dev`
  depuis les depots Debian a l'interieur des conteneurs).

### Lancement

```bash
bash build/build-linux-docker.sh
```

### Ce que fait le script

Pour chacune des 6 versions PHP (8.0 a 8.5) et chacun des 2 modes
(NTS, ZTS), le script :

1. Lance un conteneur ephemere a partir de l'image officielle
   correspondante (`php:8.X-cli-bookworm` pour NTS, `php:8.X-zts-bookworm`
   pour ZTS -- `bullseye` pour PHP 8.0, qui n'a plus d'image `bookworm`
   publiee) ;
2. Installe `zlib1g-dev` et `libssl-dev` dans le conteneur (les images
   officielles PHP fournissent deja `phpize`, `php-config`, `gcc`,
   `make` -- necessaires a `docker-php-ext-install`, donc presents
   nativement) ;
3. Copie les sources de `mbcrypt/`, execute `phpize && ./configure
   --enable-mbcryptload && make` ;
4. Recupere le `.so` produit dans `build/dist-linux/`, le "strip" et
   le rend proprietaire de l'utilisateur hote (pas de fichiers
   `root:root` residuels).

Puis compile `mbcrypt` (l'encodeur CLI) en **liaison statique**
(`make static`, deja present dans `mbcrypt/Makefile`) dans une image
`debian:bookworm` generique -- un binaire independant de toute version
de PHP, sans dependance dynamique a `libz`/`libssl` a l'execution, donc
utilisable tel quel sur n'importe quelle distribution x86_64 (voir
"Pourquoi lier le CLI statiquement" plus bas).

### Resultat dans `build/dist-linux/`

```
mbcryptload_php80_nts.so   mbcryptload_php80_zts.so
mbcryptload_php81_nts.so   mbcryptload_php81_zts.so
mbcryptload_php82_nts.so   mbcryptload_php82_zts.so
mbcryptload_php83_nts.so   mbcryptload_php83_zts.so
mbcryptload_php84_nts.so   mbcryptload_php84_zts.so
mbcryptload_php85_nts.so   mbcryptload_php85_zts.so
mbcrypt                    (CLI, lie statiquement)
```

### Pourquoi Docker plutot que le systeme hote

- **Aucun droit root requis pour compiler** : seule l'appartenance au
  groupe `docker` (deja accordee une fois) est necessaire -- pas de
  `sudo apt install`, pas d'ajout de PPA sur la machine de build.
- **Reproductible** : n'importe qui, sur n'importe quelle distribution
  hote (meme Windows/macOS avec Docker Desktop), obtient des binaires
  identiques, compiles contre les vraies images officielles PHP.
- **Portabilite maximale** : bookworm (PHP 8.1-8.5) et bullseye (PHP 8.0)
  embarquent une glibc plus ancienne que la plupart des serveurs de
  production actuels -- les `.so` produits fonctionnent donc aussi bien
  sur ces bases que sur des distributions plus recentes (voir section
  portabilite ci-dessus).
- Cette machine de developpement elle-meme ne contient **aucun**
  PHP installe (`which php` echoue) et pas d'acces `sudo` interactif --
  Docker est la seule methode qui permette de compiler quoi que ce soit
  ici sans intervention de l'utilisateur.

---

## Methode alternative : `build-ubuntu.sh` (native, systeme hote)

### Prerequis

```bash
sudo apt update && sudo apt install -y \
    build-essential zlib1g-dev libssl-dev
```

Ubuntu/Debian ne fournissent qu'**une seule version de PHP a la fois**
dans les depots officiels. Pour installer 8.0 a 8.5 en parallele
(paquets `php8.0-dev` a `php8.5-dev`), ajouter le PPA communautaire
`ondrej/php` (largement utilise, y compris par les images Docker
officielles indirectement) :

```bash
sudo add-apt-repository ppa:ondrej/php
sudo apt update
sudo apt install -y php8.0-dev php8.1-dev php8.2-dev php8.3-dev php8.4-dev php8.5-dev
```

> Sur Debian (pas Ubuntu), utiliser le depot `packages.sury.org/php`
> a la place (equivalent d'ondrej/php pour Debian).

Ce PPA installe les paquets NTS par defaut. Pour obtenir aussi une
variante ZTS, installer `libapache2-mod-php8.x` (qui embarque un
`php-config` ZTS) ou compiler PHP soi-meme avec `--enable-zts` --
`build-ubuntu.sh` ne gere que la variante NTS/ZTS deja installee sur
la machine, une seule a la fois par version.

### Lancement

```bash
bash build/build-ubuntu.sh
```

Le script detecte automatiquement les `phpizeX.Y`/`php-configX.Y`
presents (ignore silencieusement les versions non installees) et
produit `build/dist/mbcryptload_php8X.so` + `build/dist/mbcrypt`.

---

## Cas particuliers

### RHEL / CentOS / Rocky / Alma (glibc plus ancienne)

Le depot officiel PHP (`remi/php-8.x`) ou l'image Docker `php:8.x-cli`
n'ont pas d'equivalent RHEL 7/8 direct pour les versions PHP les plus
recentes. Deux options :

1. Compiler directement sur (ou dans un conteneur de) la distribution
   cible -- `docker run --rm rockylinux:8 ...` avec le depot Remi ;
2. Utiliser une base de build encore plus ancienne dans
   `build-linux-docker.sh` (modifier le tag d'image, par exemple
   `php:8.3-cli-bullseye` au lieu de `-bookworm`, voire compiler soi-meme
   PHP sur une base `centos:7`) si la glibc de la cible est plus
   ancienne que bullseye (2.31).

### Alpine Linux (musl, pas glibc)

musl et glibc sont deux implementations de la libc **binairement
incompatibles** : un `.so` compile contre glibc (meme la plus ancienne)
ne charge pas sur un PHP Alpine. Il faut recompiler specifiquement
contre une image `php:8.x-cli-alpine` (disponible officiellement pour
toutes les versions 8.0-8.5). Ajouter au script :

```bash
docker run --rm -v "$SRC_DIR":/src:ro -v "$DIST_DIR":/out \
    php:8.3-cli-alpine sh -c '
        apk add --no-cache $PHPIZE_DEPS zlib-dev openssl-dev
        cp /src/mbcryptload.c /src/mbcrypt_lib.c /src/mbcrypt_lib.h \
           /src/php_mbcryptload.h /src/config.m4 /build/ 2>/dev/null || mkdir -p /build && cp /src/*.c /src/*.h /src/config.m4 /build/
        cd /build && phpize && ./configure --enable-mbcryptload && make
    '
```

### ARM / autres architectures

Meme logique : l'architecture CPU du `.so` doit correspondre a celle
du binaire `php` cible. Docker permet de cross-compiler via
`docker run --platform linux/arm64 ...` (QEMU, plus lent) ou en
lancant directement le script sur une machine ARM (build native, plus
rapide) -- aucune autre modification necessaire, phpize/php-config
detectent l'architecture automatiquement.

---

## Pourquoi lier le CLI `mbcrypt` statiquement

Contrairement a l'extension `.so` (qui **doit** etre dynamique -- c'est
le mecanisme meme d'un module PHP), le programme `mbcrypt` (encodeur
hors-ligne) n'a aucune contrainte de ce type : c'est un simple
executable. `mbcrypt/Makefile` fournit deja une cible `make static`
(`LDFLAGS += -static`).

Un binaire statique n'a **aucune dependance a la version de glibc, de
zlib ou d'OpenSSL de la machine cible** -- il embarque tout. C'est le
choix le plus simple pour distribuer un seul `mbcrypt` qui fonctionne
sur n'importe quelle distribution x86_64, sans se soucier des
bibliotheques installees sur le poste qui chiffre les fichiers (souvent
la machine du developpeur ou un pipeline CI, distincte du serveur qui
execute le PHP dechiffre).

> Nuance technique : un binaire glibc statique n'est **pas totalement
> exempt** de sensibilite a l'environnement (resolution NSS/DNS,
> `getpwnam`...), mais `mbcrypt` ne fait ni reseau ni resolution de nom
> d'utilisateur -- uniquement de la lecture/ecriture de fichiers et du
> calcul cryptographique. Le lien statique est donc sans risque ici.

---

## Bug corrige (2026-07-17) : AES-256-GCM silencieusement absent du `.so` Linux

En verifiant la compilation Linux de bout en bout (compilation +
dechiffrement reel, jamais fait avant sur cette machine -- voir
`pistes-a-explorer.md`), le chiffrement AES-256-GCM s'est revele
**totalement non fonctionnel** dans tout `.so` produit par `phpize` /
`config.m4`, alors meme que `php --ri mbcryptload` affichait
`AES-256-GCM => enabled`.

### Cause

`mbcrypt_lib.c` est ecrit pour etre independant de PHP (partage tel
quel avec le CLI `mbcrypt.c`) et **n'inclut pas `config.h`**. Son code
AES-256-GCM est protege par `#ifdef MBCRYPT_HAS_OPENSSL`, macro
attendue en ligne de commande du compilateur.

- Le **CLI** (`mbcrypt/Makefile`) passe bien `-DMBCRYPT_HAS_OPENSSL`
  a `$(CFLAGS)`, utilise pour compiler `mbcrypt.c` **et**
  `mbcrypt_lib.c` -> fonctionne.
- Les **scripts Windows** (`build-windows*.ps1`) passent
  `/DMBCRYPT_HAS_OPENSSL` explicitement sur la ligne de commande
  `cl.exe`, appliquee aux deux fichiers -> fonctionne.
- **`mbcrypt/config.m4`** (utilise par `phpize`/`build-ubuntu.sh`/
  `build-linux-docker.sh`) ne definissait la macro que via
  `AC_DEFINE([MBCRYPT_HAS_OPENSSL])`, qui atterrit dans `config.h` --
  **inclus uniquement par `mbcryptload.c`**, jamais par
  `mbcrypt_lib.c`. Resultat : `mbcryptload.c` voit la macro (d'ou
  l'affichage trompeur "enabled" dans `--ri`), mais l'objet compile de
  `mbcrypt_lib.c` ne contient tout simplement pas le code AES -- sa
  fonction `mbcrypt_lib_decode()` prend la branche
  `#else return NULL;` pour **tout** fichier AES-256-GCM. Le mode XOR
  legacy, qui ne depend pas de cette macro, n'etait pas affecte.

### Correctif

`config.m4` passe desormais `-DMBCRYPT_HAS_OPENSSL` explicitement via
le parametre `extra_cflags` de la macro autoconf `PHP_NEW_EXTENSION`
(le meme mecanisme qu'utilise deja PHP en interne pour injecter
`-DZEND_COMPILE_DL_EXT=1`), de sorte que **les deux** fichiers sources
recoivent la macro a la compilation, quel que soit `config.h`.

### Verification

Isole via un harnais de test direct (`mbcrypt_lib_encode` +
`mbcrypt_lib_decode` en memoire, puis ecriture/lecture disque, tous
deux concluants hors PHP) puis via des traces `fprintf(stderr, ...)`
temporaires dans une copie de build, confirmant que
`aes_gcm_decrypt()` n'etait jamais appelee avant le correctif. Apres
correctif, verifie de bout en bout : `mbcrypt -p PASS -o enc.php
src.php` puis `php -d extension=mbcryptload.so ... require enc.php`
produit la sortie en clair attendue, pour les 12 combinaisons
PHP 8.0-8.5 x NTS/ZTS (voir section "Verification" en fin de document
pour le detail par version).

**Impact pour les utilisateurs existants** : toute extension `.so`
compilee avant ce correctif (via `build-ubuntu.sh` ou une compilation
manuelle `phpize`) doit etre **recompilee** pour que le mode AES-256-GCM
fonctionne reellement sur Linux -- le mode XOR legacy, lui, a toujours
fonctionne. Les builds Windows (`.dll`) ne sont pas affectees par ce
bug (mecanisme de compilation different, deja verifie).

---

## Installation de l'extension

### 1. Identifier la version et le mode (NTS/ZTS) du PHP cible

```bash
php -v                                  # version exacte
php -i | grep "Thread Safety"           # enabled = ZTS, disabled = NTS
php -i | grep ^extension_dir            # dossier des extensions
```

### 2. Copier le `.so` correspondant

```bash
sudo cp build/dist-linux/mbcryptload_php83_nts.so \
    "$(php -i | grep ^extension_dir | awk '{print $3}')/mbcryptload.so"
```

### 3. Activer dans `php.ini`

```bash
echo "extension=mbcryptload" | sudo tee /etc/php/8.3/cli/conf.d/20-mbcryptload.ini
# Pour FPM egalement :
echo "extension=mbcryptload" | sudo tee /etc/php/8.3/fpm/conf.d/20-mbcryptload.ini
sudo systemctl restart php8.3-fpm
```

> Chemins `/etc/php/8.3/...` valables pour les paquets Ubuntu/Debian
> (natifs ou PPA ondrej). Adapter pour d'autres distributions
> (`/etc/php.d/` sur RHEL/CentOS avec Remi, etc.).

### 4. Verifier

```bash
php -m | grep mbcryptload
php --ri mbcryptload
```

La sortie doit afficher `AES-256-GCM => enabled` -- et, avec le
correctif ci-dessus, cette fois le mode fonctionne reellement.

---

## Mode d'emploi (chiffrement et execution)

### Chiffrer un fichier ou un dossier

```bash
# Mode XOR legacy (pas de mot de passe)
./build/dist-linux/mbcrypt -o output.php source.php

# Mode AES-256-GCM (recommande)
./build/dist-linux/mbcrypt -p "mon-mot-de-passe" -o output.php source.php

# Dossier entier, recursif
./build/dist-linux/mbcrypt -r -p "mon-mot-de-passe" -o app_chiffree/ app_source/
```

### Fournir le mot de passe AES a l'execution (trois methodes)

Par ordre de priorite decroissante :

1. **Dans le code PHP**, avant tout `require` du fichier chiffre :
   ```php
   <?php
   mbcryptload_set_password("mon-mot-de-passe");
   require "module_chiffre.php";
   ```

2. **Directive `php.ini`** (recommande pour un appel direct sans
   fichier d'amorce) :
   ```ini
   mbcryptload.password = "mon-mot-de-passe"
   ```
   ```bash
   php fichier_chiffre.php   # fonctionne directement
   ```
   `PHP_INI_SYSTEM` : protege contre toute surcharge par `ini_set()`
   ou `.htaccess`. Proteger la lecture de `php.ini` (`chmod 640`,
   groupe dedie) puisque le mot de passe y est en clair.

3. **Variable d'environnement**, utile pour Apache/FPM sans toucher
   au code :
   ```bash
   export MBCRYPTLOAD_PASSWORD="mon-mot-de-passe"
   php script.php
   ```
   ```apache
   # Apache
   SetEnv MBCRYPTLOAD_PASSWORD "mon-mot-de-passe"
   ```
   ```ini
   ; PHP-FPM (pool www.conf)
   env[MBCRYPTLOAD_PASSWORD] = "mon-mot-de-passe"
   ```
   Visible via `phpinfo()` et les outils systeme (`ps`, `/proc`) --
   la moins sure des trois methodes.

### API PHP

```php
mbcryptload_set_password(string $password) : bool
mbcryptload_encrypt_file(string $src, string $dst) : bool                              // XOR
mbcryptload_encrypt_file(string $src, string $dst, string $password, true) : bool      // AES
mbcryptload_version() : string   // "3.0.0"
```

Voir `mbcrypt/ALGORITHME.md` pour le detail du format V3 et du
mecanisme d'interception, et `pistes-a-explorer.md` pour les
evolutions envisagees (empaquetage en archive unique).

---

## Verification

Tests executes le 2026-07-17 avec `build-linux-docker.sh`, apres le
correctif AES-256-GCM ci-dessus (matrice complete, temps de build :
~19 min pour les 12 `.so` + le CLI statique, dont l'essentiel en
telechargement des images la premiere fois) :

### 1. Compilation

Les 12 combinaisons PHP 8.0-8.5 x (NTS, ZTS) compilent sans
avertissement. Chaque `.so` fait ~23-24 Ko.

### 2. Dechiffrement reel (AES-256-GCM), bout en bout

Pour chacune des 12 combinaisons, dans le conteneur officiel
correspondant (`php:8.X-cli-bookworm`/`bullseye` ou
`php:8.X-zts-bookworm`/`bullseye`) : le `.so` produit est charge, le
fichier deja commite `test/php8X/calcul.encrypted.php` (chiffre
AES-256-GCM, mot de passe `mbcrypt-test-2026`) est dechiffre via
`mbcryptload_set_password()` + `require`, et le script s'execute
jusqu'a `OK - calcul termine.` sans erreur :

| Version PHP | NTS | ZTS |
|---|---|---|
| 8.0 | OK | OK |
| 8.1 | OK | OK |
| 8.2 | OK | OK |
| 8.3 | OK | OK |
| 8.4 | OK | OK |
| 8.5 | OK | OK |

Resultat : **12 PASS / 0 FAIL**. C'est la premiere verification
end-to-end reussie du mode AES-256-GCM sur Linux pour ce projet
(auparavant seul le build Windows avait ete verifie a l'execution --
voir `pistes-a-explorer.md`).

### 3. CLI `mbcrypt` statique

```
$ file build/dist-linux/mbcrypt
ELF 64-bit LSB executable, x86-64, statically linked, stripped
$ ldd build/dist-linux/mbcrypt
	not a dynamic executable
$ build/dist-linux/mbcrypt -v
mbcrypt 3.0.0
AES-256-GCM: enabled (OpenSSL)
```

Execute sans erreur sur la machine de developpement elle-meme, qui ne
possede **ni PHP, ni libssl-dev, ni pkg-config installes** -- preuve
concrete qu'un binaire statique ne depend d'aucune bibliotheque de la
machine cible.

---

## Structure des sources (rappel)

```
mbcrypt/
  mbcrypt_lib.c/.h       Bibliotheque core, independante de PHP
  mbcryptload.c          Extension PHP (hook zend_compile_file)
  mbcrypt.c               Encodeur CLI
  config.m4               Build Unix (phpize/configure) -- corrige ci-dessus
  Makefile                 Build du CLI (make / make static)

build/
  build-ubuntu.sh           Build natif, PHP installes sur l'hote (8.0-8.5)
  build-linux-docker.sh     Build matrice complete via Docker (recommande)
  dist/                     Sortie de build-ubuntu.sh
  dist-linux/               Sortie de build-linux-docker.sh
  GUIDE-COMPILATION-LINUX.md    Ce document
  GUIDE-COMPILATION-WINDOWS.md  Equivalent Windows
```
