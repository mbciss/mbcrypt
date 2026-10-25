# Tests mbcryptload

Auteur : CISSE Mohamed Bassirou
Date   : 2026-07-17

## Structure

```
test/
  calcul.php               Programme d'exemple (source, generique)
  calcul.encrypted.php     Meme programme, chiffre AES-256-GCM (mot de passe "mbcrypt-test-2026")
  index.php                Loader generique (mbcryptload_set_password + require)

  php80/  php81/  php82/  php83/  php84/  php85/
    calcul.php              Copie du meme programme d'exemple
    calcul.encrypted.php    Version chiffree AES-256-GCM (mot de passe "mbcrypt-test-2026")
    index.php               Loader -- indique en commentaire la DLL TS/NTS correspondante
```

Chaque dossier `phpXX/` correspond a une des versions ciblees par
`build/build-windows-all.ps1` (8.0.30, 8.1.10, 8.2.15, 8.3.16, 8.4.23,
8.5.8) et sert a verifier que la DLL `php_mbcryptload_phpXX_ts.dll` /
`_nts.dll` correspondante dechiffre et execute correctement le
fichier chiffre sous cette version precise de PHP.

Le contenu du programme d'exemple (`calcul.php`) est identique dans
tous les dossiers, l'algorithme mbcrypt ne depend pas de la version
de PHP, seule la version de l'extension (compilee contre les en-tetes
de chaque version) change. Le blob chiffre produit par `mbcrypt.exe`
est lui aussi identique quelle que soit la version PHP visee.

## Executer un test

```bat
:: Copier la DLL correspondante dans l'extension_dir du PHP a tester
copy build\dist\php_mbcryptload_php83_ts.dll  <php_dir>\ext\php_mbcryptload.dll

:: Lancer
cd test\php83
<php_dir>\php.exe -d extension=php_mbcryptload index.php
```

Sortie attendue : le programme s'execute normalement (Fibonacci,
nombres premiers, factorielle, puissance modulaire, statistiques),
termine par `OK - calcul termine.`, sans avertissement ni erreur.

## Resultats verifies (2026-07-17)

Execution reelle de `index.php` (mbcryptload_set_password +
dechiffrement + calcul), DLL TS, apres le correctif du crash au
shutdown (voir `mbcrypt/ALGORITHME.md` section 7) :

| Version   | Toolchain | Runtime utilise                                  | Resultat |
|-----------|-----------|---------------------------------------------------|----------|
| PHP 8.0.30| vs16      | Runtime temporaire (non fourni dans le depot)      | OK |
| PHP 8.1.10| vs16      | Laragon (`php-8.1.10-Win32-vs16-x64`)              | OK |
| PHP 8.2.15| vs16      | Laragon (`php-8.2.15-Win32-vs16-x64`)              | OK |
| PHP 8.3.16| vs16      | Laragon (`php-8.3.16-Win32-vs16-x64`)              | OK |
| PHP 8.4.23| vs17      | Runtime temporaire (non fourni dans le depot)      | OK |
| PHP 8.5.8 | vs17      | Laragon (`php-8.5.8-Win32-vs17-x64`)               | OK |

Les variantes NTS n'etaient pas verifiees a l'execution ici a cette
date (aucun runtime PHP NTS installe localement), seule la compilation
etait verifiee pour ces 6 DLLs. **Comble le 2026-07-22** : voir
`build/INTEGRATION-MBPAK-WINDOWS/GUIDE-INTEGRATION-COMPLETE.md`, qui
telecharge les binaires PHP NTS complets (pas seulement les devel packs)
et verifie les 12 combinaisons (NTS **et** TS) a l'execution reelle,
12/12 PASS -- plus le wrapper `mbpak://`, egalement 12/12 PASS.

## Resultats verifies -- Linux (2026-07-17)

Meme jeu de tests, execute via `build/build-linux-docker.sh` (voir
`build/GUIDE-COMPILATION-LINUX.md`) dans les images Docker officielles
`php:8.X-cli`/`php:8.X-zts` (bullseye pour 8.0, bookworm pour 8.1-8.5) :
`mbcryptload_set_password()` + dechiffrement AES-256-GCM reel +
execution complete de `calcul.encrypted.php` jusqu'a
`OK - calcul termine.`, pour les 12 combinaisons (TS **et** NTS,
contrairement aux tests Windows ci-dessus qui ne couvrent que TS) :

| Version   | NTS | ZTS |
|-----------|-----|-----|
| PHP 8.0.x | OK  | OK  |
| PHP 8.1.x | OK  | OK  |
| PHP 8.2.x | OK  | OK  |
| PHP 8.3.x | OK  | OK  |
| PHP 8.4.x | OK  | OK  |
| PHP 8.5.x | OK  | OK  |

Cette verification a mis au jour un bug reel corrige le meme jour :
`mbcrypt/config.m4` ne propageait pas la macro `MBCRYPT_HAS_OPENSSL` a
`mbcrypt_lib.c` lors d'une compilation `phpize` (elle n'atteignait que
`mbcryptload.c` via `config.h`), si bien que **tout `.so` Linux compile
avant ce correctif ne pouvait pas dechiffrer les fichiers AES-256-GCM**. `phpinfo()`/`--ri` affichait pourtant `AES-256-GCM => enabled` de
maniere trompeuse. Le mode XOR legacy n'etait pas affecte. Detail
complet dans `build/GUIDE-COMPILATION-LINUX.md` section "Bug corrige".
