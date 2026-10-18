#!/usr/bin/env python3

import argparse
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SCEN_TITLES = {
    "E1": "E1: malla grande, sin instantáneas",
    "E2": "E2: malla grande, instantánea cada 10 pasos (B=4)",
    "E3": "E3: malla pequeña, sin instantáneas",
}
NUMERIC = ["rep", "nx", "ny", "steps", "threads", "k", "B", "init_s", "run_s",
           "barrier_mean_s", "barrier_max_s", "io_push_s", "compute_s",
           "flush_s", "total_s"]


def read_meta(path):
    meta = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if "=" in line:
                    key, value = line.strip().split("=", 1)
                    meta[key] = value
    return meta


def load_csv(path):
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            for key in NUMERIC:
                r[key] = float(r[key])
            r["threads"] = int(r["threads"])
            rows.append(r)
    return rows


def quartiles_iqr(values):
    if len(values) < 2:
        return 0.0
    q = statistics.quantiles(values, n=4, method="inclusive")
    return q[2] - q[0]


def analyze(rows):
    """Devuelve (p, tabla ordenada por hilos, avisos)."""
    by_t = defaultdict(list)
    for r in rows:
        by_t[r["threads"]].append(r)
    if 1 not in by_t:
        raise ValueError("faltan mediciones con 1 hilo: no se puede calcular S(n)")

    med = lambda key, rs: statistics.median(r[key] for r in rs)  # noqa: E731
    t1 = med("total_s", by_t[1])
    compute1 = med("compute_s", by_t[1])
    p = max(0.0, min(1.0, compute1 / t1))

    table = []
    for n in sorted(by_t):
        rs = by_t[n]
        totals = [r["total_s"] for r in rs]
        tn = statistics.median(totals)
        s = t1 / tn
        entry = {
            "threads": n,
            "reps": len(rs),
            "T_med": tn,
            "T_min": min(totals),
            "T_max": max(totals),
            "T_std": statistics.pstdev(totals) if len(totals) > 1 else 0.0,
            "T_iqr": quartiles_iqr(totals),
            "S": s,
            "S_lo": t1 / max(totals),
            "S_hi": t1 / min(totals),
            "E": s / n,
            "e": ((1.0 / s - 1.0 / n) / (1.0 - 1.0 / n)) if n > 1 else float("nan"),
            "S_amdahl": 1.0 / ((1.0 - p) + p / n),
        }
        for key in ("init_s", "run_s", "barrier_mean_s", "barrier_max_s",
                    "io_push_s", "compute_s", "flush_s"):
            entry[key] = med(key, rs)
        table.append(entry)

    warnings = []
    hashes = {r["hash_ez"] for r in rows}
    if len(hashes) != 1:
        warnings.append(f"ATENCION: hash_ez distinto entre corridas ({len(hashes)} valores): "
                        "el resultado NO fue idéntico bit a bit (¿carrera de datos?)")
    return p, table, warnings


def write_summary(path, p, table):
    cols = ["threads", "reps", "T_med", "T_min", "T_max", "T_std", "T_iqr", "S", "S_lo",
            "S_hi", "E", "e", "S_amdahl", "init_s", "run_s", "barrier_mean_s",
            "barrier_max_s", "io_push_s", "compute_s", "flush_s"]
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["# p_estimada", f"{p:.6f}"])
        w.writerow(cols)
        for e in table:
            w.writerow([e[c] if c in ("threads", "reps") else f"{e[c]:.6g}" for c in cols])


def save(fig, out_dir, name, formats):
    for ext in formats:
        fig.savefig(os.path.join(out_dir, f"{name}.{ext}"), dpi=150, bbox_inches="tight")
    plt.close(fig)


def mark_cores(ax, cores):
    if cores:
        ax.axvline(cores, color="gray", ls=":", lw=1)
        ax.text(cores, ax.get_ylim()[1], " núcleos", color="gray", fontsize=8, va="top")


