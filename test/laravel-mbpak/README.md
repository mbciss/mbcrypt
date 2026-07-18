# Integration mbpak / Laravel

Procedure complete et verifiee pour faire tourner une vraie application
Laravel avec `app/`, `config/`, `routes/web.php` et `resources/views/`
empaquetes dans une archive `.mbpak` chiffree et **retires du disque**.

```bash
bash test/laravel-mbpak/setup.sh
```

Le script telecharge un Laravel neuf (`composer create-project
laravel/laravel`), construit `mbcryptload.so` + le CLI `mbpak` depuis
`../../mbcrypt` et `../../mbpak`, applique toutes les modifications
decrites ci-dessous dans l'ordre correct, empaquete, retire les
fichiers reels du disque, lance le serveur PHP integre et verifie une
route qui exerce Eloquent, l'heritage de classes entre deux fichiers
separement charges depuis l'archive, la configuration (via le cache) et
le rendu d'une vue Blade compilee depuis `mbpak://`. Tout tourne dans un
conteneur Docker persistant (`mbpak-laravel-test`) + un volume local
`work/` (ignore par git, ~80 Mo de `vendor/` Composer -- jamais commit).

Version Laravel testee : 13.20.0 (PHP 8.3). Resultat attendu :

```
GET /            -> HTTP 200
GET /mbpak-test  -> HTTP 200
{"source":"mbpak:\/\/","user_count":0,"controller_class":"App\\Http\\Controllers\\TestController",
 "parent_class":"App\\Http\\Controllers\\Controller",
 "config_test":"config chargee depuis le cache genere a partir de mbpak:\/\/",
 "view_render":"<p>Bonjour Laravel, cette vue Blade a ete compilee depuis mbpak:\/\/<\/p>"}
```

## Perimetre : quoi empaqueter, quoi laisser sur disque

| Dossier | Empaquete ? | Pourquoi |
|---|---|---|
| `app/` | **Oui** | Code metier propriétaire, aucune complication -- Composer charge les classes via `include "mbpak://..."`, direct |
| `routes/web.php` (et `api.php` s'il existe) | **Oui** | `Route::group($path)` fait un `require` direct, aucun scan de repertoire |
| `resources/views/` | **Oui** | Resolution par `file_exists()` (avec repli silencieux sur `realpath()`), pas de scan bloquant |
| `config/` | **Oui, avec condition** | Le scan par defaut de Laravel (`Finder`) est bloque par `realpath()` -- necessite `php artisan config:cache` genere **avant** l'empaquetage (voir plus bas) |
| `routes/console.php` | **Non** | Passe a `realpath($commands) !== false` dans `ApplicationBuilder::withRouting()` -- toujours faux pour un chemin `mbpak://`, silencieusement ignore |
| `vendor/` | **Non** | Code open-source, aucune valeur a cacher ; des milliers de fichiers Composer/paquets tiers, aucune raison d'exercer le wrapper sur eux |
| `storage/`, `bootstrap/cache/` (sauf `config.php` genere) | **Non** | Write-heavy, multi-worker PHP-FPM, gros fichiers -- voir la discussion "quoi packer / quoi laisser" dans `../../pistes-a-explorer.md` |

## Fichiers Laravel a modifier (recapitulatif)

1. **`public/index.php`** -- ajouter `mbpak_mount($archive, $password)`
   comme toute premiere instruction, avant `require
   .../vendor/autoload.php`.
2. **`artisan`** -- meme ajout, meme raison : `withRouting()` enregistre
   les routes a **chaque** boot de l'application, HTTP ou CLI. Sans ca,
   toute commande `artisan` echoue avec *"Unable to find the wrapper
   mbpak"*.
3. **`bootstrap/app.php`** -- `web: __DIR__.'/../routes/web.php'`
   devient `web: 'mbpak://routes/web.php'`. Laisser `commands:` sur le
   vrai chemin disque (voir tableau ci-dessus).
4. **`vendor/composer/autoload_static.php`** -- repointer chaque entree
   de la classmap `App\*` (`__DIR__ . '/../..' . '/app/Xxx.php'`) vers
   `'mbpak://app/Xxx.php'`. C'est ce fichier, pas
   `autoload_psr4.php`/`autoload_classmap.php`, qui fait foi a
   l'execution (`vendor/composer/autoload_real.php` l'utilise
   directement) -- editer les autres n'aurait aucun effet.
5. **`config/view.php`** (a publier avec `php artisan config:publish
   view` si absent -- Laravel 11+ ne le genere plus par defaut) --
   `'paths' => [resource_path('views')]` devient `'paths' =>
   ['mbpak://resources/views']`.
6. Generer **`bootstrap/cache/config.php`** via `php artisan
   config:cache` -- voir "Ordre des operations" ci-dessous, c'est le
   point le plus delicat de toute la procedure.

