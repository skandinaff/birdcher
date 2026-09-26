# Birdcher test dataset downloader

This tool collects a **small evaluation set**, not a training corpus, using
[iNaturalist API v1](https://api.inaturalist.org/v1/docs/) observations and
their photos. It targets 12 common bird species in Latvia, Lithuania, Estonia
and Poland. `--region northeast-europe` also searches Finland, Sweden,
Germany, Czech Republic and Slovakia.

From the repository root:

```sh
python3 -m venv /tmp/birdcher-dataset-venv
/tmp/birdcher-dataset-venv/bin/pip install -r tools/dataset/requirements.txt
/tmp/birdcher-dataset-venv/bin/python tools/dataset/download_dataset.py \
    --output dataset --per-species 80 --max-size 1024 --region baltics
```

The default also requests 80 `other_birds` and 80 `no_bird` candidates. For a
small smoke run, add `--per-species 2 --other-birds 2 --no-bird 2`. The command
can be rerun with the same output directory after interruption; existing
images and rows are retained. `--seed` controls page and photo shuffling, and
`--max-pages` controls how far each country/category search goes. A warning
reports any category that remains below target. The API changes over time, so
a seed does not freeze the remote source; `metadata.csv` records the actual
observations and photo URLs selected.

```text
dataset/
├── house_sparrow/ ... nuthatch/
├── other_birds/
├── no_bird/
├── metadata.csv
├── review.csv
└── rejections.csv
```

The downloader requests research-grade observations with photos, then checks
**each photo's own license**. Only CC0, CC-BY and CC-BY-SA are saved. The API's
`large` rendition is at most 1024 px on the long side; Pillow applies
`--max-size` and writes JPEG. Metadata includes scientific name, observation
and photo IDs, source URLs, photo license, photographer attribution, observed
date, country and place text when available, size and SHA-256. One photo per
observation is used. Exact hashes and a conservative perceptual hash suppress
obvious duplicates. A palette check excludes common purple audio spectrograms
stored as observation photos. The script makes requests no faster than once per second,
retries transient failures, and stays within the API's 10,000-result window.

The local set has been screened for obvious spectrograms; removed rows and
reasons are kept in `rejections.csv`. The downloader skips those observations
when resuming. This automated screen can miss other non-photographic images,
so it does not replace visual review.

To review images and draw bird boxes locally, run:

```sh
python3 tools/dataset/review_dataset.py --dataset dataset
```

Open `http://127.0.0.1:8765/` on the **same computer**. Pick a category,
inspect the full image, drag a box around each visible bird if localization
will be tested, select scene conditions, and choose a decision. `B`, `N`,
`U`, and `R` are keyboard shortcuts; arrow keys change images. Decisions are
saved immediately to `dataset/review.csv`, so review can be resumed. A
`bird_visible` decision with empty boxes confirms presence only; it is not a
localization annotation. Use `uncertain` when the bird or species cannot be
verified and `reject` for unsuitable images. The browser does not modify
`metadata.csv` or delete files.

`other_birds` comes from observations of Aves that are not one of the 12
target species. `no_bird` comes from plant observations, **which can still
contain a bird in the background**. Every row is therefore marked
`needs_review`. Before using these as ground truth, inspect the images and
remove mislabeled, highly cropped, duplicate or unsuitable photos. In
particular, iNaturalist has no reliable API field for bird size in the frame,
occlusion, feeder, blur or snow. A useful Birdcher test set needs a manual pass
for those conditions and bounding boxes if detector localization is measured.
Split by observation or photographer, not random image, to limit leakage.
For realistic negatives and tiny birds, add separately agreed IMX415 frames
from the intended scene after reviewing this initial set.

The output directory `dataset/` is ignored by Git. Keep `metadata.csv` with
any images you share: the photo license applies to the image, and CC-BY and
CC-BY-SA require attribution. The [iNaturalist Open Data guide](https://github.com/inaturalist/inaturalist-open-data/blob/main/README.md)
documents the `large` rendition and the need to respect each photographer's
license. The [API's request guidelines](https://www.inaturalist.org/pages/api+recommended+practices)
recommend roughly one request per second and warn against bulk media pulls.
