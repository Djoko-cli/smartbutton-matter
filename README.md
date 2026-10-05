# SmartButton

Bouton connecté **Matter over Thread** inspiré du **Philips Hue Smart Button**, sur ESP32-H2 (le C6 reste une cible secondaire), pile CR2450.

```
docs/     01-architecture · 02-energie · 03-hardware · 04-mecanique
firmware/ ESP-IDF + esp-matter, Generic Switch, ICD LIT
tools/    power_budget.py     — modèle d'autonomie paramétrable
          check_sdkconfig.sh  — valide les symboles Kconfig (voir plus bas)
```

---

## Deux corrections par rapport à l'idée de départ

**1. HomeSpan ne convient pas.** C'est une implémentation HomeKit/HAP **sur WiFi**. Ni Matter, ni Thread — deux piles protocolaires entièrement disjointes, rien n'est réutilisable. La voie est **ESP-IDF + esp-matter**. Détail dans [docs/01-architecture.md](docs/01-architecture.md).

**2. Le SoC.** Un ESP32 classique (ou S3, ou C3) n'a pas de radio 802.15.4, donc pas de Thread. Il faut un **H2** (retenu, sans WiFi) ou un **C6**.

## Et l'autonomie de « plusieurs années » ?

Pas sur une CR2032 avec un ESP32. Ce n'est pas une question de firmware, c'est le plancher de veille du silicium — le budget pour tenir 3 ans sur CR2032 est de **6,8 µA tout compris** :

