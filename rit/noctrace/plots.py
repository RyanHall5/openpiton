"""Static PNG plots for one run (matplotlib, headless)."""
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.colors import LinearSegmentedColormap, Normalize  # noqa: E402

from . import metrics  # noqa: E402

SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
GRID = "#e4e3df"
NOC_COLORS = {1: "#2a78d6", 2: "#eb6834", 3: "#1baf7a"}   # categorical slots 1-3
SEQ = LinearSegmentedColormap.from_list(
    "seq_blue", ["#f3f7fd", "#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5",
                 "#256abf", "#184f95", "#0d366b"])

_RC = {
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "text.color": TEXT, "axes.labelcolor": TEXT2, "xtick.color": TEXT2, "ytick.color": TEXT2,
    "axes.edgecolor": GRID, "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8,
    "axes.spines.top": False, "axes.spines.right": False, "font.size": 9,
    "axes.titlesize": 11, "axes.titleweight": "bold", "axes.titlelocation": "left",
    "legend.frameon": False,
}
# Older matplotlib (e.g. RHEL's system package) lacks some keys; skip those.
for _k, _v in _RC.items():
    try:
        plt.rcParams[_k] = _v
    except KeyError:
        pass


def _title(fig, run, what):
    """Bold title on the first line, run details on a quieter second line.
    Callers leave the top ~12% of the figure free (tight_layout rect top=0.88)."""
    fig.suptitle(what, x=0.01, y=0.985, ha="left", va="top", fontsize=12, fontweight="bold")
    fig.text(0.01, 0.985 - 0.32 / fig.get_figheight(),
             f"{run.meta.get('format')}  ·  {run.mesh.X}x{run.mesh.Y} mesh  ·  "
             f"{os.path.basename(run.meta.get('file', ''))}",
             fontsize=8, color=TEXT2, ha="left", va="top")


def _end_labels(ax, items, min_gap_frac=0.07):
    """Direct labels at line ends, nudged apart so they never overlap."""
    if not items:
        return
    lo, hi = ax.get_ylim()
    gap = (hi - lo) * min_gap_frac
    items = sorted(items, key=lambda t: t[1])
    placed = []
    for x, y, text in items:
        y2 = max(y, placed[-1][1] + gap) if placed else y
        placed.append((x, y2, text))
    for x, y2, text in placed:
        ax.annotate(text, (x, y2), xytext=(5, 0), textcoords="offset points",
                    va="center", fontsize=8, color=TEXT2, annotation_clip=False)


def traffic_over_time(run, bin_cycles, path):
    df = metrics.total_over_time(run, bin_cycles)
    fig, axes = plt.subplots(2, 1, figsize=(10, 5.6), sharex=True)
    panels = [("injected_flits", "Injected flits / cycle"),
              ("mesh_flit_hops", "Router-to-router flit-hops / cycle")]
    for ax, (col, lab) in zip(axes, panels):
        ends = []
        for noc in sorted(df["noc"].unique()):
            d = df[df["noc"] == noc]
            xs = d["bin_start"].to_numpy()
            y = (d[col] / bin_cycles).to_numpy()
            ax.plot(xs, y, color=NOC_COLORS.get(noc, TEXT2), lw=2,
                    label=f"NoC{noc}", solid_capstyle="round")
            if len(d):
                ends.append((float(xs[-1]), float(y[-1]), f"NoC{noc}"))
        ax.set_ylabel(lab)
        ax.set_ylim(bottom=0)
        _end_labels(ax, ends)
    axes[0].legend(loc="upper right", ncol=3)
    axes[-1].set_xlabel(f"cycle  (bins of {bin_cycles} cycles)")
    _title(fig, run, "NoC traffic over time")
    fig.tight_layout(rect=(0, 0, 0.96, 0.9))
    fig.savefig(path, dpi=150)
    plt.close(fig)


def source_heatmap(run, bin_cycles, path):
    df = metrics.per_source_over_time(run, bin_cycles)
    if df.empty:
        return
    piv = df.pivot_table(index="src", columns="bin_start", values="injected_flits",
                         aggfunc="sum", fill_value=0) / bin_cycles
    order = sorted(piv.index, key=lambda s: (s == "offchip", int(s[4:s.index("(")]) if s != "offchip" else 0))
    piv = piv.reindex(order)
    h = max(2.6, 0.32 * len(piv) + 1.6)
    fig, ax = plt.subplots(figsize=(10, h))
    ax.grid(False)
    cols = np.asarray(piv.columns, dtype=float)
    edges = np.append(cols, cols[-1] + bin_cycles)
    im = ax.pcolormesh(edges, np.arange(len(piv) + 1), piv.to_numpy(), cmap=SEQ,
                       shading="flat", edgecolors=SURFACE, linewidth=0.5)
    ax.set_yticks(np.arange(len(piv)) + 0.5)
    ax.set_yticklabels(list(piv.index))
    ax.invert_yaxis()
    ax.set_xlabel(f"cycle  (bins of {bin_cycles} cycles)")
    cb = fig.colorbar(im, ax=ax, pad=0.01)
    cb.set_label("injected flits / cycle (all NoCs)")
    cb.outline.set_visible(False)
    _title(fig, run, "Injection per source over time")
    fig.tight_layout(rect=(0, 0, 1, 1 - 0.75 / h))
    fig.savefig(path, dpi=150)
    plt.close(fig)


