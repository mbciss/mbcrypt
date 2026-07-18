# Pistes a explorer -- MBCRYPT

Auteur : CISSE Mohamed Bassirou  
Date   : 2026-04-08

---

## Idee principale : empaquetage d'un dossier source PHP en un seul fichier

### Probleme actuel

La distribution actuelle expose la structure interne de l'application :

```
app/
  index.php          (chiffre)
  lib/auth.php       (chiffre)
  lib/db.php         (chiffre)
  config.php         (chiffre)
```

Le client voit la structure, peut compter les fichiers, lire les noms,
et potentiellement remplacer des fichiers individuels.

### Objectif

```
app.mbpak            (un seul fichier opaque, illisible)
```

---

## Approche A -- Archive chiffree montee en memoire (stream wrapper)

A l'execution, PHP ouvre `app.mbpak`, le dechiffre en memoire, et expose
les fichiers via un stream wrapper PHP personnalise (`mbpak://`).

```
app.mbpak
  |
  | ouverture par l'extension mbcryptload
  v
mbpak://index.php        -> PHP execute depuis la memoire
mbpak://lib/auth.php     -> require "mbpak://lib/auth.php"
```

### Mecanisme

- L'archive n'est jamais extraite sur le disque
- Les chemins reels n'existent pas en tant que fichiers physiques
- PHP supporte les stream wrappers custom via `stream_wrapper_register()`
- L'extension enregistre le wrapper `mbpak://` au MINIT

### Inconvenients

- Les `require/include` internes doivent utiliser le prefixe `mbpak://`
  ou il faut un auto-loader transparent
- Complexite moderee cote extension C

---

## Approche B -- PHAR natif + mbcrypt (solution hybride)

Utiliser le format d'archive PHP natif (PHAR) et chiffrer les fichiers
internes avec mbcrypt avant de les y inserer.

```
app.phar             <- archive PHP native, fichiers chiffres en interne
  |
  v
php app.phar         <- s'execute directement via le stub PHAR
```

### Mecanisme

1. Empaqueter les sources chiffrees dans un `.phar`
2. Le stub (bootstrap interne au PHAR) appelle `mbcryptload_set_password()`
   avant que PHP charge les fichiers internes
3. mbcryptload dechiffre chaque fichier a la volee via son hook compile_file

### Avantages

- PHAR est natif PHP, reconnu par le serveur web sans configuration
- Tooling existant (`phar.phar`, `Box`, etc.) pour creer l'archive
- Aucun nouveau format a inventer

### Inconvenients

- Le format PHAR est connu et des outils existent pour l'ouvrir
- Les noms de fichiers et la structure restent visibles dans le PHAR
- La protection repose entierement sur le chiffrement mbcrypt des fichiers internes

---

## Approche C -- Format .mbpak custom (solution maximale)

Nouveau format binaire propre a mbcrypt, concu pour l'hermeticite totale.

### Format binaire propose

```
[16 octets]  Header   : magic "MBPAK_V1" + version + flags
[4 octets]   TOC_SIZE : taille de la table des chemins chiffree
[TOC_SIZE]   TOC      : table des chemins + offsets, chiffree AES-256-GCM
[N octets]   DATA     : fichiers concatenes, chiffres AES-256-GCM global
```

La TOC (Table Of Contents) chiffree est la vraie valeur ajoutee :
meme en ouvrant le fichier en hexadecimal, on ne voit ni les noms
ni le nombre de fichiers.

### Mecanisme

L'extension PHP enregistre un stream wrapper `mbpak://` au MINIT.
Le serveur web pointe vers un `index.php` non chiffre qui monte l'archive :

```php
<?php
mbpak_mount("app.mbpak", "votre-mot-de-passe");
require "mbpak://index.php";
```

Ou avec `mbcryptload.password` dans `php.ini` et `auto_prepend_file`,
meme ce fichier bootstrap disparait.

### Travail a realiser

1. `mbcrypt.exe` -- nouveau mode `-pack` pour creer un `.mbpak`
   ```
   mbcrypt.exe -pack -p "password" -o app.mbpak app/
   ```
2. `mbcrypt_lib.c` -- fonctions `mbpak_pack()` / `mbpak_unpack()` / `mbpak_read_file()`
3. `mbcryptload.c` -- enregistrer le stream wrapper `mbpak://` au MINIT
4. Nouvelles fonctions PHP exposees :
   - `mbpak_mount(string $file, string $password) : bool`
   - `mbpak_unmount() : bool`
   - `mbpak_list() : array`  (optionnel, pour debug)

