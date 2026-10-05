#!/usr/bin/env bash
# Upload files to GoFile and verify returned metadata before printing links.
# Optional env: GOFILE_TOKEN, GOFILE_FOLDER_ID.
set -euo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <file> [file...]" >&2
    exit 1
fi

API_BASE="https://api.gofile.io"
UPLOAD_URL="https://upload.gofile.io/uploadfile"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

read_account_token() {
    python3 - "$1" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as response_file:
        payload = json.load(response_file)
except (OSError, UnicodeError, json.JSONDecodeError):
    print("upload-gofile: account response was not valid JSON", file=sys.stderr)
    raise SystemExit(1)

data = payload.get("data") if isinstance(payload, dict) else None
token = data.get("token") if isinstance(data, dict) else None
if (
    not isinstance(payload, dict)
    or payload.get("status") != "ok"
    or not isinstance(token, str)
    or not token
    or "\r" in token
    or "\n" in token
):
    print("upload-gofile: account API did not return a token", file=sys.stderr)
    raise SystemExit(1)
sys.stdout.write(token)
PY
}

validate_upload() {
    python3 - "$1" "$2" "$3" <<'PY'
import hashlib
import json
import os
import re
import sys


def fail(message):
    print("upload-gofile: " + message, file=sys.stderr)
    raise SystemExit(1)


response_path, local_path, metadata_path = sys.argv[1:]
try:
    with open(response_path, encoding="utf-8") as response_file:
        payload = json.load(response_file)
except (OSError, UnicodeError, json.JSONDecodeError):
    fail("upload response was not valid JSON")

if not isinstance(payload, dict) or payload.get("status") != "ok":
    fail("GoFile upload API reported an error")
data = payload.get("data")
if not isinstance(data, dict):
    fail("upload response did not contain file metadata")

file_id = data.get("id")
if not isinstance(file_id, str) or re.fullmatch(
    r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}",
    file_id,
) is None:
    fail("upload response contained an invalid file ID")

if data.get("type") != "file":
    fail("upload response did not identify a file")
name = data.get("name")
if not isinstance(name, str) or not name or name != os.path.basename(local_path):
    fail("upload response file name did not match the local file")

size = data.get("size")
if type(size) is not int or size < 0:
    fail("upload response contained an invalid file size")
md5 = data.get("md5")
if not isinstance(md5, str) or re.fullmatch(r"[0-9a-fA-F]{32}", md5) is None:
    fail("upload response contained an invalid MD5")

try:
    local_size = os.path.getsize(local_path)
    digest = hashlib.md5()
    with open(local_path, "rb") as local_file:
        for chunk in iter(lambda: local_file.read(1024 * 1024), b""):
            digest.update(chunk)
except OSError:
    fail("could not read the local file for metadata verification")

if size != local_size:
    fail("GoFile-reported size does not match the local file")
if md5.lower() != digest.hexdigest():
    fail("GoFile-reported MD5 does not match the local file")

folder_page = data.get("downloadPage", "")
if not isinstance(folder_page, str) or re.fullmatch(
    r"https://gofile\.io/d/[A-Za-z0-9_-]+", folder_page
) is None:
    folder_page = ""

with open(metadata_path, "w", encoding="utf-8") as metadata_file:
    json.dump(
        {"id": file_id.lower(), "name": name, "size": size, "md5": md5.lower(),
         "folder_page": folder_page},
        metadata_file,
    )
PY
}

GOFILE_TOKEN="${GOFILE_TOKEN:-}"
if [ -z "$GOFILE_TOKEN" ]; then
    echo "upload-gofile: creating a guest account token" >&2
    if ! curl --silent --show-error --fail --request POST \
            "$API_BASE/accounts" --output "$TMP/account.json"; then
        echo "upload-gofile: account request failed" >&2
        exit 1
    fi
    if ! GOFILE_TOKEN="$(read_account_token "$TMP/account.json")"; then
        exit 1
    fi
fi

if [[ "$GOFILE_TOKEN" == *$'\r'* || "$GOFILE_TOKEN" == *$'\n'* ]]; then
    echo "upload-gofile: token contains invalid characters" >&2
    exit 1
fi

FOLDER_ID="${GOFILE_FOLDER_ID:-}"
METADATA_FILES=()
FAILED=0
index=0
for file in "$@"; do
    if [ ! -f "$file" ] || [ ! -r "$file" ]; then
        printf 'upload-gofile: skipping unreadable file: %s\n' "$file" >&2
        FAILED=1
        continue
    fi

    printf 'upload-gofile: uploading %s\n' "$file" >&2
    form_args=(--form "file=@$file")
    if [ -n "$FOLDER_ID" ]; then
        form_args+=(--form "folderId=$FOLDER_ID")
    fi
    response_path="$TMP/upload-$index.json"
    metadata_path="$TMP/metadata-$index.json"
    index=$((index + 1))

    if ! curl --silent --show-error --fail "${form_args[@]}" \
            --header "Authorization: Bearer $GOFILE_TOKEN" \
            --output "$response_path" "$UPLOAD_URL"; then
        printf 'upload-gofile: upload request failed for %s\n' "$file" >&2
        FAILED=1
        continue
    fi

    if validate_upload "$response_path" "$file" "$metadata_path"; then
        METADATA_FILES+=("$metadata_path")
    else
        FAILED=1
    fi
done

if [ "${#METADATA_FILES[@]}" -gt 0 ]; then
    python3 - "${METADATA_FILES[@]}" <<'PY'
import json
import sys

print("upload-gofile: GoFile-reported size and MD5 match the local files shown below;")
print("  this metadata check does not verify a downloaded copy.")
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as metadata_file:
        metadata = json.load(metadata_file)
    print(
        "  %s (%s bytes, MD5 %s)"
        % (
            json.dumps(metadata["name"], ensure_ascii=False),
            metadata["size"],
            metadata["md5"],
        )
    )
    print("    https://gofile.io/d/%s" % metadata["id"])
    if metadata["folder_page"]:
        print("    folder: %s" % metadata["folder_page"])
PY
fi

exit "$FAILED"
