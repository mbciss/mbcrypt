# MBCRYPT - Guide de compilation Windows

Auteur : CISSE Mohamed Bassirou  
Date   : 2026-07-17  
Cible  : PHP 8.0 a 8.5, TS + NTS, x64 (VS16 pour 8.0-8.3, VS17 pour 8.4-8.5)

## Trois scripts de compilation

| Mode | Script | Resultat | Usage |
|------|--------|----------|-------|
| **Extension dynamique (3 versions)** | `build-windows.ps1` | `.dll` TS pour PHP 8.1/8.2/8.3, auto-installees dans Laragon | Laragon, iteration rapide |
| **Extension dynamique (matrice complete)** | `build-windows-all.ps1` | `.dll` TS+NTS pour PHP 8.0 a 8.5 (12 DLLs) | Distribution multi-versions |
| **PHP statique** | `build-windows-static.ps1` | `php.exe` complet, mbcryptload integre | Distribution autonome |

`build-windows-all.ps1` est **autonome** : contrairement a `build-windows.ps1`,
il n'a pas besoin d'un PHP complet installe dans Laragon — il compile
directement contre la bibliotheque (`.lib`) fournie dans chaque devel
pack telecharge, et bascule automatiquement de toolchain (VS2019/vs16
pour PHP 8.0-8.3, VS2022/vs17 pour PHP 8.4-8.5) selon la version cible.

---

## Prerequis

### 1. Visual Studio 2019 Build Tools (PHP 8.0 a 8.3)

Telecharger depuis : https://visualstudio.microsoft.com/fr/downloads/  
Choisir "Build Tools pour Visual Studio 2019"  
Composant requis : **Developpement Desktop en C++** (MSVC v142 + Windows SDK 10)

Chemin attendu par le script :
```
C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\
  VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64\cl.exe
```

### 1bis. Visual Studio 2022 Build Tools (PHP 8.4 et 8.5)

A partir de PHP 8.4, windows.php.net ne publie plus de binaires/devel
packs VS16 : le toolchain minimum devient VS2022 (vs17). Necessaire
uniquement si vous ciblez PHP 8.4/8.5 (`build-windows-all.ps1`).

Telecharger depuis : https://visualstudio.microsoft.com/fr/downloads/  
Choisir "Build Tools pour Visual Studio 2022" (ou Community)  
Meme composant requis : **Developpement Desktop en C++**

Chemin attendu par le script :
```
C:\Program Files\Microsoft Visual Studio\2022\Community\
  VC\Tools\MSVC\14.43.34808\bin\Hostx64\x64\cl.exe
```

> Les deux versions de Visual Studio (2019 et 2022) peuvent coexister
> sur la meme machine sans conflit — `build-windows-all.ps1` selectionne
> le bon toolchain automatiquement selon la version PHP compilee.

### 2. Windows SDK 10

Installe automatiquement avec VS Build Tools (2019 ou 2022).  
Version utilisee : **10.0.22621.0**

Chemin :
```
C:\Program Files (x86)\Windows Kits\10\
```

### 3. Laragon (Apache + PHP)

Telecharger depuis : https://laragon.org/download/  
Fournit deux builds de zlib, un par toolchain :
- zlib VS16 (`httpd-2.4.54-win64-VS16\include` + `lib\zlib.lib`) — pour PHP 8.0-8.3
- zlib VS17 (`httpd-2.4.62-...-win64-VS17\include` + `lib\zlib.lib`) — pour PHP 8.4-8.5

Chemins zlib attendus :
```
C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\include\zlib.h
C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\lib\zlib.lib

C:\laragon\bin\apache\httpd-2.4.62-240904-win64-VS17\include\zlib.h
C:\laragon\bin\apache\httpd-2.4.62-240904-win64-VS17\lib\zlib.lib
```

