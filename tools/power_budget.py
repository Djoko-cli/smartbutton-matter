#!/usr/bin/env python3
"""
Modèle d'autonomie pour le SmartButton (Matter over Thread, ICD LIT).

Aucune dépendance. Lancer :  python3 tools/power_budget.py

⚠️ Le poste dominant n'est PAS le check-in ICD mais le **data poll Thread**.
OpenThread calcule sa période ainsi (DataPollSender::CalculatePollPeriod) :

    période_poll = Min(child_timeout - marge, ICD_SLOW_POLL_INTERVAL_MS)

Régler un slow poll long sans lever le child timeout (défaut 240 s) ne sert
donc à rien. Voir APP_THREAD_CHILD_TIMEOUT_S dans firmware/main/app_priv.h.

Les énergies par réveil sont un ajustement empirique sur des mesures publiées,
pas une décomposition physique : elles reproduisent les moyennes mesurées sur
H2 et C6, mais ne prétendent pas séparer finement radio / CPU / flash.
Remplace-les par tes propres mesures au PPK2 dès que le PCB existe.
"""

from dataclasses import dataclass

HOURS_PER_DAY = 24
SECONDS_PER_DAY = 86_400
DAYS_PER_YEAR = 365.25

# Marge retranchée au child timeout par OpenThread avant d'en faire une
# période de poll (kRetxPollPeriod * kMaxPollRetxAttempts, quelques secondes).
POLL_AHEAD_S = 5


@dataclass(frozen=True)
class Soc:
    name: str
    sleep_ua: float       # plancher light sleep, chip seul
    poll_mc: float        # charge par data poll Thread
    checkin_mc: float     # charge par check-in ICD (fenêtre active plus longue)
    press_mc: float       # charge par appui (réveil + événements Switch)
    note: str = ""


@dataclass(frozen=True)
class Battery:
    name: str
    nominal_mah: float
    derate: float         # fraction réellement exploitable (coupure, froid, pics)
    self_discharge_pct_yr: float

    @property
    def usable_mah(self) -> float:
        return self.nominal_mah * self.derate


@dataclass(frozen=True)
class Usage:
    child_timeout_s: float = 3600.0     # APP_THREAD_CHILD_TIMEOUT_S
    slow_poll_s: float = 1800.0         # CONFIG_ICD_SLOW_POLL_INTERVAL_MS
    checkin_period_s: float = 1800.0    # CONFIG_ICD_IDLE_MODE_INTERVAL_SEC
    presses_per_day: float = 20.0
    board_leak_ua: float = 0.5          # Iq boost (~0.3) + fuites diverses

    @property
    def poll_period_s(self) -> float:
        """Période effective, plafonnée par le child timeout."""
        return min(self.child_timeout_s - POLL_AHEAD_S, self.slow_poll_s)


SOCS = [
    Soc("ESP32-C6", 45.0, 5.0, 5.5, 8.5, "CIBLE — mesuré 37-50 µA, sensible à VDD"),
    Soc("ESP32-H2", 28.0, 5.0, 5.0, 8.0, "ajustement sur mesure publiée (probablement pessimiste, voir SIT)"),
    Soc("ESP32-H21", 9.0, 4.0, 4.0, 6.5, "datasheet, dispo à confirmer"),
    Soc("nRF54L15", 1.5, 1.5, 1.5, 2.5, "référence hors-ESP32"),
]

BATTERIES = [
    Battery("CR2032",          225,  0.80, 1.0),
    Battery("CR2450",          620,  0.85, 1.0),
    Battery("CR2477",         1000,  0.85, 1.0),
    Battery("2x AAA lithium", 1200,  0.90, 0.5),
]


def mc_per_day_to_ua(mc_per_day: float) -> float:
    """1 mC = 1 mA·s ; (mA·s/jour) / 86400 s = mA ; × 1000 = µA."""
    return mc_per_day / SECONDS_PER_DAY * 1000.0


def average_current_ua(soc: Soc, bat: Battery, use: Usage) -> dict:
    """Décompose le courant moyen en µA, poste par poste."""
    polls_per_day = SECONDS_PER_DAY / use.poll_period_s
    checkins_per_day = SECONDS_PER_DAY / use.checkin_period_s

    self_dis = (bat.nominal_mah * (bat.self_discharge_pct_yr / 100.0)
                / (DAYS_PER_YEAR * HOURS_PER_DAY) * 1000.0)

    parts = {
        "veille SoC": soc.sleep_ua,
        "fuites carte": use.board_leak_ua,
        "data polls": mc_per_day_to_ua(polls_per_day * soc.poll_mc),
        "check-ins": mc_per_day_to_ua(checkins_per_day * soc.checkin_mc),
        "appuis": mc_per_day_to_ua(use.presses_per_day * soc.press_mc),
        "autodécharge": self_dis,
    }
    parts["TOTAL"] = sum(parts.values())
    return parts


