#!/usr/bin/env python3
"""Quality benchmark and contact sheet generator for vision enrichment.

Extracts a stratified sample of assets across categories and sizes, runs
enrichment via Ollama, evaluates response validity, echoes, and produces
an HTML contact sheet for human verification.
"""
import argparse
import html
import json
import os
import random
import time
from pathlib import Path
from typing import Dict, List, Optional

from paths import ROOT as PROJECT_ROOT
from dungeondraft_db import AssetDatabase, DB_PATH_DEFAULT
from dungeondraft_enrich import DungeondraftEnricher, clean_object_kind, is_echoing_filename, DEFAULT_CATALOGUER_MODEL


def generate_contact_sheet(results: List[dict], out_html_path: str):
    """Generate a clean HTML contact sheet displaying thumbnails and enriched metadata."""
    html_lines = [
        "<!DOCTYPE html>",
        "<html><head><meta charset='utf-8'>",
        "<title>Dungeondraft Asset Enrichment Quality Sample</title>",
        "<style>",
        "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; background: #1a1a1a; color: #eee; padding: 20px; }",
        "  h1 { color: #fff; }",
        "  .stats { background: #2a2a2a; padding: 15px; border-radius: 8px; margin-bottom: 20px; }",
        "  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(280px, 1fr)); gap: 16px; }",
        "  .card { background: #252525; border: 1px solid #3a3a3a; border-radius: 6px; padding: 12px; display: flex; flex-direction: column; align-items: center; }",
        "  .card img { width: 180px; height: 180px; object-fit: contain; background: #808080; border-radius: 4px; }",
        "  .meta { margin-top: 10px; width: 100%; font-size: 12px; }",
        "  .kind { font-weight: bold; color: #4fc3f7; font-size: 14px; margin-bottom: 4px; }",
        "  .desc { color: #ddd; font-style: italic; margin-bottom: 6px; }",
        "  .tags { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 4px; }",
        "  .tag { background: #333; padding: 2px 6px; border-radius: 3px; font-size: 10px; color: #bbb; }",
        "  .footprint { font-weight: bold; padding: 2px 6px; border-radius: 3px; font-size: 10px; }",
        "  .footprint-floor { background: #2e7d32; color: #fff; }",
        "  .footprint-wall-mounted { background: #d32f2f; color: #fff; }",
        "  .footprint-ceiling { background: #f57c00; color: #fff; }",
        "  .footprint-overhang { background: #7b1fa2; color: #fff; }",
        "</style></head><body>",
        "<h1>Dungeondraft Asset Enrichment — Quality Check</h1>",
    ]

    total = len(results)
    echoes = sum(1 for r in results if r.get("echoed", False))
    avg_conf = sum(r.get("confidence", 0.0) for r in results) / max(1, total)

    html_lines.append(f"<div class='stats'>")
    html_lines.append(f"  <strong>Total Assets Sampled:</strong> {total} | ")
    html_lines.append(f"  <strong>Filename Echoes:</strong> {echoes} ({echoes/max(1, total)*100:.1f}%) | ")
    html_lines.append(f"  <strong>Mean Confidence:</strong> {avg_conf:.2f}")
    html_lines.append(f"</div>")

    html_lines.append("<div class='grid'>")
    for r in results:
        thumb_abs = r["thumb_abs"]
        kind = html.escape(r.get("object_kind", "unclear"))
        desc = html.escape(r.get("description", ""))
        fn = html.escape(r.get("file_name", ""))
        fp = r.get("footprint", "floor")
        conf = r.get("confidence", 0.0)

        sem_tags = r.get("semantic_tags", [])
        style_tags = r.get("style_tags", [])

        tag_spans = "".join(f"<span class='tag'>{html.escape(t)}</span>" for t in sem_tags + style_tags)

        html_lines.append("<div class='card'>")
        html_lines.append(f"  <img src='file:///{thumb_abs.replace(os.sep, '/')}' alt='{fn}'>")
        html_lines.append("  <div class='meta'>")
        html_lines.append(f"    <div class='kind'>{kind}</div>")
        html_lines.append(f"    <div class='desc'>\"{desc}\"</div>")
        html_lines.append(f"    <div><strong>File:</strong> {fn}</div>")
        html_lines.append(f"    <div><strong>Footprint:</strong> <span class='footprint footprint-{fp}'>{fp}</span> | <strong>Conf:</strong> {conf:.2f}</div>")
        if tag_spans:
            html_lines.append(f"    <div class='tags'>{tag_spans}</div>")
        html_lines.append("  </div>")
        html_lines.append("</div>")

    html_lines.append("</div></body></html>")

    out_p = Path(out_html_path)
    out_p.parent.mkdir(parents=True, exist_ok=True)
    out_p.write_text("\n".join(html_lines), encoding="utf-8")
    print(f"\nContact sheet saved to: {out_p.resolve()}")


