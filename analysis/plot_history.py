#!/usr/bin/env python3
"""Plot the logged solar history: energy ratios and measurement distributions.

Reads the CSV exported from the dashboard (``/api/export.csv``) and writes the
figures in ``docs/figures/`` in both light and dark variants.

    python3 analysis/plot_history.py                     # uses data/history.csv
    python3 analysis/plot_history.py --csv other.csv
    python3 analysis/plot_history.py --fetch             # pull a fresh export

Provenance, per docs/data-model.md: battery_v, soc, pv_v, pv_w and load_w are
`measured` (soc is the controller's own estimate). Energies, the self-sufficiency
and self-consumption ratios, and the hour splits are `calculated` by integrating
measured power. The implied pack capacity is an `estimate`. Nominal panel power
and the 7 Ah / 12 Ah cell ratings are `spec` and never appear as results here.

Energy follows the dashboard's own method: trapezoidal integration of the
measured pv_w and load_w, with intervals longer than MAX_GAP_S skipped rather
than interpolated, so a logger outage under-reports rather than inventing energy.
"""

import argparse
import csv
import datetime
import os
import subprocess
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIGDIR = os.path.join(REPO, "docs", "figures")
EXPORT_URL = "https://solar.example.com/api/export.csv"

MAX_GAP_S = 30 * 60          # matches the dashboard: longer gaps are not integrated
NOMINAL_V = 12.8             # 12 V AGM nominal, used only to express Wh as Ah

# dataviz reference palette, validated for both surfaces (ALL CHECKS PASS).
# PV/production is orange and the battery is blue, matching the live dashboard at
# solar.example.com/solar so the two can be read side by side. The hexes are this
# palette's own validated steps rather than the site's, since these figures also
# ship a dark variant; only the measure -> hue mapping is shared.
THEMES = {
    "light": dict(surface="#fcfcfb", ink="#0b0b0b", ink2="#52514e",
                  grid="#d9d8d4", solar="#eb6834", batt="#2a78d6", hist="#2a78d6"),
    "dark":  dict(surface="#1a1a19", ink="#ffffff", ink2="#c3c2b7",
                  grid="#3a3a38", solar="#d95926", batt="#3987e5", hist="#3987e5"),
}


# ---------------------------------------------------------------- data loading

def load(path):
    rows = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            rows.append(dict(
                t=int(r["device_ts"]),
                config=r["config_id"],
                bv=float(r["battery_v"]),
                soc=float(r["soc"]),
                pv_v=float(r["pv_v"]),
                pv_w=float(r["pv_w"]),
                load_w=float(r["load_w"]),
                charge_a=float(r["charge_a"]),
                load_vi=float(r["load_v"]) * float(r["load_a"]),
                charge_w=float(r["charge_a"]) * float(r["battery_v"]),
            ))
    rows.sort(key=lambda r: r["t"])
    return rows


def fetch(path):
    """Pull a fresh export. Credentials come from ~/.netrc, never from source."""
    subprocess.check_call(["curl", "-sS", "--netrc", "--fail", "-o", path, EXPORT_URL])
    return path


# ------------------------------------------------------------------- analysis