def link_map(run, path):
    links = run.links
    nocs = sorted(links["noc"].unique())
    X, Y = run.mesh.X, run.mesh.Y
    cell = 1.05
    fig, axes = plt.subplots(1, len(nocs), figsize=(len(nocs) * (X + 0.9) * cell + 1.2, Y * cell + 1.3),
                             squeeze=False, gridspec_kw={"wspace": 0.08})
    mesh_like = links[links["kind"].isin(["mesh", "to_offchip", "from_offchip"])]
    vmax = max(float(mesh_like["utilization"].max()) if len(mesh_like) else 0, 1e-9)
    norm = Normalize(0, vmax)
    off = 0.09
    for ax, noc in zip(axes[0], nocs):
        ax.set_aspect("equal")
        ax.grid(False)
        ax.axis("off")
        ax.set_title(f"NoC{noc}", color=NOC_COLORS.get(noc, TEXT))
        for i in range(X):
            for j in range(Y):
                ax.add_patch(plt.Rectangle((i - 0.17, j - 0.17), 0.34, 0.34, fc="#f0efec",
                                           ec="#c3c2b7", lw=1, zorder=3))
                ax.text(i, j, str(i + j * X), ha="center", va="center", fontsize=8,
                        color=TEXT2, zorder=4)
        ax.add_patch(plt.Rectangle((-1.05, -0.17), 0.34, 0.34, fc=SURFACE, ec="#c3c2b7",
                                   lw=1, ls="--", zorder=3))
        ax.text(-0.88, 0, "off\nchip", ha="center", va="center", fontsize=6.5, color=TEXT2, zorder=4)
        d = mesh_like[mesh_like["noc"] == noc]
        for r in d.itertuples():
            fx, fy = (r.from_x, r.from_y) if r.from_x >= 0 else (-0.88, 0)
            tx, ty = (r.to_x, r.to_y) if r.to_x >= 0 else (-0.88, 0)
            vx, vy = tx - fx, ty - fy
            n = max((vx ** 2 + vy ** 2) ** 0.5, 1e-9)
            px, py = -vy / n * off, vx / n * off          # offset to the right of travel
            sx, sy = fx + vx / n * 0.2 + px, fy + vy / n * 0.2 + py
            ex, ey = tx - vx / n * 0.2 + px, ty - vy / n * 0.2 + py
            ax.annotate("", (ex, ey), (sx, sy), zorder=2,
                        arrowprops=dict(arrowstyle="-|>", color=SEQ(0.25 + 0.75 * norm(r.utilization)),
                                        lw=1 + 3.5 * norm(r.utilization), shrinkA=0, shrinkB=0,
                                        mutation_scale=9))
        for r in d.nlargest(3, "flits").itertuples():
            fx, fy = (r.from_x, r.from_y) if r.from_x >= 0 else (-0.88, 0)
            tx, ty = (r.to_x, r.to_y) if r.to_x >= 0 else (-0.88, 0)
            vx, vy = tx - fx, ty - fy
            n = max((vx ** 2 + vy ** 2) ** 0.5, 1e-9)
            mx, my = (fx + tx) / 2 - vy / n * 0.24, (fy + ty) / 2 + vx / n * 0.24
            ax.text(mx, my, f"{r.utilization:.2f}", fontsize=7, ha="center", va="center",
                    color=TEXT, zorder=5,
                    bbox=dict(fc=SURFACE, ec="none", pad=0.6, alpha=0.9))
        ax.set_xlim(-1.3, X - 0.5)
        ax.set_ylim(Y - 0.5, -0.6)
    sm = plt.cm.ScalarMappable(norm=norm, cmap=SEQ)
    sm.set_array(np.array([]))
    cb = fig.colorbar(sm, ax=axes[0].tolist(), shrink=0.7, pad=0.02)
    cb.set_label("link utilization (flits / cycle over run)")
    cb.outline.set_visible(False)
    fig.suptitle("Link load per NoC  (arrows = direction of travel; top 3 labelled)",
                 x=0.01, ha="left", fontsize=12, fontweight="bold")
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)