| | Plancher light sleep |
|---|---|
| **ESP32-C6** *(retenu)* | 37–50 µA |
| ESP32-H2 | 24–30 µA |
| ESP32-H21 (2026) | 9 µA |
| nRF54L15 / EFR32 *(ce qu'utilise Hue)* | 1–2 µA |

Le C6 dépasse le budget **d'un facteur 7 rien qu'en dormant**, avant d'allumer la radio.

Autonomies modélisées (`python3 tools/power_budget.py`) :

| | CR2032 | **CR2450** | 2×AAA lithium |
|---|---|---|---|
| **ESP32-C6** | 4,6 mois | **1,1 an** | 2,3 ans |
| ESP32-H2 | 6,8 mois | 1,6 an | 3,4 ans |
| ESP32-H21 | 1,3 an | 3,7 ans | 7,6 ans |
| nRF54L15 | 4,6 ans | 12,1 ans | — |

Le modèle est calé sur des mesures PPK2 publiées : à 30 s de poll il prédit 232 µA sur C6 (231 mesurés) et 198 µA sur H2 (191 mesurés).

**Configuration retenue : ESP32-H2 + CR2450 → ~1,6 an.** La CR2450 fait Ø 24,5 × 5,0 mm et rentre sans problème dans le format du Hue (c'est la pile du Hue Dimmer Switch) : tu gardes l'apparence.

Pourquoi l'H2 plutôt que le C6 (détail dans [docs/02-energie.md](docs/02-energie.md)) :

- Le C6 embarque une radio **WiFi 6 inutilisée ici** qui lui coûte ~17 µA de veille en plus. Passer au H2 rendrait **+50 % d'autonomie** pour un `idf.py set-target` côté firmware (les deux `sdkconfig.defaults.*` sont fournis) et une empreinte de module différente côté PCB.
- ⚠️ Une mesure publiée montre le courant de veille du C6 **très sensible à VDD** : ~300 µA à 3,3 V contre ~37 µA à 3,8 V. Si ça se confirme, c'est rédhibitoire sur pile bouton. **C'est le premier test à faire** une fois la devkit qui tourne.

---

## Environnement

**ESP-IDF v5.5.4 + esp-matter**, installés dans `~/esp` (cibles `esp32h2` et `esp32c6`).

Les scripts chargent l'environnement tout seuls. Pour utiliser `idf.py` à la main :

```bash
source tools/env.sh
```

> `env.sh` existe parce que PlatformIO (projets BenQ / Amaran) place son propre `python3` en tête du PATH : `export.sh` cherche alors un environnement Python inexistant et **échoue sans bloquer le shell**. `env.sh` désigne explicitement le bon.

### Profils

La cible est l'**ESP32-H2**. La configuration se superpose en couches :

| Profil | Couches | Usage |
|---|---|---|
| `dev` | base + `profiles/supermini.defaults` + `profiles/dev.defaults` | Étape 1 : pas de veille, console et shell CHIP sur l'USB-C, ICD aux intervalles courts |
| `sleepy` | base + `profiles/supermini.defaults` | Étapes 2-3 : vrai profil ICD LIT + light sleep, logs via adaptateur UART |
| `pcb` | base seule | PCB final, avec quartz 32 kHz |

Chaque profil a son dossier `firmware/build-<profil>-<cible>/` : passer de l'un à l'autre ne recompile pas tout.

```bash
./tools/build.sh dev
```

```bash
./tools/build.sh dev flash monitor
```

```bash
TARGET=esp32c6 ./tools/build.sh dev
```

## Matériel pour démarrer

| | |
|---|---|
| **ESP32-C6-DevKitC-1** | pour l'étape 1 |
| Border router Thread | HomePod mini / Apple TV, Nest Hub, ou un ESP32 en OTBR |
| **Nordic PPK2** | ~90 € TTC — non optionnel dès l'étape 2, un multimètre ne sait pas mesurer un profil qui va de 40 µA à 90 mA en 200 µs |

## Feuille de route

1. **Devkit, firmware non-sleepy** → appairage réussi + les 4 gestes reconnus dans l'app cible.
2. Activer ICD LIT + light sleep → mesurer au PPK2, **dont le balayage VDD 3,0 → 3,6 V**.
3. Trancher C6 vs H2 et la tension du boost, sur la mesure.
4. Schéma + PCB (boost converter, quartz 32 kHz, antenne vs aimants).
5. Mécanique.

**Le vrai risque n°1 n'est pas l'énergie, c'est l'attestation.** Un appareil DIY porte des certificats de test : Home Assistant les accepte, Google Home demande un enregistrement en Developer Console, Apple Home est le plus strict. **À tester à l'étape 1**, avant tout investissement en PCB ou en CAO. Voir [docs/01-architecture.md §5](docs/01-architecture.md).

---

## Le piège des symboles Kconfig

Un symbole inconnu dans un `sdkconfig.defaults` est **ignoré silencieusement**. Sur un projet dont toute la valeur tient au réglage fin de l'ICD et du power management, une faute de frappe ne se voit pas au build — elle se voit six mois plus tard sur la pile.

Ce n'est pas théorique : à la première vérification, **13 des symboles ICD et Thread écrits de mémoire étaient faux**. Les vrais noms n'ont pas le préfixe `CHIP_` (`ENABLE_ICD_LIT` et non `CHIP_ICD_LIT_SUPPORT`, `ICD_IDLE_MODE_INTERVAL_SEC` et non `CHIP_ICD_IDLE_MODE_DURATION_SEC`…). Le firmware aurait compilé sans une seule alerte, et tourné avec l'ICD désactivé et un réveil toutes les 2 secondes.

D'où le vérificateur, à relancer après chaque mise à jour d'ESP-IDF ou d'esp-matter :

```bash
./tools/check_sdkconfig.sh
```

## L'autre piège : le child timeout Thread

Le poste d'énergie dominant est le **data poll Thread**, pas le check-in ICD. Et OpenThread calcule sa période ainsi :

```
période_poll = Min(child_timeout − marge, ICD_SLOW_POLL_INTERVAL_MS)
```

Le child timeout vaut **240 s par défaut** et **n'a aucun symbole Kconfig dans ESP-IDF** — il ne se règle qu'à l'exécution. Régler `ICD_SLOW_POLL_INTERVAL_MS` à 30 minutes sans y toucher **ne change rien du tout** : 72,5 µA au lieu de 54,0 µA, soit 10 mois au lieu de 1,1 an, sans aucun signe au build ni au log.

Le firmware relève donc le child timeout à 3600 s au démarrage. Détail et tableau de sensibilité dans [docs/02-energie.md §2 bis](docs/02-energie.md).

## État du code

**Le firmware compile pour l'ESP32-H2**, profils `dev` et `sleepy`. Le binaire tient dans la partition de 1900 Ko avec ~18 % de marge.

**RAM statique : 72 % (`dev`) et 76 % (`sleepy`)** des 258 Ko de DIRAM. Il reste 62 à 73 Ko pour le tas, que Matter consomme pendant le commissioning. C'est le point serré de l'H2 : surveiller le tas libre après appairage. Si ça coince, la parade est la liste d'exclusion des clusters inutilisés (`CONFIG_SUPPORT_*_CLUSTER=n`, comme dans les exemples esp-matter).

Tout est calé sur les sources installées, vérifié et non écrit de mémoire : ESP-IDF v5.5.4, esp-matter, `espressif/button` v4.2.0.

**Validé sur matériel** (ESP32-H2 SuperMini, octobre 2026) : appairage dans Apple Home, pression simple, double et appui long, **y compris en profil `sleepy`** avec light sleep, après plusieurs minutes de veille profonde.

Deux défauts trouvés et corrigés pendant ces essais :
- **le composant `espressif/button` (4.2.0, et toujours sur master) désarme le réveil EXT1 après le premier appui** quand les périphériques sont éteints en veille (`PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP`). Le bouton ne réveillait alors plus la puce. Contourné dans `app_button.cpp` en réarmant EXT1 à chaque retour en basse conso ;
- la fenêtre de double pression (180 ms) était trop courte à l'usage : elle passe à 300 ms.

Restent à valider : la consommation réelle (PPK2), et l'autonomie qui en découle en mode SIT (Apple n'active pas le LIT, voir `docs/02-energie.md` §2 ter).

## Licence

[MIT](LICENSE). « Philips » et « Hue » sont des marques de Signify : ce projet indépendant n'y est pas affilié et ne reproduit ni leur nom ni leurs logos sur le produit.