def integrate(rows):
    """Trapezoidal Wh and the hour split, skipping gaps > MAX_GAP_S."""
    out = dict(prod=0.0, cons=0.0, cons_vi=0.0, self_c=0.0,
               h_full=0.0, h_part=0.0, h_none=0.0, covered=0.0,
               gaps=0, gap_h=0.0, days={})
    for a, b in zip(rows, rows[1:]):
        dt = b["t"] - a["t"]
        if dt <= 0:
            continue
        if dt > MAX_GAP_S:
            out["gaps"] += 1
            out["gap_h"] += dt / 3600.0
            continue
        h = dt / 3600.0
        pv, ld = (a["pv_w"] + b["pv_w"]) / 2, (a["load_w"] + b["load_w"]) / 2
        ld_vi = (a["load_vi"] + b["load_vi"]) / 2
        sc = (min(a["pv_w"], a["load_w"]) + min(b["pv_w"], b["load_w"])) / 2
        out["covered"] += h
        out["prod"] += pv * h
        out["cons"] += ld * h
        out["cons_vi"] += ld_vi * h
        out["self_c"] += sc * h
        if pv <= 0:
            out["h_none"] += h
        elif pv >= ld:
            out["h_full"] += h
        else:
            out["h_part"] += h
        # Local-time days, split at midnight and allocated pro rata. Two reasons:
        # a solar day IS a local day (UTC-6 here would cut each day's sunlight
        # across two buckets), and charging a midnight-spanning interval wholly
        # to the day it started in lets a day exceed 24 h of observation.
        cur = a["t"]
        while cur < b["t"]:
            midnight = datetime.datetime.fromtimestamp(cur).replace(
                hour=0, minute=0, second=0, microsecond=0) + datetime.timedelta(days=1)
            edge = min(b["t"], int(midnight.timestamp()))
            if edge <= cur:
                break
            frac = (edge - cur) / dt
            day = datetime.datetime.fromtimestamp(cur).strftime("%m-%d")
            d = out["days"].setdefault(day, dict(cov=0.0, prod=0.0, cons=0.0,
                                                 cons_vi=0.0, self_c=0.0, soc0=None, soc1=None))
            d["cov"] += h * frac
            d["prod"] += pv * h * frac
            d["cons"] += ld * h * frac
            d["cons_vi"] += ld_vi * h * frac
            d["self_c"] += sc * h * frac
            if d["soc0"] is None:
                d["soc0"] = a["soc"]
            d["soc1"] = b["soc"]
            cur = edge
    return out


def capacity_estimate(rows):
    """Wh drawn per SOC point across PV-zero stretches -> implied pack size."""
    runs, cur = [], []
    for r in rows:
        if r["pv_w"] == 0:
            cur.append(r)
        else:
            if len(cur) > 2:
                runs.append(cur)
            cur = []
    if len(cur) > 2:
        runs.append(cur)

    wh_tot = soc_tot = 0.0
    for run in runs:
        if (run[-1]["t"] - run[0]["t"]) < 2 * 3600:
            continue
        wh, ok = 0.0, True
        for a, b in zip(run, run[1:]):
            dt = b["t"] - a["t"]
            if dt > MAX_GAP_S:
                ok = False
                break
            wh += (a["load_w"] + b["load_w"]) / 2 * dt / 3600.0
        drop = run[0]["soc"] - run[-1]["soc"]
        if ok and wh > 0 and drop > 0:
            wh_tot += wh
            soc_tot += drop
    if soc_tot == 0:
        return None
    per_point = wh_tot / soc_tot
    return dict(per_point=per_point, wh=per_point * 100,
                ah=per_point * 100 / NOMINAL_V, wh_tot=wh_tot, soc_tot=soc_tot)


# -------------------------------------------------------------------- drawing

def style(theme):
    t = THEMES[theme]
    plt.rcParams.update({
        "figure.facecolor": t["surface"], "axes.facecolor": t["surface"],
        "savefig.facecolor": t["surface"], "text.color": t["ink"],
        "axes.labelcolor": t["ink2"], "xtick.color": t["ink2"],
        "ytick.color": t["ink2"], "axes.edgecolor": t["grid"],
        "font.size": 10, "axes.titlesize": 12, "figure.dpi": 160,
    })
    return t


def footer(fig, text, t):
    fig.text(0.5, 0.015, text, ha="center", color=t["ink2"], fontsize=8)


def save(fig, name, theme):
    path = os.path.join(FIGDIR, "%s-%s.png" % (name, theme))
    fig.savefig(path, bbox_inches="tight", pad_inches=0.3)
    plt.close(fig)
    return path


