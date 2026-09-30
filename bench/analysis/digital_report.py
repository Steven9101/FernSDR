#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Digital modes through each receiver against the ideal receiver.

Each recording of the digital round is cut into FT8 and WSPR periods by
antenna time plus the receiver's median latency from the listen round (a
real operator's software would align to UTC and see that latency as DT;
the reported DT is kept). Scores per receiver: the FT8 threshold (the
lowest SNR decoded in at least half of the periods), FT8 decodes over the
whole ladder against the oracle's, WSPR decodes, and the share of RTTY and
CW text recovered.

  python3 bench/analysis/digital_report.py SESSION_ROUND ORACLE_JSON
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "digital"))
import digital  # noqa: E402
from listen_report import load, summarize  # noqa: E402


def scores(res):
    ft8 = res["ft8"]
    th = [r["snr"] for r in ft8 if r["periods"] and r["decoded"] / r["periods"] >= 0.5]
    return {"ft8_threshold_db": min(th) if th else None,
            "ft8_decodes": sum(r["decoded"] for r in ft8),
            "ft8_opportunities": sum(r["periods"] for r in ft8),
            "ft8_dt": [r["dt"] for r in ft8 if r["dt"] is not None],
            "wspr_decodes": sum(r["decoded"] for r in res["wspr"]),
            "wspr_opportunities": sum(r["periods"] for r in res["wspr"]),
            "wspr_lowest_db": min([r["snr"] for r in res["wspr"] if r["decoded"]], default=None),
            "rtty_score": {r["snr"]: r["score"] for r in res["rtty"]},
            "cw_score": {r["snr"]: r["score"] for r in res["cw"]}}


def main():
    root, oracle_path = sys.argv[1:3]
    oracle = scores(load(oracle_path))
    table = {"oracle": oracle}
    for rid in sorted(os.listdir(root)):
        rdir = os.path.join(root, rid)
        if not os.path.isdir(rdir):
            continue
        runs = []
        l = None
        for rep in sorted(os.listdir(rdir)):
            d = os.path.join(rdir, rep, "digital")
            if not os.path.exists(os.path.join(d, "audio.f32")):
                continue
            # The latency the same session's plain listener measured.
            lm = load(os.path.join(rdir, rep, "listen", "metrics.json"), {}) or {}
            l = lm.get("latency_median_ms")
            meta = load(os.path.join(d, "meta.json"), {}) or {}
            try:
                res = digital.decode_recording(d, (l or 0) / 1000, os.path.join(rdir, rep, "receiver", "pace.jsonl"))
                sc = scores(res)
                sc["gates_failed"] = [k for k, v in (meta.get("gates") or {}).items() if not v]
                with open(os.path.join(d, "digital.json"), "w") as f:
                    json.dump({"latency_ms_used": l, "result": res, "scores": sc}, f, indent=1)
                runs.append(sc)
            except Exception as e:
                runs.append({"error": str(e)[:300]})
        table[rid] = {
            "runs": len(runs), "latency_ms_used": l,
            "ft8_threshold_db": summarize([r.get("ft8_threshold_db") for r in runs]),
            "ft8_decode_share": summarize([r["ft8_decodes"] / r["ft8_opportunities"] for r in runs if r.get("ft8_opportunities")]),
            "wspr_decode_share": summarize([r["wspr_decodes"] / r["wspr_opportunities"] for r in runs if r.get("wspr_opportunities")]),
            "wspr_lowest_db": summarize([r.get("wspr_lowest_db") for r in runs]),
            "rtty_low": summarize([(r.get("rtty_score") or {}).get(-6) for r in runs]),
            "rtty_high": summarize([(r.get("rtty_score") or {}).get(4) for r in runs]),
            "cw_low": summarize([(r.get("cw_score") or {}).get(10) for r in runs]),
            "cw_high": summarize([(r.get("cw_score") or {}).get(20) for r in runs]),
            "errors": [r["error"] for r in runs if "error" in r],
        }
    with open(os.path.join(root, "summary-digital.json"), "w") as f:
        json.dump(table, f, indent=1)
    o = oracle
    lines = ["| receiver | FT8 threshold dB | FT8 decoded | WSPR decoded | WSPR lowest dB | RTTY -6 dB | RTTY +4 dB | CW +10 dB | CW +20 dB |",
             "|---|---|---|---|---|---|---|---|---|",
             f"| ideal receiver | {o['ft8_threshold_db']} | {o['ft8_decodes']}/{o['ft8_opportunities']} | "
             f"{o['wspr_decodes']}/{o['wspr_opportunities']} | {o['wspr_lowest_db']} | {o['rtty_score'].get(-6, 0):.2f} | "
             f"{o['rtty_score'].get(4, 0):.2f} | {o['cw_score'].get(10, 0):.2f} | {o['cw_score'].get(20, 0):.2f} |"]
    f = lambda s, p="{:.2f}": "n/a" if not s else p.format(s["median"])  # noqa: E731
    for rid, v in table.items():
        if rid == "oracle":
            continue
        lines.append(f"| {rid} | {f(v['ft8_threshold_db'], '{:.0f}')} | {f(v['ft8_decode_share'])} | {f(v['wspr_decode_share'])} | "
                     f"{f(v['wspr_lowest_db'], '{:.0f}')} | {f(v['rtty_low'])} | {f(v['rtty_high'])} | {f(v['cw_low'])} | {f(v['cw_high'])} |")
    with open(os.path.join(root, "summary-digital.md"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
