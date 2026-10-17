"""Offline checks for draft identity, upload integrity and retry boundaries."""

import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, call, patch

import requests

from zenodo_upload import UploadError, Zenodo, hashes, upload


class RequestTests(unittest.TestCase):
    def response(self, status):
        return Mock(status_code=status, text="server error fake-token",
                    json=Mock(return_value={"id": "23122650"}))

    def test_transient_get_recovers(self):
        for status in (500, 502, 503, 504):
            with self.subTest(status=status):
                api = Zenodo("fake-token", 23122650)
                failed = self.response(status)
                with patch.object(api.session, "request", side_effect=[
                    failed, self.response(200)
                ]) as request, patch("zenodo_upload.time.sleep") as sleep, \
                        contextlib.redirect_stderr(io.StringIO()) as output:
                    self.assertEqual(api.request("GET", api.root), {"id": "23122650"})
                self.assertEqual(request.call_count, 2)
                failed.close.assert_called_once()
                sleep.assert_called_once_with(5)
                self.assertIn("retry 2/3", output.getvalue())
                self.assertNotIn("fake-token", output.getvalue())

    def test_persistent_get_failure_is_bounded_and_redacted(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api.session, "request", side_effect=[
            self.response(500) for _ in range(3)
        ]) as request, patch("zenodo_upload.time.sleep") as sleep, \
                contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(UploadError) as error:
                api.request("GET", api.root + "/draft")
        self.assertEqual(request.call_count, 3)
        self.assertEqual(sleep.call_args_list, [call(5), call(15)])
        self.assertEqual(error.exception.status, 500)
        self.assertNotIn("fake-token", str(error.exception))

    def test_mutations_and_nontransient_errors_are_not_retried(self):
        for method, status in (("POST", 500), ("PUT", 503), ("GET", 401),
                               ("GET", 403), ("GET", 404), ("GET", 429)):
            with self.subTest(method=method, status=status):
                api = Zenodo("fake-token", 23122650)
                with patch.object(api.session, "request", return_value=self.response(status)) as request, \
                        patch("zenodo_upload.time.sleep") as sleep:
                    with self.assertRaises(UploadError):
                        api.request(method, api.root)
                request.assert_called_once()
                sleep.assert_not_called()


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "tool.zip"
        self.path.write_bytes(b"archive bytes\x00\xff")
        self.digests = hashes(self.path)
        self.files = "https://zenodo.org/api/records/23122650/draft/files"
        self.file_url = self.files + "/tool.zip"
        self.pending = {"key": "tool.zip", "status": "pending", "links": {
            "content": self.file_url + "/content", "commit": self.file_url + "/commit"}}
        self.complete = dict(self.pending, status="completed", size=self.path.stat().st_size,
                             checksum="md5:" + self.digests["md5"])
        self.output = contextlib.ExitStack()
        self.output.enter_context(contextlib.redirect_stdout(io.StringIO()))
        self.output.enter_context(contextlib.redirect_stderr(io.StringIO()))
        self.addCleanup(self.output.close)

    def run_upload(self, api):
        upload(api, self.files, self.path, "tool.zip", self.digests)

    def test_wrong_draft_identity_never_mutates(self):
        for wrong in ("id", "doi"):
            with self.subTest(wrong=wrong):
                api = Zenodo("fake-token", 23122650)
                record = {"id": "23122650", "pids": {
                    "doi": {"identifier": "10.5281/zenodo.23122650"}}}
                draft = dict(record, links={"files": self.files})
                if wrong == "id":
                    draft["id"] = "23122651"
                else:
                    draft["pids"] = {"doi": {"identifier": "10.5281/zenodo.23122651"}}
                with patch.object(api, "request", side_effect=[record, draft]) as call:
                    with self.assertRaises(UploadError):
                        api.draft_files(23122650)
                    self.assertTrue(all(c.args[0] == "GET" for c in call.call_args_list))

    def test_existing_identical_file_is_not_uploaded_again(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api, "request", return_value={"enabled": True, "entries": [self.complete]}) as call:
            self.run_upload(api)
            self.assertEqual(call.call_count, 1)

    def test_different_completed_file_is_not_overwritten(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api, "request", return_value={"enabled": True, "entries": [dict(self.complete, checksum="md5:other")]}) as call:
            with self.assertRaises(UploadError):
                self.run_upload(api)
            self.assertEqual(call.call_count, 1)

    def test_full_bytes_retried_and_committed_without_publication(self):
        api = Zenodo("fake-token", 23122650)
        transmissions = []

        def request(method, url, **kwargs):
            self.assertFalse("publish" in url or "versions" in url or method == "DELETE")
            if url == self.files:
                return {"enabled": True, "entries": [] if method == "GET" else [self.pending]}
            if method == "PUT":
                transmissions.append(kwargs["data"].read())
                if len(transmissions) == 1:
                    raise requests.ConnectionError("Connection lost after partial transfer")
                return self.pending
            if method == "POST":
                return self.complete
            return self.pending if len(transmissions) == 1 else self.complete

        with patch.object(api, "request", side_effect=request), patch("zenodo_upload.time.sleep"):
            self.run_upload(api)
        self.assertEqual(transmissions, [self.path.read_bytes()] * 2)

    def test_bad_remote_checksum_fails(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api, "request", side_effect=[
            {"enabled": True, "entries": [self.pending]}, self.pending,
            self.complete, dict(self.complete, checksum="md5:corrupted")]):
            with self.assertRaisesRegex(UploadError, "verification failed"):
                self.run_upload(api)

    def test_foreign_upload_host_receives_no_token(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api.session, "request") as call:
            with self.assertRaises(UploadError):
                api.request("PUT", "https://other.example/file")
            call.assert_not_called()


if __name__ == "__main__":
    unittest.main()
