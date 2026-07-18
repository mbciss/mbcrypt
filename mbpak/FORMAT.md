# Format `.mbpak` v3

Archive chiffrée, lecture/écriture, construite au-dessus de `mbcrypt_lib`
(`../mbcrypt/mbcrypt_lib.c`, non modifié). Un seul fichier binaire, aucune
extraction sur disque nécessaire pour le lire.

## Décision de conception : AES-256-GCM uniquement

Contrairement à `mbcrypt` (qui garde un mode XOR legacy pour la
compatibilité V2), `.mbpak` **exige un mot de passe et n'a pas de mode
XOR**. C'est un nouveau format sans contrainte de compatibilité
ascendante, et l'objectif affiché (protection maximale, TOC illisible)
n'a pas de sens sans authentification. `mbpak_create()`/`mbpak_open()`
refusent un mot de passe vide.

## Vue d'ensemble

```
┌─────────────────────────────────────────────────────────┐
│ HEADER (40 o, clair, offset fixe 0, jamais deplace)      │
├─────────────────────────────────────────────────────────┤
│ RECORD  [8 o len][ blob MB_ENC_V3 du fichier 1 ]         │
│ RECORD  [8 o len][ blob MB_ENC_V3 du fichier 2 ]         │
│ ...     (append-only -- jamais reecrit en place)         │
├─────────────────────────────────────────────────────────┤
│ RECORD  [8 o len][ blob MB_ENC_V3 de la TOC serialisee ] │
└─────────────────────────────────────────────────────────┘
```

Chaque fichier *et* la TOC elle-meme sont stockés comme un `RECORD`
identique : un blob `MB_ENC_V3` structurellement identique à ce que
produit `mbcrypt_lib_encode()` (le meme format qu'un `.php` chiffré
seul par `mbcrypt`), mais généré par `mbcrypt_lib_encode_with_key()`
(voir "Dérivation de clé" plus bas) -- toujours zéro nouvelle primitive
cryptographique, juste la manière dont la clé AES arrive à cette
primitive qui change entre v1/v2 et v3.

## HEADER (40 octets)

| Offset | Taille | Champ          | Rôle |
|--------|--------|----------------|------|
| 0      | 8      | `magic`        | `"MBPAK001"` |
| 8      | 1      | `version`      | = 3 |
| 9      | 3      | `reserved`     | zero-pad |
| 12     | 16     | `kdf_salt`     | salt PBKDF2 pour toute l'archive (v3, voir plus bas) |
| 28     | 8      | `toc_offset`   | u64 LE — offset absolu du RECORD contenant la TOC courante |
| 36     | 4      | `header_crc32` | CRC32 des octets [0..36) — détecte une écriture de header tronquée ; **ce n'est pas un secret**, juste une protection anti-corruption |

C'est le **seul** endroit du fichier réécrit en place. Tout le reste est
en append-only.

## RECORD (fichier ou TOC)

```
[8 o]  blob_len   u64 LE
[blob_len o]  blob MB_ENC_V3
```

Le blob est la sortie brute de `mbcrypt_lib_encode_with_key(data, len,
key)` : tag `"MB_ENC_V3"` + version + method + reserved + champ salt
(16 o, **vestige structurel, ignoré en lecture depuis v3** -- voir
"Dérivation de clé") + nonce GCM (12 o, **frais et aléatoire à chaque
record**) + tag GCM (16 o) + ciphertext zlib-compressé. Lu et vérifié
par `mbcrypt_lib_decode_with_key()`.

La longueur du record n'est **pas** dupliquée dans la TOC : elle est
lue depuis le préfixe de 8 octets du RECORD lui-même à `offset`. Une
seule source de vérité.

## Dérivation de clé (v3) — la clé se dérive une fois par archive, pas une fois par fichier

