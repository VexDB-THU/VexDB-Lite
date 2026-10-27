"""Keep primary and separately registered quality verdicts distinct."""
import argparse
import json
from pathlib import Path

from run_pq_graph import audit, save, sha, verify
from validate_pq_graph_quality import check_selection


def main(run):
    protocol = verify(run)
    quality = check_selection(run)
    primary = json.loads((run / "selection.json").read_text())
    verdicts = {}
    total = 0
    for phase, choice in (("validation", primary), ("quality-validation", quality)):
        for path, digest in json.loads((run / (phase + "-hashes.json")).read_text()).items():
            if sha(run / path) != digest:
                raise RuntimeError("result hash mismatch: " + path)
        checked = audit(run, protocol, phase)
        records = []
        for name, points in checked.items():
            case = next(c for c in protocol["cases"] if c["name"] == name.split("-round")[0])
            total += sum(p["calls"] for p in points.values())
            for connections in (1, 8):
                candidate = points[f"graph-ef{choice['ef']}-r{choice['factor']}-c{connections}"]
                control = points[f"full-r{choice['factor']}-c{connections}"]
                gates = {
                    "recall_at_least_95": candidate["hits"] >= 950,
                    "loss_at_most_0_2pp": candidate["hits"] >= control["hits"] - 2,
                    "code_evaluations_at_most_half": candidate["mean_codes"] <= case["rows"] / 2,
                    "p95_at_least_10pct_lower": candidate["p95_ms"] <= control["p95_ms"] * .9,
                }
                records.append({"case_round": name, "primary_case": case["primary"], "connections": connections,
                                "gates": gates, "passed": all(gates.values()),
                                "recall_loss_pp": (control["hits"] - candidate["hits"]) / 10,
                                "code_evaluations_reduced_pct": 100 * (1 - candidate["mean_codes"] / case["rows"]),
                                "p95_reduced_pct": 100 * (1 - candidate["p95_ms"] / control["p95_ms"]),
                                "candidate": candidate, "control": control})
        verdicts[phase] = {"primary_cases_passed": all(r["passed"] for r in records if r["primary_case"]),
                           "all_cases_passed": all(r["passed"] for r in records), "records": records}
    fit = audit(run, protocol, "fit")
    total += sum(p["calls"] for points in fit.values() for p in points.values())
    result = {"scope": "existing dev queries; macOS ARM64 DuckDB 1.5.2; one graph seed; excludes fresh overnight holdouts",
              "sql_top10_lists_recomputed": total, "verdicts": verdicts,
              "primary_selection_sha256": sha(run / "selection.json"),
              "quality_selection_sha256": sha(run / "quality-selection.json"),
              "protocol_sha256": sha(run / "protocol.json"),
              "model_parity": all(r["equal"] for r in json.loads((run / "model-parity.json").read_text()))}
    dest = run / "final-review.json"
    if dest.exists():
        if json.loads(dest.read_text()) != result:
            raise RuntimeError("final review disagrees with saved verdict")
    else:
        save(dest, result)
    for phase, verdict in verdicts.items():
        print(phase, "primary datasets:", verdict["primary_cases_passed"], "all sizes:", verdict["all_cases_passed"])
        for r in verdict["records"]:
            if not r["passed"]:
                print("FAIL", r["case_round"], "c=" + str(r["connections"]), [k for k, v in r["gates"].items() if not v])
    print("independent raw-ID review complete:", total, "SQL lists")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    main(parser.parse_args().run.resolve())
