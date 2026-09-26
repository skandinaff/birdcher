#!/usr/bin/env python3
"""Local browser reviewer for Birdcher images and bird bounding boxes."""

import argparse
import csv
import json
import os
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlsplit


FIELDS = ("path", "decision", "boxes", "conditions", "notes", "reviewed_at", "method")
DECISIONS = {"bird_visible", "no_bird", "uncertain", "reject"}
CONDITIONS = {
    "tiny", "multiple", "occluded", "feeder", "low_light", "snow",
    "blur", "busy_background", "side_or_back",
}


def read_reviews(path):
    if not path.exists():
        return {}
    with path.open(newline="", encoding="utf-8") as file:
        return {row["path"]: row for row in csv.DictReader(file)}


def write_reviews(path, reviews):
    temporary = path.with_suffix(".csv.tmp")
    with temporary.open("w", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(reviews[key] for key in sorted(reviews))
        file.flush()
        os.fsync(file.fileno())
    os.replace(temporary, path)


def validate_review(data, item):
    if not isinstance(data, dict):
        raise ValueError("review must be an object")
    if data.get("decision") not in DECISIONS:
        raise ValueError("invalid decision")
    if not isinstance(data.get("boxes"), list) or not isinstance(data.get("conditions"), list):
        raise ValueError("boxes and conditions must be arrays")
    if not set(data["conditions"]) <= CONDITIONS:
        raise ValueError("invalid condition")
    boxes = []
    width, height = int(item["width"]), int(item["height"])
    for box in data["boxes"]:
        if not isinstance(box, dict):
            raise ValueError("invalid box")
        try:
            x, y, w, h = (int(box[key]) for key in ("x", "y", "w", "h"))
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError("invalid box coordinates") from exc
        if x < 0 or y < 0 or w < 2 or h < 2 or x + w > width or y + h > height:
            raise ValueError("box outside image")
        boxes.append({"x": x, "y": y, "w": w, "h": h})
    if data["decision"] == "no_bird" and boxes:
        raise ValueError("no_bird cannot have boxes")
    notes = str(data.get("notes", ""))[:1000]
    return {
        "path": item["path"], "decision": data["decision"],
        "boxes": json.dumps(boxes, separators=(",", ":")),
        "conditions": ",".join(sorted(set(data["conditions"]))),
        "notes": notes,
        "reviewed_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "method": "browser_review",
    }


def make_handler(dataset, items, reviews):
    html = (Path(__file__).with_name("review.html")).read_bytes()

    class Handler(BaseHTTPRequestHandler):
        def send_bytes(self, data, content_type, status=200):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            parsed = urlsplit(self.path)
            if parsed.path == "/":
                self.send_bytes(html, "text/html; charset=utf-8")
            elif parsed.path == "/api/items":
                category = parse_qs(parsed.query).get("category", [""])[0]
                result = [{**item, "review": reviews.get(path)} for path, item in items.items()
                          if not category or item["category"] == category]
                self.send_bytes(json.dumps(result).encode(), "application/json")
            elif parsed.path.startswith("/images/"):
                relative = unquote(parsed.path[len("/images/"):])
                if relative not in items:
                    self.send_error(404)
                    return
                image = dataset / relative
                if not image.is_file():
                    self.send_error(404)
                    return
                self.send_bytes(image.read_bytes(), "image/jpeg")
            else:
                self.send_error(404)

        def do_POST(self):
            if self.path != "/api/review":
                self.send_error(404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 100_000:
                    raise ValueError("invalid request size")
                data = json.loads(self.rfile.read(length))
                if not isinstance(data, dict):
                    raise ValueError("review must be an object")
                path = data.get("path")
                if path not in items:
                    raise ValueError("unknown image")
                row = validate_review(data, items[path])
                reviews[path] = row
                write_reviews(dataset / "review.csv", reviews)
            except (ValueError, TypeError, json.JSONDecodeError) as exc:
                self.send_bytes(json.dumps({"error": str(exc)}).encode(), "application/json", 400)
                return
            self.send_bytes(json.dumps(row).encode(), "application/json")

        def log_message(self, format, *args):
            if not self.path.startswith("/images/"):
                super().log_message(format, *args)

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, default=Path("dataset"))
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    dataset = args.dataset.expanduser().resolve()
    with (dataset / "metadata.csv").open(newline="", encoding="utf-8") as file:
        items = {row["path"]: row for row in csv.DictReader(file)}
    reviews = read_reviews(dataset / "review.csv")
    server = HTTPServer(("127.0.0.1", args.port), make_handler(dataset, items, reviews))
    print(f"Review {len(items)} images at http://127.0.0.1:{args.port}/")
    print(f"Reviews: {dataset / 'review.csv'}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
