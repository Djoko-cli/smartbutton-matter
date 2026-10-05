# Hardware

## 1. Le piège d'alimentation, en premier

**L'ESP32-C6 comme l'ESP32-H2 exigent VDD = 3,0 – 3,6 V** (`VDDPST`, `VDD3P3`, `VBAT`, `VDDA_PMU` — datasheets, §Recommended Operating Conditions).

Une CR2032/CR2450 démarre à 3,0–3,2 V et **passe sous 3,0 V après ~10 % de sa capacité**, puis passe l'essentiel de sa vie entre 2,6 et 2,9 V avant de tomber vers 2,0 V.

→ **Une pile bouton branchée directement sur le SoC est hors spec sur ~85 % de sa capacité.** C'est une erreur silencieuse : ça « marche » sur l'établi avec une pile neuve, et l'appareil devient instable au bout de quelques semaines (brownouts pendant les pics TX).

### Solution : boost converter à très faible Iq

```
CR2450 (2,0–3,2 V) ──┬── L 2,2 µH ──┬── [TPS610995 / TPS61099]  ──┬── 3,3 V ── SoC
                     │              │   Iq ≈ 300 nA (power-save)  │
                    C_in           GND                          C_buf (220 µF, low ESR)
                    22 µF                                        + 10 µF X7R + 100 nF
```

- Iq du boost ≈ **0,3 µA**, soit ~1 % du budget. Acceptable.
- Bénéfice : on exploite la pile jusqu'à ~2,0 V, soit **la totalité** de sa capacité au lieu de 15 %.
- Alternative si tu veux éviter le boost : passer à **2×AAA lithium** (3,0 V nominal, 2 × 1,5 V) — mais la plage descend aussi sous 3,0 V en fin de vie, donc le boost reste préférable.

### ⚠️ Point de tension de sortie — à mesurer avant de figer le schéma (spécifique C6)

Une mesure publiée indique que le courant de veille du **C6** dépend fortement de VDD : **~300 µA à 3,3 V contre ~37 µA à 3,8 V**. L'hypothèse est que le DC-DC interne du C6 ne s'engage pas sans marge de tension et retombe en mode LDO.

Si cela se confirme sur ton exemplaire, un boost réglé à 3,3 V est le **pire** choix possible, et il faudrait viser 3,5–3,6 V — c'est-à-dire le haut de la plage admissible, sans marge. C'est un argument fort en faveur du H2 pour la version finale.

→ **Balayer VDD de 3,0 à 3,6 V au PPK2 sur la devkit, et choisir la tension de sortie du boost sur cette courbe.** Ne pas router le PCB avant.

