# MBCRYPT v3 — Documentation algorithmique

Auteur : CISSE Mohamed Bassirou
Date   : 2026-07-17

Ce document decrit le fonctionnement interne du code source
(`mbcrypt_lib.c`, `mbcrypt_lib.h`, `mbcryptload.c`, `mbcrypt.c`) :
les algorithmes de chiffrement, le format binaire, et le mecanisme
d'interception qui permet a PHP d'executer un fichier chiffre sans
jamais ecrire sa version dechiffree sur le disque.

Pour l'utilisation (CLI, API PHP) voir `build/dist/USAGE.md`.
Pour la compilation voir `build/GUIDE-COMPILATION-WINDOWS.md`.

---

## 1. Vue d'ensemble

MBCRYPT est compose de deux parties qui partagent la meme bibliotheque
core (`mbcrypt_lib.c`, sans dependance PHP — uniquement `malloc`/`free`) :

```
mbcrypt.exe (CLI)          mbcryptload (extension PHP)
      |                            |
      +---- mbcrypt_lib_encode ----+   chiffrement (hors ligne)
      |                            |
      |             mbcryptload_compile_file
      |                     |
      |          mbcrypt_lib_detect / mbcrypt_lib_decode
      |                     |
      |            (dechiffrement a la volee)
```

- **`mbcrypt.exe`** transforme un fichier `.php` en clair en un blob
  chiffre (`mbcrypt_lib_encode`), a distribuer a la place du source.
- **`mbcryptload`** (extension PHP) intercepte le chargement de chaque
  fichier PHP (`zend_compile_file`), detecte s'il est chiffre, le
  dechiffre en memoire (`mbcrypt_lib_decode`) et donne le resultat au
  compilateur Zend — le fichier en clair n'existe jamais sur disque.

---

## 2. Format binaire V3

Le format est defini dans `mbcrypt_lib.h` (`mbcrypt_v3_header_t`,
`pragma pack(1)` — aucun octet de padding).

```
Offset  Taille  Contenu
------  ------  -------------------------------------------
0       9       Tag "MB_ENC_V3"                    (MBCRYPTTAG_V3_STR)
9       1       Version = 3
10      1       Methode : 0 = XOR legacy, 1 = AES-256-GCM
11      6       Reserve (zero-pad, extensions futures)
--- Si methode == AES-256-GCM (MBCRYPT_V3_AES_LEN = 61 octets d'en-tete) ---
17      16      Sel PBKDF2       (aleatoire, genere par fichier)
33      12      Nonce/IV GCM     (aleatoire, genere par fichier)
45      16      Tag d'authentification GCM
--- Donnees, dans tous les cas ---
61 (ou 17) N    Contenu PHP compresse (zlib) puis chiffre
```

Le format V2 (`MBCRYPTTAG_V2_STR`, tag seul de 9 octets suivi
directement des donnees XOR) reste reconnu en lecture pour la
retrocompatibilite (`mbcrypt_lib_detect`), mais plus jamais produit
en ecriture.

`mbcrypt_lib_detect()` lit uniquement les 9 (ou 17) premiers octets
pour identifier le format — c'est une verification bon marche
executee **avant** toute operation cryptographique, pour que les
fichiers PHP non chiffres traversent le hook sans surcout.

---

## 3. Algorithme de chiffrement — `mbcrypt_lib_encode()`

Pipeline pour un fichier PHP source, en deux etapes fixes :

```
source.php (clair)
     |
     |  1. compression zlib (deflate, niveau par defaut)
     v
donnees compressees
     |
     |  2a. mode AES : chiffrement AES-256-GCM (cle derivee du mot de passe)
     |  2b. mode legacy : XOR avec la cle statique MBCRYPTKEY
     v
blob V3 (tag + en-tete + [sel+iv+tag GCM] + donnees)
```

**Pourquoi compresser avant de chiffrer, jamais l'inverse ?**
Un contenu chiffre a une entropie proche du hasard, donc incompressible.
En compressant le PHP en clair d'abord (le code source est tres
redondant — mots-cles, indentation, noms repetes), le blob final est
a la fois plus petit et le chiffrement s'applique sur un flux dense
sans structure exploitable.

### 3a. Mode AES-256-GCM (`aes_gcm_encrypt`, requiert OpenSSL)

1. Generer un sel PBKDF2 de 16 octets et un nonce GCM de 12 octets
   avec `RAND_bytes` (generateur cryptographique OpenSSL).