def donut(ax, values, labels, colors, center, sub, t):
    """Two-slice part-of-a-whole with a hero number in the hole."""
    wedges, _ = ax.pie(
        values, colors=colors, startangle=90, counterclock=False,
        wedgeprops=dict(width=0.38, edgecolor=t["surface"], linewidth=2),
    )
    ax.text(0, 0.10, center, ha="center", va="center",
            fontsize=25, color=t["ink"], fontweight="bold")
    ax.text(0, -0.22, sub, ha="center", va="center", fontsize=9, color=t["ink2"])
    # Direct labels, so identity never rests on colour alone.
    for w, lab in zip(wedges, labels):
        ang = np.deg2rad((w.theta1 + w.theta2) / 2)
        ax.annotate(lab, xy=(np.cos(ang) * 1.02, np.sin(ang) * 1.02),
                    xytext=(np.cos(ang) * 1.28, np.sin(ang) * 1.28),
                    ha="center" if abs(np.cos(ang)) < 0.4 else
                       ("left" if np.cos(ang) > 0 else "right"),
                    va="center", fontsize=9, color=t["ink"],
                    arrowprops=dict(arrowstyle="-", color=t["grid"], lw=1))
    ax.set(aspect="equal")


def fig_ratios(E, theme):
    """The two headline ratios, each a genuine part of a whole."""
    t = style(theme)
    fig, axes = plt.subplots(1, 2, figsize=(10, 5.1))
    fig.suptitle("Self-sufficiency and self-consumption", color=t["ink"],
                 fontsize=14, fontweight="bold", y=0.98)

    ss = E["self_c"] / E["cons"]
    solar_h, batt_h = ss * E["covered"], (1 - ss) * E["covered"]
    donut(axes[0], [solar_h, batt_h],
          ["Solar\n%.1f h" % solar_h, "Battery\n%.1f h" % batt_h],
          [t["solar"], t["batt"]], "%.1f%%" % (100 * ss),
          "self-sufficiency", t)
    axes[0].set_title("Load energy: %.0f Wh over %.1f h observed"
                      % (E["cons"], E["covered"]), color=t["ink2"], pad=14)

    sc = E["self_c"] / E["prod"] if E["prod"] else 0
    donut(axes[1], [E["self_c"], E["prod"] - E["self_c"]],
          ["To load\n%.0f Wh" % E["self_c"],
           "To battery\n%.0f Wh" % (E["prod"] - E["self_c"])],
          [t["solar"], t["batt"]], "%.1f%%" % (100 * sc),
          "self-consumption", t)
    axes[1].set_title("PV energy: %.0f Wh generated" % E["prod"],
                      color=t["ink2"], pad=14)

    footer(fig, "calculated — trapezoidal integration of measured pv_w and load_w; "
                "gaps > 30 min skipped, not interpolated", t)
    return save(fig, "ratios", theme)


