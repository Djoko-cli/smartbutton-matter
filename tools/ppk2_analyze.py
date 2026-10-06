#!/usr/bin/env python3
"""
Analyse d'un export CSV du Nordic PPK2 (nRF Connect > Power Profiler).

    python3 tools/ppk2_analyze.py capture.csv
    python3 tools/ppk2_analyze.py capture.csv --threshold-ua 500
    python3 tools/ppk2_analyze.py --selftest

Ce qu'il calcule, en un seul passage (mémoire constante, adapté aux exports de
plusieurs millions de lignes) :

  - le courant moyen sur toute la capture ;
  - le plancher de veille (médiane des échantillons hors réveils) ;
  - chaque réveil : début, durée, pic, charge nette au-dessus du plancher ;
  - les réveils PÉRIODIQUES (polls Thread, check-ins) et leur charge typique,
    séparés des autres (appuis, rapports) ;
  - l'autonomie qui en découle, avec le modèle de tools/power_budget.py.

Aucune dépendance. Voir docs/05-mesure.md pour la façon de capturer.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import random
import statistics
import sys
import tempfile
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import power_budget as pb  # noqa: E402


# --------------------------------------------------------------------------
# Lecture
# --------------------------------------------------------------------------

_TIME_UNITS = {"ms": 1e-3, "us": 1e-6, "µs": 1e-6, "s": 1.0, "ns": 1e-9}
_CURRENT_UNITS_TO_UA = {"ua": 1.0, "µa": 1.0, "na": 1e-3, "ma": 1e3, "a": 1e6}


def _unit(header: str) -> str:
    """'Current(uA)' -> 'ua'."""
    if "(" in header and header.endswith(")"):
        return header[header.index("(") + 1:-1].strip().lower()
    return ""


def open_samples(path: str):
    """Itère sur (temps en s, courant en µA). Détecte colonnes et unités."""
    f = open(path, newline="")
    reader = csv.reader(f)
    header = next(reader)
    t_col = next((i for i, h in enumerate(header) if h.lower().startswith("timestamp")), None)
    i_col = next((i for i, h in enumerate(header) if h.lower().startswith("current")), None)
    if t_col is None or i_col is None:
        raise SystemExit(f"Colonnes Timestamp/Current introuvables dans l'en-tête : {header}")

    t_scale = _TIME_UNITS.get(_unit(header[t_col]))
    i_scale = _CURRENT_UNITS_TO_UA.get(_unit(header[i_col]))
    if t_scale is None or i_scale is None:
        raise SystemExit(f"Unités non reconnues : {header[t_col]!r}, {header[i_col]!r}")

    def gen():
        with f:
            for row in reader:
                if len(row) <= max(t_col, i_col) or not row[i_col]:
                    continue
                yield float(row[t_col]) * t_scale, float(row[i_col]) * i_scale

    return gen()


# --------------------------------------------------------------------------
# Analyse
# --------------------------------------------------------------------------

@dataclass
class Event:
    start_s: float
    end_s: float
    raw_uc: float          # charge brute pendant l'événement (µC)
    peak_ua: float
    net_uc: float = 0.0    # charge au-dessus du plancher, calculée à la fin

    @property
    def duration_ms(self) -> float:
        return (self.end_s - self.start_s) * 1e3


@dataclass
class Result:
    duration_s: float = 0.0
    total_uc: float = 0.0
    floor_ua: float = 0.0
    events: list = field(default_factory=list)


class _Histogram:
    """Histogramme à pas fixe pour une médiane en mémoire constante."""

    def __init__(self, max_ua: float, step_ua: float = 0.05):
        self.step = step_ua
        self.bins = [0] * (int(max_ua / step_ua) + 2)
        self.n = 0

    def add(self, ua: float):
        idx = min(max(int(ua / self.step), 0), len(self.bins) - 1)
        self.bins[idx] += 1
        self.n += 1

    def percentile(self, p: float) -> float:
        if self.n == 0:
            return 0.0
        target = p * self.n
        acc = 0
        for idx, c in enumerate(self.bins):
            acc += c
            if acc >= target:
                return (idx + 0.5) * self.step
        return len(self.bins) * self.step


def analyze(samples, threshold_ua: float, merge_gap_ms: float) -> Result:
    res = Result()
    hist = _Histogram(max_ua=threshold_ua)
    merge_gap_s = merge_gap_ms / 1e3

    prev_t = None
    first_t = None
    ev = None               # événement en cours
    pending_uc = 0.0        # charge des échantillons bas depuis le dernier pic
    pending_hist = []       # ces échantillons, à rendre au plancher si fin

    for t, ua in samples:
        if first_t is None:
            first_t = t
        dt = 0.0 if prev_t is None else t - prev_t
        prev_t = t
        if dt < 0:
            continue
        q = ua * dt  # µA·s = µC
        res.total_uc += q

        if ua >= threshold_ua:
            if ev is None:
                ev = Event(start_s=t, end_s=t, raw_uc=q, peak_ua=ua)
            else:
                # Pic dans la fenêtre de fusion : les échantillons bas
                # intermédiaires font partie de l'événement.
                ev.raw_uc += pending_uc + q
                ev.end_s = t
                ev.peak_ua = max(ev.peak_ua, ua)
            pending_uc = 0.0
            pending_hist.clear()
        else:
            if ev is not None and t - ev.end_s <= merge_gap_s:
                pending_uc += q
                pending_hist.append(ua)
                continue
            if ev is not None:
                res.events.append(ev)
                ev = None
                for v in pending_hist:
                    hist.add(v)
                pending_uc = 0.0
                pending_hist.clear()
            hist.add(ua)

    if ev is not None:
        res.events.append(ev)
    for v in pending_hist:
        hist.add(v)

    res.duration_s = (prev_t - first_t) if prev_t is not None else 0.0
    res.floor_ua = hist.percentile(0.5)
    for e in res.events:
        e.net_uc = max(e.raw_uc - res.floor_ua * (e.end_s - e.start_s), 0.0)
    return res


def split_periodic(events, tolerance: float = 0.2):
    """Sépare les réveils réguliers (polls) des autres.

    Un réveil est « périodique » si l'écart avec son voisin précédent ou
    suivant est proche (±20 %) de l'intervalle médian entre réveils."""
    if len(events) < 3:
        return [], list(events), None
    gaps = [b.start_s - a.start_s for a, b in zip(events, events[1:])]
    period = statistics.median(gaps)

    def close(g):
        return abs(g - period) <= tolerance * period

    periodic, others = [], []
    for i, e in enumerate(events):
        before = close(gaps[i - 1]) if i > 0 else False
        after = close(gaps[i]) if i < len(gaps) else False
        (periodic if (before or after) else others).append(e)
    return periodic, others, period


