"""Render source-backed static figures for the repository Markdown report."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path


def main(run, out):
    os.environ.setdefault("MPLCONFIGDIR", str(run / "matplotlib-cache"))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager, ticker

    state = json.loads((run / "state.json").read_text())
    if state["status"] != "complete":
        raise RuntimeError("queue is not complete")
    review_file = run / "review.json"
    review = json.loads(review_file.read_text())
    protocol = json.loads((run / "protocol.json").read_text())
    if review["protocol_sha256"] != hashlib.sha256((run / "protocol.json").read_bytes()).hexdigest():
        raise RuntimeError("review/protocol mismatch")
    out.mkdir(parents=True, exist_ok=False)
    font_path = Path("/System/Library/Fonts/Hiragino Sans GB.ttc")
    font_manager.fontManager.addfont(str(font_path))
    font = font_manager.FontProperties(fname=str(font_path))
    plt.rcParams.update({"font.family": font.get_name(), "font.size": 12,
                         "axes.unicode_minus": False, "axes.spines.top": False,
                         "axes.spines.right": False, "axes.labelsize": 13,
                         "legend.fontsize": 11.5, "figure.facecolor": "white",
                         "axes.facecolor": "white", "savefig.facecolor": "white"})
    # Stable related colors for increasing raw/PQ search widths. Marker shapes
    # and labels preserve identities in grayscale; round two is dashed.
    palette = {
        "raw-ef40": ("#7292ad", "o", "原图 ef40"),
        "raw-ef128": ("#426b8d", "s", "原图 ef128"),
        "raw-ef256": ("#275b84", "^", "原图 ef256"),
        "raw-ef512": ("#173e60", "v", "原图 ef512"),
        "pq-ef256-r16": ("#d47113", "D", "PQ图 ef256 / 精排160"),
        "pq-ef512-r16": ("#9a4d06", "P", "PQ图 ef512 / 精排160"),
        "full-r16": ("#606060", "X", "PQ全扫 / 精排160"),
    }
    rows = review["points"]
    exported, image_records = [], []
    for p in rows:
        exported.append({k: p[k] for k in ("round", "point", "connections", "calls", "recall", "qps",
                                           "p50_ms", "p95_ms", "p99_ms", "cpu_cores", "mean_active_clients")})
    for key, title, ylabel, filename in (
        ("qps", "10万条文本 · 吞吐随并发变化", "吞吐（请求/秒，越高越快）", "throughput.png"),
        ("p95_ms", "10万条文本 · P95延迟随并发变化", "P95查询延迟（毫秒，越低越快）", "p95-latency.png"),
        ("recall", "10万条文本 · 召回随并发变化", "Recall@10（%）", "recall.png"),
    ):
        fig = plt.figure(figsize=(12, 6.8), dpi=140)
        ax = fig.add_axes([.09, .25, .61, .57])
        fig.text(.09, .935, title, fontsize=17, weight="bold", ha="left", color="#172d3d")
        fig.text(.09, .875, "速度比较须结合召回；右侧列出各配置的实测召回", fontsize=11.5, color="#555555")
        handles, labels = [], []
        for method in protocol["methods"]:
            name = method["point"]
            color, marker, label = palette[name]
            relevant = [p for p in rows if p["point"] == name]
            low, high = min(p["recall"] for p in relevant) * 100, max(p["recall"] for p in relevant) * 100
            quality = f"R={low:.2f}%" if abs(high - low) < 1e-10 else f"R={low:.2f}–{high:.2f}%"
            for round_id in (1, 2):
                series = sorted((p for p in relevant if p["round"] == round_id), key=lambda p: p["connections"])
                if len(series) != len(protocol["connections"]):
                    raise RuntimeError("incomplete chart series")
                x = [p["connections"] for p in series]
                y = [p[key] * (100 if key == "recall" else 1) for p in series]
                line, = ax.plot(x, y, color=color, marker=marker, markersize=6, linewidth=1.8,
                                linestyle="-" if round_id == 1 else "--", alpha=1 if round_id == 1 else .8)
                if round_id == 1:
                    handles.append(line)
                    labels.append(label + "\n" + quality)
        ax.set_xscale("log", base=2)
        ax.set_xticks(protocol["connections"])
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.xaxis.set_minor_locator(ticker.NullLocator())
        ax.set_xlim(.85, 75)
        ax.set_xlabel("设置的并发SQL连接数（log2刻度）", labelpad=10)
        ax.set_ylabel(ylabel, labelpad=9)
        ax.grid(axis="y", color="#e1e5e8", linewidth=.7)
        ax.set_axisbelow(True)
        if key == "recall":
            minimum = min(p["recall"] for p in rows) * 100
            lower = max(0, math.floor(minimum * 10) / 10 - .15)
            ax.set_ylim(lower, 100.05)
            ax.text(.02, .12, f"纵轴从{lower:.2f}%起，放大召回差异", transform=ax.transAxes,
                    fontsize=10.5, color="#555555")
        else:
            if key == "p95_ms" and max(p[key] for p in rows) / min(p[key] for p in rows) > 20:
                ax.set_yscale("log")
                ax.set_ylabel("P95查询延迟（毫秒，log10刻度）", labelpad=9)
                ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
            else:
                ax.set_ylim(bottom=0)
            ax.axvline(12, color="#8a8f93", linestyle=":", linewidth=1)
            ax.text(12.5, .98, "12个逻辑CPU", transform=ax.get_xaxis_transform(),
                    fontsize=10.5, va="top", color="#555555")
        legend = fig.legend(handles, labels, loc="center left", bbox_to_anchor=(.73, .51), frameon=False,
                            labelspacing=.85, handlelength=2.1, handletextpad=.7)
        fig.text(.09, .10, "实线：第1轮；虚线：第2轮（反序）。两轮是观测重复，未计算置信区间。",
                 fontsize=11, color="#444444")
        fig.text(.09, .045, "100K × 1536维文本｜200条已用开发查询｜每连接3遍｜12核64 GiB｜本地闭环SQL",
                 fontsize=10.5, color="#555555")
        fig.canvas.draw()
        bounds = fig.get_window_extent()
        for text in list(legend.get_texts()) + list(fig.texts):
            box = text.get_window_extent(fig.canvas.get_renderer())
            if box.x0 < -1 or box.y0 < -1 or box.x1 > bounds.x1 + 1 or box.y1 > bounds.y1 + 1:
                raise RuntimeError("chart text escapes canvas: " + text.get_text())
        image_path = out / filename
        fig.savefig(image_path, dpi=140)
        plt.close(fig)
        image_records.append({"file": filename, "metric": key, "sha256": hashlib.sha256(image_path.read_bytes()).hexdigest()})
    record = {"review_sha256": hashlib.sha256(review_file.read_bytes()).hexdigest(),
              "renderer_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "font": str(font_path), "rounds": "two separately drawn observations, not a confidence interval",
              "axis": "client count on log2; recall magnified with explicit nonzero lower bound",
              "images": image_records, "rows": exported}
    (out / "chart-source.json").write_text(json.dumps(record, ensure_ascii=False, indent=2))
    print("rendered 3 source-backed report figures", out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    main(args.run.resolve(), args.out.resolve())