def fig_hours(E, theme):
    """Where the 35% sits in clock time, and how it moved across the history."""
    t = style(theme)
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.6),
                             gridspec_kw=dict(width_ratios=[1, 1.35]))
    fig.suptitle("The ratio in hours", color=t["ink"], fontsize=14,
                 fontweight="bold", y=1.0)

    ax = axes[0]
    names = ["PV ≥ load", "PV partial", "PV = 0"]
    vals = [E["h_full"], E["h_part"], E["h_none"]]
    cols = [t["solar"], t["solar"], t["batt"]]
    bars = ax.barh(names, vals, height=0.62, color=cols, edgecolor=t["surface"],
                   linewidth=2)
    bars[1].set_alpha(0.55)          # partial coverage, same identity as solar
    for b, v in zip(bars, vals):
        ax.text(v + max(vals) * 0.02, b.get_y() + b.get_height() / 2,
                "%.1f h" % v, va="center", fontsize=9, color=t["ink"])
    ax.set_xlim(0, max(vals) * 1.22)
    ax.set_xlabel("hours observed")
    ax.set_title("What the panel was doing", color=t["ink2"], pad=10)
    ax.invert_yaxis()
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.grid(axis="x", color=t["grid"], lw=0.6, alpha=0.7)
    ax.set_axisbelow(True)

    ax = axes[1]
    days = list(E["days"].keys())
    ssd = [(E["days"][d]["self_c"] / E["days"][d]["cons"]) if E["days"][d]["cons"] else 0
           for d in days]
    solar = [s * E["days"][d]["cov"] for s, d in zip(ssd, days)]
    batt = [(1 - s) * E["days"][d]["cov"] for s, d in zip(ssd, days)]
    ax.bar(days, solar, width=0.62, color=t["solar"], label="Solar",
           edgecolor=t["surface"], linewidth=2)
    ax.bar(days, batt, width=0.62, bottom=solar, color=t["batt"], label="Battery",
           edgecolor=t["surface"], linewidth=2)
    for i, (s, d) in enumerate(zip(solar, days)):
        # Only label inside the segment when it is tall enough to hold the text;
        # on a barely-observed day it would sit on the axis and be unreadable.
        if s >= 1.5:
            ax.text(i, s / 2, "%.1f h" % s, ha="center", va="center", fontsize=8.5,
                    color="#ffffff", fontweight="bold")
        ax.text(i, E["days"][d]["cov"] + 0.5, "%.0f%%" % (100 * ssd[i]),
                ha="center", fontsize=9, color=t["ink"])
    ax.set_ylabel("hours observed")
    ax.set_ylim(0, max(E["days"][d]["cov"] for d in days) * 1.16)
    ax.set_title("Per day — coverage varies, so read the %", color=t["ink2"], pad=10)
    ax.legend(frameon=False, loc="upper left", fontsize=9,
              labelcolor=t["ink2"], ncol=2)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(axis="y", color=t["grid"], lw=0.6, alpha=0.7)
    ax.set_axisbelow(True)

    fig.tight_layout(rect=(0, 0.07, 1, 0.93))
    footer(fig, "calculated — hours are the energy ratio applied to observed time; "
                "a short bar means the logger was down, not that the load stopped", t)
    return save(fig, "hours", theme)


def modes(counts, floor=0.12):
    """Count significant local maxima in a histogram - the unimodality test.

    A normal fit is only drawn over a single-peaked channel; on this system SOC
    and battery voltage are genuinely two-peaked (an absorption/float plateau
    plus an overnight discharge band) and a bell curve over them would describe
    a distribution the hardware does not have.
    """
    thr = counts.max() * floor
    peaks = []
    for i, c in enumerate(counts):
        if c < thr:
            continue
        left = counts[i - 1] if i else 0
        right = counts[i + 1] if i + 1 < len(counts) else 0
        if c >= left and c >= right:
            if peaks and i - peaks[-1] <= 1:
                continue
            peaks.append(i)
    return peaks