def plot_scenario(env, scen, rows, p, table, cores, out_dir, formats):
    n = [e["threads"] for e in table]
    fig, ax = plt.subplots(1, 3, figsize=(15, 4.3))
    nmax = max(n)

    # --- aceleración
    s = [e["S"] for e in table]
    lo = [e["S"] - e["S_lo"] for e in table]
    hi = [e["S_hi"] - e["S"] for e in table]
    ax[0].plot([1, nmax], [1, nmax], color="lightgray", ls="--", label="ideal $S=n$")
    ns = sorted(set(n + [x for x in range(1, nmax + 1) if x & (x - 1) == 0]))
    ax[0].plot(ns, [1.0 / ((1 - p) + p / x) for x in ns], "C1-", label=f"Amdahl ($p$={p:.3f})")
    ax[0].errorbar(n, s, yerr=[lo, hi], fmt="o-", color="C0", capsize=3, label="real (mediana, min–máx)")
    ax[0].set_title("Aceleración")
    ax[0].set_xlabel("hilos $n$")
    ax[0].set_ylabel("$S(n)=T(1)/T(n)$")
    ax[0].legend(fontsize=8)
    mark_cores(ax[0], cores)

    # --- eficiencia
    ax[1].plot(n, [e["E"] for e in table], "o-", color="C0", label="real")
    ax[1].plot(ns, [(1.0 / ((1 - p) + p / x)) / x for x in ns], "C1-", label="Amdahl")
    ax[1].axhline(1.0, color="lightgray", ls="--")
    ax[1].set_title("Eficiencia")
    ax[1].set_xlabel("hilos $n$")
    ax[1].set_ylabel("$E(n)=S(n)/n$")
    ax[1].set_ylim(0, max(1.1, max(e["E"] for e in table) * 1.05))
    ax[1].legend(fontsize=8)
    mark_cores(ax[1], cores)

    # --- Karp-Flatt
    kn = [e["threads"] for e in table if e["threads"] > 1]
    ke = [e["e"] for e in table if e["threads"] > 1]
    ax[2].plot(kn, ke, "o-", color="C3", label="$e(n)$ experimental")
    ax[2].axhline(1 - p, color="C1", ls="-", label=f"$1-p$ = {1 - p:.3f} (Amdahl)")
    ax[2].set_title("Fracción serial de Karp–Flatt")
    ax[2].set_xlabel("hilos $n$")
    ax[2].set_ylabel("$e(n)$")
    ax[2].legend(fontsize=8)
    mark_cores(ax[2], cores)

    for a in ax:
        a.set_xscale("log", base=2)
        a.set_xticks(n)
        a.set_xticklabels([str(x) for x in n])
        a.grid(alpha=0.3)
    if max(s) > 4:
        ax[0].set_yscale("log", base=2)
        ax[0].set_yticks([2 ** i for i in range(0, int(max(s)).bit_length() + 1)])
        ax[0].set_yticklabels([str(2 ** i) for i in range(0, int(max(s)).bit_length() + 1)])

    r0 = rows[0]
    fig.suptitle(f"{env} — {SCEN_TITLES.get(scen, scen)}  "
                 f"({int(r0['nx'])}×{int(r0['ny'])}, {int(r0['steps'])} pasos)", fontsize=11)
    fig.tight_layout()
    save(fig, out_dir, f"{env}_{scen}", formats)

    # --- tiempo por etapa
    fig, ax = plt.subplots(figsize=(7.5, 4.3))
    idx = list(range(len(n)))
    parts = [
        ("compute_s", "cómputo", "C0"),
        ("barrier_mean_s", "espera en barreras (media/hilo)", "C3"),
        ("io_push_s", "publicación/E-S del hilo 0", "C2"),
        ("flush_s", "espera al escritor a disco", "C4"),
        ("init_s", "inicialización", "C7"),
    ]
    bottom = [0.0] * len(n)
    for key, label, color in parts:
        vals = [e[key] for e in table]
        ax.bar(idx, vals, bottom=bottom, label=label, color=color, width=0.6)
        bottom = [b + v for b, v in zip(bottom, vals)]
    ax.set_xticks(idx)
    ax.set_xticklabels([str(x) for x in n])
    ax.set_xlabel("hilos $n$")
    ax.set_ylabel("tiempo (s, mediana)")
    ax.set_title(f"{env} — {scen}: tiempo por etapa")
    ax.legend(fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    save(fig, out_dir, f"{env}_{scen}_etapas", formats)


def plot_comparison(scen, results, out_dir, formats):
    fig, ax = plt.subplots(1, 2, figsize=(11, 4.3))
    nmax = max(max(e["threads"] for e in t) for _, (_, t, _) in results.items())
    ax[0].plot([1, nmax], [1, nmax], color="lightgray", ls="--", label="ideal")
    for i, (env, (p, table, _)) in enumerate(sorted(results.items())):
        n = [e["threads"] for e in table]
        ax[0].plot(n, [e["S"] for e in table], "o-", color=f"C{i}", label=f"{env} (p={p:.3f})")
        ax[1].plot(n, [e["E"] for e in table], "o-", color=f"C{i}", label=env)
    ax[0].set_ylabel("$S(n)$")
    ax[1].set_ylabel("$E(n)$")
    for a in ax:
        a.set_xscale("log", base=2)
        a.set_xlabel("hilos $n$")
        a.grid(alpha=0.3)
        a.legend(fontsize=8)
    fig.suptitle(f"Comparación entre ambientes — {SCEN_TITLES.get(scen, scen)}", fontsize=11)
    fig.tight_layout()
    save(fig, out_dir, f"comparacion_{scen}", formats)


def print_table(env, scen, p, table):
    print(f"\n{env} / {scen}   p estimada = {p:.4f}   (serial 1-p = {1 - p:.4f}, "
          f"S máx. teórica = {1 / (1 - p) if p < 1 else float('inf'):.1f})")
    print(f"{'n':>4} {'T mediana':>10} {'IQR':>8} {'S real':>8} {'S Amdahl':>9} {'E':>6} {'e(n)':>8}")
    for e in table:
        ke = "   -   " if e["threads"] == 1 else f"{e['e']:8.4f}"
        print(f"{e['threads']:>4} {e['T_med']:>10.4f} {e['T_iqr']:>8.4f} {e['S']:>8.3f} "
              f"{e['S_amdahl']:>9.3f} {e['E']:>6.3f} {ke}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--evidencia", default="evidencia", help="carpeta con evidencia/<ambiente>/bench_*.csv")
    ap.add_argument("--out", default="docs/figuras", help="carpeta de salida de las figuras")
    ap.add_argument("--formatos", default="png,pdf", help="formatos de figura separados por coma")
    args = ap.parse_args()
    formats = [f.strip() for f in args.formatos.split(",") if f.strip()]

    files = sorted(glob.glob(os.path.join(args.evidencia, "*", "bench_*.csv")))
    if not files:
        print(f"No hay archivos bench_*.csv en {args.evidencia}/<ambiente>/. Ejecute scripts/bench.sh primero.")
        return 1
    os.makedirs(args.out, exist_ok=True)

    per_scen = defaultdict(dict)
    problems = 0
    for path in files:
        env = os.path.basename(os.path.dirname(path))
        scen = os.path.basename(path)[len("bench_"):-len(".csv")]
        rows = load_csv(path)
        if not rows:
            print(f"{path}: vacío, se omite")
            continue
        try:
            p, table, warnings = analyze(rows)
        except ValueError as exc:
            print(f"{path}: {exc}")
            problems += 1
            continue

        cores = None
        meta = read_meta(os.path.join(os.path.dirname(path), "meta.txt"))
        if meta.get("cores", "").isdigit():
            cores = int(meta["cores"])

        print_table(env, scen, p, table)
        for w in warnings:
            print("  " + w)
            problems += 1
        write_summary(os.path.join(os.path.dirname(path), f"resumen_{scen}.csv"), p, table)
        plot_scenario(env, scen, rows, p, table, cores, args.out, formats)
        per_scen[scen][env] = (p, table, rows)

    for scen, results in per_scen.items():
        if len(results) >= 2:
            plot_comparison(scen, results, args.out, formats)

    print(f"\nFiguras en {args.out}/  |  resúmenes en {args.evidencia}/<ambiente>/resumen_*.csv")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())