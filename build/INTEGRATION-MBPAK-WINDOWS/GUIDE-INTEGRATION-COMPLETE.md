# Integration Windows complete : mbcrypt + mbpak (PHP 8.0 -> 8.5, NTS + TS)

Auteur : CISSE Mohamed Bassirou
Date   : 2026-07-22

Ce dossier documente la parite Windows du travail deja fait sur Ubuntu
(`build/GUIDE-COMPILATION-LINUX.md`, `build/build-linux-docker.sh`) : les
deux modules du depot (`mbcrypt` et `mbpak`) compiles, testes de bout en
bout sur de vrais runtimes PHP NTS **et** TS, versions 8.0 a 8.5, sur cette
machine Windows. `logs/` contient la sortie brute de chaque etape (rien de
resume ou de reformule) pour tracabilite complete.

---

## 0. Point de depart : ce qui manquait avant cette session

`mbcrypt` avait deja un guide Windows complet (`build/GUIDE-COMPILATION-WINDOWS.md`)
et 12 DLLs dans `build/dist/`. Mais deux ecarts existaient entre l'etat
Linux et l'etat Windows du depot :

| Ecart | Cote Linux | Cote Windows (avant cette session) |
|---|---|---|
| `mbpak.exe` (CLI archive) | Compile et teste (`mbpak/Makefile`, `mbpak/test/run.sh`) | **Inexistant** -- aucun script de build |
| Support `mbpak://` dans les DLLs `mbcryptload` | Present et verifie (`mbpak_mount` trouve dans les `.so`) | **Absent** -- les 12 `.dll` dataient du 2026-07-17 09:44, soit **avant** l'integration mbpak (commit `c35b85e`, 2026-07-18 11:16) ; elles ne contenaient pas `mbpak_mount` |

Verifie concretement en grep-ant la chaine `mbpak_mount` dans les binaires
avant toute action : presente dans `mbcryptload_php83_zts.so` (Linux),
absente de `php_mbcryptload_php83_ts.dll` (Windows). Les scripts
(`build-windows.ps1`, `build-windows-all.ps1`) referencaient deja
`mbcrypt\mbpak_lib.c` dans leur liste de sources -- seuls les **binaires
deja presents dans `build/dist/`** etaient perimes, pas les scripts.

---

## 1. Ce qui a ete fait dans cette session

1. **Recompilation des 12 DLLs `mbcryptload`** (`build/build-windows-all.ps1`,
   deja existant, aucune modification necessaire) -- desormais 37-40 Ko
   chacune (contre 21-23 Ko avant, difference = le code `mbpak_lib.c`
   desormais effectivement lie).
2. **Nouveau script `build/build-windows-mbpak.ps1`** -- compile
   `mbpak.exe`, qui n'avait jamais ete construit sous Windows. Ne depend
   d'aucune version de PHP (uniquement zlib + OpenSSL, comme
   `mbpak/Makefile` sur Linux) : **un seul binaire pour toutes les
   versions cible**.
3. **Bug reel trouve et corrige** : `mbpak compact` echouait
   systematiquement sous Windows (voir section 2).
4. **Nouveau script `build/test-windows-runtimes.ps1`** -- telecharge les
   binaires PHP complets (pas seulement les devel packs) NTS **et** TS
   pour les 6 versions 8.0 a 8.5 depuis `windows.php.net`, et executologique
   deux tests reels par combinaison : dechiffrement+execution `mbcrypt`,
   et montage+`require` via le wrapper `mbpak://`. **12 x 2 = 24 executions
   reelles, 24 PASS** (detail section 4).
5. **Test fonctionnel `mbpak/test/run.sh` porte sous Windows** (via Git Bash) :
   9/9 PASS (detail section 3).

---

## 2. Bug corrige : `mbpak compact` sur Windows (rename() n'ecrase pas)

### Symptome

```
[5/9] rm + compact reclaims space
Error: compact failed (original archive left untouched)
```

`mbpak/test/run.sh` echouait a l'etape 5/9 des la premiere execution sous
Windows -- alors que la meme suite passe integralement sous Linux.

### Cause

`mbpak_lib.c`, fonction `mbpak_compact()` :

```c
fclose(pak->fp);
pak->fp = NULL;
if (rename(tmp_path, pak->path) != 0) {   /* <-- echoue sous Windows */
```