def print_quality_summary(results: List[dict], attempted: int, model: str) -> None:
    """Say in numbers whether this model is worth running on the whole library.

    The contact sheet is for the eye; this is the part that can be compared
    between two models, and the part that says stop. A cached bad description is
    worse than no description, so the decision is taken before the long pass,
    not after it.
    """
    answered = len(results)
    if answered == 0:
        print("\nNo asset was catalogued. Nothing to judge.")
        return

    echoed = sum(1 for r in results if r["echoed"])
    unclear = sum(1 for r in results if r["object_kind"] == "unclear")
    low_conf = sum(1 for r in results if r["confidence"] < 0.5)
    no_desc = sum(1 for r in results if len(r["description"].split()) < 4)
    mean_conf = sum(r["confidence"] for r in results) / answered

    footprints: Dict[str, int] = {}
    for r in results:
        footprints[r["footprint"]] = footprints.get(r["footprint"], 0) + 1

    print(f"\nQuality of '{model}' on {attempted} sampled assets:")
    print(f"  answered                 : {answered}/{attempted}")
    print(f"  echoed the file name     : {echoed} ({100.0 * echoed / answered:.1f} %)")
    print(f"  said 'unclear'           : {unclear} ({100.0 * unclear / answered:.1f} %)")
    print(f"  description under 4 words: {no_desc}")
    print(f"  confidence below 0.5     : {low_conf}")
    print(f"  mean confidence          : {mean_conf:.2f}")
    print("  footprint                : "
          + ", ".join(f"{k} {v}" for k, v in sorted(footprints.items(), key=lambda kv: -kv[1])))

    # An echo is the failure that matters: the model read the file name back
    # instead of looking at the picture, and it looks like a real answer.
    if echoed > answered * 0.25:
        print("\nMore than a quarter of these are the file name reworded. This model is "
              "not looking at the pictures - try another one before cataloguing the library.")
    elif answered < attempted * 0.9:
        print("\nToo many assets failed outright. Check Ollama before starting a long pass.")
    else:
        print("\nGood enough to run at scale. Open the contact sheet and read twenty of "
              "them yourself before you commit to it.")


def run_benchmark(sample_size: int = 200, model: str = DEFAULT_CATALOGUER_MODEL, out_html: str = "output/enrichment_sample.html"):
    enricher = DungeondraftEnricher(model=model)
    try:
        if not enricher.verify_model():
            print(f"Error: model {model} not ready.")
            return

        cur = enricher.db.conn.cursor()
        # Stratified selection across categories
        cur.execute("SELECT DISTINCT category FROM assets WHERE state = 'ok';")
        cats = [row[0] for row in cur.fetchall()]

        sample_rows = []
        per_cat = max(5, sample_size // max(1, len(cats)))

        for cat in cats:
            cur.execute("""
            SELECT a.content_hash, a.thumb_path, a.file_name, a.category, a.subpath,
                   a.width, a.height, a.grid_w, a.grid_h, a.pack_tags, p.name as pack_name
            FROM assets a
            JOIN packs p ON a.pack_id = p.id
            WHERE a.state = 'ok' AND a.category = ?
            ORDER BY RANDOM()
            LIMIT ?
            """, (cat, per_cat))
            sample_rows.extend(cur.fetchall())

        random.shuffle(sample_rows)
        sample_rows = sample_rows[:sample_size]

        print(f"Running quality benchmark on {len(sample_rows)} stratified sample assets with '{model}'...", flush=True)
        results = []

        for i, row in enumerate(sample_rows, 1):
            c_hash = row["content_hash"]
            thumb_rel = row["thumb_path"]
            thumb_abs = enricher.db.thumbs_dir / thumb_rel

            p_tags = []
            try:
                p_tags = json.loads(row["pack_tags"] or "[]")
            except Exception:
                pass

            data = enricher.enrich_single(
                thumb_path=thumb_abs,
                file_name=row["file_name"],
                category=row["category"],
                subpath=row["subpath"] or "",
                width=row["width"],
                height=row["height"],
                grid_w=row["grid_w"],
                grid_h=row["grid_h"],
                pack_name=row["pack_name"] or "",
                pack_tags=p_tags,
            )

            if not data:
                continue

            echoed = is_echoing_filename(data.get("object_kind", ""), row["file_name"])

            res_entry = {
                "file_name": row["file_name"],
                "category": row["category"],
                "thumb_abs": str(thumb_abs),
                "object_kind": data.get("object_kind", "unclear"),
                "description": data.get("description", ""),
                "semantic_tags": data.get("semantic_tags", []),
                "style_tags": data.get("style_tags", []),
                "footprint": data.get("footprint", "floor"),
                "confidence": float(data.get("confidence", 0.5)),
                "echoed": echoed,
            }
            results.append(res_entry)

            print(f"  [{i}/{len(sample_rows)}] {row['file_name']} -> {res_entry['object_kind']} (conf {res_entry['confidence']:.2f}, {res_entry['footprint']})", flush=True)

        generate_contact_sheet(results, out_html)
        print_quality_summary(results, len(sample_rows), model)
        return results

    finally:
        enricher.close()


def main():
    parser = argparse.ArgumentParser(description="Quality benchmark for vision enrichment")
    parser.add_argument("--sample", type=int, default=200, help="Number of sample assets to evaluate")
    parser.add_argument("--model", type=str, default=DEFAULT_CATALOGUER_MODEL, help="Model to use")
    parser.add_argument("--out", type=str, default="output/enrichment_sample.html", help="HTML contact sheet path")
    args = parser.parse_args()

    run_benchmark(sample_size=args.sample, model=args.model, out_html=args.out)


if __name__ == "__main__":
    main()