def fig_distributions(rows, theme):
    """Histograms of the measured channels; a normal fit only where it holds."""
    t = style(theme)
    fig, axes = plt.subplots(2, 2, figsize=(10.5, 7.4))
    fig.suptitle("Distribution of the measured channels", color=t["ink"],
                 fontsize=14, fontweight="bold")

    panels = [
        ("Battery SOC", [r["soc"] for r in rows], "%", None),
        ("Battery voltage", [r["bv"] for r in rows], "V", None),
        ("PV voltage (daylight only)", [r["pv_v"] for r in rows if r["pv_v"] > 0], "V",
         "%d of %d samples; the %d night samples sit at 0 V"
         % (sum(1 for r in rows if r["pv_v"] > 0), len(rows),
            sum(1 for r in rows if r["pv_v"] == 0))),
        ("Load power", [r["load_w"] for r in rows], "W", None),
    ]
    for ax, (name, data, unit, note) in zip(axes.ravel(), panels):
        d = np.array(data, dtype=float)
        bins = int(min(30, max(8, len(set(data)))))
        counts, edges = np.histogram(d, bins=bins)
        ax.hist(d, bins=bins, color=t["hist"], edgecolor=t["surface"], linewidth=0.8)

        mu, sd = d.mean(), d.std()
        peaks = modes(counts)
        # A channel that sits on one value is not "normal with a small sigma" -
        # load_w is a near-constant 2 W, and a bell over it describes spread the
        # measurement does not have.
        pinned = counts.max() / counts.sum() > 0.6
        lines = ["\u03bc = %.2f %s" % (mu, unit), "\u03c3 = %.2f %s" % (sd, unit)]
        if len(peaks) <= 1 and sd > 0 and not pinned:
            x = np.linspace(d.min(), d.max(), 200)
            pdf = np.exp(-((x - mu) ** 2) / (2 * sd ** 2)) / (sd * np.sqrt(2 * np.pi))
            ax.plot(x, pdf * counts.sum() * (edges[1] - edges[0]),
                    color=t["ink2"], lw=2)
            lines.append("normal fit drawn")
        else:
            centres = [(edges[i] + edges[i + 1]) / 2 for i in peaks]
            for c in centres:
                ax.axvline(c, ymax=0.60, color=t["ink2"], lw=1.2, ls=(0, (4, 3)))
            if pinned:
                lines.append("%.0f%% of samples in one bin" % (100 * counts.max() / counts.sum()))
                lines.append("no normal fit \u2014 near-constant")
            else:
                lines.append("%d modes at %s %s" %
                             (len(peaks), ", ".join("%.1f" % c for c in centres), unit))
                lines.append("no normal fit \u2014 not unimodal")
        if note:
            lines.append(note)

        # Put the note on whichever half of the panel carries less ink.
        half = len(counts) // 2
        side = "right" if counts[:half].max() >= counts[half:].max() else "left"
        ax.text(0.97 if side == "right" else 0.03, 0.96, "\n".join(lines),
                transform=ax.transAxes, ha=side, va="top", fontsize=8,
                color=t["ink2"], linespacing=1.5)

        ax.set_title(name, color=t["ink"], pad=8)
        ax.set_xlabel(unit)
        ax.set_ylabel("samples")
        ax.margins(y=0.28)
        for sp in ("top", "right"):
            ax.spines[sp].set_visible(False)
        ax.grid(axis="y", color=t["grid"], lw=0.6, alpha=0.7)
        ax.set_axisbelow(True)

    fig.tight_layout(rect=(0, 0.045, 1, 0.94))
    footer(fig, "measured \u2014 soc is the controller's own estimate, not a coulomb count; "
                "dashed lines mark the modes of a two-peaked channel", t)
    return save(fig, "distributions", theme)