`compact` reconstruit l'archive dans un fichier temporaire
(`<archive>.compact.tmp`) puis le renomme par-dessus l'original. Sur
POSIX, `rename()` remplace **atomiquement** une destination existante --
c'est le comportement documente et exploite ici. Sur Windows, la CRT
`rename()` encapsule `MoveFileA`, qui **refuse** d'ecraser un fichier
destination deja present (`errno EEXIST`) : `pak->path` existe toujours
a ce moment (c'est justement l'archive qu'on est en train de compacter),
donc l'appel echoue a chaque fois, inconditionnellement, sous Windows.

### Correctif

```c
fclose(pak->fp);
pak->fp = NULL;
#ifdef _WIN32
    /* Unlike POSIX rename(), Win32 MoveFile (which the CRT rename() wraps)
     * refuses to replace an existing destination -- drop the original
     * first. The compacted archive is already complete and valid on disk
     * (tmp_path) at this point, so this is safe: worst case on a crash
     * between the two calls is a missing pak->path with tmp_path left
     * over, recoverable by hand, never silent data loss. */
    remove(pak->path);
#endif
if (rename(tmp_path, pak->path) != 0) {
```

Applique dans **les deux copies** du fichier (elles doivent rester
synchronisees, voir la note en tete de `mbcrypt/mbpak_lib.h`) :

- `mbpak/mbpak_lib.c` (CLI `mbpak.exe`)
- `mbcrypt/mbpak_lib.c` (vendored, compile dans l'extension PHP
  `mbcryptload` -- c'est la meme fonction qu'expose `mbpak_compact()`
  cote PHP)

**Impact avant correctif** : `mbpak compact` (CLI) et `mbpak_compact()`
(PHP) echouaient a 100% sous Windows, sur toute archive existante -- le
`rm` (tombstone) fonctionnait, seule la purge d'espace derriere echouait.
Aucun autre sous-comportement de `mbpak` n'est affecte (uniquement
`mbpak_compact()` utilise `rename()` sur une destination deja existante ;
`mbpak_create()`/`mbpak_flush()` ecrivent directement sans passer par un
fichier temporaire renomme).

**Portee** : bug specifique a `.mbpak` -- `mbcrypt_lib.c` (le format `.php`
chiffre) n'utilise jamais `rename()`, seul `mbpak_lib.c` le fait. N'affecte
pas Linux (deja verifie via `mbpak/test/run.sh`, 9/9 avant comme apres ce
correctif).

Diff complet : `git diff` sur `mbpak/mbpak_lib.c` / `mbcrypt/mbpak_lib.c`,
fonction `mbpak_compact()`.

---

## 3. Test fonctionnel CLI : `mbpak/test/run.sh` sous Windows

Le script existant est ecrit en Bash (`set -euo pipefail`, `stat -c%s`,
`python3` pour le test d'alteration). Adapte pour Git Bash sans modifier
l'original :

```bash
cd mbpak
cp ../build/dist/mbpak.exe ./mbpak.exe
sed -e 's#^BIN=.*#BIN=./mbpak.exe#' \
    -e 's#\bpython3\b#/c/Python312/python#' \
    test/run.sh > test/run-windows.sh
bash test/run-windows.sh
```

Deux adaptations, aucune ne touche a la logique testee :

- `BIN=./mbpak` (executable Linux) -> `BIN=./mbpak.exe`
- `python3` -> chemin explicite vers l'installation Python 3.12 de la
  machine (l'alias `python3` du Microsoft Store, present dans le `PATH`
  par defaut sur Windows 11 sans Python installe via le Store, ouvre une
  invite d'installation au lieu d'executer quoi que ce soit -- piege
  Windows classique, sans rapport avec mbpak lui-meme)

`stat -c%s` (taille de fichier, format GNU) et `diff -r` fonctionnent tels
quels sous Git Bash (coreutils fournis).

### Resultat (`logs/mbpak-functional-test.log`)

```
[1/9] pack
[2/9] list
[3/9] extract + integrity
[4/9] put (update, append-only)
[5/9] rm + compact reclaims space
[6/9] wrong password rejected
[7/9] tamper detection (GCM auth)
[8/9] empty archive
[9/9] refuse to overwrite an existing archive
ALL TESTS PASSED
```

**9/9 PASS**, y compris la detection d'alteration GCM (etape 7, qui
depend de Python pour corrompre un octet) et le refus d'ecrasement
(etape 9). Echoue systematiquement avant le correctif de la section 2
(etape 5) ; passe integralement apres.

---

## 4. Matrice complete d'execution : 12 versions PHP x (NTS, TS) x 2 tests

Contrairement aux devel packs (utilises pour compiler, headers + `.lib`
seulement), cette section telecharge et execute de **vrais binaires
PHP complets** (`php.exe`) -- exactement l'equivalent Windows de ce que
`build-linux-docker.sh` fait avec les images Docker officielles
`php:8.X-cli` / `php:8.X-zts`. C'etait le principal ecart de couverture
restant : `test/README.md` notait explicitement *"Les variantes NTS ne
sont pas verifiees a l'execution ici (aucun runtime PHP NTS installe
localement)"*.

