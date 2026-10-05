# Mécanique — boîtier inspiré du Hue Smart Button

## 1. Le format d'origine

Le Hue Smart Button (Signify, ref. ROM001) est un palet rond d'environ **44 mm de diamètre** pour **~14 mm d'épaisseur**, alimenté par une CR2032, avec une plaque de fixation aimantée.

> ⚠️ Ces dimensions sont approximatives. **Mesure l'original au pied à coulisse** avant de dessiner quoi que ce soit — c'est 10 minutes de travail contre une itération de CAO perdue.

Éléments de design à reproduire :

- Toute la face avant est le bouton : le capot supérieur bascule/s'enfonce sur un switch central.
- Chanfrein périphérique, pas d'arête vive.
- Aucune LED visible en fonctionnement.
- Base aimantée + plaque adhésive murale, avec détrompeur d'orientation.

## 2. Empilement retenu

De haut en bas :

```
 ┌─────────────────────────────┐  capot mobile (bouton)
 │  ░░░░░░░░░░░░░░░░░░░░░░░░░  │  ← course ~0,4 mm, retour par nervures flexibles
 ├─────────────────────────────┤
 │   ●  switch tactile SMD     │  PCB face composants vers le haut
 │  ═══════════════════════    │  PCB Ø ~38 mm
 │   [module H2]  [antenne]    │  ← zone antenne dégagée
 ├─────────────────────────────┤
 │   ▭▭▭  CR2450 (Ø24,5×5,0)   │  support pile face inférieure
 ├─────────────────────────────┤
 │  base + aimants             │
 └─────────────────────────────┘
```

Épaisseur estimée : capot 2 mm + course 0,4 + PCB 0,8 + support pile ~6 + base 2 ≈ **11,5 mm**, marge confortable sous les 14 mm de l'original. La CR2450 passe donc **sans épaissir le boîtier** par rapport au Hue.

## 3. Trois contraintes qui vont dicter la CAO

### a) Antenne vs aimants — le conflit principal

Un aimant néodyme et une plaque métallique directement sous une antenne PCB détruisent le rayonnement. Options, par ordre de préférence :

1. **Aimants en couronne périphérique**, antenne au centre, avec au moins 8–10 mm de dégagement — la keep-out zone du module doit rester entièrement libre.
2. Aimants d'un seul côté, module décalé à l'opposé.
3. Module **ESP32-H2-MINI-1U** + antenne externe (u.FL) collée dans le capot plastique.
4. Fixation adhésive seule, sans aimant. Le plus sûr techniquement, le moins fidèle au produit.

**À valider en mesure** (RSSI / LQI à distance constante, avec et sans les aimants) avant de figer la CAO.

### b) Le ressenti du clic

C'est ce qui fait la différence entre un objet abouti et un jouet. Le capot doit transmettre l'appui au switch **sans jeu et sans point dur** :

- Nervures flexibles (living hinge) moulées dans le capot plutôt qu'un guidage à coulisse — moins de friction, pas de blocage en biais.
- Un plot central rigide aligné à ±0,3 mm sur le switch. Un désalignement produit un clic mou sur les bords.
- Switch avec un vrai retour tactile (~160 gf, course 0,25 mm) : c'est lui qui fait le bruit, pas le plastique.
- Prévoir de faire varier l'épaisseur des nervures sur les premiers tirages (0,6 / 0,8 / 1,0 mm) : c'est le paramètre à régler empiriquement.

### c) Accès à la pile

L'ouverture du compartiment ne doit pas nécessiter de démonter le capot bouton. Base rotative à baïonnette, ou trappe dans la base.

## 4. Fabrication

- **Prototypes : impression 3D résine (SLA)** pour la finesse des chanfreins et la tenue des nervures. Le FDM ne rendra ni l'état de surface ni le comportement élastique.
- Le capot en résine standard sera fragile en flexion : utiliser une résine « tough »/ABS-like pour les nervures, ou imprimer le capot en deux parties.
- Pour une série, moulage sous vide en silicone, puis injection.

## 5. Propriété intellectuelle

Reproduire le design d'un produit commercial pour un usage **personnel** ne pose pas de difficulté. En revanche, la forme d'un produit peut être protégée par un **dessin ou modèle communautaire**, et « Hue » est une marque déposée : ni distribution ni vente d'un clone visuel, et pas d'usage du nom ni des logos Philips/Hue.

## 6. Fichiers

CAO à déposer dans `mechanical/` (FreeCAD ou Fusion). Exporter les STL dans `mechanical/stl/`.
