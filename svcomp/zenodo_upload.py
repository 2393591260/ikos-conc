#!/usr/bin/env python3
"""Upload a ZIP to an existing Zenodo file-edit draft; never publish or version it.

API: https://inveniordm.docs.cern.ch/reference/rest_api_drafts_records/
The owner must first enable Edit published files in the Zenodo web interface.
"""

import argparse
import getpass
import hashlib
from pathlib import Path
import sys
import time
from urllib.parse import quote, urljoin, urlsplit
import zipfile

import requests


class UploadError(RuntimeError):
    def __init__(self, message, status=0):
        super().__init__(message)
        self.status = status


class ProgressReader:
    """Track client reads without changing the file bytes or HTTP body length."""

    def __init__(self, stream, size):
        self.stream = stream
        self.size = size
        self.sent = 0
        self.started = self.updated = time.monotonic()
        self.complete = False
        self.show()

    def __len__(self):
        return self.size

    def tell(self):
        return self.stream.tell()

    def show(self):
        now = time.monotonic()
        rate = self.sent / max(now - self.started, 0.001)
        fraction = self.sent / self.size if self.size else 1
        filled = min(20, int(fraction * 20))
        eta = f"{(self.size - self.sent) / rate:.0f}s" if rate else "--"
        print(f"\r[{'#' * filled}{'-' * (20 - filled)}] {fraction:6.1%} "
              f"{self.sent}/{self.size} B | {rate / 1024:.1f} KiB/s | ETA {eta}   ",
              end="", file=sys.stderr, flush=True)
        self.updated = now

    def read(self, size=-1):
        block = self.stream.read(size)
        self.sent += len(block)
        if not self.complete and self.sent == self.size:
            self.show()
            print("\nFile read complete; waiting for server response (not yet verified).",
                  file=sys.stderr, flush=True)
            self.complete = True
        elif not self.complete and time.monotonic() - self.updated >= 0.5:
            self.show()
        return block

    def finish(self):
        if not self.complete:
            self.show()
            print(file=sys.stderr, flush=True)


class Zenodo:
    def __init__(self, token, record):
        self.token = token
        self.root = f"https://zenodo.org/api/records/{record}"
        self.session = requests.Session()
        self.session.headers.update({
            "Authorization": f"Bearer {token}", "Accept": "application/json"})

    def request(self, method, url, **kwargs):
        url = urljoin("https://zenodo.org", url)
        parsed = urlsplit(url)
        if parsed.scheme != "https" or parsed.netloc != "zenodo.org":
            raise UploadError("Unexpected API host; token was not sent.")
        # urllib3 also uses the connect socket timeout while writing the body.
        timeout = (120, 600) if method == "PUT" else (20, 300)
        # Retry transient reads only; mutations have their own recovery logic.
        for attempt in range(1, 4):
            response = self.session.request(
                method, url, timeout=timeout, allow_redirects=False, **kwargs
            )
            if (method != "GET" or response.status_code not in (500, 502, 503, 504)
                    or attempt == 3):
                break
            delay = 5 if attempt == 1 else 15
            print(f"HTTP {response.status_code} reading {url}; "
                  f"retry {attempt + 1}/3 in {delay}s…", file=sys.stderr, flush=True)
            response.close()
            time.sleep(delay)
        if not 200 <= response.status_code < 300:
            detail = response.text[:800].replace(self.token, "[redacted]")
            raise UploadError(f"HTTP {response.status_code}: {url}\n{detail}",
                              response.status_code)
        try:
            return response.json()
        except ValueError as exc:
            raise UploadError(f"API returned non-JSON content: {url}") from exc

    def draft_files(self, record):
        expected = f"10.5281/zenodo.{record}"
        # Metadata uses native RDM JSON; file endpoints retain application/json.
        headers = {"Accept": "application/vnd.inveniordm.v1+json"}
        published = self.request("GET", self.root, headers=headers)
        if str(published.get("id")) != str(record):
            raise UploadError("Published record ID does not match.")
        doi = published.get("pids", {}).get("doi", {}).get("identifier") or published.get("doi")
        if doi != expected:
            raise UploadError(f"Published DOI {doi!r} does not match {expected}.")
        draft = self.request("GET", self.root + "/draft", headers=headers)
        if str(draft.get("id")) != str(record):
            raise UploadError("Draft ID differs; refusing to upload to a new version.")
        draft_doi = draft.get("pids", {}).get("doi", {}).get("identifier") or draft.get("doi")
        if draft_doi != expected:
            raise UploadError(f"Draft DOI {draft_doi!r} does not match {expected}.")
        url = draft.get("links", {}).get("files")
        if not url:
            raise UploadError("Draft has no file endpoint.")
        url = urljoin("https://zenodo.org", url)
        if url.rstrip("/") != self.root + "/draft/files":
            raise UploadError("Unexpected draft file endpoint.")
        print(f"Checked existing record and draft: {expected}", flush=True)
        return url


def hashes(path):
    md5 = hashlib.md5()
    sha256 = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            md5.update(block)
            sha256.update(block)
    return {"md5": md5.hexdigest(), "sha256": sha256.hexdigest()}


