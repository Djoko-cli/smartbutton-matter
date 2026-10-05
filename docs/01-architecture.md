# Architecture — SmartButton (inspiré du Hue Smart Button, Matter over Thread)

## 1. Décisions de stack

| Question | Décision | Pourquoi |
|---|---|---|
| Protocole | **Matter over Thread** | Demandé. Pas de pont propriétaire, multi-écosystème. |
| SoC — prototype | **ESP32-C6** (module `ESP32-C6-MINI-1`) | Choix retenu. 802.15.4 + BLE + WiFi 6 (le WiFi reste inutilisé). |
| SoC — cible | **ESP32-H2** ou **H21** à évaluer | Le H2 n'a pas de WiFi : ~28 µA de veille contre ~45 µA pour le C6, soit **+50 % d'autonomie à carte égale**. Bascule = `idf.py set-target`. Voir `02-energie.md`. |
| SDK | **ESP-IDF + esp-matter** (C++) | esp-matter enveloppe `connectedhomeip`. C'est la seule voie Matter sur Espressif. |
| ~~HomeSpan~~ | **Écarté** | HomeSpan = HomeKit/HAP **over WiFi**. Il ne parle ni Matter ni Thread. Aucune passerelle possible : ce sont deux piles protocolaires entièrement disjointes. |
| Type d'appareil Matter | **Generic Switch** (`0x000F`) | C'est exactement le modèle du Hue Smart Button. |
| Modèle d'énergie | **ICD LIT** (Long Idle Time) | Sans ça, l'autonomie se compte en semaines. Voir `02-energie.md`. |

### Pourquoi pas HomeSpan, en détail

HomeSpan implémente le *HomeKit Accessory Protocol* : mDNS + HTTP/HAP sur TCP/IP, transporté par WiFi. Matter est une autre pile (IPv6 + UDP + MRP + TLV + clusters, attestation DAC/PAA, commissioning BLE). Thread est encore une autre couche (802.15.4 + 6LoWPAN + mesh). Aucun code HomeSpan n'est réutilisable.

Si tu veux **quand même** rester en HomeKit natif : c'est possible, mais ce sera un accessoire WiFi alimenté au secteur. Autonomie sur pile ≈ exclue (le WiFi coûte ~10–20 mA en DTIM sleep, soit 1000× le budget visé).

---

## 2. Modèle Matter

### Endpoints

```
EP0  Root Node
     ├── Basic Information
     ├── ICD Management        ← critique (LIT + CIP + UAT)
     ├── Power Source          ← niveau de pile (BatPercentRemaining)
     └── OTA Requestor         ← optionnel, coûte cher en veille
EP1  Generic Switch (0x000F)
     └── Switch cluster, features : MS | MSR | MSL | MSM
```

### Features du cluster Switch

| Feature | Code | Rôle | Événement émis |
|---|---|---|---|
| `MS`  | Momentary Switch | bouton non maintenu | `InitialPress` |
| `MSR` | Momentary Switch Release | relâchement | `ShortRelease` |
| `MSL` | Momentary Switch LongPress | appui long | `LongPress`, `LongRelease` |
| `MSM` | Momentary Switch MultiPress | double / triple clic | `MultiPressOngoing`, `MultiPressComplete` |

`NumberOfPositions = 2`, `MultiPressMax = 3` (simple / double / triple, comme le Hue).

Le contrôleur **s'abonne** à ces événements. Le bouton ne fait que pousser — c'est le cas d'usage idéal pour un ICD : aucun trafic descendant en régime normal.

### Mapping avec le Hue Smart Button

| Geste Hue | Événements Matter émis |
|---|---|
| Appui simple | `InitialPress` → `ShortRelease` → `MultiPressComplete(1)` |
| Double appui | … → `MultiPressComplete(2)` |
| Triple appui | … → `MultiPressComplete(3)` |
| Appui long | `InitialPress` → `LongPress` → `LongRelease` |

---

## 3. Cycle de vie ICD (le cœur du design)

Un **ICD LIT** (Matter ≥ 1.2, consolidé en 1.4) dort *vraiment* : pas de polling toutes les quelques secondes.