### Script : `build/test-windows-runtimes.ps1`

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\test-windows-runtimes.ps1
```

Pour chacune des 12 combinaisons (8.0.30, 8.1.10, 8.2.15, 8.3.16, 8.4.23,
8.5.8 x NTS, TS) :

1. Telecharge `php-<version>[-nts]-Win32-<toolset>-x64.zip` depuis
   `windows.php.net/downloads/releases/` (repli sur `archives/` pour les
   versions qui n'y sont plus -- 8.1 a 8.3, deja constate) dans
   `build/php-runtimes/<version>-<mode>/`.
2. Copie la DLL correspondante (`build/dist/php_mbcryptload_php<ver>_<mode>.dll`)
   dans `ext/php_mbcryptload.dll`, avec un `php-test.ini` minimal
   (`extension_dir` + `extension=php_mbcryptload` seulement -- pas de
   `php.ini` systeme, pour eviter tout module parasite).
3. **Test A (mbcrypt)** : execute `test/php<ver>/index.php`
   (`mbcryptload_set_password()` + `require calcul.encrypted.php`),
   verifie la presence de `OK - calcul termine.` dans la sortie.
4. **Test B (mbpak)** : empaquete un mini-projet (`main.php` +
   `lib/greet.php`) avec `mbpak.exe`, puis execute un script PHP qui fait
   `mbpak_mount()` + `require "mbpak://main.php"` (qui a son tour fait
   `require "mbpak://lib/greet.php"`) + `mbpak_list()`, verifie
   `OK - test mbpak wrapper termine.`.

### Resultat (`logs/test-windows-runtimes-12x2-matrix.log`)

**Test A -- `mbcryptload` : dechiffrement + execution reelle**

| Version PHP | NTS | TS |
|---|---|---|
| 8.0.30 | PASS | PASS |
| 8.1.10 | PASS | PASS |
| 8.2.15 | PASS | PASS |
| 8.3.16 | PASS | PASS |
| 8.4.23 | PASS | PASS |
| 8.5.8  | PASS | PASS |

**Test B -- wrapper `mbpak://` : montage + `require` imbrique + listing**

| Version PHP | NTS | TS |
|---|---|---|
| 8.0.30 | PASS | PASS |
| 8.1.10 | PASS | PASS |
| 8.2.15 | PASS | PASS |
| 8.3.16 | PASS | PASS |
| 8.4.23 | PASS | PASS |
| 8.5.8  | PASS | PASS |

**24/24 PASS.** Combine au test CLI (section 3, 9/9), **33 verifications
automatisees passent** sur cette session. Ceci couvre, pour la premiere
fois sur Windows, la variante **NTS a l'execution** (jusqu'ici seule la
compilation NTS etait verifiee -- voir `test/README.md`, tableau
"Resultats verifies (2026-07-17)").

> Note sur `require "mbpak://lib/greet.php"` (test B) : le chemin est
> ecrit en absolu (prefixe `mbpak://` complet), pas en relatif
> (`__DIR__ . "/../lib/greet.php"`). C'est une contrainte documentee de
> PHP sur les wrappers de flux personnalises (`dirname("mbpak://x.php")`
> renvoie `"mbpak:"`, pas un chemin utilisable), deja notee dans
> `mbpak/README.md` section "Limitation a connaitre" -- pas un bug, un
> essai initial du script de test l'a heurte de plein fouet avant
> correction du test lui-meme (pas du produit).

---

## 5. Build : recapitulatif des trois scripts

| Script | Nouveau ? | Produit | Depend de PHP ? |
|---|---|---|---|
| `build/build-windows-all.ps1` | Non (deja existant) | 12 `php_mbcryptload_php8X_{ts,nts}.dll` | Oui -- une par version/mode |
| `build/build-windows-mbpak.ps1` | **Oui** | `mbpak.exe` (1 seul binaire) | Non -- uniquement zlib + OpenSSL |
| `build/test-windows-runtimes.ps1` | **Oui** | Rien (script de verification) | Telecharge les runtimes complets pour tester |

Prerequis : identiques a `build/GUIDE-COMPILATION-WINDOWS.md` (VS2019
BuildTools pour 8.0-8.3, VS2022 pour 8.4-8.5, Windows SDK 10, zlib via
Laragon, OpenSSL Win64) -- rien de supplementaire pour `mbpak.exe`, qui
reutilise le meme zlib et le meme OpenSSL, toolchain VS2019 (vs16, choix
arbitraire pour coherence avec le CLI `mbcrypt.exe` de
`build-windows.ps1` -- `mbpak.exe` n'ayant aucune dependance PHP, VS2022
aurait fonctionne a l'identique).

### Compiler mbpak.exe

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File build\build-windows-mbpak.ps1
```

Resultat : `build\dist\mbpak.exe` (~85 Ko, lie dynamiquement a zlib.dll /
libssl-3-x64.dll / libcrypto-3-x64.dll -- contrairement a `mbcrypt.exe`
sous Linux qui est lie statiquement, aucune cible `make static`
equivalente n'existe cote Windows/MSVC ici).

Le fichier source `mbpak/mbpak_cli.c` etait deja portable (utilise
`#ifdef _WIN32` / `WIN32_FIND_DATAA` + `FindFirstFileA` pour le parcours
recursif de repertoire, `CreateDirectoryA` pour la creation de dossiers)
-- **aucune modification de source necessaire**, compilation MSVC propre
des le premier essai.

