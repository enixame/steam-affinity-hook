# steam-affinity-hook

Hook Linux `LD_PRELOAD` pour contourner un problème d'affinité CPU appliquée par Steam / Proton / wineserver.

## Problème

Dans certains environnements Linux, notamment en conteneur / cloud VM, le `cpuset` amont est correct, mais Steam ou wineserver appellent ensuite `sched_setaffinity()` avec un masque CPU trop restrictif.

Exemple typique :

- `cpuset` autorisé : `0-7`
- Steam / wineserver impose ensuite : `0,2,4,6`

Selon la topologie SMT, cela peut dégrader fortement les performances si le masque choisi ne reflète pas correctement les cœurs physiques disponibles.

Ce projet fournit un hook `LD_PRELOAD` qui intercepte les appels à :

- `sched_setaffinity()`
- `pthread_setaffinity_np()`

et permet de :

- **bloquer** certains changements d'affinité
- ou **réécrire** certains masques CPU vers une valeur plus saine

## Fonctionnement

Le hook supporte deux modes principaux :

### 1. Mode exact avec `AFFINITY_HOOK_MATCH`

Si `AFFINITY_HOOK_MATCH` est défini :

- si le masque demandé correspond exactement à `AFFINITY_HOOK_MATCH`
  - `AFFINITY_HOOK_BLOCK=1` : le hook bloque l'appel et retourne succès
  - sinon :
    - si `AFFINITY_HOOK_REWRITE` est défini : réécriture vers ce masque
    - sinon : réécriture vers `0-(nproc --all - 1)`

### 2. Mode implicite sans `AFFINITY_HOOK_MATCH`

Si `AFFINITY_HOOK_MATCH` n'est pas défini :

- le hook considère comme masque "normal" le masque complet machine `0-(nproc --all - 1)`
- si le masque demandé est différent :
  - si `AFFINITY_HOOK_REWRITE` est défini : réécriture vers ce masque
  - sinon : réécriture vers `0-(nproc --all - 1)`

## Variables d'environnement

### `AFFINITY_HOOK_LOG=1`

Active les logs de debug sur `stderr`.

### `AFFINITY_HOOK_MATCH=...`

Masque CPU exact à détecter.

Exemples :

- `0,2`
- `0,2,4,6`
- `0-3`

### `AFFINITY_HOOK_REWRITE=...`

Masque CPU de remplacement.

Exemples :

- `0,1`
- `0-7`
- `0-3`

### `AFFINITY_HOOK_BLOCK=1`

Si le masque matche et que cette variable est activée, l'appel est ignoré mais retourne succès.

## Compilation

### Version 64-bit

```bash
gcc -shared -fPIC -O2 -Wall -Wextra -D_GNU_SOURCE \
    -o libaffinity_hook.so \
    affinity_hook_fullmask.c \
    -ldl -pthread
