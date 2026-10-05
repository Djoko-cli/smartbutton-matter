# Budget énergie — le vrai sujet

## TL;DR

> **« Plusieurs années sur une CR2032 avec un ESP32 » n'est pas atteignable.**
> Le plancher de veille de l'ESP32-C6 est ~7× trop haut, celui du H2 ~4×. Ce n'est pas un problème de firmware, c'est le silicium.

Ce qui est atteignable :

| Combinaison | Autonomie modélisée |
|---|---|
| **ESP32-C6 + CR2450** | **~1,1 an** ← *configuration retenue* |
| ESP32-C6 + CR2032 | ~4,6 mois |
| ESP32-H2 + CR2450 | ~1,6 an (+45 % vs C6, même carte) |
| ESP32-H2 + 2×AAA lithium | ~3,4 ans |
| ESP32-H21 + CR2450 | ~3,7 ans (quand le silicium sera dispo) |
| nRF54L15 + CR2032 | ~4,6 ans (référence : c'est ce que fait Hue) |

> Ces chiffres supposent le **child timeout Thread relevé à 3600 s** (§2 bis).
> Sans ce réglage, retirer ~25 % : le C6 tombe à ~10 mois sur CR2450.
>
> ⚠️ **Ils supposent aussi le mode ICD LIT, qu'Apple Home n'active pas** (constaté le 05/10/2026, §2 ter). Avec Apple, l'appareil reste en SIT, et l'autonomie dépend du coût réel d'un poll, pas encore mesuré : entre ~2 et ~11 mois sur CR2450.

---

## 1. D'où vient le plancher

L'appareil passe **99,99 % de son temps endormi**. Donc c'est le courant de veille qui décide de tout, pas l'efficacité du code.

Un nœud Thread sleepy ne peut pas utiliser le *deep sleep* en régime normal : la pile OpenThread + Matter vit en RAM, et la ré-attache au parent Thread coûte 2 à 5 secondes de radio allumée (mesuré par la communauté). Sur un bouton qui doit répondre en < 300 ms, c'est doublement rédhibitoire. On vit donc en **light sleep avec rétention RAM**.

Courants de veille en light sleep (chip seul) :

| SoC | Light sleep | Deep sleep | Source |
|---|---|---|---|
| **ESP32-C6** (cible) | **37–50 µA** | ~7 µA | mesures communauté au PPK2 |
| ESP32-H2 | **24–30 µA** | ~7 µA | mesures communauté au PPK2 |
| ESP32-H21 | **9 µA** | 5 µA | annonce Espressif (CES 2026) |
| nRF54L15 | ~1,5 µA | ~0,4 µA | datasheet Nordic |
| EFR32 / JN5189 (ce qu'utilise Hue) | ~1–2 µA | — | — |

Le budget pour tenir 3 ans sur CR2032 (~180 mAh utiles) est de **6,8 µA de moyenne, tout compris**. Le C6 comme le H2 dépassent ce budget *rien qu'en dormant*, avant même d'allumer la radio.

### Le coût du C6 par rapport au H2

Le C6 embarque en plus une radio **WiFi 6**, inutilisée dans ce projet. Die plus grand, plus de domaines d'alimentation, plus de fuites : **~45 µA contre ~28 µA** en light sleep, soit **+60 % sur le poste qui décide de tout**. Sur CR2450, cela fait 1,2 an au lieu de 1,8 an.

Si le choix du C6 tient à la disponibilité ou à des cartes déjà en stock, c'est un arbitrage parfaitement défendable pour le prototype. Le passage au H2 en production ne coûte qu'un `idf.py set-target esp32h2` côté firmware (les deux fichiers `sdkconfig.defaults.*` sont fournis) et une empreinte de module différente côté PCB.

### ⚠️ Risque spécifique au C6 : sensibilité à VDD

Une mesure publiée rapporte, sur la même carte et le même firmware :

| VDD | Courant de veille mesuré |
|---|---|
| 3,3 V | **~300 µA** |
| 3,8 V | **~37 µA** |

Un facteur 8. L'explication probable est le **DC-DC interne du C6** qui ne s'engage pas correctement sans marge de tension et retombe en mode LDO, bien moins efficace. C'est une mesure isolée, à confirmer — mais si elle se vérifie, elle est **rédhibitoire pour une alimentation par pile bouton**, puisqu'une CR2450 ne dépasse jamais 3,2 V.

→ **Test n°1 à faire dès que la devkit tourne :** balayer VDD de 3,0 à 3,6 V en mesurant le courant de veille. Le résultat conditionne toute l'architecture d'alimentation (voir `03-hardware.md §1`). Le H2 n'est pas concerné par ce risque.

## 2. Le modèle de coût

Quatre postes récurrents :

```
I_moyen = I_veille
        + (N_polls    × E_poll)   /86400      ← data poll Thread
        + (N_checkins × E_checkin)/86400      ← check-in ICD
        + (N_appuis   × E_appui)  /86400
        + I_autodécharge
```

**`E_poll` ≈ 5 mC par réveil.** Dérivé d'une mesure publiée : 11,47 mC/min sur ESP32-H2 en ICD LIT avec `slow poll = 30 s` (2 réveils/min), moins le plancher de 28 µA. Ce n'est pas la transmission qui coûte — c'est le réveil lui-même : sortie du light sleep, réinitialisation du flash, remise en route de la PHY.

> Ces énergies par réveil sont un **ajustement empirique** sur deux mesures publiées, pas une décomposition physique. Le modèle les reproduit (à `slow poll = 30 s` il prédit 198 µA pour le H2 contre 191 mesurés, et 232 µA pour le C6 contre 231), mais il ne prétend pas séparer finement radio / CPU / flash. À remplacer par tes propres mesures.

**`E_appui` ≈ 8 mC.** Réveil + poll + émission des événements + fenêtre active. Les 3 événements d'un triple-clic tiennent dans une seule fenêtre → un multi-clic ne coûte pas plus qu'un simple clic.

**Autodécharge :** ~1 %/an sur une lithium-manganèse. À 10 µA de conso, ça représente déjà 2,5 % du budget — non négligeable, inclus dans le modèle.

Lance `python3 tools/power_budget.py` pour rejouer le calcul avec tes propres hypothèses.

## 2 bis. Le piège du child timeout Thread

**Le poste dominant n'est pas le check-in ICD, c'est le data poll Thread.** Et sa période n'est pas celle qu'on croit avoir réglée.

OpenThread la calcule ainsi (`DataPollSender::CalculatePollPeriod`, vérifié dans le source d'ESP-IDF v5.5.4) :

```
période_poll = Min(child_timeout − marge, ICD_SLOW_POLL_INTERVAL_MS)
```

Le **child timeout vaut 240 s par défaut** (`OPENTHREAD_CONFIG_MLE_CHILD_TIMEOUT_DEFAULT`) et **n'a aucun symbole Kconfig dans ESP-IDF** : il ne se règle qu'à l'exécution, par `otThreadSetChildTimeout()`.

Conséquence : régler `ICD_SLOW_POLL_INTERVAL_MS` à 30 minutes **sans toucher au child timeout ne change strictement rien**. L'appareil continue de poller toutes les ~235 s.

| Réglage | Poll effectif | ESP32-C6 + CR2450 |
|---|---|---|
| child 240 s (défaut) + slow poll 20 s | 20 s | 301 µA — 2 mois |
| child 240 s (défaut) + slow poll 1800 s | **235 s** | 72,5 µA — 9,9 mois |
| **child 3600 s + slow poll 1800 s** | **1800 s** | **54,0 µA — 1,1 an** |

Le firmware relève donc le child timeout à 3600 s au démarrage (`app_set_thread_child_timeout()` dans `app_main.cpp`). 3600 s laisse une marge de 2× sur le slow poll de 1800 s : au-delà, plus aucun gain — c'est le slow poll qui borne.

⚠️ **À valider avec ton border router** : rien ne garantit qu'un parent Thread accepte un child timeout aussi long ni conserve l'entrée dans sa child table. Si l'appareil se fait éjecter, la première pression après une longue veille demandera une ré-attache de 2 à 5 s — inacceptable pour un interrupteur. C'est un réglage à confirmer en usage réel, pas seulement au banc.

## 2 ter. Constat du 05/10/2026 : Apple Home n'enregistre pas de client ICD

Mesuré sur la SuperMini H2 appairée dans Maison (profil `dev`, log `ICD : mode …` à chaque appui) : **l'appareil reste en mode SIT**, pendant l'appairage comme plusieurs minutes après. Apple s'abonne normalement, mais n'appelle jamais `RegisterClient` sur le cluster ICD Management.

Or CHIP ne passe en LIT **que si au moins un client est enregistré** (`ICDManager::UpdateICDMode`). En SIT, il plafonne le slow poll à **15 s**, quelle que soit la config (`ICDConfigurationData::GetSlowPollingInterval`, règle de la spec). Les 30 min du profil `sleepy` ne s'appliqueront donc pas avec Apple.

Ce qui fonctionne quand même : l'ICD aligne l'intervalle maximal des abonnements Apple sur `IdleModeDuration` (négocié à `Max = 120 s` en profil `dev`), et aucun trafic n'a été observé au repos entre deux rapports.

Conséquence sur l'autonomie (H2 + CR2450, poll 15 s) :

| Coût réel d'un poll | Courant moyen | Autonomie |
|---|---|---|
| 5 mC (ajustement sur mesure publiée) | 367 µA | ~2 mois |
| 2 mC | 167 µA | ~4 mois |
| 0,5 mC | 67 µA | ~11 mois |

**Le coût réel d'un poll sur H2 devient la donnée décisive**, et il n'est pas mesuré. Une puce comme l'EFR32 fait un poll pour ~0,1 à 0,2 mC, ce qui explique qu'un bouton du commerce tienne un an en SIT. La mesure au PPK2 remonte donc en priorité.

## 3. Ce qui va réellement tuer ton budget (par ordre de gravité)

1. **Une devkit non modifiée.** Mesures publiées sur un ESP32-H2 SuperMini : 488 µA carte nue → 223 µA sans la LED WS2812 → 148 µA sans le LDO → 115 µA sans l'IC de charge → **17 µA** une fois tout retiré. Les périphériques de la carte consomment **3× le chip**. Ne conclus jamais quoi que ce soit d'une mesure sur devkit.
2. **Le light sleep jamais autorisé.** `CONFIG_PM_ENABLE` et le tickless idle ne suffisent pas : sans un appel à `esp_pm_configure()` avec `light_sleep_enable = true`, la puce ne dort **jamais**, en silence (même `PM_DFS_INIT_AUTO` ne règle que la fréquence). Ce serait des milliampères au lieu de dizaines de µA. Fait dans `app_enable_light_sleep()`, repris de l'exemple esp-matter `icd_app`.
3. **La radio non mise en veille.** `CONFIG_IEEE802154_SLEEP_ENABLE` et `CONFIG_BT_LE_SLEEP_ENABLE` valent `n` par défaut : le module 802.15.4 et le contrôleur BLE restent alors alimentés pendant le light sleep.
4. **Le child timeout Thread laissé à 240 s** (§2 bis). Plafonne la période de poll et annule tout réglage de `ICD_SLOW_POLL_INTERVAL_MS`. +34 % de conso, sans le moindre signe au build ni au log.
5. **Le BLE laissé en advertising** après commissioning. `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=y`.
6. **`ICD_IDLE_MODE_INTERVAL_SEC` trop court.** Défaut esp-matter : **2 s**. À relever à 1800.
7. **Un régulateur LDO à fort Iq.** Un LDO courant à 50 µA d'Iq quadruple la conso totale.
8. **Les GPIO flottantes** en sleep (`CONFIG_PM_SLP_DISABLE_GPIO=y`) et les pull-ups permanentes sur le bouton — voir `03-hardware.md`.
9. **OTA Requestor actif.** Coûteux en abonnements. À désactiver si tu n'en as pas besoin.
10. **Le WiFi du C6 laissé compilé/actif.** `CONFIG_ESP_WIFI_ENABLED=n` et `CONFIG_ENABLE_WIFI_STATION=n` — sinon les domaines d'alimentation WiFi restent alimentés en veille. **Spécifique au C6, et c'est exactement ce que le H2 t'évite d'avoir à gérer.**

## 4. Arbitrage recommandé

Le Hue Smart Button fait ~44 mm de diamètre pour ~14 mm d'épaisseur. Une **CR2450** (Ø 24,5 × 5,0 mm, 620 mAh) rentre sans difficulté dans ce volume — c'est d'ailleurs ce que Philips utilise dans le Hue Dimmer Switch.

→ **Design pour CR2450**, c'est la décision retenue. Tu gardes le format et l'aspect du Hue, et tu passes de ~5 mois (CR2032) à ~1,2 an sur C6, ~1,8 an si tu bascules sur H2. Le tiroir à pile est le seul élément mécanique impacté.

### Trajectoire proposée

1. **Prototype sur ESP32-C6** (choix retenu). Objectif : valider l'appairage, les gestes, et **mesurer la sensibilité à VDD** (§2). Les deux fichiers `sdkconfig.defaults.esp32c6` et `.esp32h2` sont fournis : basculer de cible est un `idf.py set-target`.
2. **Si le test VDD est mauvais** — courant de veille qui explose sous 3,4 V — le C6 est disqualifié pour une pile bouton et il faut passer au H2. Décision prise sur mesure, pas sur intuition.
3. **PCB pensé pour le H2 ou le H21.** Le H21 a un DC-DC intégré et 9 µA de light sleep : il place la CR2450 à ~4,3 ans, mieux que le Hue original. *(Vérifier la compatibilité de brochage des modules sur les datasheets : elle n'est pas garantie.)*

Si « plusieurs années » redevient un critère non négociable et non un souhait, la conclusion honnête reste qu'il faut **changer de silicium** (nRF54L15 sous nRF Connect SDK, excellent support Matter/Thread). Le firmware est à réécrire, mais toute l'architecture Matter de `01-architecture.md` reste valable telle quelle.

## 5. Protocole de mesure

Non négociable — le modèle ci-dessus vaut ce que valent ses hypothèses.

- **Instrument :** Nordic PPK2 (~90 € TTC) ou Joulescope. Un multimètre ne sait pas mesurer un profil qui va de 20 µA à 90 mA en 200 µs.
- **Cible :** PCB final, pas devkit. Alimentation en source, 3,0 V.
- **Métrique :** intégrer la charge (mC) sur **une fenêtre ≥ 2× `IdleModeDuration`**, pas lire un courant instantané.
- **Cas à mesurer séparément :** (a) veille pure, (b) un cycle check-in, (c) un appui simple, (d) un appui long.

---

### Sources

- [ESP32-C6 : mesures Matter/Thread ICD, sensibilité à VDD (3,3 V vs 3,8 V) — tomasmcguinness.com](https://tomasmcguinness.com/2025/01/06/lowering-power-consumption-in-esp32-c6/)
- [ESP32-H2 : mesures Matter/Thread ICD LIT au PPK2 — tomasmcguinness.com](https://tomasmcguinness.com/2025/08/29/matter-low-power-on-an-esp32-h2/)
- [ESP32-H2 + Thread deep sleep : mesures carte par carte — Home Assistant Community](https://community.home-assistant.io/t/esp32-h2-thread-with-deep-sleep-for-cheap-esp32-battery-sensors/919132)
- [Espressif annonce l'ESP32-H21 (9 µA light sleep / 5 µA deep sleep)](https://www.espressif.com/en/news/ESP32_H21)
- [ESP32-H21 — page produit Espressif](https://www.espressif.com/en/products/socs/esp32-h21)
- [Matter ESP32 LIT ICD example — connectedhomeip](https://project-chip.github.io/connectedhomeip-doc/examples/lit-icd-app/esp32/README.html)
- [ESP32-H2 Series Datasheet (plage VDD 3,0–3,6 V)](https://www.espressif.com/sites/default/files/documentation/esp32-h2_datasheet_en.pdf)
- [Mesure de consommation des modules ESP32-H2 — ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.2/esp32h2/api-guides/current-consumption-measurement-modules.html)