---

## 6. Installation et utilisation de `mbpak.exe`

Identique a l'usage documente dans `mbpak/README.md` (Linux), le binaire
`.exe` remplace simplement `mbpak` :

```bat
mbpak.exe pack    -p "mot-de-passe" -o app.mbpak app\
mbpak.exe list    -p "mot-de-passe" app.mbpak
mbpak.exe extract -p "mot-de-passe" -o out\ app.mbpak
mbpak.exe extract -p "mot-de-passe" -o out\ app.mbpak lib\auth.php
mbpak.exe put     -p "mot-de-passe" app.mbpak lib\auth.php nouveau_auth.php
mbpak.exe get     -p "mot-de-passe" app.mbpak lib\auth.php
mbpak.exe rm      -p "mot-de-passe" app.mbpak lib\auth.php
mbpak.exe compact -p "mot-de-passe" app.mbpak
```

### Depuis PHP (mbcryptload.dll doit etre charge -- voir section 0 de
`build/dist/USAGE.md` pour choisir la bonne variante TS/NTS)

```php
<?php
mbpak_mount("app.mbpak", "mot-de-passe");
require "mbpak://index.php";           // prefixe absolu obligatoire
echo file_get_contents("mbpak://config.php");
var_dump(file_exists("mbpak://index.php"));  // true, sans dechiffrer (url_stat)
mbpak_put("config.php", "<?php return ['db'=>'staging'];\n");
mbpak_delete("lib/old.php");
mbpak_compact();                       // fonctionne desormais sous Windows (section 2)
print_r(mbpak_list());
mbpak_unmount();
```

---

## 7. Ce qui n'a volontairement pas ete refait dans cette session

- **Recompilation `php.exe` statique** (`build-windows-static.ps1`) :
  deja verifiee et documentee dans `build/GUIDE-COMPILATION-WINDOWS.md`
  section "Piege connu" -- ne compile pas `mbpak_lib.c` (mbpak n'est pas
  concerne par le mode statique, qui ne construit que `mbcryptload`).
  Aucun changement necessaire ni effectue ici.
- **Scenario Laravel complet sous Windows** (`test/laravel-mbpak/`) :
  reste documente et verifie cote Linux/Docker uniquement
  (`test/laravel-mbpak/README.md`). Le test B de la section 4
  (montage + `require` imbrique + listing) couvre le meme mecanisme
  cote extension (wrapper `mbpak://`, chargement de classes via
  `require`) sur les 12 combinaisons Windows, mais sans reproduire
  l'application Laravel entiere (Composer, Eloquent, Blade) qui
  necessiterait Docker ou une installation PHP+Composer completes,
  hors perimetre de cette session.

---

## 8. Contenu de ce dossier

```
build/INTEGRATION-MBPAK-WINDOWS/
  GUIDE-INTEGRATION-COMPLETE.md          Ce document
  logs/
    build-windows-all-12dlls.log          Sortie complete : recompilation des 12 DLLs
    build-windows-mbpak-exe.log           Sortie complete : compilation de mbpak.exe
    mbpak-functional-test.log             Sortie complete : test/run.sh (9/9 PASS)
    test-windows-runtimes-12x2-matrix.log Sortie complete : matrice 12x2 (24/24 PASS)
```

Binaires produits (non dupliques ici, deja dans `build/dist/` --
convention existante du depot) :

```
build/dist/mbpak.exe                          Nouveau
build/dist/php_mbcryptload_php8X_{ts,nts}.dll  Recompiles (mbpak_lib.c desormais lie)
```

Fichiers sources modifies (le correctif de la section 2) :

```
mbpak/mbpak_lib.c      mbpak_compact() -- fix rename() Windows
mbcrypt/mbpak_lib.c     Meme fix, copie synchronisee (vendored dans l'extension PHP)
```

Runtimes PHP complets telecharges pour les tests (non commites --
~30-40 Mo x 12, purement des artefacts de verification, a nettoyer a la
demande) :

```
build/php-runtimes/8.0.30-nts/  8.0.30-ts/
build/php-runtimes/8.1.10-nts/  8.1.10-ts/
build/php-runtimes/8.2.15-nts/  8.2.15-ts/
build/php-runtimes/8.3.16-nts/  8.3.16-ts/
build/php-runtimes/8.4.23-nts/  8.4.23-ts/
build/php-runtimes/8.5.8-nts/   8.5.8-ts/
```