### Avantages

- Un seul fichier a distribuer
- Structure et noms de fichiers invisibles (TOC chiffree)
- Aucune extraction sur le disque
- Coherent avec l'existant (meme cle, meme mot de passe AES)

### Inconvenients

- Developpement le plus long (nouveau format + stream wrapper C)
- Les erreurs PHP affichent des chemins `mbpak://` (a gerer proprement)

---

## Comparatif des trois approches

| Critere                   | A -- Stream wrapper | B -- PHAR + mbcrypt | C -- .mbpak custom |
|---------------------------|---------------------|---------------------|--------------------|
| Nb de fichiers visibles   | 1                   | 1                   | 1                  |
| Noms de fichiers visibles | non                 | oui (dans phar)     | non                |
| Structure visible         | non                 | oui                 | non                |
| Outillage existant        | a creer             | phar natif          | a creer            |
| Complexite implementation | moyenne             | faible              | elevee             |
| Protection                | bonne               | bonne               | maximale           |
| Require internes          | prefixe mbpak://    | transparent         | prefixe mbpak://   |

---

## Recommandation

L'**approche C (.mbpak custom)** est la plus coherente avec l'architecture
existante de mbcrypt. Elle etend simplement la logique de "fichier chiffre"
a "archive chiffree". La TOC chiffree est la vraie valeur ajoutee par
rapport a PHAR.

L'**approche B (PHAR)** est la plus rapide a prototyper si on veut tester
le concept rapidement sans modifier l'extension.

---

## Autres pistes independantes

### Directive php.ini mbcryptload.password
- **Statut : implemente**
- Permet d'appeler un fichier chiffre directement sans `require` ni code intermediaire
- `PHP_INI_SYSTEM` : non surchargeable par ini_set() ou .htaccess

### Build PHP statique
- **Statut : implemente et verifie (2026-07-17)**
- mbcryptload compile directement dans php.exe (confirme via `--ri mbcryptload`,
  `-m`, et dechiffrement reel de `test/php83/calcul.encrypted.php` -- y compris
  appel direct du fichier chiffre avec mot de passe en php.ini, sans loader)
- Aucune ligne `extension=` necessaire
- Utile pour distribution d'un PHP autonome avec protection integree
- Le script `build-windows-static.ps1` ne s'etait en realite jamais execute
  avec succes avant cette date (aucune trace, aucun artefact) malgre ce
  statut. Six bugs bloquants corriges pour obtenir un build reel -- detail
  dans `build/GUIDE-COMPILATION-WINDOWS.md` section "Piege connu : build
  PHP statique et outillage SDK".

### Arginfo PHP 8.x
- **Statut : implemente**
- Suppression des warnings "Missing arginfo"
- Types de retour declares pour mbcryptload_set_password, mbcryptload_encrypt_file, mbcryptload_version