## Ordre des operations -- le point critique

**Toutes les commandes `composer`/`artisan` (dump-autoload, migrations,
`config:cache`, `package:discover`) doivent tourner AVANT les etapes
4 et 3 ci-dessus (repointage de la classmap et de `bootstrap/app.php`).**

Raison : des qu'un seul de ces deux fichiers pointe vers `mbpak://`,
**tout** boot de l'application -- y compris un simple `composer
dump-autoload` (qui declenche `@php artisan package:discover` en
post-hook) -- tente de charger une classe ou une route depuis
`mbpak://` et echoue avec *"Unable to find the wrapper mbpak"*, parce
que ces commandes tournent avec un PHP nu, sans l'extension chargee ni
l'archive montee.

Sequence validee par `setup.sh` :

```
1. composer create-project laravel/laravel
2. Ecrire le code applicatif (app/, config/, routes/, resources/views/)
   -- fichiers reels, rien de mbpak-specifique encore
3. Publier + editer config/view.php (etape 5 ci-dessus)
4. composer dump-autoload
5. php artisan config:cache          <- capture 'mbpak://resources/views'
                                          dans bootstrap/cache/config.php
6. mbpak pack app/ config/ resources/views/ routes/web.php -> app.mbpak
7. Repointer vendor/composer/autoload_static.php (etape 4)
8. Repointer bootstrap/app.php (etape 3)
9. Ajouter mbpak_mount() a public/index.php et artisan (etapes 1-2)
10. Retirer app/, config/, resources/views/, routes/web.php du disque
```

## Le compromis honnete sur `config/`

Empaqueter `config/` protege les **fichiers source** (organisation,
commentaires, structure) contre une inspection disque -- mais les
**valeurs resolues** doivent exister en clair dans
`bootstrap/cache/config.php` pour que Laravel demarre : le chargement
par defaut (`LoadConfiguration::getConfigurationFiles()`) fait
`realpath($app->configPath())`, qui renvoie toujours `false` pour un
chemin `mbpak://` (`realpath()` ne resout jamais un schema de flux
personnalise, limitation PHP fondamentale, pas quelque chose qu'un
wrapper peut contourner) -- et Laravel retourne alors silencieusement
une liste de fichiers de configuration **vide**. Le contournement est
`config:cache`, deja la pratique de production recommandee par Laravel
independamment de mbpak : le cache est verifie en tout premier
(`file_exists($app->getCachedConfigPath())`) et court-circuite
entierement le scan par repertoire.

En clair : `config/` empaquete protege la distribution/le depot des
sources, pas les valeurs en memoire au moment de l'execution -- rien ne
peut proteger ca sans casser le fonctionnement de l'application.

## Pieges rencontres pendant la mise au point (et pourquoi)

- **`realpath($commands) !== false` bloque `routes/console.php`** --
  verifie dans le vrai code source
  (`Illuminate\Foundation\Configuration\ApplicationBuilder::withRouting()`,
  Laravel 11.x/13.x) : ce test s'applique uniquement au parametre
  `commands:`, pas a `web:`/`api:` quand ils sont passes comme chaine
  simple (le cas par defaut) -- d'ou l'asymetrie entre `routes/web.php`
  (fonctionne) et `routes/console.php` (ne fonctionne pas).
- **`SplFileInfo::getRealPath()` renvoie `ValueError: Path cannot be
  empty`** sur un resultat de `Symfony\Component\Finder\Finder` iterant
  `mbpak://` -- utiliser `getPathname()` a la place.
  `Illuminate\View\FileViewFinder::resolvePath()` s'en sort seule
  (`return realpath($path) ?: $path;` -- repli silencieux), mais du
  code personnalise qui appellerait `getRealPath()` directement sur un
  resultat Finder casserait de la meme maniere.
- **Chemins d'archive incoherents avec la structure attendue** :
  `mbpak pack` cree des chemins virtuels relatifs au dossier empaquete
  -- empaqueter `app/` seul donne `Models/User.php`, empaqueter un
  dossier de mise en scene contenant `app/`, `config/`, etc. donne
  `app/Models/User.php`. Le prefixe `mbpak://...` cote PHP
  (`config('view.paths')`, la classmap Composer, `bootstrap/app.php`)
  doit correspondre exactement -- une erreur ici donne des `file_exists()`
  qui renvoient silencieusement `false`, pas une erreur explicite.

Le detail de l'implementation cote extension (`url_stat`, `dir_opener`,
format `.mbpak` v3, bugs C trouves et corriges) est dans
`../../pistes-a-explorer.md`, section "Approche C".

## Nettoyage

```bash
docker rm -f mbpak-laravel-test
sudo rm -rf test/laravel-mbpak/work   # root-owned (cree par le conteneur)
```