# --------------------------------------------------------------------------
# Rapport
# --------------------------------------------------------------------------

def report(res: Result, presses_per_day: float) -> dict:
    avg_ua = res.total_uc / res.duration_s if res.duration_s else 0.0
    periodic, others, period = split_periodic(res.events)

    print("=" * 72)
    print("ANALYSE PPK2")
    print("=" * 72)
    print(f"Durée de capture        {res.duration_s:10.1f} s")
    print(f"Courant moyen           {avg_ua:10.2f} µA")
    print(f"Plancher de veille      {res.floor_ua:10.2f} µA   (médiane hors réveils)")
    print(f"Réveils détectés        {len(res.events):10d}")

    poll_mc = None
    if periodic:
        charges = [e.net_uc for e in periodic]
        poll_mc = statistics.median(charges) / 1e3
        durations = [e.duration_ms for e in periodic]
        print()
        print(f"Réveils périodiques     {len(periodic):10d}   période ≈ {period:.2f} s")
        print(f"  charge médiane        {poll_mc * 1e3:10.1f} µC   ({poll_mc:.4f} mC)")
        print(f"  charge min / max      {min(charges):10.1f} / {max(charges):.1f} µC")
        print(f"  durée médiane         {statistics.median(durations):10.2f} ms")
        print(f"  pic médian            {statistics.median(e.peak_ua for e in periodic) / 1e3:10.2f} mA")

    if others:
        print()
        print("Autres réveils (appuis, rapports, check-ins) :")
        for e in others[:20]:
            print(f"  t={e.start_s:9.3f} s  durée {e.duration_ms:8.2f} ms  "
                  f"pic {e.peak_ua / 1e3:6.2f} mA  charge {e.net_uc:9.1f} µC")
        if len(others) > 20:
            print(f"  … et {len(others) - 20} autres")

    if poll_mc is not None and period:
        print()
        print("-" * 72)
        print("AUTONOMIE (modèle tools/power_budget.py, valeurs mesurées)")
        press_mc = statistics.median(e.net_uc for e in others) / 1e3 if others else 8.0
        soc = pb.Soc("mesuré", res.floor_ua, poll_mc, poll_mc, press_mc)
        use = pb.Usage(child_timeout_s=max(period + pb.POLL_AHEAD_S + 1, 3600),
                       slow_poll_s=period, checkin_period_s=1800,
                       presses_per_day=presses_per_day, board_leak_ua=0.0)
        for bat in pb.BATTERIES:
            parts = pb.average_current_ua(soc, bat, use)
            print(f"  {bat.name:<16} {parts['TOTAL']:7.2f} µA  →  "
                  f"{pb.fmt_life(pb.life_years(soc, bat, use)):>10}")
        print(f"  (appui pris à {press_mc:.2f} mC, {presses_per_day:.0f} appuis/jour ; "
              "fuites carte déjà incluses dans le plancher)")

    return {"avg_ua": avg_ua, "floor_ua": res.floor_ua, "poll_mc": poll_mc, "period_s": period}


