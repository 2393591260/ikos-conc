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
        response = self.session.request(
            method, url, timeout=(20, 300), allow_redirects=False, **kwargs
        )
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
        published = self.request("GET", self.root)
        if str(published.get("id")) != str(record):
            raise UploadError("Published record ID does not match.")
        doi = published.get("pids", {}).get("doi", {}).get("identifier") or published.get("doi")
        if doi != expected:
            raise UploadError(f"Published DOI {doi!r} does not match {expected}.")
        draft = self.request("GET", self.root + "/draft")
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


def upload(api, files_url, path, filename, digests):
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
    # PUT is retried with the full file. This does not promise byte-offset resume.
    for attempt in range(1, 4):
        try:
            print(f"Uploading {size} bytes, attempt {attempt}/3…", flush=True)
            with path.open("rb") as stream:
                api.request("PUT", entry["links"]["content"], data=stream,
                            headers={"Content-Type": "application/octet-stream"})
            api.request("POST", entry["links"]["commit"])
            break
        except (requests.RequestException, UploadError) as exc:
            print(str(exc), file=sys.stderr, flush=True)
            if (isinstance(exc, UploadError) and exc.status != 429
                    and not 500 <= exc.status < 600):
                raise
            # A response may have been lost after the server completed commit.
            try:
                current = api.request("GET", file_url)
            except (requests.RequestException, UploadError):
                if attempt == 3:
                    raise
                current = {}
            if matches(current, size, digests):
                break
            if current.get("status") == "completed" or attempt == 3:
                raise
            time.sleep(5)
    current = api.request("GET", file_url)
    if not matches(current, size, digests):
        raise UploadError(f"Remote file verification failed: {current.get('checksum')!r}.")
    print(f"Verified remote size and checksum: {current['checksum']}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--record", required=True, type=int)
    parser.add_argument("--file", required=True, type=Path)
    parser.add_argument("--filename", help="Remote name; defaults to the local filename")
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
    upload(api, api.draft_files(args.record), path, filename, digests)
    print(f"Upload complete. Review and publish the file-edit draft in your browser:\n"
          f"https://zenodo.org/uploads/{args.record}\n"
          "This script did not publish or create a version.", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (UploadError, requests.RequestException, OSError, zipfile.BadZipFile) as exc:
        print(f"Stopped: {exc}", file=sys.stderr)
        sys.exit(1)
