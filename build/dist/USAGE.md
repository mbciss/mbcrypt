# MBCRYPT v3 - Guide d'utilisation

Auteur : CISSE Mohamed Bassirou  
Date   : 2026-04-08

---

## Contenu du dossier dist/

```
mbcrypt.exe                        Encodeur CLI (chiffrement de fichiers PHP)
php_mbcryptload_php81_ts.dll       Extension PHP pour PHP 8.1 (ZTS x64)
php_mbcryptload_php82_ts.dll       Extension PHP pour PHP 8.2 (ZTS x64)
php_mbcryptload_php83_ts.dll       Extension PHP pour PHP 8.3 (ZTS x64)
```

---

## 1. Installation de l'extension PHP

### Laragon (Windows)

Copier la DLL correspondant a votre version de PHP :

```bat
copy php_mbcryptload_php83_ts.dll  C:\laragon\bin\php\php-8.3.16-Win32-vs16-x64\ext\php_mbcryptload.dll
copy php_mbcryptload_php82_ts.dll  C:\laragon\bin\php\php-8.2.15-Win32-vs16-x64\ext\php_mbcryptload.dll
copy php_mbcryptload_php81_ts.dll  C:\laragon\bin\php\php-8.1.10-Win32-vs16-x64\ext\php_mbcryptload.dll
```

Ajouter dans `php.ini` :

```ini
extension=php_mbcryptload
```

Verifier que l'extension est bien chargee :

```bat
php.exe -m | findstr mbcryptload
php.exe --ri mbcryptload
```

---

## 2. Chiffrement de fichiers avec mbcrypt.exe

### Syntaxe

```
mbcrypt.exe [OPTIONS] -o OUTPUT SOURCE
mbcrypt.exe [OPTIONS] -r -o OUTPUT_DIR SOURCE_DIR
```

### Options

| Option              | Description                                |
|---------------------|--------------------------------------------|
| `-o FILE`           | Fichier ou dossier de sortie (obligatoire) |
| `-r`                | Mode recursif (chiffre tout un dossier)    |
| `-p PASSWORD`       | Mot de passe AES-256-GCM (recommande)      |
| `--legacy`          | Force le mode XOR (sans mot de passe)      |
| `-v`                | Affiche la version                         |
| `-h`                | Affiche l'aide                             |

### Exemples

**Fichier unique - mode XOR (sans mot de passe) :**
```bat
mbcrypt.exe -o output.php source.php
```

**Fichier unique - mode AES-256-GCM (recommande) :**
```bat
mbcrypt.exe -p "mon-mot-de-passe" -o output.php source.php
```

**Dossier entier - mode XOR :**
```bat
mbcrypt.exe -r -o C:\mon_app_chiffree\ C:\mon_app\
```

**Dossier entier - mode AES-256-GCM :**
```bat
mbcrypt.exe -r -p "mon-mot-de-passe" -o C:\mon_app_chiffree\ C:\mon_app\
```

> Les fichiers non-PHP sont ignores. Les fichiers deja chiffres sont ignores (protection contre le double chiffrement).

---

## 3. Dechiffrement automatique a l'execution

L'extension PHP dechiffre les fichiers a la volee au moment ou PHP les charge.  
Aucune action supplementaire n'est requise pour les fichiers XOR.

Pour les fichiers AES, fournir le mot de passe **avant** tout `require` / `include` :

```php
<?php
// index.php (point d'entree - non chiffre)
mbcryptload_set_password("mon-mot-de-passe");

require "mon_module_chiffre.php";   // dechiffre automatiquement
```

### Via directive php.ini (appel direct sans require, methode recommandee)

La directive `mbcryptload.password` dans `php.ini` est lue au demarrage de PHP,
avant toute execution de script. C'est la methode la plus propre pour appeler
un fichier chiffre directement, sans code intermediaire.

```ini
; php.ini
extension=php_mbcryptload
mbcryptload.password = "mon-mot-de-passe"
```

Une fois en place, tout fichier chiffre s'execute directement :

```bat
php.exe fichier_chiffre.php
```

Verification que le mot de passe est bien pris en compte :

```bat
php.exe --ri mbcryptload
```

La sortie affichera :
```
mbcryptload.password    set (hidden)
```

> **Note :** cette directive est `PHP_INI_SYSTEM` -- elle ne peut etre definie
> que dans `php.ini`. Elle ne peut pas etre surchargee par `ini_set()` ou
> `.htaccess`, ce qui la protege contre toute modification par un script.

---

### Via variable d'environnement (appel direct sans require)

La variable d'environnement `MBCRYPTLOAD_PASSWORD` est lue par l'extension au moment
du chargement de chaque fichier. Elle permet d'appeler un fichier chiffre **directement**,
sans point d'entree intermediaire ni appel a `mbcryptload_set_password()`.