# --------------------------------------------------------------------------
# Auto-test
# --------------------------------------------------------------------------

def selftest() -> int:
    """Signal synthétique : plancher 28 µA bruité, poll toutes les 15 s
    (4 ms à 25 mA, soit ~100 µC), un appui de 8 mC à t = 40 s."""
    rate = 100_000
    floor, poll_ma, poll_ms, period = 28.0, 25.0, 4.0, 15.0
    duration = 62.0
    rnd = random.Random(1)
    expected_poll_uc = (poll_ma * 1e3 - floor) * poll_ms / 1e3

    fd, path = tempfile.mkstemp(suffix=".csv")
    with os.fdopen(fd, "w") as f:
        f.write("Timestamp(ms),Current(uA),D0-D7\n")
        n = int(duration * rate)
        for k in range(n):
            t = k / rate
            ua = floor + rnd.gauss(0, 1.5)
            phase = (t - 1.0) % period
            if t >= 1.0 and phase < poll_ms / 1e3:
                ua = poll_ma * 1e3
            if 40.0 <= t < 40.2:            # appui : 200 ms à 40 mA = 8 mC
                ua = 40_000.0
            f.write(f"{t * 1e3:.3f},{ua:.3f},00000000\n")

    try:
        res = analyze(open_samples(path), threshold_ua=300.0, merge_gap_ms=50.0)
        out = report(res, presses_per_day=20)
    finally:
        os.unlink(path)

    ok = True

    def check(name, got, want, rel):
        nonlocal ok
        good = got is not None and math.isclose(got, want, rel_tol=rel)
        ok &= good
        print(f"  [{'OK' if good else 'ÉCHEC'}] {name}: obtenu {got}, attendu {want} (±{rel:.0%})")

    print()
    print("AUTO-TEST")
    check("plancher (µA)", round(res.floor_ua, 2), floor, 0.05)
    check("période (s)", out["period_s"], period, 0.01)
    check("charge par poll (µC)", out["poll_mc"] * 1e3 if out["poll_mc"] else None, expected_poll_uc, 0.05)
    press = [e for e in res.events if e.net_uc > 1000]
    check("charge de l'appui (µC)", press[0].net_uc if press else None, 8000 - floor * 0.2, 0.02)
    return 0 if ok else 1


# --------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="?", help="export CSV du Power Profiler")
    ap.add_argument("--threshold-ua", type=float, default=300.0,
                    help="seuil de détection d'un réveil (défaut 300 µA)")
    ap.add_argument("--merge-gap-ms", type=float, default=50.0,
                    help="fusionne deux pics séparés de moins de N ms (défaut 50)")
    ap.add_argument("--presses-per-day", type=float, default=20.0)
    ap.add_argument("--selftest", action="store_true", help="vérifie l'analyse sur un signal synthétique")
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if not args.csv:
        ap.error("chemin du CSV manquant (ou --selftest)")
    res = analyze(open_samples(args.csv), args.threshold_ua, args.merge_gap_ms)
    report(res, args.presses_per_day)
    return 0


if __name__ == "__main__":
    sys.exit(main())