def matches(entry, size, digests):
    checksum = entry.get("checksum") or ""
    algorithm, separator, digest = checksum.partition(":")
    return (
        entry.get("status") == "completed"
        and entry.get("size") == size
        and bool(separator)
        and digests.get(algorithm) == digest
    )


def finish_upload(api, files_url, path, filename, digests):
    """Commit an existing upload without sending its content again."""
    file_url = files_url.rstrip("/") + "/" + quote(filename, safe="")
    current = api.request("GET", file_url)
    size = path.stat().st_size
    print(f"Remote file: status={current.get('status')}, "
          f"size={current.get('size')}, checksum={current.get('checksum')}", flush=True)
    if not matches(current, size, digests):
        if current.get("status") != "pending":
            raise UploadError("Remote file is not a matching completed file or pending upload.")
        # Pending metadata can report zero before content has been committed.
        if current.get("size") not in (None, 0, size):
            raise UploadError("Remote file size differs; no commit was attempted.")
        print("Finishing the existing upload; no ZIP bytes will be retransmitted…", flush=True)
        api.request("POST", file_url + "/commit")
        current = api.request("GET", file_url)
    if not matches(current, size, digests):
        raise UploadError("Remote file verification failed after commit; do not publish. "
                          "The server may not have received the complete file.")
    print(f"Verified remote size and checksum: {current['checksum']}", flush=True)


def upload(api, files_url, path, filename, digests, progress=True):
    listing = api.request("GET", files_url)
    if not listing.get("enabled"):
        raise UploadError("Draft files are disabled; enable Edit published files first.")
    entry = next((e for e in listing["entries"] if e["key"] == filename), None)
    size = path.stat().st_size
    if entry and entry.get("status") == "completed":
        if matches(entry, size, digests):
            print("The same file is already uploaded and verified.")
            return
        raise UploadError("A different completed file has this name. No file was deleted.")
    if entry is None:
        initialized = api.request("POST", files_url, json=[{"key": filename}])
        entry = next(e for e in initialized["entries"] if e["key"] == filename)
    file_url = files_url.rstrip("/") + "/" + quote(filename, safe="")
    # A disconnected PUT may still be running remotely; do not start another.
    try:
        print(f"Uploading {size} bytes (one attempt; no automatic retransmission)…", flush=True)
        with path.open("rb") as stream:
            reader = ProgressReader(stream, size) if progress else stream
            try:
                api.request("PUT", entry["links"]["content"], data=reader,
                            headers={"Content-Type": "application/octet-stream"})
            finally:
                if progress:
                    reader.finish()
        print("Server accepted upload content; committing file…", flush=True)
        api.request("POST", entry["links"]["commit"])
    except (requests.RequestException, UploadError) as exc:
        # A response may have been lost after the server completed commit.
        try:
            current = api.request("GET", file_url)
        except (requests.RequestException, UploadError):
            raise exc
        if not matches(current, size, digests):
            raise UploadError(
                f"{exc}\nRemote file: status={current.get('status')}, "
                f"size={current.get('size')}, checksum={current.get('checksum')}. "
                "No automatic retransmission was attempted.",
                getattr(exc, "status", 0),
            ) from exc
    print("Checking remote size and checksum…", flush=True)
    current = api.request("GET", file_url)
    if not matches(current, size, digests):
        raise UploadError(f"Remote file verification failed: {current.get('checksum')!r}.")
    print(f"Verified remote size and checksum: {current['checksum']}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", required=True, type=int)
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--filename", help="Remote name; defaults to the local filename")
    parser.add_argument("--finish-upload", action="store_true",
                        help="Commit and verify an existing upload without retransmitting its content")
    parser.add_argument("--no-progress", action="store_true",
                        help="Disable the client-read progress bar")
    args = parser.parse_args()
    path = args.file.resolve(strict=True)
    filename = args.filename or path.name
    if filename in ("", ".", "..") or "/" in filename or "\\" in filename:
        parser.error("--filename must be a filename without directory components")
    with zipfile.ZipFile(path) as archive:
        bad = archive.testzip()
        if bad:
            raise UploadError(f"Damaged ZIP member: {bad}")
    digests = hashes(path)
    print(f"Local ZIP: {path}\nSHA256: {digests['sha256']}", flush=True)
    token = getpass.getpass("Zenodo token (hidden; input is not saved): ").strip()
    if not token:
        raise UploadError("Empty token.")
    api = Zenodo(token, args.record)
    files_url = api.draft_files(args.record)
    if args.finish_upload:
        finish_upload(api, files_url, path, filename, digests)
    else:
        upload(api, files_url, path, filename, digests, progress=not args.no_progress)
    print(f"Upload complete. Review and publish the file-edit draft in your browser:\n"
          f"https://zenodo.org/uploads/{args.record}\n"
          "This script did not publish or create a version.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (UploadError, requests.RequestException, OSError, zipfile.BadZipFile) as exc:
        print(f"Stopped: {exc}", file=sys.stderr)
        sys.exit(1)