`build-windows.ps1` a en plus besoin d'installations PHP completes
dans Laragon (`C:\laragon\bin\php\php-8.x.x-Win32-vs16-x64\`) pour
l'auto-copie de la DLL compilee dans `ext\`. `build-windows-all.ps1`
n'en a pas besoin (il se sert du `.lib` du devel pack).

### 4. OpenSSL Win64 (optionnel - pour AES-256-GCM)

Telecharger le binaire Win64 depuis : https://slproweb.com/products/Win32OpenSSL.html  
Prendre **Win64 OpenSSL v3.x.x** (pas Light).

Chemin attendu :
```
C:\Program Files\OpenSSL-Win64\include\openssl\evp.h
C:\Program Files\OpenSSL-Win64\lib\VC\x64\MD\libssl.lib
C:\Program Files\OpenSSL-Win64\lib\VC\x64\MD\libcrypto.lib
```

Sans OpenSSL, le mode AES-256-GCM est desactive. Seul le mode XOR legacy reste disponible.
Cette bibliotheque OpenSSL (liee en `/MD`, UCRT) est partagee entre les
deux toolchains VS2019/VS2022 sans recompilation.

---

## Compilation

### Mode 1 -- Extension dynamique (.dll)

#### Script automatique (recommande)

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build-windows.ps1
```

Le script :
1. Verifie les outils (cl.exe, zlib, OpenSSL)
2. Telecharge les devel packs PHP 8.1.10 / 8.2.15 / 8.3.16 depuis windows.php.net
3. Compile une DLL pour chaque version
4. Copie les DLLs dans `C:\laragon\bin\php\php-x.x.x-...\ext\`

Resultat dans `build\dist\` :
```
php_mbcryptload_php81_ts.dll   (~20 KB)
php_mbcryptload_php82_ts.dll   (~20 KB)
php_mbcryptload_php83_ts.dll   (~20 KB)
```

### Mode 1bis -- Extension dynamique, matrice complete (TS+NTS, PHP 8.0-8.5)

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build-windows-all.ps1
```

Le script :
1. Verifie les deux toolchains (cl.exe VS2019 et VS2022)
2. Pour chacune des 6 versions PHP (8.0.30, 8.1.10, 8.2.15, 8.3.16,
   8.4.23, 8.5.8) et chacun des 2 modes (TS, NTS) :
   - telecharge le devel pack correspondant depuis windows.php.net
     (releases courantes, puis `archives/` en repli pour les versions
     obsoletes)
   - compile en liant directement contre le `.lib` fourni dans le
     devel pack (`lib\php8ts.lib` en TS, `lib\php8.lib` en NTS) —
     aucune installation PHP complete requise
3. Bascule automatiquement de toolchain (vs16/vs17) selon la version

Resultat dans `build\dist\` (12 DLLs) :
```
php_mbcryptload_php80_ts.dll    php_mbcryptload_php80_nts.dll
php_mbcryptload_php81_ts.dll    php_mbcryptload_php81_nts.dll
php_mbcryptload_php82_ts.dll    php_mbcryptload_php82_nts.dll
php_mbcryptload_php83_ts.dll    php_mbcryptload_php83_nts.dll
php_mbcryptload_php84_ts.dll    php_mbcryptload_php84_nts.dll
php_mbcryptload_php85_ts.dll    php_mbcryptload_php85_nts.dll
```

Pour cibler d'autres versions/patchs, modifier le tableau `$MATRIX`
en tete du script (`Ver`, `FullVer`, `Toolset` : `vs16` ou `vs17`).

> Ce script n'installe rien automatiquement dans Laragon (contrairement
> a `build-windows.ps1`) — voir section "Installation dans Laragon"
> plus bas pour le deploiement manuel de la bonne DLL (TS/NTS).

#### Compilation manuelle (une seule version)

Exemple pour PHP 8.3 :

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

set PHP_INC=build\devel-packs\8.3.16\include
set PHP_LIB=C:\laragon\bin\php\php-8.3.16-Win32-vs16-x64\dev\php8ts.lib
set ZLIB_INC=C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\include
set ZLIB_LIB=C:\laragon\bin\apache\httpd-2.4.54-win64-VS16\lib\zlib.lib
set SSL_INC=C:\Program Files\OpenSSL-Win64\include
set SSL_LIB=C:\Program Files\OpenSSL-Win64\lib\VC\x64\MD

cl.exe /LD /MD /Ox /W3 /nologo ^
  /DWIN32 /D_WINDOWS /DWIN64 /DPHP_WIN32 /DZEND_WIN32 ^
  /DZTS /DUSING_ZTS=1 /DZEND_DEBUG=0 ^
  /DCOMPILE_DL_MBCRYPTLOAD /DZLIB_WINAPI ^
  /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE ^
  /DMBCRYPT_HAS_OPENSSL ^
  /I"%PHP_INC%" /I"%PHP_INC%\main" /I"%PHP_INC%\Zend" ^
  /I"%PHP_INC%\TSRM" /I"%PHP_INC%\ext" ^
  /I"%ZLIB_INC%" /I"%SSL_INC%" ^
  mbcrypt\mbcryptload.c mbcrypt\mbcrypt_lib.c ^
  /link /DLL /MACHINE:X64 /INCREMENTAL:NO ^
  /OUT:php_mbcryptload_php83_ts.dll ^
  "%PHP_LIB%" "%ZLIB_LIB%" ^
  "%SSL_LIB%\libssl.lib" "%SSL_LIB%\libcrypto.lib" ^
  kernel32.lib user32.lib ws2_32.lib advapi32.lib
```

---

### Mode 2 -- PHP statique (mbcryptload integre dans php.exe)

Dans ce mode, mbcryptload est compile directement dans `php.exe`.
Aucune ligne `extension=` n'est necessaire dans `php.ini` -- l'extension est
toujours active.

#### Prerequis supplementaires

Le script `build-windows-static.ps1` telecharge automatiquement :
- **PHP SDK binary tools** (github.com/php/php-sdk-binary-tools)
- **Sources PHP** pour chaque version (windows.php.net)
- **Dependances binaires** via `phpsdk_deps` (zlib, openssl, curl, sqlite3...)

Aucun telechargement manuel n'est requis.

#### Lancement

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build-windows-static.ps1
```

> Chaque version PHP prend **10 a 20 minutes** (compilation complete de PHP).

#### Modules compiles dans php.exe

| Module | Mode |
|--------|------|
| mbcryptload | **statique** -- toujours actif |
| zlib, openssl, bcmath, mbstring | statique |
| ctype, filter, hash, tokenizer | statique |
| pdo, sqlite3, pdo_sqlite | statique |
| sockets, fileinfo, curl | statique |
| opcache | partage (.dll) -- ne peut pas etre statique |

#### Resultat dans `build\dist\static\php-8.x\`

```
php.exe              PHP CLI avec mbcryptload integre (~8-12 MB)
php.ini              Pre-configure (mbcryptload.password a renseigner)
ext\
  php_opcache.dll    Seul module restant en .dll
*.dll                Dependances (zlib, openssl, libcurl...)
```

#### Verification

```bat
dist\static\php-8.3\php.exe --ri mbcryptload
dist\static\php-8.3\php.exe -m | findstr mbcryptload
```

La sortie de `--ri mbcryptload` doit afficher le module sans mention de chargement externe.

#### Configuration du mot de passe dans php.ini

```ini
; php.ini (dans dist\static\php-8.3\)
; mbcryptload est statique -- pas de ligne extension= necessaire

mbcryptload.password = "votre-mot-de-passe"

zend_extension=php_opcache.dll
opcache.enable=1
opcache.enable_cli=1
extension_dir=ext
```

---

## Installation dans Laragon (mode extension dynamique)

1. Copier la DLL dans le dossier `ext\` de PHP :
   ```
   copy build\dist\php_mbcryptload_php83_ts.dll
         C:\laragon\bin\php\php-8.3.16-Win32-vs16-x64\ext\php_mbcryptload.dll
   ```

2. Ajouter dans `php.ini` :
   ```ini
   extension=php_mbcryptload
   ```

3. Verifier :
   ```bat
   C:\laragon\bin\php\php-8.3.16-Win32-vs16-x64\php.exe -m | findstr mbcryptload
   C:\laragon\bin\php\php-8.3.16-Win32-vs16-x64\php.exe --ri mbcryptload
   ```

> Avant de copier la DLL, verifier si le PHP cible est TS ou NTS
> (`php.exe -i | findstr "Thread Safety"`) et prendre la DLL
> correspondante — voir `build\dist\USAGE.md` section 0.

---

## Piege connu : globals ZTS et DLL dynamique (corrige)

Jusqu'au 2026-07-17, les DLLs `*_ts.dll` faisaient planter PHP au
demarrage (meme sans appeler aucune fonction de l'extension — un
simple `php -m` suffisait a reproduire). Cause :

- L'extension enregistre ses globals via `ZEND_INIT_MODULE_GLOBALS`
  avec un `globals_dtor` (fonction de nettoyage).
- Ce destructeur est invoque par TSRM tres tard dans l'arret du
  processus (`tsrm_shutdown()`), *apres* que Windows a deja decharge
  la DLL de l'extension (`FreeLibrary`).
- Le pointeur de fonction pointe alors vers une page memoire deja
  liberee -> acces memoire invalide -> crash.

**Correctif** : ne plus enregistrer de `globals_dtor` du tout (`NULL`
passe a `ZEND_INIT_MODULE_GLOBALS`) ; le nettoyage est fait a la main
dans `PHP_MSHUTDOWN_FUNCTION`, qui s'execute pendant que le module est
encore charge. Cela a revele un second bug : le mot de passe stocke
dans les globals etait alloue avec `estrndup()` (memoire **liee a la
requete PHP**), automatiquement recyclee par Zend a la fin de chaque
requete — donc le `efree()` manuel dans MSHUTDOWN devenait un
double-free (`zend_mm_heap corrupted`). Le champ est desormais alloue
avec `pestrndup()`/`pefree()` (memoire **persistante**), coherente
avec le fait qu'il doit survivre a la requete qui l'a definie.

Voir `mbcrypt/ALGORITHME.md` section "Cycle de vie du module" pour le
detail, et le commit `62461a5` pour le correctif complet.

---

## Drapeaux importants

| Drapeau | Raison |
|---------|--------|
| `/MD` | CRT DLL multithread - compatible avec PHP officiel |
| `/DZTS` + `/DUSING_ZTS=1` | Thread-safety pour PHP en mode ZTS |
| `/DZEND_DEBUG=0` | Requis : non defini dans les headers PHP, provoque C2065 sinon |
| `/DZLIB_WINAPI` | zlib compilee avec WINAPI calling convention sur Windows |
| `/DCOMPILE_DL_MBCRYPTLOAD` | Genere ZEND_GET_MODULE() pour chargement dynamique |
| `/DMBCRYPT_HAS_OPENSSL` | Active le code AES-256-GCM (omis si OpenSSL absent) |

---

## Structure des sources

```
mbcrypt/
  mbcrypt_lib.h         Header unifie (format V3, constantes, API publique)
  mbcrypt_lib.c         Bibliotheque core : XOR, zlib, AES-256-GCM
  mbcryptload.c         Extension PHP (hook zend_compile_file, ZTS-safe)
  mbcrypt.c             Encodeur CLI (batch -r, AES/XOR)
  php_mbcryptload.h     Macros PHP (version, module entry, arginfo)
  config.m4             Build Unix (phpize/configure)
  config.w32            Build Windows (PHP SDK, statique et dynamique)
  init_key              Script Perl : regenerer la cle XOR unique
  ALGORITHME.md          Documentation algorithmique du code (FR)

build/
  build-windows.ps1         Extension dynamique (.dll) pour Laragon, PHP 8.1-8.3 TS
  build-windows-all.ps1     Extension dynamique, matrice complete PHP 8.0-8.5 TS+NTS
  build-windows-static.ps1  PHP complet avec mbcryptload statique
  build-ubuntu.sh            Build Linux (extension .so + CLI)
  dist/
    php_mbcryptload_php80_ts.dll   php_mbcryptload_php80_nts.dll
    php_mbcryptload_php81_ts.dll   php_mbcryptload_php81_nts.dll
    php_mbcryptload_php82_ts.dll   php_mbcryptload_php82_nts.dll
    php_mbcryptload_php83_ts.dll   php_mbcryptload_php83_nts.dll
    php_mbcryptload_php84_ts.dll   php_mbcryptload_php84_nts.dll
    php_mbcryptload_php85_ts.dll   php_mbcryptload_php85_nts.dll
    mbcrypt.exe                    Encodeur CLI Windows
    USAGE.md                       Guide d'utilisation
    static/
      php-8.1\php.exe              PHP 8.1 avec mbcryptload statique
      php-8.2\php.exe              PHP 8.2 avec mbcryptload statique
      php-8.3\php.exe              PHP 8.3 avec mbcryptload statique
  devel-packs/               Devel packs PHP TS+NTS (headers + .lib pour build dynamique)
  php-sdk-tools/             PHP SDK binary tools (pour build statique)
  php-static-builds/         Sources PHP et logs de build statique
```

---

## Format de fichier chiffre V3

```
Offset  Taille  Contenu
------  ------  -------
0       9       Tag "MB_ENC_V3"
9       1       Version (3)
10      1       Methode : 0=XOR legacy, 1=AES-256-GCM
11      6       Reserve (zeros)
--- Mode AES uniquement ---
17      16      Sel PBKDF2 (aleatoire par fichier)
33      12      Nonce GCM / IV (aleatoire par fichier)
45      16      Tag d'authentification GCM
--- Donnees ---
61      N       Donnees zlib compressees puis chiffrees
```

Derivation de cle : PBKDF2-SHA256, 100 000 iterations, sel 16 octets -> cle 32 octets.

---

## Methodes de fourniture du mot de passe AES

Trois methodes disponibles, par ordre de priorite decroissante :

| Priorite | Methode | Avantage |
|----------|---------|----------|
| 1 | `mbcryptload_set_password("pass")` dans le code PHP | Flexible, par requete |
| 2 | `mbcryptload.password = "pass"` dans `php.ini` | Aucun code PHP requis, appel direct |
| 3 | Variable d'env `MBCRYPTLOAD_PASSWORD` | Configuration serveur (Apache, FPM) |

La directive `php.ini` est de type `PHP_INI_SYSTEM` : elle ne peut pas etre
surchargee par `ini_set()` ou `.htaccess`.

Dans `--ri mbcryptload`, la valeur configuree apparait comme `set (hidden)`
(jamais exposee en clair).

---

## Notes de securite

- **Ne pas distribuer** `mbcrypt_lib.c` avec la cle XOR d'origine.
  Lancer `perl init_key` depuis `mbcrypt/` pour generer une cle unique avant toute distribution.
- Le mot de passe AES n'est jamais ecrit sur le disque.
  Le configurer via `mbcryptload.password` (php.ini), `mbcryptload_set_password()` (PHP)
  ou `MBCRYPTLOAD_PASSWORD` (variable d'environnement).
- Le mode AES-256-GCM authentifie les donnees : tout fichier corrompu ou falsifie echoue au dechiffrement.
- En mode statique, proteger `php.exe` en lecture seule pour les utilisateurs web.