**CLI :**
```bat
set MBCRYPTLOAD_PASSWORD=mon-mot-de-passe
php.exe encrypted.php
```

**CLI - variable sur une seule ligne (non persistante) :**
```bat
cmd /C "set MBCRYPTLOAD_PASSWORD=mon-mot-de-passe && php.exe encrypted.php"
```

**Serveur web Apache / Laragon** — ajouter dans le VirtualHost ou `.htaccess` :
```apache
SetEnv MBCRYPTLOAD_PASSWORD "mon-mot-de-passe"
```

**Serveur web Nginx + PHP-FPM** — ajouter dans le bloc `location` ou `fastcgi_params` :
```nginx
fastcgi_param MBCRYPTLOAD_PASSWORD "mon-mot-de-passe";
```

**PHP-FPM (pool www.conf)** :
```ini
env[MBCRYPTLOAD_PASSWORD] = "mon-mot-de-passe"
```

> **Note de securite :** la variable d'environnement est visible via `phpinfo()` et
> les outils systeme. Proteger l'acces a `phpinfo()` en production et restreindre
> les permissions sur les fichiers de configuration du serveur web.

---

### Priorite des methodes de configuration du mot de passe

Quand plusieurs methodes sont configurees en meme temps, l'ordre de priorite est :

| Priorite | Methode | Portee |
|----------|---------|--------|
| 1 (plus forte) | `mbcryptload_set_password()` | Par requete, dans le code PHP |
| 2 | `mbcryptload.password` dans `php.ini` | Global, au demarrage de PHP |
| 3 | Variable d'environnement `MBCRYPTLOAD_PASSWORD` | Systeme / serveur web |

---

## 4. API PHP

```php
// Definir le mot de passe de dechiffrement
mbcryptload_set_password(string $password) : bool

// Chiffrer un fichier depuis PHP
mbcryptload_encrypt_file(string $source, string $dest) : bool               // XOR
mbcryptload_encrypt_file(string $source, string $dest, string $password, true) : bool  // AES

// Version de l'extension
mbcryptload_version() : string   // retourne "3.0.0"
```

---

## 5. Modes de chiffrement

| Mode       | Commande                        | Securite  | Remarque                              |
|------------|---------------------------------|-----------|---------------------------------------|
| XOR legacy | `mbcrypt.exe -o out.php in.php` | Faible    | Compatible V2, pas de mot de passe    |
| AES-256-GCM| `mbcrypt.exe -p PASS ...`       | Forte     | Recommande, necessite un mot de passe |

### Format du fichier chiffre V3

```
[9 octets]  Tag "MB_ENC_V3"
[1 octet]   Version (3)
[1 octet]   Methode : 0=XOR, 1=AES-256-GCM
[6 octets]  Reserve (zeros)
--- Mode AES uniquement ---
[16 octets] Sel PBKDF2 (aleatoire par fichier)
[12 octets] Nonce GCM (aleatoire par fichier)
[16 octets] Tag d'authentification GCM
--- Donnees ---
[N octets]  Contenu PHP compresse (zlib) puis chiffre
```

---

## 6. Securite

- **AES-256-GCM** : chiffrement authentifie. Tout fichier corrompu ou falsifie echoue au dechiffrement.
- **PBKDF2-SHA256** : derivation de cle avec 100 000 iterations et sel aleatoire par fichier.
- **Cle XOR unique** : regenerer avec `perl init_key` depuis le dossier source avant distribution.
- **Mot de passe** : ne jamais stocker en clair dans un fichier chiffre ou dans le code source.
- **show_source() / highlight_file()** : bloques sur les fichiers chiffres.

---

## 7. Workflow recommande

```
1. Generer une cle XOR unique (une seule fois par projet) :
       perl mbcrypt\init_key
       puis recompiler mbcrypt.exe et les DLLs

2. Chiffrer l'application :
       mbcrypt.exe -r -p "MOT_DE_PASSE_FORT" -o app_chiffree\ app_source\

3. Copier la DLL dans le serveur cible :
       php_mbcryptload_php83_ts.dll  ->  ext\php_mbcryptload.dll

4. Activer l'extension (php.ini) :
       extension=php_mbcryptload

5. Definir le mot de passe -- choisir UNE des trois methodes :

   a) php.ini (recommande -- aucun code a ecrire) :
          mbcryptload.password = "MOT_DE_PASSE_FORT"

   b) Variable d'environnement (Apache/FPM) :
          SetEnv MBCRYPTLOAD_PASSWORD "MOT_DE_PASSE_FORT"   (Apache)
          env[MBCRYPTLOAD_PASSWORD] = "MOT_DE_PASSE_FORT"   (PHP-FPM)

   c) Appel PHP dans le point d'entree (index.php non chiffre) :
          mbcryptload_set_password("MOT_DE_PASSE_FORT");

6. Deployer uniquement app_chiffree\ (sans app_source\)
```