`kdf_salt` (dans le HEADER) sert à dériver la clé AES de l'archive
**une seule fois**, à `mbpak_open()`/`mbpak_create()`, via
`mbcrypt_lib_derive_key()` (PBKDF2-SHA256, volontairement lent --
~100ms). Cette clé unique est ensuite réutilisée pour **tous** les
RECORDs via `mbcrypt_lib_encode_with_key()`/`decode_with_key()` --
chaque record garde son propre nonce GCM aléatoire (obligatoire pour la
sécurité d'AES-GCM : jamais réutiliser un couple (clé, nonce)), mais
plus aucun n'effectue sa propre dérivation PBKDF2.

**v1/v2 dérivaient une clé fraîche à partir du mot de passe pour
CHAQUE record** (`mbcrypt_lib_encode()`/`decode()`, chacun avec son
propre salt aléatoire) -- ce qui voulait dire que N fichiers coûtaient
N x le coût PBKDF2 (~100ms x N). Ça n'a pas été repéré en théorie mais
en pratique : en empaquetant le dossier `app/` d'une vraie application
Laravel et en chargeant une page réelle, `mbpak_get()` coûtait ~96ms
**par classe autoloadée**. Sur un dossier `app/` réaliste (dizaines de
fichiers), ça aurait donné plusieurs secondes de temps de chargement
rien qu'en dérivation de clé. Voir `pistes-a-explorer.md` pour le
détail complet de cette session de test.

Le champ `salt` conservé dans chaque blob `MB_ENC_V3` (pour rester
structurellement identique au format `mbcrypt` mono-fichier) est donc
**vestigial** en v3 : rempli avec des octets aléatoires à l'écriture,
jamais lu au déchiffrement (`mbcrypt_lib_decode_with_key()` reçoit la
clé directement, elle n'a pas besoin de ce champ).

## TOC (texte clair avant chiffrement)

```
[4 o]  entry_count   u32 LE
répété entry_count fois :
  [2 o]  path_len     u16 LE
  [path_len o]  path  UTF-8, séparateur '/', sans '/' initial
  [8 o]  offset       u64 LE -- début du RECORD du fichier
  [4 o]  flags        u32 LE -- bit0 = MBPAK_FLAG_TOMBSTONE
  [8 o]  mtime        u64 LE -- timestamp unix, informatif
  [8 o]  size         u64 LE -- taille en clair, en octets (v2+)
```

Ce buffer est passé tel quel à `mbcrypt_lib_encode_with_key()` -- la
TOC est chiffrée avec la même clé dérivée que les fichiers.
`entry_count == 0` produit un buffer de 4 octets, jamais vide
(important : `mbcrypt_lib_encode_with_key()` refuse une entrée de
longueur 0).

**`size` est dupliquée volontairement (v2)** : c'est la seule métadonnée
qui ne peut pas se déduire du RECORD sans un déchiffrement AES-GCM +
décompression zlib complets. Le wrapper `mbpak://` en a besoin pour
répondre à `file_exists()`/`filesize()`/`is_file()` sans rien déchiffrer
(voir `mbpak_stream_url_stat()` dans `../mbcrypt/mbcryptload.c`) --
important pas seulement par principe : l'autoloader PSR-4 de Composer
appelle `file_exists()` sur chaque chemin candidat en résolvant une
classe, donc payer un déchiffrement complet par `stat()` serait un vrai
coût, pas juste une question de pureté.

**Historique de version** : ni v1 ni v2 n'ont jamais été distribuées
(fixtures de test locales uniquement à chaque fois), donc chaque bump
s'est fait sans migration. v1 -> v2 : ajout du champ `size` par entrée.
v2 -> v3 : ajout de `kdf_salt` dans le HEADER + dérivation de clé
unique par archive (voir plus haut) -- le HEADER passe de 24 à 40
octets. `read_header()` refuse d'ouvrir une archive dont l'octet de
version ne correspond pas exactement, à chaque fois -- pas de tentative
de lecture partielle d'une version antérieure avec un parseur plus
récent.

## Protocole d'écriture (commit)

1. `mbpak_put(path, data)` : chiffre `data` -> nouveau blob, **append**
   en fin de fichier, met à jour la TOC **en mémoire seulement**. Les
   autres RECORDs ne sont jamais touchés.
2. `mbpak_flush()` : sérialise la TOC en mémoire, la chiffre, l'append
   en fin de fichier, puis réécrit les 40 octets du HEADER pour
   pointer sur ce nouveau RECORD.
3. Point de commit = l'écriture du HEADER. Un crash avant cette écriture
   laisse l'archive sur son état précédent, toujours valide (ancienne
   TOC, anciens records, rien de corrompu). Limite connue : pas de
   `fsync()` explicite avant la réécriture du header, donc pas
   d'atomicité garantie face à une coupure d'alimentation (seulement
   face à un crash logiciel). À ajouter si le format est un jour utilisé
   en environnement hostile à l'intégrité disque.
4. Les anciennes versions d'un fichier réécrit restent en octets
   orphelins jusqu'à `mbpak_compact()` (réécrit l'archive en ne gardant
   que les records vivants, via fichier temporaire + `rename()`).

