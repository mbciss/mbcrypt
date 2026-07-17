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
- **Statut : implemente**
- Script `build-ubuntu.sh` : detection automatique des versions PHP installees
- Compilation extension .so + CLI mbcrypt
