# mbpak

Archive PHP chiffrée, **lecture/écriture**, en un seul fichier `.mbpak` :
noms de fichiers et structure de répertoires invisibles (TOC chiffrée),
mise à jour incrémentale sans réécrire toute l'archive.

Prototype issu de la reflexion "Approche C" documentée dans
`../pistes-a-explorer.md`. Sous-projet indépendant : ce dossier `mbpak/`
ne modifie pas `../mbcrypt/` -- il réutilise en lecture seule
`mbcrypt_lib.c`/`mbcrypt_lib.h` (compilés ici en objets locaux) pour
toute la cryptographie (AES-256-GCM + PBKDF2-SHA256 + zlib).

Le montage direct depuis PHP (`mbpak_mount()`, wrapper `mbpak://`) est
implémenté cote extension dans `../mbcrypt/mbcryptload.c`, qui vendorise
une copie synchronisée de `mbpak_lib.c`/`.h` (voir la note en tête de
`../mbcrypt/mbpak_lib.h`) -- ce fichier-ci reste le seul indépendant du
reste de l'extension et peut se lire/tester en isolation.

Le format binaire complet (header, records, TOC, protocole de commit,
limites connues) est détaillé dans [`FORMAT.md`](FORMAT.md).

## Pourquoi

Un `.php` chiffré seul via `mbcrypt` protège le contenu d'un fichier,
mais la structure du dossier (noms, arborescence) reste visible sur le
serveur de production. `.mbpak` empaquette un dossier entier dans un
seul fichier opaque, avec une table des chemins elle-même chiffrée.

## État des composants

| Composant | Statut |
|---|---|
| Format binaire `.mbpak` (header + records + TOC) | Fait, testé |
| Bibliothèque C `mbpak_lib` (create/open/put/get/delete/list/flush/compact/close) | Fait, testé |
| CLI `mbpak` (pack/list/extract/put/get/rm/compact) | Fait, testé |
| Wrapper de flux PHP `mbpak://` + fonctions `mbpak_mount/unmount/list/get/put/delete/compact` | Fait, testé (PHP 8.0-8.5, NTS+ZTS) -- voir `../mbcrypt/mbcryptload.c` |

## Build

Dépendances : zlib (déjà requis par `mbcrypt`) et OpenSSL >= 1.1 (ici
**obligatoire**, pas de mode dégradé -- `.mbpak` est AES-256-GCM
uniquement).

```bash
cd mbpak/
make            # build ./mbpak
make debug      # build avec AddressSanitizer + UBSan
make test       # build puis lance test/run.sh (scenario de bout en bout)
make clean
```

Le Makefile compile `../mbcrypt/mbcrypt_lib.c` en objet local
(`mbcrypt_lib.o`) sans jamais écrire dans `../mbcrypt/`.

### Windows

```powershell
powershell.exe -ExecutionPolicy Bypass -File ..\build\build-windows-mbpak.ps1
```

Produit `..\build\dist\mbpak.exe` (zlib + OpenSSL Win64, un seul binaire
pour toutes les versions de PHP -- `mbpak.exe` n'a aucune dependance
PHP). `mbpak_cli.c` était déjà portable (chemin `#ifdef _WIN32` avec
`FindFirstFileA`/`CreateDirectoryA`), aucune modification de source n'a
été nécessaire. Test fonctionnel équivalent à `test/run.sh` porté sous
Git Bash : voir
`../build/INTEGRATION-MBPAK-WINDOWS/GUIDE-INTEGRATION-COMPLETE.md`
(9/9 PASS, plus la matrice complète 12 versions PHP x NTS/TS du wrapper
`mbpak://`, 12/12 PASS).

## Usage

```bash
# Empaqueter un dossier entier (récursif)
mbpak pack -p "mot-de-passe" -o app.mbpak app/

# Lister le contenu (necessite le mot de passe -- la TOC est chiffree)
mbpak list -p "mot-de-passe" app.mbpak

# Extraire (tout, ou un seul fichier)
mbpak extract -p "mot-de-passe" -o out/ app.mbpak
mbpak extract -p "mot-de-passe" -o out/ app.mbpak lib/auth.php

# Ajouter ou remplacer un fichier -- append, ne reecrit pas l'archive
mbpak put -p "mot-de-passe" app.mbpak lib/auth.php nouveau_auth.php

# Lire un seul fichier (stdout, ou -o pour un fichier)
mbpak get -p "mot-de-passe" app.mbpak lib/auth.php

# Supprimer (tombstone -- l'espace n'est libere qu'au compact)
mbpak rm -p "mot-de-passe" app.mbpak lib/auth.php

# Purger les octets orphelins (anciennes versions, fichiers supprimes)
mbpak compact -p "mot-de-passe" app.mbpak
```

## Garanties vérifiées par `test/run.sh`

- Extraction fidèle : contenu identique bit à bit après pack + extract.
- Mise à jour incrémentale : `put` fait grandir le fichier par append,
  ne réécrit pas les autres records.
