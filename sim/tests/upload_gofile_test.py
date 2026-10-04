#!/usr/bin/env python3
"""Offline tests for GoFile uploads using a PATH-mocked curl executable."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SIM = Path(__file__).resolve().parents[1]
UPLOADER = SIM / "upload-gofile.sh"
FILE_ID = "a" * 8 + "-aaaa-4aaa-8aaa-" + "a" * 12
GUEST_TOKEN = "mock-account-token-do-not-log"
MOCK_CURL = r'''#!/usr/bin/env python3
import hashlib
import json
import os
from pathlib import Path
import sys

args = sys.argv[1:]
output = None
method = None
forms = []
headers = []
index = 0
while index < len(args):
    arg = args[index]
    if arg in ("-o", "--output", "-X", "--request", "-F", "--form", "-H", "--header"):
        value = args[index + 1]
        if arg in ("-o", "--output"):
            output = value
        elif arg in ("-X", "--request"):
            method = value
        elif arg in ("-F", "--form"):
            forms.append(value)
        else:
            headers.append(value)
        index += 2
    else:
        index += 1

url = next(arg for arg in args if arg.startswith("https://"))
if method is None:
    method = "POST" if forms else "GET"
record = {
    "url": url,
    "method": method,
    "forms": forms,
    "has_authorization": any(h.lower().startswith("authorization: bearer ") for h in headers),
}
with open(os.environ["MOCK_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps(record) + "\n")

if url == "https://api.gofile.io/accounts":
    payload = {"status": "ok", "data": {"token": "mock-account-token-do-not-log"}}
    result = 0
elif url == "https://upload.gofile.io/uploadfile":
    file_forms = [form[len("file=@"):] for form in forms if form.startswith("file=@")]
    if len(file_forms) != 1 or output is None or not Path(file_forms[0]).is_file():
        payload = {"status": "error", "message": "invalid mock form"}
        result = 1
    else:
        local_path = Path(file_forms[0])
        name = local_path.name
        body = local_path.read_bytes()
        digest = hashlib.md5(body).hexdigest()
        mode = os.environ.get("MOCK_MODE", "success")
        bad_name = os.environ.get("MOCK_API_ERROR_NAME")
        if bad_name == name:
            payload = {
                "status": "error",
                "data": {"token": "must-not-be-printed"},
                "message": "credential-bearing error marker",
            }
            result = 0
        elif mode == "malformed":
            payload = None
            result = 0
        elif mode == "size_mismatch":
            payload = {
                "status": "ok",
                "data": {"id": os.environ["MOCK_FILE_ID"], "type": "file",
                         "name": name, "size": len(body) + 1, "md5": digest},
            }
            result = 0
        elif mode == "md5_mismatch":
            payload = {
                "status": "ok",
                "data": {"id": os.environ["MOCK_FILE_ID"], "type": "file",
                         "name": name, "size": len(body), "md5": "0" * 32},
            }
            result = 0
        elif mode in ("invalid_id", "invalid_type", "invalid_name", "invalid_size", "invalid_md5"):
            payload = {
                "status": "ok",
                "data": {
                    "id": "not-a-uuid" if mode == "invalid_id" else os.environ["MOCK_FILE_ID"],
                    "type": "folder" if mode == "invalid_type" else "file",
                    "name": "wrong-name.zip" if mode == "invalid_name" else name,
                    "size": str(len(body)) if mode == "invalid_size" else len(body),
                    "md5": "invalid" if mode == "invalid_md5" else digest,
                },
            }
            result = 0
        elif mode == "http_failure":
            payload = {"status": "error", "message": "must-not-be-printed"}
            result = 22
        else:
            payload = {
                "status": "ok",
                "data": {
                    "id": os.environ["MOCK_FILE_ID"],
                    "type": "file",
                    "name": name,
                    "size": len(body),
                    "md5": digest,
                    "downloadPage": os.environ.get("MOCK_FOLDER_PAGE", ""),
                    "servers": [],
                    "code": "",
                },
            }
            result = 0
else:
    payload = {"status": "error", "message": "unexpected endpoint"}
    result = 1

if output is not None:
    if payload is None:
        Path(output).write_text('{"status":', encoding="utf-8")
    else:
        Path(output).write_text(json.dumps(payload), encoding="utf-8")
if result == 22:
    print("mock curl: HTTP failure", file=sys.stderr)
sys.exit(result)
'''


class UploadGoFileTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        mock_curl = self.bin / "curl"
        mock_curl.write_text(MOCK_CURL, encoding="utf-8")
        mock_curl.chmod(0o755)
        self.log = self.root / "curl.jsonl"

    def environment(self, **extra):
        environment = {
            "PATH": os.pathsep.join((str(self.bin), os.defpath)),
            "MOCK_LOG": str(self.log),
            "MOCK_FILE_ID": FILE_ID,
        }
        environment.update({key: str(value) for key, value in extra.items()})
        return environment

    def run_upload(self, *files, **environment):
        return subprocess.run(
            ["bash", str(UPLOADER), *(str(path) for path in files)],
            cwd=SIM,
            env=self.environment(**environment),
            capture_output=True,
            text=True,
        )

    def calls(self):
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def make_file(self, name, contents=b"fresh public image zip"):
        path = self.root / name
        path.write_bytes(contents)
        return path

    def test_guest_upload_verifies_metadata_with_spaces_and_empty_optional_fields(self):
        image = self.make_file("lineage image with spaces.zip")
        result = self.run_upload(image, GOFILE_FOLDER_ID="existing-folder-id")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("lineage image with spaces.zip", result.stdout)
        self.assertIn("https://gofile.io/d/" + FILE_ID, result.stdout)
        self.assertIn(hashlib.md5(image.read_bytes()).hexdigest(), result.stdout)
        self.assertIn("does not verify a downloaded copy", result.stdout)
        self.assertNotIn(GUEST_TOKEN, result.stdout + result.stderr)

        calls = self.calls()
        self.assertEqual([call["url"] for call in calls], [
            "https://api.gofile.io/accounts",
            "https://upload.gofile.io/uploadfile",
        ])
        self.assertEqual(calls[0]["method"], "POST")
        self.assertFalse(calls[0]["has_authorization"])
        self.assertTrue(calls[1]["has_authorization"])
        self.assertIn("file=@" + str(image), calls[1]["forms"])
        self.assertIn("folderId=existing-folder-id", calls[1]["forms"])

    def test_supplied_token_skips_guest_account_creation_and_is_not_printed(self):
        image = self.make_file("already-authorized.zip")
        token = "mock-supplied-secret"
        result = self.run_upload(image, GOFILE_TOKEN=token)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(token, result.stdout + result.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["url"], "https://upload.gofile.io/uploadfile")
        self.assertTrue(calls[0]["has_authorization"])

    def test_http_200_api_error_keeps_failure_status_when_another_upload_succeeds(self):
        good = self.make_file("good image.zip")
        bad = self.make_file("bad image.zip")
        result = self.run_upload(good, bad, MOCK_API_ERROR_NAME=bad.name)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("https://gofile.io/d/" + FILE_ID, result.stdout)
        self.assertIn("GoFile upload API reported an error", result.stderr)
        self.assertNotIn("must-not-be-printed", result.stdout + result.stderr)
        self.assertEqual(len([call for call in self.calls() if call["url"].endswith("/uploadfile")]), 2)

    def test_malformed_json_fails_without_printing_response_body(self):
        image = self.make_file("malformed response.zip")
        result = self.run_upload(image, MOCK_MODE="malformed")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("upload response was not valid JSON", result.stderr)
        self.assertNotIn('{"status":', result.stdout + result.stderr)

    def test_size_mismatch_fails_before_printing_a_link(self):
        image = self.make_file("wrong size.zip")
        result = self.run_upload(image, MOCK_MODE="size_mismatch")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("size does not match", result.stderr)
        self.assertNotIn("https://gofile.io/d/", result.stdout)

    def test_invalid_response_metadata_is_rejected(self):
        for mode in ("invalid_id", "invalid_type", "invalid_name", "invalid_size", "invalid_md5"):
            with self.subTest(mode=mode):
                image = self.make_file(mode + ".zip")
                result = self.run_upload(image, MOCK_MODE=mode)

                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("https://gofile.io/d/", result.stdout)

    def test_md5_mismatch_fails_before_printing_a_link(self):
        image = self.make_file("wrong checksum.zip")
        result = self.run_upload(image, MOCK_MODE="md5_mismatch")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MD5 does not match", result.stderr)
        self.assertNotIn("https://gofile.io/d/", result.stdout)

    def test_folder_page_is_kept_separate_from_the_file_page(self):
        image = self.make_file("image.zip")
        folder_page = "https://gofile.io/d/sharecode"
        result = self.run_upload(image, MOCK_FOLDER_PAGE=folder_page)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("https://gofile.io/d/" + FILE_ID, result.stdout)
        self.assertIn("folder: " + folder_page, result.stdout)

    def test_http_failure_fails_without_printing_response_body_or_link(self):
        image = self.make_file("http failure.zip")
        result = self.run_upload(image, MOCK_MODE="http_failure")

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("upload request failed", result.stderr)
        self.assertNotIn("must-not-be-printed", result.stdout + result.stderr)
        self.assertNotIn("https://gofile.io/d/", result.stdout)


if __name__ == "__main__":
    unittest.main()