def fig_balance(rows, E, cap, theme):
    """Why the daily books do not close: the three power channels disagree.

    Left: for every day the logger saw in full, what the panel was measured to
    generate, what the load actually drew, and what generation the battery's own
    SOC says must have happened. Right: the per-sample test from data-model.md -
    pv_power against pv_voltage*pv_current is not the only cross-check available;
    charge_a * battery_v + load power is what physically leaves the panel node.
    """
    t = style(theme)
    fig, axes = plt.subplots(1, 2, figsize=(11.6, 4.8),
                             gridspec_kw=dict(width_ratios=[1.15, 1]))
    fig.suptitle("The daily energy balance does not close", color=t["ink"],
                 fontsize=14, fontweight="bold")

    wh_pt = cap["per_point"] if cap else 0.0
    full = [(d, e) for d, e in E["days"].items()
            if e["cov"] / 24 >= 0.90 and e["soc0"] is not None]

    ax = axes[0]
    if full:
        days = [d for d, _ in full]
        gen = [e["prod"] for _, e in full]
        out = [e["cons_vi"] for _, e in full]
        need = [e["cons_vi"] + (e["soc1"] - e["soc0"]) * wh_pt for _, e in full]
        x = np.arange(len(days))
        ax.bar(x - 0.19, gen, width=0.34, color=t["solar"], label="Generated (measured)",
               edgecolor=t["surface"], linewidth=2)
        ax.bar(x + 0.19, out, width=0.34, color=t["batt"], label="Drawn by the load",
               edgecolor=t["surface"], linewidth=2)
        ax.plot(x - 0.19, need, "D", ms=9, color=t["ink"], zorder=5,
                markeredgecolor=t["surface"], markeredgewidth=1.5,
                label="Generation the battery's SOC requires")
        for xi, g, n in zip(x, gen, need):
            ax.annotate("", xy=(xi - 0.19, n), xytext=(xi - 0.19, g),
                        arrowprops=dict(arrowstyle="->", color=t["ink"], lw=1.4))
            ax.text(xi - 0.30, (g + n) / 2, "%+.0f Wh" % (n - g), ha="right",
                    va="center", fontsize=9, color=t["ink"], fontweight="bold")
        ax.set_xticks(x)
        ax.set_xticklabels(days)
        ax.set_xlim(-0.75, len(days) - 0.25)   # room for the shortfall labels
        ax.set_ylabel("Wh over the day")
        ax.set_title("Days the logger saw in full", color=t["ink2"], pad=10)
        ax.legend(frameon=False, fontsize=8.5, labelcolor=t["ink2"], loc="upper left")
        ax.margins(y=0.22)
    else:
        ax.text(0.5, 0.5, "no day with \u226590% coverage yet",
                ha="center", va="center", transform=ax.transAxes, color=t["ink2"])
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)
    ax.grid(axis="y", color=t["grid"], lw=0.6, alpha=0.7)
    ax.set_axisbelow(True)

    ax = axes[1]
    day_rows = [r for r in rows if r["pv_w"] > 0]
    xs = np.array([r["pv_w"] for r in day_rows])
    ys = np.array([r["charge_w"] + r["load_vi"] for r in day_rows])
    lim = max(xs.max(), ys.max()) * 1.08
    ax.plot([0, lim], [0, lim], color=t["ink2"], lw=1.6, ls=(0, (5, 4)),
            label="agreement (y = x)")
    ax.scatter(xs, ys, s=26, color=t["solar"], alpha=.55,
               edgecolor=t["surface"], linewidth=.6, zorder=3)
    ratio = float(np.median(ys / xs))
    ax.plot([0, lim], [0, lim * ratio], color=t["ink"], lw=2,
            label="observed (y = %.2fx)" % ratio)
    ax.set_xlim(0, lim)
    ax.set_ylim(0, lim)
    ax.set_xlabel("pv_w reported by register 0x0109  (W)")
    ax.set_ylabel("charge_a \u00d7 battery_v + load  (W)")
    ax.set_title("Power leaving the panel node, %d daylight samples" % len(day_rows),
                 color=t["ink2"], pad=10)
    ax.legend(frameon=False, fontsize=8.5, labelcolor=t["ink2"], loc="lower right")
    ax.set_aspect("equal", adjustable="box")
    for sp in ("top", "right"):
        ax.spines[sp].set_visible(False)
    ax.grid(color=t["grid"], lw=0.6, alpha=0.7)
    ax.set_axisbelow(True)

    fig.tight_layout(rect=(0, 0.06, 1, 0.93))
    footer(fig, "0x0109 (pv_power) is marked UNVERIFIED in protocol-bt1.md; "
                "0x0106 (load) was verified at Stage 8 \u2014 the arrows and the slope are the same finding", t)
    return save(fig, "balance", theme)