### Support Ubuntu / Linux
- **Statut : implemente et verifie (2026-07-17)**
- Script `build-ubuntu.sh` (natif, PHP installes sur l'hote) : detection
  automatique des versions PHP installees, etendu a 8.0-8.5
- Script `build-linux-docker.sh` (nouveau) : matrice complete PHP 8.0-8.5
  x NTS+ZTS (12 `.so`) + CLI `mbcrypt` statique, via Docker -- sans droit
  root sur l'hote, reproductible, glibc la plus ancienne possible pour
  une portabilite ascendante maximale (voir
  `build/GUIDE-COMPILATION-LINUX.md`)
- Dechiffrement AES-256-GCM reel verifie de bout en bout pour les 12
  combinaisons (jamais fait avant sur Linux pour ce projet) -- a
  revele et corrige un bug bloquant : `config.m4` ne propageait pas
  `MBCRYPT_HAS_OPENSSL` a `mbcrypt_lib.c`, rendant l'AES-256-GCM
  silencieusement non fonctionnel sur tout `.so` Linux compile par
  `phpize` avant ce correctif (`phpinfo()` l'annoncait pourtant comme
  actif). Detail dans `build/GUIDE-COMPILATION-LINUX.md`.

### Approche C -- format .mbpak (bibliotheque + CLI + integration PHP)
- **Statut : implemente et verifie de bout en bout (2026-07-18),
  y compris le wrapper de flux PHP `mbpak://`**
- Nouveau sous-dossier independant `mbpak/`, ne modifie pas `mbcrypt/` --
  reutilise `mbcrypt_lib.c`/`mbcrypt_lib.h` en lecture seule (compiles en
  objet local par le Makefile de `mbpak/`) pour toute la cryptographie
- Format binaire final (detail complet dans `mbpak/FORMAT.md`) : header
  24 octets (clair, reecrit en place a chaque commit), records
  append-only `[8 o len][blob MB_ENC_V3]` pour chaque fichier ET pour la
  TOC elle-meme -- aucune nouvelle primitive crypto, uniquement le
  conteneur. AES-256-GCM exclusif (pas de mode XOR legacy pour ce
  nouveau format)
- API C : `mbpak_create/open/put/get/delete/list/flush/compact/close`
  dans `mbpak_lib.c`. CLI `mbpak pack|list|extract|put|get|rm|compact`
  dans `mbpak_cli.c`
- Ecriture incrementale verifiee : `mbpak put` fait grandir l'archive
  par append (ne reecrit pas les autres fichiers), `mbpak compact`
  reduit reellement la taille apres suppression
- Bug trouve et corrige pendant les tests (avant toute documentation
  "termine") : sur une archive neuve, le tout premier `write_record`
  (TOC vide) atterrissait a l'offset 0 avant que le header n'existe --
  l'ecriture du header qui suivait l'ecrasait alors. Corrige en
  reservant les 24 octets du header avant le premier commit. Detecte par
  le test "archive vide" de `mbpak/test/run.sh`
- Verifie aussi : mauvais mot de passe rejete, alteration d'un octet
  dans le ciphertext d'un fichier detectee (tag GCM) sans affecter les
  autres fichiers de la meme archive, aucune fuite memoire ni UB sous
  AddressSanitizer/UBSan (`make debug`) sur le scenario complet
- `libssl-dev`, `pkg-config` et `valgrind` installes sur la machine de
  dev (absents auparavant, `apt-get` execute avec l'autorisation
  explicite de l'utilisateur)

#### Integration PHP (`mbcryptload.c`) -- session du 2026-07-18

Avant de toucher au code fonctionnel existant, verification de l'impact :
le hook `zend_compile_file` existant (dechiffrement transparent d'un
`.php` seul) ne pouvait pas etre affecte, car son `fh_read_file()` fait
un `fopen()` C standard sur le nom de fichier -- pour un chemin
`mbpak://...` ce `fopen()` echoue simplement (ce n'est pas un chemin
disque reel) et retombe sur `org_compile_file()`, qui lui sait resoudre
les wrappers de flux enregistres. Les deux mecanismes cohabitent sans
interference ; confirme empiriquement par le test de non-regression
ci-dessous.

- `mbcrypt/mbpak_lib.c`/`.h` : copie vendorisee de `mbpak/mbpak_lib.{c,h}`
  (identique, note de synchronisation en tete des deux fichiers) --
  necessaire pour que `mbcrypt/` reste auto-suffisant pour le workflow
  documente `cp -avf mbcrypt/ ~/php-x.y.z/ext/mbcryptload/` (sinon
  cette distribution casserait pour tout le monde, pas seulement pour
  mbpak)
- `mbcryptload.c` : ajouts strictement additifs -- nouveau champ
  `mounted` dans les globals (persiste entre requetes comme `password`,
  meme raisonnement : `mbpak_t` est en malloc/free pur, pas de
  dependance PHP, libere sans risque depuis MSHUTDOWN), 7 nouvelles
  fonctions PHP (`mbpak_mount/unmount/list/get/put/delete/compact`),
  wrapper de flux `mbpak://` en lecture seule enregistre au MINIT.
  Aucune fonction existante modifiee.
- Ecriture volontairement hors du wrapper de flux : `fopen("mbpak://
  ...", "w")` est refuse proprement -- ecrire via `mbpak_put()` reflete
  le vrai protocole d'append + commit de l'archive plutot que de
  simuler un fichier POSIX que l'archive n'est pas
- `config.m4`/`config.w32` : `mbpak_lib.c` ajoute aux SOURCES de
  `PHP_NEW_EXTENSION`/`EXTENSION()` -- herite automatiquement du meme
  `MBCRYPTLOAD_EXTRA_CFLAGS` (`-DMBCRYPT_HAS_OPENSSL`) que
  `mbcrypt_lib.c`, meme mecanisme deja en place, aucun changement a la
  detection OpenSSL existante
- **Piege d'API PHP trouve et corrige pendant les tests** (avant toute
  documentation "termine") : `php_stream_memory_open()` a change de
  signature entre PHP 8.0 et 8.1 -- `(mode, char*, size_t)` avant,
  `(mode, zend_string*)` a partir de 8.1 (verifie directement dans
  `main/streams/memory.c` de php-src pour les deux branches). Sans
  garde de version, le build echouait a la compilation sur PHP 8.0.
  Corrige avec `#if PHP_VERSION_ID >= 80100`, meme convention que la
  macro `MBCRYPT_FH_FILENAME` deja presente dans ce fichier pour la
  meme raison (changement de `zend_file_handle.filename` en 8.1)
- **Limitation confirmee** (deja anticipee plus haut dans ce document,
  section Approche A) : `require __DIR__ . "/lib/auth.php"` a l'interieur
  d'un fichier `mbpak://index.php` echoue, car `dirname("mbpak://
  index.php")` renvoie `"mbpak:"` (PHP collapse le `//` -- comportement
  standard sur les chemins a schema URL, pas specifique a ce wrapper).
  Les require internes doivent utiliser le prefixe absolu
  `mbpak://chemin`. Documente dans `mbpak/README.md`