*(L'ESP32-H21 intègre un DC-DC et est conçu pour fonctionner à tension plus basse : ce bloc externe disparaîtrait avec lui.)*

## 2. Pics de courant et résistance interne de la pile

L'émission 802.15.4 tire **~30 mA à 0 dBm**, jusqu'à **~90–100 mA à +20 dBm**.

La résistance interne d'une CR2450 est de ~15 Ω neuve et grimpe au-delà de **100 Ω** en fin de vie et à froid. À 30 mA × 100 Ω = **3 V de chute** → brownout garanti.

Mitigations, cumulatives :

1. **Baisser la puissance TX.** Un bouton mural est à quelques mètres d'un routeur Thread (ampoule, prise, HomePod…). `CONFIG_ESP_PHY_MAX_TX_POWER` à **+8 dBm** suffit très largement. Gain : facteur ~4 sur le pic.
2. **Condensateur tampon 220–470 µF low-ESR** au plus près du module, qui fournit le pic ; la pile ne voit qu'un courant moyen.
3. **Résistance série 10–47 Ω** entre pile et tampon pour limiter le courant de recharge du condensateur (sinon on recrée le pic).
4. **Seuil de brownout** réglé bas et cohérent avec la sortie du boost.

## 3. Horloge de veille

**Quartz 32,768 kHz externe obligatoire** (`CONFIG_RTC_CLK_SRC_EXT_CRYS=y`).

L'oscillateur RC interne (RC32K) dérive de plusieurs % — un nœud Thread sleepy rate alors ses rendez-vous avec son parent, se ré-attache, et la conso explose. Un quartz coûte 0,20 € et c'est le meilleur rapport gain/coût du BOM.

## 4. Le bouton lui-même

Le Hue Smart Button utilise **toute la face avant comme bouton** : le capot appuie sur un unique switch tactile central.

- **Switch tactile SMD**, course ~0,25 mm, force ~160 gf, avec un vrai retour haptique (le « clic » fait partie de l'expérience Hue).
- **Câblage : switch entre GPIO et GND, pull-up interne activée uniquement pendant l'échantillonnage** — ou pull-up externe forte (1 MΩ). Une pull-up de 10 kΩ maintenue bouton enfoncé tire 300 µA, soit 10× le budget total.
- **Pas de bouton capacitif.** Mesures publiées : ~1 mA en continu. Rédhibitoire.
- **Réveil :** GPIO wake depuis le light sleep, géré nativement par le framework `esp_pm` — le firmware n'a rien à piloter manuellement. Attention à ne pas câbler le bouton sur une **broche de strapping** (GPIO9 sur C6 et H2) : maintenue à 0 pendant le reset, elle fait entrer la puce en mode download.
- **Anti-rebond en logiciel** (le composant `espressif/button` le fait), pas de RC : un RC ajoute une fuite permanente.

## 5. LED

Le Hue Smart Button n'a pas de LED visible en fonctionnement normal. Ici on en garde une, **uniquement pour le commissioning et le feedback d'appui** :

- LED basse conso (2 mA suffisent avec une LED moderne à haut rendement), résistance dimensionnée en conséquence.
- **Éteinte en veille**, GPIO en sortie basse. Jamais de LED d'alimentation.
- Flash de 50 ms sur appui = 0,1 mC, négligeable face aux 8 mC de la radio. Tu peux te la permettre.

## 6. Antenne

- Simple : module **ESP32-C6-MINI-1** (ou `H2-MINI-1`, empreintes différentes — voir §7). Antenne PCB intégrée : respecter la **keep-out zone**, pas de plan de masse, pas de piste, pas de pile, pas de plaque métallique sous l'antenne.
- ⚠️ Le Hue Smart Button a une **plaque de fixation magnétique**. Un aimant + une plaque métallique juste derrière une antenne PCB dégradent fortement le rayonnement. Prévoir soit un décalage de l'antenne du côté opposé, soit une version `MINI-1U` avec antenne externe. **À valider en mesure, c'est un vrai risque.**

## 7. BOM prévisionnel (prototype)

| Réf | Composant | Note |
|---|---|---|
| U1 | **ESP32-C6-MINI-1** (4 MB flash) | Alternative : `ESP32-H2-MINI-1`, ~50 % d'autonomie en plus. **Empreintes non identiques** — figer le SoC avant de router. |
| U2 | TPS610995 (ou équiv. boost Iq < 500 nA) | Tension de sortie **à déterminer par la mesure**, cf. §1 |
| Y1 | Quartz 32,768 kHz + 2× C charge | **non optionnel** |
| L1 | Inductance 2,2 µH shielded | selon datasheet boost |
| C_buf | 220 µF low-ESR (tantale ou polymère) | tampon pic TX |
| SW1 | Switch tactile SMD 160 gf | course haptique |
| BT1 | Support **CR2450** | voir `02-energie.md` |
| D1 | LED 0603 haut rendement | + R série |
| J1 | Pads de test UART + GPIO9 (boot) | pas de connecteur USB sur la carte finale |

Pas de puce USB-série sur la carte finale (Iq + coût). Flashage par pads de test + adaptateur externe.

> Le C6 et le H2 n'ont **pas le même brochage ni la même empreinte de module**. Le choix du SoC doit être figé à l'issue de l'étape 3 de la feuille de route, avant tout routage.

## 8. Partitions et certificats

Prévoir une partition **`fctry`** pour les *device attestation credentials* (DAC, PAI, CD) et le Discriminator/Passcode, générés avec `chip-cert` + `esp-matter-mfg-tool`. Cela évite de recompiler le firmware pour chaque unité et permet de flasher un identifiant unique par appareil.

Voir `firmware/partitions.csv`.