# ------------------------------------------------------------------------ cli

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", default=os.path.join(REPO, "data", "history.csv"))
    ap.add_argument("--fetch", action="store_true",
                    help="download a fresh export first (needs a ~/.netrc entry)")
    args = ap.parse_args()

    if args.fetch:
        fetch(args.csv)
    if not os.path.exists(args.csv):
        sys.exit("no CSV at %s - pass --csv or --fetch" % args.csv)

    rows = load(args.csv)
    E = integrate(rows)
    cap = capacity_estimate(rows)
    os.makedirs(FIGDIR, exist_ok=True)

    span = (rows[-1]["t"] - rows[0]["t"]) / 3600.0
    fmt = lambda s: datetime.datetime.fromtimestamp(s).strftime("%Y-%m-%d %H:%M local")
    print("samples      : %d  (config_id %s)"
          % (len(rows), ",".join(sorted({r["config"] for r in rows}))))
    print("span         : %s -> %s  (%.2f h)" % (fmt(rows[0]["t"]), fmt(rows[-1]["t"]), span))
    print("observed     : %.2f h  (%d gaps skipped, %.2f h)"
          % (E["covered"], E["gaps"], E["gap_h"]))
    print("production   : %8.2f Wh   [calculated]" % E["prod"])
    print("consumption  : %8.2f Wh   [calculated]" % E["cons"])
    print("self-consumed: %8.2f Wh   [calculated]" % E["self_c"])
    ss = E["self_c"] / E["cons"]
    print("self-sufficiency : %5.1f%%  = %.2f h of %.2f h observed"
          % (100 * ss, ss * E["covered"], E["covered"]))
    print("self-consumption : %5.1f%%" % (100 * E["self_c"] / E["prod"]))
    full = {d: e for d, e in E["days"].items() if e["cov"] / 24 >= 0.90}
    print("\ndaily generation  [calculated] - partial days are NOT scaled to 24 h")
    print("  %-8s %10s %10s %9s" % ("day", "gen Wh", "observed", "coverage"))
    for d, e in E["days"].items():
        print("  %-8s %10.1f %9.1f h %8.0f %%%s"
              % (d, e["prod"], e["cov"], 100*e["cov"]/24,
                 "" if d in full else "  partial"))
    if full:
        mean = sum(e["prod"] for e in full.values()) / len(full)
        print("  mean over %d full day(s): %.1f Wh/day" % (len(full), mean))
    else:
        print("  no day with >=90%% coverage yet - no per-day rate quoted")

    if cap:
        print("pack capacity    : %.2f Wh/SOC-point -> ~%.0f Wh ~ %.1f Ah at %.1f V  [estimate]"
              % (cap["per_point"], cap["wh"], cap["ah"], NOMINAL_V))
        soc = rows[-1]["soc"]
        load_w = rows[-1]["load_w"]
        if load_w > 0:
            print("at SOC %.0f%% and %.1f W: %.0f h to empty, %.0f h to a 20%% floor  [estimate]"
                  % (soc, load_w, soc * cap["per_point"] / load_w,
                     max(soc - 20, 0) * cap["per_point"] / load_w))

    day_rows = [r for r in rows if r["pv_w"] > 0]
    if day_rows:
        ratio = float(np.median([(r["charge_w"] + r["load_vi"]) / r["pv_w"] for r in day_rows]))
        print("\nconservation at the panel node  [calculated]")
        print("  charge_a*battery_v + load(V*I) vs pv_w: median ratio %.2f over %d daylight samples"
              % (ratio, len(day_rows)))
        print("  -> pv_w (register 0x0109, UNVERIFIED) reports ~%.0f%% of the power actually flowing"
              % (100 / ratio))
    print("load truncation   : reported load_w is %.1f%% below load_v*load_a  [calculated]"
          % (100 * (1 - E["cons"] / E["cons_vi"])) if E["cons_vi"] else "")

    for theme in ("light", "dark"):
        for path in (fig_ratios(E, theme), fig_hours(E, theme),
                     fig_distributions(rows, theme), fig_balance(rows, E, cap, theme)):
            print("wrote", os.path.relpath(path, REPO))


if __name__ == "__main__":
    main()