- `compact` réduit effectivement la taille du fichier après un `rm`.
- Mauvais mot de passe rejeté (échec d'authentification GCM sur la TOC).
- Altération d'un octet dans le ciphertext d'un fichier : ce fichier
  précis échoue à la lecture, **les autres restent lisibles** --
  propriété directe de l'authentification par fichier indépendante
  (contrairement à un chiffrement global de tout le blob de données).
- Archive vide valide immédiatement après création.
- Refus d'écraser une archive existante.

Vérifié aussi sous AddressSanitizer + UndefinedBehaviorSanitizer
(`make debug`) sur le même scénario : aucune fuite mémoire, aucun UB
détecté.

## Usage depuis PHP

```php
<?php
mbpak_mount("app.mbpak", "mot-de-passe");   // decrypte la TOC immediatement (echec si mauvais mot de passe)

require "mbpak://index.php";                // lecture transparente via le stream wrapper
echo file_get_contents("mbpak://config.php");

var_dump(file_exists("mbpak://index.php")); // true -- url_stat, sans dechiffrer
var_dump(filesize("mbpak://index.php"));    // taille exacte, lue depuis la TOC

mbpak_put("config.php", "<?php return ['db'=>'staging'];\n"); // ecriture -- append + commit immediat
mbpak_delete("lib/old.php");                                   // tombstone
mbpak_compact();                                                // purge les octets orphelins

print_r(mbpak_list());
mbpak_unmount();
```

`file_exists()`/`is_file()`/`filesize()`/`filemtime()` marchent sur les
chemins `mbpak://` sans rien déchiffrer (`url_stat`, taille et date
lues directement dans la TOC) -- c'est ce qui permet à l'autoloader
PSR-4 de Composer (qui appelle `file_exists()` sur chaque chemin
candidat) de fonctionner contre une archive montée. `mbpak_mount()`,
`mbpak_unmount()`, `mbpak_put()` et `mbpak_delete()` appellent chacun
`clearstatcache()` en interne -- sans ça, un `file_exists()` déjà
appelé sur un chemin avant un unmount/write continuerait de renvoyer le
résultat mis en cache par PHP (`ext/standard/filestat.c` mémorise le
dernier `stat()` par nom de fichier, tous wrappers confondus).

Limitation a connaitre : les `require`/`include` **internes** a un
fichier `mbpak://` doivent utiliser le prefixe absolu
(`require "mbpak://lib/auth.php";`), pas `__DIR__ . "/lib/auth.php"` --
`dirname("mbpak://index.php")` renvoie `"mbpak:"` (comportement standard
de PHP sur les chemins a schema URL, deja anticipe dans
`../pistes-a-explorer.md`). Le wrapper est volontairement en lecture
seule : `fopen("mbpak://...", "w")` est refuse -- on ecrit via
`mbpak_put()`, qui reflete le vrai protocole d'append + commit de
l'archive au lieu de simuler un fichier POSIX classique.

Detail de l'implementation cote extension (pieges rencontres compris) :
voir `../pistes-a-explorer.md`, section "Approche C".

## Validé sur une vraie application Laravel

Un projet Laravel fraîchement installé (`composer create-project
laravel/laravel`) a été testé avec `app/` empaqueté en `.mbpak` et
**retiré du disque** (aucun repli possible) : page d'accueil, route
avec contrôleur héritant de la classe de base + requête Eloquent réelle
sur SQLite -- tout fonctionne, avec Composer configuré en mode
classmap-optimisé (défaut de Laravel) *et* en résolution PSR-4
dynamique (celle qui sollicite vraiment `url_stat`). Détail complet,
y compris comment repointer `vendor/composer/autoload_static.php` vers
`mbpak://`, dans `../pistes-a-explorer.md`.

Cette expérience a révélé un vrai bug de passage à l'échelle (corrigé,
voir `FORMAT.md` -- format v3) : `mbpak_get()` dérivait une clé PBKDF2
complète (~100ms) à **chaque fichier lu**, ce qui aurait rendu
inutilisable un `app/` réaliste de plusieurs dizaines de classes. Ce
n'est qu'en chargeant une vraie page, avec de vraies classes
autoloadées, que le problème est devenu visible -- exactement le genre
de défaut qu'un test synthétique à un seul fichier ne révèle pas.

**Suite (`test/laravel-mbpak/`)** : `config/`, `routes/web.php` et
`resources/views/` ont depuis été ajoutés au périmètre empaqueté (en
plus de `app/`) sur un Laravel complet, avec vue Blade compilée et
config chargée depuis l'archive. Ça a motivé l'implémentation de
`dir_opener` (`scandir()`/Symfony Finder fonctionnent sur `mbpak://`) et
a mis au jour deux limitations propres à Laravel, pas à mbpak
(`realpath()` bloque `routes/console.php` et le scan par défaut de
`config/` -- contournable via `php artisan config:cache`, détail complet
et procédure reproductible dans `../test/laravel-mbpak/README.md`).

Ce que ni ce test ni le précédent ne couvrent : `vendor/` (dépendances
Composer, volontairement laissé sur disque -- code open-source, pas de
valeur à cacher, des milliers de fichiers), `storage/`, les caches
`bootstrap/cache/` autres que `config.php` (voir la discussion "quoi
packer / quoi laisser" plus bas dans `pistes-a-explorer.md`).

## Auteur

CISSE Mohamed Bassirou — <mohamedbachir.cisse@gmail.com>
