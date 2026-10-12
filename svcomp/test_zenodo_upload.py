"""Offline checks for draft identity, upload integrity and retry boundaries."""

import contextlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import io
import json
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch

import requests

from zenodo_upload import ProgressReader, UploadError, Zenodo, hashes, upload


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

    def test_progress_stream_preserves_body_and_content_length(self):
        size = self.path.stat().st_size
        with ProgressReader(self.path.open("rb", buffering=0), size) as stream:
            request = requests.Request("PUT", self.file_url, data=stream).prepare()
            self.assertEqual(request.headers["Content-Length"], str(size))
            self.assertNotIn("Transfer-Encoding", request.headers)
            self.assertIs(request.body, stream)
            self.assertEqual(stream.read(4) + stream.read(), self.path.read_bytes())
            self.assertEqual(stream.sent, size)

    def test_actual_http_body_is_identical_with_and_without_progress(self):
        payload = bytes(range(256)) * 8192
        self.path.write_bytes(payload)
        received = []

        class Handler(BaseHTTPRequestHandler):
            def do_PUT(self):
                size = int(self.headers["Content-Length"])
                received.append((self.headers.get("Transfer-Encoding"),
                                 self.rfile.read(size)))
                self.send_response(200)
                self.send_header("Content-Length", "2")
                self.end_headers()
                self.wfile.write(b"{}")

            def log_message(self, *args):
                pass

        with HTTPServer(("127.0.0.1", 0), Handler) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                with requests.Session() as session:
                    session.trust_env = False
                    url = f"http://127.0.0.1:{server.server_port}/content"
                    for progress in (False, True):
                        with (ProgressReader(self.path.open("rb", buffering=0), len(payload))
                              if progress else self.path.open("rb")) as stream:
                            response = session.put(url, data=stream, timeout=(5, 5))
                        self.assertEqual(response.status_code, 200)
                    if shutil.which("curl"):
                        api = Zenodo("fake-token", 23122650)
                        with patch.object(api, "checked_url", return_value=url):
                            self.assertEqual(api.upload_with_curl(url, self.path, progress=False), {})
            finally:
                server.shutdown()
                worker.join(timeout=5)
        count = 3 if shutil.which("curl") else 2
        self.assertEqual(received, [(None, payload)] * count)

    def test_curl_token_is_not_in_argv_and_response_errors_are_redacted(self):
        api = Zenodo("fake-token", 23122650)

        def run(command, **kwargs):
            self.assertEqual(command[1:4], ["--disable", "--config", "-"])
            self.assertNotIn(api.token, " ".join(command))
            self.assertIn(api.token, kwargs["input"])
            self.assertNotIn("--location", command)
            self.assertNotIn("--insecure", command)
            output = Path(command[command.index("--output") + 1])
            output.write_text(json.dumps({"error": api.token}))
            return Mock(returncode=0, stdout="403 16", stderr="")

        with patch("zenodo_upload.shutil.which", return_value="/usr/bin/curl"), \
                patch("zenodo_upload.subprocess.run", side_effect=run):
            with self.assertRaises(UploadError) as caught:
                api.upload_with_curl(self.file_url + "/content", self.path, progress=False)
        self.assertEqual(caught.exception.status, 403)
        self.assertNotIn(api.token, str(caught.exception))
        self.assertIn("[redacted]", str(caught.exception))

    def test_curl_upload_commits_and_verifies_without_publication(self):
        api = Zenodo("fake-token", 23122650)

        def request(method, url, **kwargs):
            self.assertFalse("publish" in url or "versions" in url or method == "DELETE")
            if url == self.files:
                return {"enabled": True, "entries": [self.pending]}
            self.assertIn(url, [self.file_url, self.pending["links"]["commit"]])
            return self.complete

        with patch.object(api, "request", side_effect=request), \
                patch.object(api, "upload_with_curl", return_value=self.pending) as body:
            upload(api, self.files, self.path, "tool.zip", self.digests,
                   transport="curl", attempts=1, progress=False)
        body.assert_called_once_with(self.pending["links"]["content"], self.path, progress=False)

    def test_single_attempt_stops_or_recovers_after_lost_reply(self):
        for current in (self.pending, self.complete):
            with self.subTest(status=current["status"]):
                api = Zenodo("fake-token", 23122650)
                transmissions = []

                def request(method, url, **kwargs):
                    if url == self.files:
                        return {"enabled": True, "entries": [self.pending]}
                    if method == "PUT":
                        transmissions.append(kwargs["data"].read())
                        raise requests.ReadTimeout("Response lost")
                    self.assertEqual(method, "GET")
                    self.assertEqual(url, self.file_url)
                    return current

                with patch.object(api, "request", side_effect=request), \
                        patch("zenodo_upload.time.sleep") as sleep:
                    if current is self.complete:
                        upload(api, self.files, self.path, "tool.zip", self.digests, attempts=1)
                    else:
                        with self.assertRaises(requests.ReadTimeout):
                            upload(api, self.files, self.path, "tool.zip", self.digests, attempts=1)
                    sleep.assert_not_called()
                self.assertEqual(transmissions, [self.path.read_bytes()])

    def test_request_uses_explicit_upload_timeout(self):
        api = Zenodo("fake-token", 23122650)
        response = Mock(status_code=200)
        response.json.return_value = {}
        with patch.object(api.session, "request", return_value=response) as call:
            api.request("GET", api.root)
            self.assertEqual(call.call_args.kwargs["timeout"], (20, 300))
            api.request("PUT", self.file_url + "/content", timeout=(300, 300))
            self.assertEqual(call.call_args.kwargs["timeout"], (300, 300))

    def test_no_progress_upload_uses_plain_stream_and_verifies_checksum(self):
        api = Zenodo("fake-token", 23122650)
        transmissions = []

        def request(method, url, **kwargs):
            if url == self.files:
                return {"enabled": True, "entries": [self.pending]}
            if method == "PUT":
                self.assertNotIsInstance(kwargs["data"], ProgressReader)
                self.assertEqual(kwargs["timeout"], (300, 300))
                transmissions.append(kwargs["data"].read())
                return self.pending
            return self.complete

        with patch.object(api, "request", side_effect=request):
            upload(api, self.files, self.path, "tool.zip", self.digests, progress=False)
        self.assertEqual(transmissions, [self.path.read_bytes()])

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
        with patch("zenodo_upload.subprocess.run") as call:
            with self.assertRaises(UploadError):
                api.upload_with_curl("https://other.example/file", self.path)
            call.assert_not_called()


if __name__ == "__main__":
    unittest.main()
