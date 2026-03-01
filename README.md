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
```

### Version 32-bit

```bash
gcc -m32 -shared -fPIC -O2 -Wall -Wextra -D_GNU_SOURCE \
    -o libaffinity_hook32.so \
    affinity_hook_fullmask.c \
    -ldl -pthread
```

### Dépendances

Pour compiler la version 32-bit sur Ubuntu/Debian :

```bash
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install gcc-multilib libc6-dev-i386
```

### Utilisation

Bloquer exactement `0,2,4,6`

```bash
AFFINITY_HOOK_LOG=1 \
AFFINITY_HOOK_MATCH=0,2,4,6 \
AFFINITY_HOOK_BLOCK=1 \
LD_PRELOAD="/chemin/libaffinity_hook32.so:/chemin/libaffinity_hook.so" \
steam
```

Réécrire exactement `0,2,4,6` vers `0-7`

```bash
AFFINITY_HOOK_LOG=1 \
AFFINITY_HOOK_MATCH=0,2,4,6 \
AFFINITY_HOOK_REWRITE=0-7 \
LD_PRELOAD="/chemin/libaffinity_hook32.so:/chemin/libaffinity_hook.so" \
steam
```

Sans `MATCH`, réécrire tout masque non complet vers `0-7`

```bash
AFFINITY_HOOK_LOG=1 \
AFFINITY_HOOK_REWRITE=0-7 \
LD_PRELOAD="/chemin/libaffinity_hook32.so:/chemin/libaffinity_hook.so" \
steam
```

## Vérification

### Vérifier que la bibliothèque est chargée

Les lignes suivantes indiquent que la bibliothèque est bien chargée :
```txt
[affinity-hook] loaded pid=...
```

### Vérifier qu'un appel est réellement intercepté

Si le hook intercepte un appel réel, on doit voir :
```txt
[affinity-hook] init ...
[affinity-hook] sched_setaffinity ...
```

ou:
```txt
[affinity-hook] pthread_setaffinity_np ...
```

### Vérifier l'affinité réelle
Attention : `sched_setaffinity()` s'applique aux threads Linux, pas seulement au PID principal.

Pour vérifier les threads d'un processus :
```bash
for t in /proc/<pid>/task/*; do
    tid=$(basename "$t")
    taskset -pc "$tid"
done
```

Ou :
```bash
for t in /proc/<pid>/task/*; do
    tid=$(basename "$t")
    echo -n "tid=$tid "
    grep '^Cpus_allowed_list:' "$t/status"
done
```

## Limitations

### 1. `LD_PRELOAD` doit atteindre le bon processus
Steam, Steam Runtime, Pressure Vessel, Proton et wineserver peuvent lancer différents sous-processus 32-bit et 64-bit.

Il faut donc en pratique fournir :
- une bibliothèque 64-bit
- une bibliothèque 32-bit

### 2. Le hook ne voit que les symboles interceptés
Ce hook intercepte :
- `sched_setaffinity()`
- `pthread_setaffinity_np()`
- `__sched_setaffinity()`
- `__pthread_setaffinity_np()`

Si un composant utilise un chemin différent, par exemple un syscall direct, alors ce hook peut être chargé sans jamais voir l'appel fautif.

Dans ce cas, les logs montreront souvent uniquement :
```txt
[affinity-hook] loaded pid=...
```

sans aucun :
```txt
[affinity-hook] init ...
```

### 3. Les messages `wrong ELF class` sont partiellement normaux
Quand `LD_PRELOAD` contient à la fois une version 32-bit et une version 64-bit :
- un processus 64-bit ignorera la `.so` 32-bit
- un processus 32-bit ignorera la `.so` 64-bit

Cela produit des messages `wrong ELF class`, ce qui est attendu dans ce mode mixte.

## Cas d'usage recommandé

Le mode le plus simple pour Steam est souvent :
```bash
AFFINITY_HOOK_LOG=1 \
AFFINITY_HOOK_MATCH=0,2,4,6 \
AFFINITY_HOOK_REWRITE=0-7 \
LD_PRELOAD="/chemin/libaffinity_hook32.so:/chemin/libaffinity_hook.so" \
steam
```