- Verifie via Docker (images officielles `php:X.Y-cli-bookworm/bullseye`,
  memes images que `build-linux-docker.sh`) sur **PHP 8.0 a 8.5, NTS et
  ZTS (8.3)** : compilation sans warning, puis scenario complet --
  non-regression sur `mbcryptload_encrypt_file`/`set_password`/`require`
  d'un fichier seul chiffre, `mbpak_mount` (mauvais mot de passe rejete,
  bon mot de passe accepte), `mbpak_list`, `require "mbpak://index.php"`
  (avec require interne absolu vers `mbpak://lib/auth.php`),
  `mbpak_get`, `file_get_contents("mbpak://...")`, `mbpak_put` suivi
  d'une relecture immediate, `require` apres `put` (reflete la nouvelle
  valeur), `mbpak_delete` + `mbpak_compact`, refus propre de
  `fopen("mbpak://...", "w")`, `mbpak_unmount`
- Scripts de build mis a jour pour inclure `mbpak_lib.c`/`.h` (sinon
  echec de link, `mbcryptload.c` reference desormais ces symboles sans
  condition) : `build/build-linux-docker.sh`, `build/build-ubuntu.sh`,
  `build/build-windows.ps1`, `build/build-windows-all.ps1`,
  `build/build-windows-static.ps1`. Les trois scripts Windows n'ont
  **pas** pu etre testes ici (pas d'environnement Windows) -- edits
  mecaniques, mais a verifier a la prochaine compilation Windows reelle
- Script officiel `build/build-linux-docker.sh` execute de bout en bout
  (pas seulement des commandes Docker ad-hoc equivalentes) : les 12
  `.so` (PHP 8.0-8.5, NTS+ZTS) + le CLI `mbcrypt` statique se generent
  sans erreur avec le fichier `mbpak_lib.c`/`.h` ajoute a la copie

#### `url_stat` -- session du 2026-07-18 (suite)

Objectif : faire fonctionner `file_exists()`/`is_file()`/`filesize()`/
`filemtime()` sur `mbpak://`, necessaire pour qu'un autoloader Composer
(PSR-4) puisse resoudre des classes packees -- discute avant
implementation : pour un projet type Laravel, seul le code source
statique (`app/`, `config/`, `routes/`, vues Blade) est un bon candidat
pour `.mbpak` ; `storage/` (uploads, logs) doit rester sur le systeme de
fichiers normal (cout par ecriture O(n) avec `mbpak_put()` inadapte a
un log qui grossit ligne par ligne, aucune securite multi-ecrivain entre
workers PHP-FPM, pas de lecture en streaming pour de gros fichiers).

- **Format `.mbpak` v2** : ajout d'un champ `size` (u64 LE, taille en
  clair) a chaque entree de la TOC -- c'est la seule metadonnee qui ne
  peut pas se deduire du RECORD sans dechiffrement AES-GCM + inflate
  zlib complets. `MBPAK_VERSION` passe de 1 a 2 (aucune archive v1
  distribuee, seulement des fixtures de test locales -- bump sans
  migration). Detail dans `mbpak/FORMAT.md`