```
        ┌──────────────── IdleModeDuration (ex. 30 min) ────────────────┐
        │                                                              │
   [ LIGHT SLEEP ~28 µA, radio OFF ]                              [ check-in ]
        │                                                              │
        └── appui bouton ──► ActiveMode (~1 s, fast poll 500 ms) ──────┘
                             envoi des événements Switch
```

Trois mécanismes obligatoires pour un LIT :

- **CIP** (Check-In Protocol) — l'appareil se réveille toutes les `IdleModeDuration` et envoie un *check-in* aux clients enregistrés, au lieu de maintenir un abonnement coûteux.
- **UAT** (User Active Mode Trigger) — l'utilisateur doit pouvoir forcer le mode actif. Sur un bouton c'est gratuit : **appui long 10 s → mode actif**. On renseigne `UserActiveModeTriggerHint` en conséquence.
- **Fallback SIT** — tant qu'aucun client n'est enregistré (juste après commissioning), l'appareil doit se comporter en SIT (poll ≤ 15 s). Il bascule en LIT une fois enregistré.

> ⚠️ Les valeurs exactes des bornes spec (`IdleModeDuration` max, `ActiveModeThreshold` min pour LIT) sont à revérifier dans la version de la spec Matter que cible ton esp-matter. Les valeurs retenues dans `firmware/sdkconfig.defaults.esp32h2` sont des valeurs de travail, pas des valeurs certifiées.

---

## 4. Commissioning et gestes système

Un seul bouton doit tout faire. Convention retenue :

| Geste | Action |
|---|---|
| 1 / 2 / 3 appuis | événements `MultiPressComplete` |
| Appui long (≥ 1 s) | `LongPress` / `LongRelease` |
| Maintien 10 s | Force le mode actif (UAT) + rouvre la fenêtre de commissioning |
| Maintien 20 s | Factory reset (efface les fabrics) |

Le commissioning initial se fait en **BLE**, puis l'appareil rejoint le réseau Thread et **désactive définitivement la radio BLE** (`CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING`). Laisser le BLE advertising tourner est l'erreur classique qui tue le budget énergie.

Sur C6, il faut en plus **exclure le WiFi de la compilation** (`CONFIG_ESP_WIFI_ENABLED=n`) : sinon ses domaines d'alimentation restent alimentés en veille, pour une radio qu'on n'utilise jamais.

---

## 5. Attestation / écosystèmes — le piège pratique

Un appareil Matter présente un **DAC** (Device Attestation Certificate) signé par une PAA reconnue par le CSA. Un projet perso utilise les certificats de test (`VID 0xFFF1`). Conséquences, par ordre de permissivité :

| Écosystème | Appareil avec DAC de test |
|---|---|
| **Home Assistant** (+ OTBR) | Accepte. Aucun obstacle. |
| **Google Home** | Nécessite d'enregistrer le VID/PID de test dans la *Google Home Developer Console*. |
| **Apple Home** | Le plus strict sur la chaîne d'attestation. À valider tôt — ne découvre pas le problème après le PCB. |

**À faire dès le prototype sur devkit** : tester l'appairage sur *ton* écosystème cible avant de dessiner quoi que ce soit. C'est le risque n°1 du projet, avant même l'énergie.

Pour un usage strictement personnel, générer un jeu de certificats de test avec `chip-cert` et les flasher dans la partition `fctry` suffit (voir `03-hardware.md`).

---

## 6. Plan de marche

1. **Devkit ESP32-C6-DevKitC-1** — firmware Generic Switch, non-sleepy. Objectif : appairage réussi + gestes reconnus dans l'app cible.
2. Activer ICD LIT + light sleep. Mesurer au PPK2 / Joulescope, **dont le balayage VDD 3,0 → 3,6 V** (risque C6, `02-energie.md §2`).
3. Confronter la mesure au modèle (`tools/power_budget.py`). Arbitrer C6 vs H2, et la pile.
4. Schéma + PCB (boost converter, quartz 32 kHz, antenne vs aimants).
5. Mécanique.
