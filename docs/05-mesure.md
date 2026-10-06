# Mesure de consommation au PPK2

Objectif : remplacer les hypothèses de `tools/power_budget.py` par des valeurs mesurées sur l'ESP32-H2. Deux chiffres décident de tout (voir `02-energie.md` §2 ter) :

1. **le plancher de veille** : la puce attachée au réseau Thread, RAM conservée ;
2. **la charge d'un réveil de poll** : Apple laisse l'appareil en SIT, avec un poll toutes les 15 s au plus.

Selon la charge réelle d'un poll (de 0,1 à 5 mC), l'autonomie sur CR2450 va de ~1,6 an à ~2 mois. C'est cette mesure qui tranche entre garder l'H2 et passer au nRF54L15.

## Matériel

- Nordic **PPK2** et l'app *nRF Connect for Desktop > Power Profiler* ;
- une **SuperMini H2 dédiée à la mesure**. Une fois son régulateur retiré, elle ne pourra plus être alimentée par l'USB ;
- un fer à souder fin, de la tresse, et des fils Dupont.

## Étape 1 : préparer la carte, avant toute modification

L'appareil doit être mesuré **dans les conditions réelles** : appairé dans Maison et rattaché au réseau Thread. L'ordre compte :

1. Carte sur le Mac, en USB : flasher le profil `dev` et **appairer** dans Maison, comme d'habitude.
2. Flasher le profil `measure` **sans effacer**, ce qui conserve l'appairage :
   ```bash
   ./tools/build.sh measure -p /dev/cu.usbmodemXXXX flash
   ```
   Si le port n'apparaît pas : BOOT maintenu, RST, BOOT relâché.
3. Vérifier dans Maison que le bouton déclenche toujours la lampe.

Le profil `measure`, c'est `sleepy` sans aucun log, sans console, sans module USB et sans LED. **Les flashs suivants imposent le mode téléchargement** (BOOT + RST), puisque l'USB est désactivé dans ce profil.

## Étape 2 : retirer ce qui consomme sur la carte

Mesures publiées sur cette carte (communauté Home Assistant, PPK2) : **488 µA → 17 µA** une fois les composants parasites retirés. La puce compte pour moins de 5 % de la consommation d'une SuperMini non modifiée.

| Composant | Où | Pourquoi |
|---|---|---|
| **LED WS2812** | entre BOOT et RST, sérigraphie « 8 » | courant de repos ~1 mA même éteinte |
| **Régulateur (LDO)** | près de l'USB-C | courant de repos, et fuite en sens inverse quand on alimente par la broche 3V3 |
| **Circuit de charge LiPo** et **sa LED verte** | près des pastilles B+/B- et de la broche BAT | c'est la LED qui clignote sans batterie |

Les références exactes varient selon les lots de cartes. **Prends la carte en photo, de près, des deux côtés, avant de dessouder** : on identifiera chaque composant avant d'y toucher.

## Étape 3 : brancher le PPK2

- PPK2 en **mode Source** (*Source meter*).
- **VOUT du PPK2 → broche 3V3** de la SuperMini, **GND → GND**.
- **Tension : 3,3 V.** C'est ce que fournira le boost du PCB final. L'ESP32-H2 exige 3,0 V minimum : ne descends pas en dessous.
- **Aucun autre câble** :
  - **pas d'USB**, qui ferait deux alimentations en parallèle ;
  - **pas d'adaptateur UART**. Ses lignes peuvent alimenter partiellement la puce par ses broches TX/RX, et fausser la mesure.

## Étape 4 : capturer

Dans le Power Profiler : **100 000 échantillons/s**, puis *Start*.

1. Alimente la carte et attends **1 min**, le temps qu'elle se rattache à Thread et rouvre ses sessions avec le hub Apple.
2. **5 min sans toucher à rien**, pour une vingtaine de polls à 15 s.
3. Puis, en espaçant de **20 s** : 3 pressions simples, 1 double, 1 appui long.
4. *Stop*. Sélectionne la plage utile, puis **Export > CSV**.

Un export de 7 min à 100 000 échantillons/s pèse plusieurs centaines de Mo. C'est normal : le script le lit en flux continu.

## Étape 5 : analyser

```bash
python3 tools/ppk2_analyze.py capture.csv
```

Le script donne :
- le **plancher de veille** ;
- la **période et la charge médiane des réveils périodiques**, c'est-à-dire les polls ;
- la **liste des autres réveils** (appuis, rapports au hub) avec leur charge ;
- l'**autonomie** par type de pile, avec le modèle de `power_budget.py`.

Options utiles :
- `--threshold-ua 500` si le plancher est plus bruité que prévu (seuil de détection d'un réveil, 300 µA par défaut) ;
- `--merge-gap-ms 100` si un même réveil apparaît coupé en deux.

Pour vérifier le script lui-même : `python3 tools/ppk2_analyze.py --selftest`.

### Correction pour le boost (PCB final)

Le PPK2 mesure le courant **à 3,3 V**. Sur le PCB, le boost prélève ce courant sur la pile, à une tension plus basse et avec des pertes :

```
I_pile ≈ I_mesuré × 3,3 / (V_pile × rendement)
```

Avec une pile à 2,8 V et un rendement de 90 %, le courant prélevé sur la pile est donc ~1,3 fois le courant mesuré. Le script ne fait pas encore cette correction : applique-la à la main en attendant de connaître le rendement réel du boost choisi.

## Étape 6 : reporter les valeurs

Mets à jour la ligne `ESP32-H2` de `SOCS` dans `tools/power_budget.py` (`sleep_ua`, `poll_mc`, `press_mc`), avec la date et les conditions de mesure en note. Le tableau d'autonomie de `02-energie.md` et du README se recalcule alors avec `python3 tools/power_budget.py`.
