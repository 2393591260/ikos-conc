"""Offline checks for draft identity, upload integrity and retry boundaries."""

import contextlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import io
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import Mock, call, patch

import requests

from zenodo_upload import ProgressReader, UploadError, Zenodo, finish_upload, hashes, upload


class ProgressTests(unittest.TestCase):
    def test_actual_http_body_and_length_match_with_or_without_progress(self):
        payload = bytes(range(256)) * 1024
        received = []

        class Handler(BaseHTTPRequestHandler):
            def do_PUT(self):
                length = self.headers.get("Content-Length")
                received.append((length, self.headers.get("Transfer-Encoding"),
                                 self.rfile.read(int(length))))
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"{}")

            def log_message(self, *args):
                pass

        with HTTPServer(("127.0.0.1", 0), Handler) as server, requests.Session() as session:
            session.trust_env = False
            thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.05},
                                      daemon=True)
            thread.start()
            try:
                for progress in (False, True):
                    with self.subTest(progress=progress), tempfile.TemporaryFile() as stream, \
                            contextlib.redirect_stderr(io.StringIO()) as output:
                        stream.write(payload)
                        stream.seek(0)
                        reader = ProgressReader(stream, len(payload)) if progress else stream
                        response = session.put(
                            f"http://127.0.0.1:{server.server_port}/upload", data=reader,
                            headers={"Content-Type": "application/octet-stream"}, timeout=(2, 3))
                        self.assertEqual(response.status_code, 200)
                        if progress:
                            reader.finish()
                            self.assertIn("100.0%", output.getvalue())
                            self.assertIn("not yet verified", output.getvalue())
                        else:
                            self.assertEqual(output.getvalue(), "")
            finally:
                server.shutdown()
                thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
        self.assertEqual(received, [(str(len(payload)), None, payload)] * 2)

    def test_interrupted_read_does_not_report_completion(self):
        with contextlib.redirect_stderr(io.StringIO()) as output, \
                patch("zenodo_upload.time.monotonic", return_value=10):
            reader = ProgressReader(io.BytesIO(b"abcdefgh"), 8)
            self.assertEqual(reader.read(2), b"ab")
            reader.finish()
        text = output.getvalue()
        self.assertIn("0.0%", text)
        self.assertIn("25.0%", text)
        self.assertIn("2/8 B", text)
        self.assertNotIn("100.0%", text)
        self.assertNotIn("File read complete", text)


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

    def test_metadata_requests_native_rdm_json_only(self):
        api = Zenodo("fake-token", 23122650)
        record = {"id": "23122650", "pids": {
            "doi": {"identifier": "10.5281/zenodo.23122650"}}}
        draft = dict(record, links={"files": self.files})
        responses = [Mock(status_code=200, json=Mock(return_value=value))
                     for value in (record, draft, {"enabled": True, "entries": []})]
        with patch.object(api.session, "request", side_effect=responses) as request:
            self.assertEqual(api.draft_files(23122650), self.files)
            api.request("GET", self.files)
        self.assertEqual([c.args[:2] for c in request.call_args_list], [
            ("GET", api.root), ("GET", api.root + "/draft"), ("GET", self.files)])
        for c in request.call_args_list[:2]:
            self.assertEqual(c.kwargs["headers"]["Accept"],
                             "application/vnd.inveniordm.v1+json")
        self.assertNotIn("headers", request.call_args_list[2].kwargs)
        self.assertEqual(api.session.headers["Accept"], "application/json")

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
                self.assertEqual(kwargs["data"].tell(), 0)
                prepared = requests.Request("PUT", url, data=kwargs["data"]).prepare()
                self.assertEqual(prepared.headers["Content-Length"], str(self.path.stat().st_size))
                self.assertNotIn("Transfer-Encoding", prepared.headers)
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

    def test_progress_can_be_disabled_for_upload(self):
        api = Zenodo("fake-token", 23122650)

        def request(method, url, **kwargs):
            if url == self.files:
                return {"enabled": True, "entries": [self.pending]}
            if method == "PUT":
                self.assertNotIsInstance(kwargs["data"], ProgressReader)
                self.assertEqual(kwargs["data"].read(), self.path.read_bytes())
            return self.complete

        with patch.object(api, "request", side_effect=request):
            upload(api, self.files, self.path, "tool.zip", self.digests, progress=False)

    def test_bad_remote_checksum_fails(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api, "request", side_effect=[
            {"enabled": True, "entries": [self.pending]}, self.pending,
            self.complete, dict(self.complete, checksum="md5:corrupted")]):
            with self.assertRaisesRegex(UploadError, "verification failed"):
                self.run_upload(api)

    def test_finish_pending_upload_does_not_retransmit_or_publish(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api, "request", side_effect=[
            self.pending, self.complete, self.complete
        ]) as request:
            finish_upload(api, self.files, self.path, "tool.zip", self.digests)
        self.assertEqual(request.call_args_list, [
            call("GET", self.file_url), call("POST", self.file_url + "/commit"),
            call("GET", self.file_url)])

    def test_finish_matching_completed_upload_only_reads(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api, "request", return_value=self.complete) as request:
            finish_upload(api, self.files, self.path, "tool.zip", self.digests)
        request.assert_called_once_with("GET", self.file_url)

    def test_finish_rejects_wrong_completed_file_and_known_wrong_size(self):
        for entry in (dict(self.complete, checksum="md5:other"),
                      dict(self.pending, size=self.path.stat().st_size - 1)):
            with self.subTest(entry=entry):
                api = Zenodo("fake-token", 23122650)
                with patch.object(api, "request", return_value=entry) as request:
                    with self.assertRaises(UploadError):
                        finish_upload(api, self.files, self.path, "tool.zip", self.digests)
                request.assert_called_once_with("GET", self.file_url)

    def test_finish_never_accepts_pending_or_corrupted_content(self):
        for entry in (self.pending, dict(self.complete, checksum="md5:corrupted")):
            with self.subTest(entry=entry):
                api = Zenodo("fake-token", 23122650)
                with patch.object(api, "request", side_effect=[
                    self.pending, self.pending, entry
                ]) as request:
                    with self.assertRaisesRegex(UploadError, "verification failed"):
                        finish_upload(api, self.files, self.path, "tool.zip", self.digests)
                self.assertEqual(request.call_count, 3)

    def test_finish_missing_content_does_not_reupload(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api, "request", side_effect=[
            self.pending, UploadError("Content not found", 404)
        ]) as request:
            with self.assertRaises(UploadError):
                finish_upload(api, self.files, self.path, "tool.zip", self.digests)
        self.assertEqual(request.call_args_list, [
            call("GET", self.file_url), call("POST", self.file_url + "/commit")])

    def test_foreign_upload_host_receives_no_token(self):
        api = Zenodo("fake-token", 23122650)
        with patch.object(api.session, "request") as call:
            with self.assertRaises(UploadError):
                api.request("PUT", "https://other.example/file")
            call.assert_not_called()


if __name__ == "__main__":
    unittest.main()
