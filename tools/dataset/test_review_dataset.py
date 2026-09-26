import json
import tempfile
import unittest
import urllib.error
import urllib.request
from http.server import HTTPServer
from pathlib import Path
from threading import Thread

from review_dataset import make_handler, read_reviews, validate_review


class ReviewDatasetTest(unittest.TestCase):
    def test_bounding_box_validation(self):
        item = {"path": "house_sparrow/1.jpg", "width": "640", "height": "480"}
        row = validate_review({"decision": "bird_visible", "boxes": [
            {"x": 10, "y": 20, "w": 100, "h": 80}], "conditions": ["tiny"]}, item)
        self.assertEqual(json.loads(row["boxes"]), [{"x": 10, "y": 20, "w": 100, "h": 80}])
        with self.assertRaises(ValueError):
            validate_review({"decision": "no_bird", "boxes": [
                {"x": 10, "y": 20, "w": 100, "h": 80}], "conditions": []}, item)
        with self.assertRaises(ValueError):
            validate_review({"decision": "bird_visible", "boxes": [
                {"x": 600, "y": 20, "w": 100, "h": 80}], "conditions": []}, item)
        with self.assertRaises(ValueError):
            validate_review([], item)

    def test_http_review_persists_and_blocks_unknown_image(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = "house_sparrow/1.jpg"
            (root / "house_sparrow").mkdir()
            (root / path).write_bytes(b"fake jpeg")
            items = {path: {"path": path, "category": "house_sparrow", "width": "640", "height": "480"}}
            server = HTTPServer(("127.0.0.1", 0), make_handler(root, items, {}))
            thread = Thread(target=server.serve_forever, daemon=True)
            thread.start()
            base = f"http://127.0.0.1:{server.server_port}"
            try:
                body = json.dumps({"path": path, "decision": "bird_visible", "boxes": [], "conditions": ["occluded"]}).encode()
                request = urllib.request.Request(base + "/api/review", data=body,
                                                 headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(request) as response:
                    self.assertEqual(response.status, 200)
                self.assertEqual(read_reviews(root / "review.csv")[path]["decision"], "bird_visible")
                with self.assertRaises(urllib.error.HTTPError) as failure:
                    urllib.request.urlopen(base + "/images/../review.csv")
                self.assertEqual(failure.exception.code, 404)
            finally:
                server.shutdown()
                server.server_close()
                thread.join()


if __name__ == "__main__":
    unittest.main()