- `mbpak_stream_url_stat()` dans `mbcryptload.c` : synthetise un
  `struct stat` (`S_IFREG`, taille et mtime depuis la TOC) sans jamais
  dechiffrer -- modele directement sur `phar_wrapper_stat()` dans
  `ext/phar/stream.c` de php-src (meme probleme : synthetiser un stat
  pour un membre d'archive qui n'a pas d'inode reel). Gere
  `PHP_STREAM_URL_STAT_QUIET` pour rester silencieux sur les chemins
  absents, comme le fait un `file_exists()` normal sur un fichier disque
  absent (essentiel : un autoloader teste des dizaines de chemins
  candidats qui n'existent pas, ca ne doit jamais spammer de warning)
- **Bug trouve et corrige pendant les tests** (avant toute documentation
  "termine") : apres `mbpak_unmount()`, `file_exists("mbpak://index.php")`
  continuait de renvoyer `true` alors que plus rien n'etait monte.
  Cause : PHP memorise le resultat du dernier `stat()` par nom de
  fichier (`ext/standard/filestat.c`, meme mecanisme que celui purge par
  `clearstatcache()` en PHP userland), **tous wrappers confondus** --
  pas specifique a `mbpak://`. Le wrapper de fichiers standard s'en
  protege en appelant `php_clear_stat_cache()` a la fin de son
  `unlink()`/`rename()` (verifie dans `main/streams/plain_wrapper.c` de
  php-src) ; `mbpak_mount()`, `mbpak_unmount()`, `mbpak_put()` et
  `mbpak_delete()` font maintenant de meme. Detecte par le test [15] du
  scenario Docker (`file_exists()` appele sur le meme chemin avant et
  apres un `mbpak_unmount()`)
- Verifie via Docker sur **PHP 8.0 a 8.5, NTS et ZTS (8.3)** :
  `file_exists`/`is_file`/`filesize` (taille exacte comparee a
  `mbpak_get()`)/`filemtime` sur un fichier present, `file_exists` sur
  un chemin absent sans aucun warning capture (`set_error_handler`),
  `file_exists` redevenant `false` juste apres `mbpak_unmount()` sans
  appel explicite a `clearstatcache()` cote PHP -- plus le scenario de
  non-regression complet du 2026-07-18 rejoue integralement
- Reste non fait : `stream_stat` (fstat sur un handle deja ouvert, rien
  n'en a eu besoin), `dir_opener` (listage de repertoire depuis
  `mbpak://` -- pas de notion de repertoire dans le format aujourd'hui,
  seulement des chemins plats dans la TOC)

#### Test grandeur nature : une vraie application Laravel -- session du 2026-07-18 (suite)

Objectif : telecharger un Laravel frais, empaqueter une partie de
l'application avec `.mbpak`, l'executer, et corriger mbpak jusqu'a ce
que ca tourne correctement -- pas un test synthetique a un seul
fichier comme les precedents, une vraie application avec un vrai
autoloader Composer et une vraie base de donnees.

**Perimetre choisi (discute avant execution)** : seul `app/` (12 Ko,
3-4 classes) est empaquete et retire du disque. `vendor/` (80 Mo, code
open-source, aucune valeur a cacher, aurait demande d'implementer
`is_dir()`/listage de repertoire pour des milliers de fichiers Composer)
reste sur disque, de meme que `config/`, `routes/`, `resources/views/`,
`storage/` et `bootstrap/cache/` -- coherent avec la recommandation
donnee avant `url_stat` (les fichiers write-heavy/multi-workers comme
les uploads et les logs sont un mauvais candidat pour `.mbpak`, voir
plus haut dans ce document).

**Mise en place** : `composer create-project laravel/laravel` (Laravel
13.20.0, PHP 8.3, SQLite) dans un conteneur Docker persistant (pas
`--rm`, pour iterer sans tout reinstaller). Baseline verifiee (page
d'accueil HTTP 200) avant tout packing. `app/` empaquete via
`mbpak pack`, puis **deplace hors du disque** (`mv app app.REAL_DISABLED`)
pour chaque test -- aucun repli silencieux possible, si ca repond c'est
que ca vient reellement de l'archive.

**Branchement sur l'autoload Composer** : `composer.json` de Laravel
active `optimize-autoloader` par defaut, donc les classes `App\*` sont
normalement resolues via une classmap statique
(`vendor/composer/autoload_static.php`, chemins directs, AUCUN
`file_exists()` -- `ClassLoader::findFile()` retourne le chemin de la
classmap sans le verifier). Les deux mecanismes ont ete testes :
1. **Mode classmap (celui que Laravel utilise reellement par defaut)** :
   `sed` repointe chaque entree `'App\\Xxx' => '.../app/Xxx.php'` vers
   `'mbpak://Xxx.php'`. `include "mbpak://..."` marche directement via
   le stream wrapper deja en place, aucun changement necessaire.
2. **Mode PSR-4 dynamique (sollicite reellement `url_stat`)** :
   `optimize-autoloader` desactive, `prefixDirsPsr4['App\\']` repointe
   sur `'mbpak:/'` (un seul slash -- la concatenation Composer
   `$dir . '/Chemin.php'` doit produire `'mbpak://Chemin.php'`, pas
   `'mbpak:///Chemin.php'`). Les deux modes ont ete verifies avec succes,
   independamment.
`mbpak_mount()` ajoute en toute premiere ligne de `public/index.php`,
avant `require vendor/autoload.php`.

**Verifie en conditions reelles (avec `app/` absent du disque)** :
- Page d'accueil Laravel (HTTP 200, HTML identique a la baseline)
- Route personnalisee avec un `TestController extends Controller`
  (heritage entre deux classes chargees separement depuis l'archive,
  verifie via `get_parent_class()`) qui execute une vraie requete
  Eloquent (`User::count()`, `User::first()`) sur la base SQLite reelle
- Echec propre sans aucun `mbpak_mount()` (HTTP 500, exception Laravel
  normale "classe introuvable", aucun repli silencieux, aucune fuite
  d'information)
- Mauvais mot de passe : `mbpak_mount()` renvoie `false`, message clair,
  aucun contenu servi

**Bug de passage a l'echelle trouve et corrige (le plus important de
cette session)** : en mesurant le temps de reponse (~0,5-0,65s pour une
archive de 4 fichiers, anormalement lent), `mbpak_get()` s'est revele
couter ~96ms **par fichier lu** -- une derivation PBKDF2-SHA256
complete (100 000 iterations, volontairement lente) a chaque appel, en
plus des ~104ms de `mbpak_mount()`. Sur un `app/` reel de plusieurs
dizaines de classes, ca aurait donne plusieurs secondes de chargement
de page rien qu'en derivation de cle -- un test synthetique a 1-4
fichiers ne pouvait pas reveler ce probleme, seul un test avec un vrai
volume de fichiers autoloades l'a fait apparaitre.

Cause : `mbpak_put()`/`mbpak_get()`/le chiffrement de la TOC appelaient
`mbcrypt_lib_encode()`/`decode()` (le mot de passe), qui derivent une
cle PBKDF2 **fraiche a partir d'un salt aleatoire genere a chaque
appel** -- concu pour proteger un fichier `.php` isole (un seul
dechiffrement par compilation), pas pour un conteneur avec des dizaines
de fichiers lus sous le meme mot de passe.

Correctif (format `.mbpak` v3, detail complet dans `mbpak/FORMAT.md`) :
- Nouvelles fonctions additives dans `mbcrypt_lib.c`/`.h` (aucun
  changement de comportement des fonctions existantes, verifie par un
  test de regression dedie) : `mbcrypt_lib_random_salt()`,
  `mbcrypt_lib_derive_key()`, `mbcrypt_lib_encode_with_key()`,
  `mbcrypt_lib_decode_with_key()` -- memes primitives AES-GCM/PBKDF2
  internes (`aes_gcm_encrypt`/`decrypt` refactorees en variantes
  `_with_key`, les fonctions historiques bassees sur mot de passe
  devenues de simples wrappers dessus), juste separees de la derivation
  de cle pour pouvoir la faire une fois et la reutiliser
- `.mbpak` header passe de 24 a 40 octets (ajout de `kdf_salt`, 16
  octets, fixe pour la duree de vie de l'archive) ; `MBPAK_VERSION`
  passe de 2 a 3
- La cle est derivee **une fois** a `mbpak_open()`/`mbpak_create()` et
  reutilisee pour tous les RECORDs (fichiers et TOC) ; seul le nonce
  GCM reste frais et aleatoire par record (obligatoire pour la
  securite d'AES-GCM -- reutiliser une cle avec des nonces distincts
  est la maniere standard de chiffrer en masse sous une seule cle)
- Propriete de securite critique re-verifiee apres ce refactoring
  crypto : alteration d'un octet dans le ciphertext d'un fichier ->
  seul ce fichier echoue a l'authentification, les autres records de
  la meme archive restent lisibles (meme test que documente plus haut,
  rejoue explicitement sur le nouveau format)
- Mesure : `mbpak extract` sur 30 fichiers passe de ~3,1s (attendu,
  31 x ~100ms) a ~89ms mesures (mount + 30 dechiffrements). Sur le
  test Laravel reel (4 fichiers), le temps de reponse passe de
  ~0,5-0,65s a ~0,14-0,17s par requete
- Reverifie sur toute la matrice **PHP 8.0-8.5, NTS+ZTS** (build Docker,
  meme scenario de non-regression que les sessions precedentes) +
  `mbpak/test/run.sh` + AddressSanitizer/UBSan sur un scenario
  pack/get/put/rm/compact avec des fichiers de taille reelle -- aucune
  fuite, aucun UB, aucune regression

#### `dir_opener` + test Laravel etendu (app+config+routes+resources) -- session du 2026-07-18 (suite)

Objectif : empaqueter `app/`, `config/`, `routes/` et `resources/` (pas
seulement `app/` comme la session precedente) dans un vrai Laravel, et
creer un dossier de test permanent + une procedure documentee (au lieu
d'un test Docker jetable comme avant).

**Blocage prevu des le depart** : Laravel scanne `config/` via
`Symfony\Component\Finder\Finder` (utilise par
`LoadConfiguration::getConfigurationFiles()`) plutot que de `require`r
des chemins connus -- `app/`, `routes/web.php` et
`resources/views/` n'ont pas ce probleme (require/`file_exists()`
directs, deja valides). Implementation de `dir_opener` pour `mbpak://`
avant d'attaquer le test, modelisee directement sur
`ext/phar/dirstream.c` de php-src (meme probleme : synthetiser une
arborescence de repertoires a partir d'un manifeste plat, aucune notion
de dossier reel dans le format).

- `mbpak_stream_dir_opener()` dans `mbcryptload.c` : parcourt
  `mbpak_list()`, extrait pour un prefixe donne le premier segment de
  chemin apres ce prefixe (fichier feuille ou sous-dossier synthetise),
  deduplique via `zend_hash_str_add_empty_element()` dans une
  `HashTable`, trie avec `zend_hash_sort()`. Reutilise `mbpak_dir_ops`
  (`read`/`close`/`write`/`flush`) sur le meme modele que
  `phar_dir_ops`
- **Trois bugs trouves et corriges pendant les tests** (avant toute
  documentation "termine") :
  1. Pas de `seek` sur le flux repertoire -> `RecursiveDirectoryIterator`
     (utilise en interne par Symfony Finder) echouait avec *"Stream does
     not support seeking"* et n'explorait que le premier niveau. Corrige
     en ajoutant `mbpak_dir_seek()`, copie quasi a l'identique de
     `phar_dir_seek()`
  2. `is_dir("mbpak://config")` renvoyait toujours `false` -- `url_stat`
     ne cherchait qu'une correspondance exacte de fichier dans la TOC, un
     repertoire synthetise (aucune entree exacte, seulement des fichiers
     "en dessous") n'etait jamais reconnu. Sans ca,
     `RecursiveDirectoryIterator` ne descendait dans aucun sous-dossier.
     Corrige en detectant aussi les repertoires synthetises dans
     `mbpak_stream_url_stat()` (`S_IFDIR` si au moins une entree vivante
     commence par `vpath/`)
  3. `scandir()`/`opendir()` sur un chemin qui n'existe pas du tout
     renvoyait un tableau vide au lieu d'echouer (`false`) -- corrige en
     verifiant qu'au moins une entree corresponde avant de retourner un
     flux repertoire valide
- Verifie avec de vrais outils, pas seulement des iterateurs SPL a la
  main : `scandir()`, `opendir()`/`readdir()`/`closedir()`,
  `RecursiveDirectoryIterator`, et **`Symfony\Component\Finder\Finder`**
  installe via Composer dans le conteneur de test, avec exactement le
  meme appel que `LoadConfiguration::getConfigurationFiles()`
  (`Finder::create()->files()->name('*.php')->in(...)`)

**Blocages Laravel reels decouverts en lisant le vrai code source**
(pas des suppositions -- `laravel/framework` 11.x/13.x sur GitHub) :
- `Illuminate\Foundation\Configuration\ApplicationBuilder::withRouting()`
  fait `realpath($commands) !== false` avant d'enregistrer
  `routes/console.php` (parametre `commands:`) -- toujours faux pour un
  chemin `mbpak://`, silencieusement ignore. Ce test ne s'applique **pas**
  a `web:`/`api:` quand ils sont passes en chaine simple (le defaut),
  d'ou l'asymetrie : `routes/web.php` fonctionne, `routes/console.php`
  doit rester sur disque
- `LoadConfiguration::getConfigurationFiles()` fait
  `realpath($app->configPath())` avant de lancer le `Finder` -- renvoie
  toujours `false` pour `mbpak://config` (limitation PHP fondamentale,
  `realpath()` ne resout jamais un schema de flux personnalise, aucun
  wrapper ne peut changer ca), et Laravel retourne alors une liste de
  fichiers de configuration **vide**, silencieusement. Le vrai
  `dir_opener` ci-dessus ne suffit donc pas seul pour `config/` --
  necessite `php artisan config:cache` genere **avant** l'empaquetage
  (mecanisme deja recommande par Laravel en production, independamment
  de mbpak) ; une fois le cache present,
  `file_exists($app->getCachedConfigPath())` court-circuite le scan
  entierement
- Consequence subtile : des que `bootstrap/app.php` pointe `web:` vers
  `mbpak://`, **tout** boot de l'application (HTTP ou CLI) tente de
  `require` cette route au demarrage -- y compris un simple `composer
  dump-autoload` (qui declenche `@php artisan package:discover` en
  post-hook) ou n'importe quelle commande `artisan`. D'ou l'obligation
  de faire tourner toutes les commandes composer/artisan **avant** de
  repointer `bootstrap/app.php` et la classmap Composer vers `mbpak://`,
  et d'ajouter `mbpak_mount()` aussi dans `artisan` (pas seulement
  `public/index.php`) pour que les commandes CLI fonctionnent ensuite
- `SplFileInfo::getRealPath()` sur un resultat de Finder iterant
  `mbpak://` leve `ValueError: Path cannot be empty` -- utiliser
  `getPathname()`. `FileViewFinder::resolvePath()` s'en sort seule
  (`realpath($path) ?: $path`, repli silencieux), mais tout code qui
  appelle `getRealPath()` directement casserait pareil

**Resultat final** : Laravel 13.20.0 sert sa page d'accueil (vue
`welcome` compilee depuis `mbpak://resources/views`) et une route de
verification (JSON avec `User::count()` reel sur SQLite, heritage
`TestController extends Controller` entre deux fichiers separement
charges depuis l'archive, valeur de config lue depuis
`bootstrap/cache/config.php` genere a partir de `mbpak://`) -- avec
`app/`, `config/`, `resources/views/` et `routes/web.php` **totalement
absents du disque**. `php artisan route:list` et `artisan tinker`
confirment aussi le support CLI. `vendor/`, `storage/`,
`bootstrap/cache/` (hormis `config.php`) restent volontairement sur
disque (voir tableau perimetre dans
`test/laravel-mbpak/README.md`).

Procedure automatisee et documentee dans **`test/laravel-mbpak/`**
(nouveau, permanent, contrairement aux tests Docker jetables des
sessions precedentes) : `setup.sh` reproduit tout le scenario de bout
en bout (execute deux fois de suite avec succes pendant la mise au
point -- a d'ailleurs revele un bug du script lui-meme : `set -o
pipefail` + `docker exec ... | head -N` fait echouer le script une
fois sur deux par SIGPIPE quand `head` ferme le tube en premier ;
corrige en capturant la sortie dans une variable avant de la tronquer),
`README.md` documente le perimetre (quoi empaqueter/laisser), la liste
exacte des fichiers Laravel a modifier, l'ordre des operations
(critique), le compromis honnete sur `config/`, et les trois pieges
ci-dessus.

Verifie aussi sous Valgrind (`--leak-check=full`) sur le scenario
`dir_opener` complet (scandir/opendir/Finder recursif) : 0 erreur, 0
fuite, tous les blocs liberes.