def life_years(soc: Soc, bat: Battery, use: Usage) -> float:
    total_ua = average_current_ua(soc, bat, use)["TOTAL"]
    hours = bat.usable_mah / (total_ua / 1000.0)
    return hours / HOURS_PER_DAY / DAYS_PER_YEAR


def fmt_life(years: float) -> str:
    if years < 1.0:
        return f"{years * 12:.1f} mois"
    return f"{years:.1f} ans"


def main() -> None:
    use = Usage()
    ref_bat = BATTERIES[1]  # CR2450

    print("=" * 76)
    print("AUTONOMIE MODÉLISÉE — SmartButton Matter/Thread")
    print(f"child timeout {use.child_timeout_s:.0f} s · slow poll {use.slow_poll_s:.0f} s "
          f"-> poll effectif {use.poll_period_s:.0f} s")
    print(f"check-in {use.checkin_period_s / 60:.0f} min · "
          f"{use.presses_per_day:.0f} appuis/jour · fuites carte {use.board_leak_ua} µA")
    print("=" * 76)

    print(f"{'SoC':<14}" + "".join(f"{b.name:>17}" for b in BATTERIES))
    print("-" * 76)
    for soc in SOCS:
        row = f"{soc.name:<14}"
        for bat in BATTERIES:
            row += f"{fmt_life(life_years(soc, bat, use)):>17}"
        print(row)

    print()
    print(f"Décomposition du courant moyen (µA) — {ref_bat.name}")
    print("-" * 76)
    for soc in SOCS:
        parts = average_current_ua(soc, ref_bat, use)
        detail = "  ".join(f"{k} {v:.2f}" for k, v in parts.items() if k != "TOTAL")
        print(f"{soc.name:<12} TOTAL {parts['TOTAL']:6.2f}   ({detail})")

    print()
    print(f"EFFET DU CHILD TIMEOUT — {SOCS[0].name} + {ref_bat.name}")
    print("(slow poll fixé à 1800 s ; c'est le child timeout qui borne le poll)")
    print("-" * 76)
    for ct in (240, 600, 900, 1800, 3600, 7200):
        u = Usage(child_timeout_s=ct)
        parts = average_current_ua(SOCS[0], ref_bat, u)
        flag = "  ← défaut OpenThread" if ct == 240 else (
               "  ← retenu" if ct == 3600 else "")
        print(f"  {ct:>6} s → poll {u.poll_period_s:>6.0f} s   "
              f"{parts['TOTAL']:6.2f} µA   {fmt_life(life_years(SOCS[0], ref_bat, u)):>10}"
              f"{flag}")

    print()
    print("Sans lever le child timeout, allonger ICD_SLOW_POLL_INTERVAL_MS n'a")
    print("aucun effet : les deux lignes 240 s ci-dessus et ci-dessous sont égales.")
    print()
    for sp in (20, 300, 1800):
        u = Usage(child_timeout_s=240, slow_poll_s=sp)
        parts = average_current_ua(SOCS[0], ref_bat, u)
        print(f"  child 240 s + slow poll {sp:>5.0f} s → poll {u.poll_period_s:>5.0f} s   "
              f"{parts['TOTAL']:6.2f} µA")

    print()
    print("SANS CLIENT ICD ENREGISTRÉ (Apple Home, constaté le 05/10/2026)")
    print("L'appareil reste en SIT. Mesuré sur la SuperMini le 09/10/2026 avec")
    print("CONFIG_APP_POWER_STATS : un réveil toutes les 7,7 s en moyenne, 5,5 ms")
    print("éveillé par réveil. BORNE BASSE : le temps compté en « veille » par ESP-IDF")
    print("inclut les transitions d'entrée et de sortie de veille. Courant pendant")
    print("l'éveil SUPPOSÉ (20 mA, fourchette ±30 %) ; plancher supposé (28 µA).")
    print("-" * 76)
    h2 = SOCS[1]
    for label, wake_mc in (("bas", 0.077), ("estimé", 0.11), ("haut", 0.143)):
        soc = Soc(h2.name, h2.sleep_ua, wake_mc, 0.0, h2.press_mc)
        u = Usage(child_timeout_s=3600, slow_poll_s=7.7, checkin_period_s=1e9)
        for bat in (BATTERIES[1], BATTERIES[3]):
            parts = average_current_ua(soc, bat, u)
            print(f"  {label:<7} {wake_mc:.3f} mC/réveil, {bat.name:<15} → "
                  f"{parts['TOTAL']:6.1f} µA   {fmt_life(life_years(soc, bat, u)):>10}")
    print()
    print("Hypothèses par SoC :")
    for soc in SOCS:
        print(f"  {soc.name:<12} veille {soc.sleep_ua:>5.1f} µA   {soc.note}")


if __name__ == "__main__":
    main()
