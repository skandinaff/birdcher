#!/usr/bin/env python3
"""Download a small, attributable iNaturalist evaluation set for Birdcher."""

import argparse
import csv
import hashlib
import io
import json
import logging
import math
import os
import random
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image, ImageOps, UnidentifiedImageError


API = "https://api.inaturalist.org/v1"
LICENSES = {"cc0", "cc-by", "cc-by-sa"}
SPECIES = {
    "house_sparrow": "Passer domesticus",
    "tree_sparrow": "Passer montanus",
    "great_tit": "Parus major",
    "blue_tit": "Cyanistes caeruleus",
    "coal_tit": "Periparus ater",
    "robin": "Erithacus rubecula",
    "chaffinch": "Fringilla coelebs",
    "goldfinch": "Carduelis carduelis",
    "siskin": "Spinus spinus",
    "greenfinch": "Chloris chloris",
    "bullfinch": "Pyrrhula pyrrhula",
    "nuthatch": "Sitta europaea",
}
COMMON_NAMES = {
    "house_sparrow": "House Sparrow", "tree_sparrow": "Eurasian Tree Sparrow",
    "great_tit": "Great Tit", "blue_tit": "Eurasian Blue Tit",
    "coal_tit": "Coal Tit", "robin": "European Robin",
    "chaffinch": "Common Chaffinch", "goldfinch": "European Goldfinch",
    "siskin": "Eurasian Siskin", "greenfinch": "European Greenfinch",
    "bullfinch": "Eurasian Bullfinch", "nuthatch": "Eurasian Nuthatch",
}
REGIONS = {
    "baltics": ("Latvia", "Lithuania", "Estonia", "Poland"),
    "northeast-europe": (
        "Latvia", "Lithuania", "Estonia", "Poland", "Finland", "Sweden",
        "Germany", "Czech Republic", "Slovakia",
    ),
}
FIELDS = (
    "path", "category", "species", "scientific_name", "observation_id",
    "photo_id", "observation_url", "image_url", "license", "attribution",
    "observer", "place", "country", "observed_on", "width", "height",
    "sha256", "label_status",
)
LOG = logging.getLogger("birdcher.dataset")


class Requester:
    def __init__(self, delay, retries, timeout):
        self.delay = delay
        self.retries = retries
        self.timeout = timeout
        self.last_request = 0.0

    def get(self, url, limit=20_000_000):
        for attempt in range(self.retries + 1):
            pause = self.delay - (time.monotonic() - self.last_request)
            if pause > 0:
                time.sleep(pause)
            self.last_request = time.monotonic()
            try:
                request = urllib.request.Request(
                    url, headers={"User-Agent": "Birdcher-dataset/1.0 (research evaluation)"}
                )
                with urllib.request.urlopen(request, timeout=self.timeout) as response:
                    data = response.read(limit + 1)
                    if len(data) > limit:
                        raise ValueError(f"response too large: {url}")
                    return data
            except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as exc:
                if isinstance(exc, urllib.error.HTTPError) and exc.code not in (429, 500, 502, 503, 504):
                    raise
                if attempt == self.retries:
                    raise
                retry_after = exc.headers.get("Retry-After") if isinstance(exc, urllib.error.HTTPError) else None
                wait = min(60.0, max(float(retry_after), 2 ** attempt)) if retry_after and retry_after.isdigit() else min(60.0, 2 ** attempt)
                LOG.warning("request failed (%s); retry %d/%d in %.1fs", exc, attempt + 1, self.retries, wait)
                time.sleep(wait)

    def api(self, endpoint, params):
        url = API + endpoint + "?" + urllib.parse.urlencode(params)
        return json.loads(self.get(url).decode("utf-8"))


def resolve_places(client, names):
    places = []
    for name in names:
        data = client.api("/places/autocomplete", {"q": name})
        # The v1 autocomplete response exposes the country type as numeric 12;
        # place_type_name is absent in current responses.
        matches = [p for p in data.get("results", [])
                   if p.get("name", "").casefold() == name.casefold()
                   and p.get("place_type") == 12]
        if len(matches) != 1:
            raise RuntimeError(f"cannot uniquely resolve country {name!r}: {matches}")
        places.append((name, matches[0]["id"]))
    return places


def photo_url(photo):
    url = photo.get("url") or ""
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or not parsed.hostname or not (
        parsed.hostname == "inaturalist-open-data.s3.amazonaws.com"
        or parsed.hostname == "static.inaturalist.org"
        or parsed.hostname.endswith(".inaturalist.org")
    ):
        return None
    path = re.sub(r"/(square|thumb|small|medium|original|large)\.", "/large.", parsed.path)
    if path == parsed.path and "/large." not in path:
        return None
    return urllib.parse.urlunparse(parsed._replace(path=path))