2. Deriver une cle AES de 256 bits : `PBKDF2-HMAC-SHA256(mot_de_passe,
   sel, 100 000 iterations)` (`derive_key()`, `MBCRYPT_PBKDF2_ITER`).
   100 000 iterations rendent une attaque par force brute sur le mot
   de passe couteuse (ralentit chaque tentative), le sel empeche les
   tables precalculees (rainbow tables) et garantit qu'un meme mot de
   passe produit une cle differente pour chaque fichier.
3. Chiffrer avec `EVP_EncryptUpdate`/`EVP_EncryptFinal_ex` en mode
   GCM : AES-GCM produit un texte chiffre de la **meme taille** que
   l'entree (mode de chiffrement par flux, pas de padding), plus un
   tag d'authentification de 128 bits calcule sur l'ensemble des
   donnees.
4. La cle en clair est effacee de la pile (`memset`) immediatement
   apres usage — elle ne doit jamais survivre plus longtemps que
   necessaire en memoire.
5. Assembler le blob : en-tete V3 + sel + iv + tag + texte chiffre.

### 3b. Mode XOR legacy (`mbcrypt_lib_xor`, sans mot de passe)

Chiffrement symetrique trivial : chaque octet est combine par XOR
avec un octet d'une cle courte (`MBCRYPTKEY`, seize valeurs `short`),
apres une permutation fixe des elements de la cle ("key scramble",
regeneree par `init_key`) et une inversion binaire (`~`) de l'octet
source :

```c
buf[i] = k[(len - i) % klen] ^ (~(unsigned char)buf[i]);
```

C'est un mode de **compatibilite** (V2 historique), pas une protection
cryptographique reelle — la meme cle est compilee en dur dans le
binaire (`mbcrypt.exe` et l'extension), donc quiconque a acces au
binaire peut retrouver la cle et dechiffrer. `init_key` permet de
regenerer une cle et une permutation propres a un projet avant
distribution, mais ne remplace pas AES-256-GCM en cas de besoin de
confidentialite reelle.

---

## 4. Algorithme de dechiffrement — `mbcrypt_lib_decode()`

Chemin inverse, pilote par le resultat de `mbcrypt_lib_detect()` :

```
blob (V2 ou V3, XOR ou AES-GCM)
     |
     |  1. detect() : lecture du tag -> methode
     v
     |  2a. AES  : extraire sel/iv/tag, deriver la cle (meme PBKDF2),
     |             EVP_DecryptUpdate + EVP_DecryptFinal_ex
     |  2b. XOR  : meme operation XOR (symetrique) sur les donnees
     v
donnees compressees
     |
     |  3. decompression zlib (inflate, par chunks de 64 Ko)
     v
source.php (clair, en memoire uniquement)
```

**Authentification GCM** : `EVP_DecryptFinal_ex` verifie le tag
d'authentification calcule a l'etape 3a d'`aes_gcm_decrypt` *avant*
de considerer le dechiffrement reussi. Si le tag ne correspond pas
(mauvais mot de passe **ou** fichier modifie/corrompu), la fonction
retourne une erreur et `mbcrypt_lib_decode()` renvoie `NULL` — aucune
donnee partiellement dechiffree n'est jamais exposee a l'appelant.
C'est la difference fondamentale avec le XOR : XOR "dechiffre" toujours
quelque chose (du bruit avec la mauvaise cle), GCM sait dire "non".

**Decompression sans buffer fixe** (`mbcrypt_lib_zdecompress`) : la
taille du fichier original n'est pas connue a l'avance, donc la
decompression tourne en boucle par blocs de `ZLIB_CHUNK` (64 Ko),
en reallouant (`realloc`, doublement de capacite) le buffer de sortie
au fur et a mesure — jamais de buffer de taille fixe sur la pile,
ce qui evite tout debordement quelle que soit la taille du fichier
source (bug corrige par rapport a la v2, qui utilisait un buffer
statique de 100 Ko).

---

## 5. Mecanisme d'interception a l'execution (`mbcryptload.c`)

C'est la piece qui rend le dechiffrement **transparent** : PHP execute
un fichier chiffre exactement comme un fichier normal, sans aucune
etape manuelle de dechiffrement dans le code applicatif.

### 5.1 Le hook `zend_compile_file`

A chaque fois que PHP doit compiler un fichier (`include`, `require`,
point d'entree du script), il appelle un pointeur de fonction global
`zend_compile_file`. Au demarrage du module (`PHP_MINIT_FUNCTION`),
mbcryptload sauvegarde le pointeur original et le remplace par le
sien :

```c
org_compile_file  = zend_compile_file;
zend_compile_file = mbcryptload_compile_file;
```

`mbcryptload_compile_file()` s'execute alors pour **chaque** fichier
PHP charge, chiffre ou non :

```
mbcryptload_compile_file(file_handle, type)
  |
  |-- pas de nom de fichier (eval, stdin...) -> org_compile_file()  [passthrough]
  |-- appel depuis show_source()/highlight_file() -> org_compile_file()  [protection]
  |-- lecture brute du fichier (fh_read_file, fopen/fread)
  |-- detect() == NOT_ENCRYPTED -> org_compile_file()  [fichier normal, aucun cout]
  |-- resolution du mot de passe (voir 5.2)
  |-- decode() echoue (mauvais mdp / corrompu) -> org_compile_file()
  |                                                [PHP affichera son erreur de syntaxe habituelle]
  |-- decode() reussit -> injection du buffer dechiffre dans file_handle
  '-- org_compile_file(file_handle, type)   [PHP compile le contenu dechiffre]
```

Dans tous les cas, l'appel se termine par `org_compile_file()` — le
hook ne remplace jamais le compilateur Zend, il se contente de
substituer le **contenu** du fichier juste avant.

### 5.2 Injection du buffer dechiffre (`mbcrypt_inject_buf`)

```c
fh->buf = estrndup(data, len);   /* copie dans la memoire geree par Zend */
fh->len = len;
fh->handle.stream.closer = NULL; /* evite un close() sur un flux deja ferme */
```

Le buffer dechiffre (alloue par `malloc`, cote bibliotheque C pure)
est copie dans une chaine geree par l'allocateur Zend (`estrndup`,
memoire *liee a la requete* — c'est correct ici car ce buffer n'a
besoin de vivre que le temps de la compilation du fichier courant).
Le buffer `malloc` d'origine est libere juste apres (`free(decrypted)`
dans `mbcryptload_compile_file`).