def path_matrix(run, path):
    m = metrics.path_matrix(run)
    m = m.loc[(m.sum(axis=1) > 0) | (m.sum(axis=0) > 0)]
    m = m.loc[:, (m.sum(axis=0) > 0) | m.columns.isin(m.index)]
    n = max(len(m.index), len(m.columns))
    fig, ax = plt.subplots(figsize=(min(2.5 + 0.55 * len(m.columns), 16), min(2 + 0.45 * len(m.index), 14)))
    ax.grid(False)
    im = ax.imshow(m.to_numpy(), cmap=SEQ, aspect="auto")
    ax.set_xticks(range(len(m.columns)))
    ax.set_xticklabels(list(m.columns), rotation=45, ha="right")
    ax.set_yticks(range(len(m.index)))
    ax.set_yticklabels(list(m.index))
    ax.set_xlabel("destination")
    ax.set_ylabel("source")
    if n <= 12:
        vmax = m.to_numpy().max() or 1
        for i in range(len(m.index)):
            for j in range(len(m.columns)):
                v = m.iat[i, j]
                if v:
                    ax.text(j, i, f"{v:,}", ha="center", va="center", fontsize=7,
                            color="white" if v > 0.55 * vmax else TEXT)
    cb = fig.colorbar(im, ax=ax, pad=0.01)
    cb.set_label("flits (all NoCs)")
    cb.outline.set_visible(False)
    ax.set_title("Traffic by path: source → destination")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def top_paths(run, path, top=15):
    p = metrics.per_path(run)
    g = p.groupby(["src", "dst"])["flits"].sum().nlargest(top)[::-1]
    fig, ax = plt.subplots(figsize=(8, 0.3 * len(g) + 1.4))
    ax.grid(axis="y", visible=False)
    labels = [f"{s} → {d}" for s, d in g.index]
    ax.barh(np.arange(len(g)), g.to_numpy(), color=NOC_COLORS[1], height=0.6)
    ax.set_yticks(np.arange(len(g)))
    ax.set_yticklabels(labels)
    for i, v in enumerate(g.to_numpy()):
        ax.text(v, i, f" {v:,}", va="center", fontsize=8, color=TEXT2)
    ax.set_xlabel("flits (all NoCs)")
    ax.set_title(f"Top {len(g)} paths by flits")
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def _sig_title(app, core, rnd):
    t = f"{app} — Core {core}" if app else f"Core {core}"
    return t + (f", Round {rnd}" if rnd is not None else "")


def signature_plots(sig, window, outdir, app=None, rnd=None, metric="injected",
                    unit="packets"):
    """One packets-per-window line plot per core (sig = metrics.signature()),
    plus one figure with every core on a shared y-axis."""
    import os
    d = os.path.join(outdir, "signatures")
    os.makedirs(d, exist_ok=True)
    xlabel = f"Cycle window ({window:,} cycles each)"
    ylabel = f"Number of {unit}" + ("" if metric == "injected" else f" ({metric})")
    made = []
    cores = sorted(sig["core"].unique())
    for c in cores:
        s = sig[sig["core"] == c]
        fig, ax = plt.subplots(figsize=(8, 4.2))
        ax.plot(s["window"].to_numpy(), s[unit].to_numpy(), color=NOC_COLORS[1], lw=1.6)
        ax.set_title(_sig_title(app, c, rnd), loc="center")
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        ax.set_ylim(bottom=0)
        fig.tight_layout()
        p = os.path.join(d, f"core{c}.png")
        fig.savefig(p, dpi=150)
        plt.close(fig)
        made.append(p)

    n = len(cores)
    ncol = min(4, n)
    nrow = (n + ncol - 1) // ncol
    fig, axes = plt.subplots(nrow, ncol, figsize=(3.4 * ncol, 2.4 * nrow + 0.8),
                             sharex=True, sharey=True, squeeze=False)
    for ax in axes.flat[n:]:
        ax.set_visible(False)
    for ax, c in zip(axes.flat, cores):
        s = sig[sig["core"] == c]
        ax.plot(s["window"].to_numpy(), s[unit].to_numpy(), color=NOC_COLORS[1], lw=1.2)
        ax.set_title(f"Core {c}", loc="center", fontsize=10)
    ymax = float(sig[unit].max()) if len(sig) else 1.0
    axes.flat[0].set_ylim(0, ymax * 1.08 or 1)   # shared y: one limit for every core
    for ax in axes[-1]:
        ax.set_xlabel("Cycle window")
    for ax in axes[:, 0]:
        ax.set_ylabel(unit.capitalize())
    head = (f"{app} — " if app else "") + f"{unit} per core per window"
    head += (f", Round {rnd}" if rnd is not None else "") + f"  ({window:,} cycles/window, shared y-axis)"
    fig.suptitle(head, x=0.01, ha="left", fontsize=11, fontweight="bold")
    fig.tight_layout(rect=(0, 0, 1, 0.95 if nrow > 1 else 0.9))
    p = os.path.join(outdir, "signatures_all_cores.png")
    fig.savefig(p, dpi=150)
    plt.close(fig)
    made.append(p)
    return made


def all_plots(run, bin_cycles, outdir):
    import os
    made = []
    for name, fn in [("traffic_over_time.png", lambda p: traffic_over_time(run, bin_cycles, p)),
                     ("injection_by_source.png", lambda p: source_heatmap(run, bin_cycles, p)),
                     ("link_map.png", lambda p: link_map(run, p)),
                     ("path_matrix.png", lambda p: path_matrix(run, p)),
                     ("top_paths.png", lambda p: top_paths(run, p))]:
        p = os.path.join(outdir, name)
        fn(p)
        if os.path.exists(p):
            made.append(p)
    return made