def image_jpeg(raw, max_size):
    with Image.open(io.BytesIO(raw)) as image:
        image = ImageOps.exif_transpose(image)
        image.thumbnail((max_size, max_size), Image.Resampling.LANCZOS)
        if image.width < 64 or image.height < 64:
            raise ValueError("image too small")
        image = image.convert("RGB")
        gray = image.convert("L").resize((9, 8), Image.Resampling.LANCZOS)
        pixels = list(gray.getdata())
        dhash = sum((pixels[y * 9 + x] > pixels[y * 9 + x + 1]) << (y * 8 + x)
                    for y in range(8) for x in range(8))
        buffer = io.BytesIO()
        image.save(buffer, format="JPEG", quality=88, optimize=True)
        return buffer.getvalue(), image.width, image.height, dhash


def existing_rows(output):
    path = output / "metadata.csv"
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as file:
        rows = list(csv.DictReader(file))
    valid = [row for row in rows if (output / row.get("path", "")).is_file()]
    if len(valid) != len(rows):
        LOG.warning("removed %d metadata rows with missing files", len(rows) - len(valid))
        temp = path.with_suffix(".csv.tmp")
        with temp.open("w", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=FIELDS)
            writer.writeheader()
            writer.writerows(valid)
        os.replace(temp, path)
    return valid


def candidates(client, params, rng, max_pages):
    query = {**params, "per_page": 200, "page": 1, "order_by": "observed_on", "order": "desc"}
    first = client.api("/observations", query)
    total = min(math.ceil(first.get("total_results", 0) / 200), 50)
    if not total:
        return
    pages = list(range(1, total + 1))
    rng.shuffle(pages)
    for page in pages[:max_pages]:
        data = first if page == 1 else client.api("/observations", {**query, "page": page})
        observations = data.get("results", [])
        rng.shuffle(observations)
        yield from observations


def save_candidate(obs, category, expected_name, country, output, client, max_size,
                   seen_observations, seen_photos, hashes, dhashes, writer, file, rng):
    obs_id = obs.get("id")
    taxon = obs.get("taxon") or {}
    name = taxon.get("name", "")
    if not obs_id or obs_id in seen_observations or not name:
        return False
    if category in SPECIES and name != expected_name:
        return False
    if category == "other_birds" and (
        taxon.get("rank") != "species" or
        name in SPECIES.values() or
        taxon.get("iconic_taxon_name") != "Aves"
    ):
        return False
    if category == "no_bird" and taxon.get("iconic_taxon_name") != "Plantae":
        return False
    photos = list(obs.get("photos") or [])
    rng.shuffle(photos)
    for photo in photos:
        photo_id = photo.get("id")
        license_code = str(photo.get("license_code") or "").lower()
        url = photo_url(photo)
        if not photo_id or photo_id in seen_photos or license_code not in LICENSES or not url:
            continue
        try:
            image, width, height, dhash = image_jpeg(client.get(url), max_size)
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError,
                OSError, ValueError, UnidentifiedImageError) as exc:
            LOG.warning("skip photo %s: %s", photo_id, exc)
            continue
        digest = hashlib.sha256(image).hexdigest()
        if digest in hashes or any((dhash ^ previous).bit_count() <= 3 for previous in dhashes):
            LOG.debug("duplicate photo %s", photo_id)
            continue
        relative = Path(category) / f"{obs_id}-{photo_id}.jpg"
        target = output / relative
        temporary = target.with_suffix(".jpg.tmp")
        temporary.write_bytes(image)
        os.replace(temporary, target)
        observer = obs.get("user") or {}
        credit = photo.get("attribution") or observer.get("name") or observer.get("login") or "unknown photographer"
        row = {
            "path": str(relative), "category": category,
            "species": COMMON_NAMES[category] if category in COMMON_NAMES else
                       taxon.get("preferred_common_name", "") if category == "other_birds" else "",
            "scientific_name": name, "observation_id": obs_id,
            "photo_id": photo_id,
            "observation_url": f"https://www.inaturalist.org/observations/{obs_id}",
            "image_url": url, "license": license_code.upper(),
            "attribution": credit, "observer": observer.get("login", ""),
            "place": obs.get("place_guess") or "", "country": country,
            "observed_on": obs.get("observed_on") or "",
            "width": width, "height": height, "sha256": digest,
            "label_status": "needs_review",
        }
        writer.writerow(row)
        file.flush()
        seen_observations.add(obs_id)
        seen_photos.add(photo_id)
        hashes.add(digest)
        dhashes.append(dhash)
        LOG.info("%s: %s (%s, %s)", category, relative, country, license_code)
        return True
    return False