## Piège rencontré pendant l'implémentation (corrigé)

Sur une archive fraîchement créée, `write_record()` fait
`fseek(fp, 0, SEEK_END)` pour trouver où append. Si le HEADER n'a pas
encore été écrit, l'EOF d'un fichier tout juste ouvert est l'offset 0 --
donc le tout premier RECORD (la TOC vide) atterrissait à l'offset 0, et
l'écriture du HEADER qui suit dans `mbpak_flush()` l'écrasait ensuite
(header et record au même endroit). Corrigé en réservant l'espace du
HEADER (`write_header(fp, 0)` en placeholder) **avant** le premier
`mbpak_flush()` dans `mbpak_create()`. Détecté par le test "archive
vide" (`test/run.sh`, étape 8) avant toute écriture de documentation.

## Ce qui n'est PAS authentifié

Le tag `"MB_ENC_V3"`, `version`, `method` et les 6 octets `reserved` du
blob ne sont pas couverts par le tag GCM (`aes_gcm_encrypt()` n'utilise
pas d'AAD). Modifier ces octets ne fait pas échouer l'authentification.
En pratique ça reste sans conséquence exploitable : `reserved` est
inutilisé, et corrompre `tag`/`version`/`method` fait juste échouer la
détection du format (`mbcrypt_lib_detect()`) -- même résultat (échec),
juste par une voie différente. Ce comportement est hérité de
`mbcrypt_lib` telle quelle, pas introduit par `.mbpak`.

## Performance (après le passage en v3)

Mesuré sur `mbpak/mbpak_cli.c` (30 fichiers empaquetés, `mbpak extract`
= 1 mount + 30 déchiffrements) : ~89ms au total, contre ~3,1s attendus
avec le modèle v1/v2 (1 dérivation PBKDF2 par fichier, ~100ms x 31).
Le coût de dérivation de clé est désormais O(1) par archive montée,
plus O(N) déchiffrements AES-GCM (rapides, pas de PBKDF2) pour N
fichiers lus -- au lieu de O(N) dérivations PBKDF2 complètes.

## Limitations connues (état actuel)

- Un seul écrivain à la fois (aucun verrouillage de fichier).
- Offsets/longueurs manipulés via `long`/`ftell()` (cohérent avec le
  reste du projet) -- pas testé au-delà de quelques Go.
- Intégration PHP : implémentée (`mbpak_mount`/`unmount`/`list`/`get`/
  `put`/`delete`/`compact` + wrapper `mbpak://` en lecture seule +
  `url_stat` -- `file_exists()`/`is_file()`/`filesize()`/`filemtime()`
  fonctionnent sans rien déchiffrer, voir `../mbcrypt/mbcryptload.c` et
  `../pistes-a-explorer.md` section "Approche C" pour le détail, y
  compris deux pièges rencontrés et corrigés pendant les tests). Le
  wrapper ne supporte pas `fopen(..., "w")` -- c'est un choix, pas un
  oubli : écrire via `mbpak_put()` reflète le vrai protocole d'append +
  commit au lieu de simuler un fichier POSIX que l'archive n'est pas.
  `stream_stat` (fstat sur un handle déjà ouvert) n'est pas implémenté,
  rien n'en a eu besoin jusqu'ici.