### 5.3 Resolution du mot de passe

Trois sources possibles, dans cet ordre de priorite strict :

```c
password = MBCRYPTLOAD_G(password);                        /* 1. mbcryptload_set_password() */
if (!password || !*password) password = MBCRYPTLOAD_G(ini_password); /* 2. php.ini */
if (!password || !*password) password = getenv("MBCRYPTLOAD_PASSWORD"); /* 3. env */
```

La premiere source non vide gagne — un appel explicite en PHP prime
toujours sur la configuration globale.

### 5.4 Protection anti-divulgation

Avant meme de lire le fichier, le hook verifie si l'appelant est
`show_source()` ou `highlight_file()` (via `EG(current_execute_data)`,
qui donne acces a la fonction en cours d'execution dans la pile Zend)
et, si oui, laisse `org_compile_file()` gerer l'appel sans dechiffrer
— ces fonctions affichent le contenu brut d'un fichier, donc les
laisser passer sur un fichier chiffre revient a ne rien divulguer
(elles afficheront le blob chiffre illisible, pas le code source).

### 5.5 Compatibilite PHP 7.0 – 8.x

Le seul changement d'ABI significatif entre ces versions qui touche
`mbcryptload.c` est le type de `zend_file_handle.filename` :
`const char*` avant PHP 8.1, `zend_string*` a partir de 8.1. La macro
`MBCRYPT_FH_FILENAME()` (compilee conditionnellement sur
`PHP_VERSION_ID`) absorbe cette difference a un seul endroit du code :

```c
#if PHP_VERSION_ID >= 80100
#  define MBCRYPT_FH_FILENAME(fh) ((fh)->filename ? ZSTR_VAL((fh)->filename) : NULL)
#else
#  define MBCRYPT_FH_FILENAME(fh) ((fh)->filename)
#endif
```

`zend_file_handle.buf`/`.len`, en revanche, sont restes des
`char*`/`size_t` stables sur toute la plage 7.0-8.x — aucune macro
n'est necessaire pour `mbcrypt_inject_buf()`.

---

## 6. Cycle de vie du module PHP (MINIT / RINIT / RSHUTDOWN / MSHUTDOWN)

```
MINIT (une fois, au demarrage de PHP)
  - ZEND_INIT_MODULE_GLOBALS : reserve un emplacement de stockage
    par-thread pour la structure { password, ini_password }
  - REGISTER_INI_ENTRIES : declare mbcryptload.password (PHP_INI_SYSTEM)
  - installe le hook zend_compile_file

RINIT (a chaque requete)
  - ZTS uniquement : rafraichit le cache TSRM local au thread

[execution du script, appels a mbcryptload_set_password() eventuels]

RSHUTDOWN (fin de chaque requete)
  - rien de specifique a mbcryptload -- Zend recycle automatiquement
    toute la memoire allouee pendant la requete (emalloc/estrndup)

MSHUTDOWN (une fois, a l'arret de PHP)
  - restaure le hook zend_compile_file original
  - UNREGISTER_INI_ENTRIES
  - libere le mot de passe stocke dans les globals (voir section 7)
```

Les globals sont declares avec `ZEND_BEGIN_MODULE_GLOBALS` /
`ZEND_DECLARE_MODULE_GLOBALS`, un mecanisme standard de l'API Zend qui
fournit une structure **par thread** en mode ZTS (chaque thread PHP a
sa propre copie de `password`/`ini_password`, evitant toute donnee
partagee entre requetes concurrentes) et une structure globale unique
en mode NTS. L'acces se fait toujours via la macro `MBCRYPTLOAD_G(v)`,
qui pointe vers l'implementation correcte selon le mode de compilation :

```c
#ifdef ZTS
#  define MBCRYPTLOAD_G(v) TSRMG(mbcryptload_globals_id, zend_mbcryptload_globals *, v)
#else
#  define MBCRYPTLOAD_G(v) (mbcryptload_globals.v)
#endif
```

---

## 7. Piege corrige : destructeur ZTS appele apres dechargement de la DLL

Historique et diagnostic complet dans `build/GUIDE-COMPILATION-WINDOWS.md`
("Piege connu : globals ZTS et DLL dynamique"). Resume algorithmique :

`ZEND_INIT_MODULE_GLOBALS(module, ctor, dtor)` accepte un destructeur
optionnel, appele automatiquement par TSRM lors du demontage du
systeme de threads (`tsrm_shutdown()`). Sur Windows, pour une extension
chargee dynamiquement (`.dll`) en mode ZTS, ce demontage intervient
**apres** que le systeme d'exploitation a deja decharge la DLL de
l'extension — appeler ce pointeur de fonction saute alors dans une
zone memoire qui ne contient plus le code de la fonction, provoquant
un acces memoire invalide.

Solution retenue : ne jamais enregistrer de destructeur automatique
(`NULL` en troisieme argument de `ZEND_INIT_MODULE_GLOBALS`), et
liberer explicitement les ressources dans `PHP_MSHUTDOWN_FUNCTION`,
qui s'execute **avant** que PHP ne decharge le module — a un moment
ou le code de l'extension est garanti present en memoire.

Cela a fait apparaitre un second probleme lie a la duree de vie de la
memoire : `mbcryptload_set_password()` allouait la copie du mot de
passe avec `estrndup()`, une fonction **liee au cycle de la requete
PHP courante**. Or, entre le moment ou le mot de passe est defini et
le moment ou `MSHUTDOWN` s'execute, la requete qui a defini ce mot de
passe est deja terminee — et Zend a deja recycle automatiquement toute
sa memoire (`estrndup` y compris) a la fin de cette requete
(`RSHUTDOWN`). Appeler `efree()` sur ce pointeur dans `MSHUTDOWN`
revenait donc a liberer une memoire deja liberee (double-free),
corrompant le tas Zend (`zend_mm_heap corrupted`).

La correction complete utilise de la memoire **persistante**
(`pestrndup(pass, pass_len, 1)` / `pefree(ptr, 1)`), qui n'est liee a
aucune requete et reste valide jusqu'a ce que le module la libere
explicitement — coherent avec le role reel du champ : une valeur
globale qui doit survivre a la requete qui l'a definie.

---

## 8. Protections contre le double chiffrement

`mbcrypt.c` (CLI) et `mbcryptload_encrypt_file()` (API PHP) verifient
tous deux, avant de chiffrer, que le fichier source ne commence pas
deja par un tag `MB_ENC_V2` ou `MB_ENC_V3` — un fichier deja chiffre
est ignore (`Skip (already encrypted)`) plutot que rechiffre par
erreur, ce qui produirait un fichier illisible (double encapsulation
que rien ne sait decoder).

---

## 9. Recapitulatif des garanties de securite

| Garantie | Mecanisme |
|----------|-----------|
| Confidentialite (mode AES) | AES-256-GCM, cle derivee du mot de passe |
| Resistance a la force brute sur le mot de passe | PBKDF2-SHA256, 100 000 iterations |
| Cle unique par fichier | Sel + IV aleatoires generes a chaque chiffrement |
| Detection de falsification | Tag d'authentification GCM (128 bits) |
| Mot de passe jamais sur disque | Fourni a l'execution (fonction PHP, php.ini ou variable d'env) |
| Pas de fuite via show_source/highlight_file | Hook bloque explicitement ces deux fonctions |
| Robustesse memoire | Aucun buffer de taille fixe sur la pile (heap-only, zlib en streaming) |