def run(args):
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    categories = {**SPECIES, "other_birds": "Aves", "no_bird": "Plantae"}
    for category in categories:
        (output / category).mkdir(exist_ok=True)
    rows = existing_rows(output)
    counts = {category: 0 for category in categories}
    seen_observations = set()
    seen_photos = set()
    hashes = set()
    dhashes = []
    for row in rows:
        counts[row["category"]] = counts.get(row["category"], 0) + 1
        seen_observations.add(int(row["observation_id"]))
        seen_photos.add(int(row["photo_id"]))
        hashes.add(row.get("sha256", ""))
        try:
            with Image.open(output / row["path"]) as image:
                gray = image.convert("L").resize((9, 8), Image.Resampling.LANCZOS)
                pixels = list(gray.getdata())
                dhashes.append(sum((pixels[y * 9 + x] > pixels[y * 9 + x + 1]) << (y * 8 + x)
                                    for y in range(8) for x in range(8)))
        except OSError:
            pass
    rng = random.Random(args.seed)
    client = Requester(args.delay, args.retries, args.timeout)
    places = resolve_places(client, REGIONS[args.region])
    path = output / "metadata.csv"
    with path.open("a", newline="", encoding="utf-8") as file:
        writer = csv.DictWriter(file, fieldnames=FIELDS)
        if not rows and path.stat().st_size == 0:
            writer.writeheader()
            file.flush()
        for category, scientific_name in categories.items():
            target_count = args.per_species if category in SPECIES else args.other_birds if category == "other_birds" else args.no_bird
            if counts[category] >= target_count:
                continue
            country_order = list(places)
            rng.shuffle(country_order)
            # Round-robin keeps Poland's larger iNaturalist pool from dominating.
            pools = []
            for country, place_id in country_order:
                params = {
                    "place_id": place_id,
                    "photos": "true", "quality_grade": "research",
                    "photo_license": ",".join(sorted(LICENSES)),
                }
                if category == "other_birds":
                    params["taxon_id"] = 3  # Aves
                elif category == "no_bird":
                    params["taxon_id"] = 47126  # Plantae
                else:
                    params["taxon_name"] = scientific_name
                pools.append((country, iter(candidates(client, params, rng, args.max_pages))))
            while pools and counts[category] < target_count:
                remaining = []
                for country, pool in pools:
                    if counts[category] >= target_count:
                        break
                    try:
                        obs = next(pool)
                    except StopIteration:
                        continue
                    remaining.append((country, pool))
                    if save_candidate(obs, category, scientific_name, country, output,
                                      client, args.max_size, seen_observations,
                                      seen_photos, hashes, dhashes, writer, file, rng):
                        counts[category] += 1
                pools = remaining
            LOG.info("%s: %d/%d images", category, counts[category], target_count)
            if counts[category] < target_count:
                LOG.warning("%s under target; try northeast-europe or more pages", category)
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("dataset"))
    parser.add_argument("--per-species", type=int, default=80)
    parser.add_argument("--other-birds", type=int, default=80)
    parser.add_argument("--no-bird", type=int, default=80)
    parser.add_argument("--max-size", type=int, default=1024)
    parser.add_argument("--region", choices=REGIONS, default="baltics")
    parser.add_argument("--seed", type=int, default=20260926)
    parser.add_argument("--max-pages", type=int, default=8,
                        help="maximum 200-observation pages per country/category")
    parser.add_argument("--delay", type=float, default=1.1,
                        help="minimum seconds between requests")
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=25)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    if min(args.per_species, args.other_birds, args.no_bird) < 0 or args.max_size < 64 or not 1 <= args.max_pages <= 50 or args.delay < 1 or args.retries < 0:
        parser.error("counts >= 0, max-size >= 64, max-pages 1..50, delay >= 1, retries >= 0 required")
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    try:
        counts = run(args)
    except KeyboardInterrupt:
        LOG.warning("interrupted; rerun with the same --output to resume")
        return 130
    except (OSError, ValueError, RuntimeError, urllib.error.URLError, json.JSONDecodeError) as exc:
        LOG.error("download stopped: %s", exc)
        return 1
    LOG.info("done: %s", counts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
