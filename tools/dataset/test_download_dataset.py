import csv
import io
import random
import tempfile
import unittest
from pathlib import Path

from PIL import Image

import download_dataset as dataset


class FakeClient:
    def __init__(self, image):
        self.image = image
        self.downloads = 0

    def get(self, url):
        self.downloads += 1
        return self.image


class DownloadDatasetTest(unittest.TestCase):
    def test_license_resize_deduplicate_and_resume_metadata(self):
        image = Image.new("RGB", (1600, 800), "#456789")
        raw = io.BytesIO()
        image.save(raw, format="JPEG")
        client = FakeClient(raw.getvalue())
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            (output / "house_sparrow").mkdir()
            metadata = output / "metadata.csv"
            with metadata.open("w", newline="", encoding="utf-8") as file:
                writer = csv.DictWriter(file, fieldnames=dataset.FIELDS)
                writer.writeheader()
                common = (set(), set(), set(), [])
                observation = {
                    "id": 12, "taxon": {"name": "Passer domesticus"},
                    "photos": [
                        {"id": 100, "license_code": "cc-by-nc", "url": "https://static.inaturalist.org/photos/100/square.jpg"},
                        {"id": 101, "license_code": "cc-by", "url": "https://static.inaturalist.org/photos/101/square.jpg", "attribution": "© Example"},
                    ],
                }
                saved = dataset.save_candidate(
                    observation, "house_sparrow", "Passer domesticus", "Latvia",
                    output, client, 640, *common, writer, file, random.Random(2)
                )
                self.assertTrue(saved)
                self.assertEqual(client.downloads, 1)
                self.assertFalse(dataset.save_candidate(
                    observation, "house_sparrow", "Passer domesticus", "Latvia",
                    output, client, 640, *common, writer, file, random.Random(2)
                ))
            rows = dataset.existing_rows(output)
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["license"], "CC-BY")
            self.assertEqual(rows[0]["photo_id"], "101")
            self.assertEqual(rows[0]["attribution"], "© Example")
            with Image.open(output / rows[0]["path"]) as saved_image:
                self.assertEqual(saved_image.size, (640, 320))

    def test_taxon_guard_and_untrusted_url(self):
        self.assertIsNone(dataset.photo_url({"url": "https://example.org/photos/1/square.jpg"}))
        self.assertEqual(
            dataset.photo_url({"url": "https://static.inaturalist.org/photos/1/square.jpg"}),
            "https://static.inaturalist.org/photos/1/large.jpg",
        )


if __name__ == "__main__":
    unittest.main()
