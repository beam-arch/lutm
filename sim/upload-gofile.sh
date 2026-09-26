#!/bin/bash
#
# Upload one or more files to gofile.io and print the canonical page links.
#
# Usage: sim/upload-gofile.sh <file> [file...]
#
# Optional env:
#   GOFILE_FOLDER      folder name to try to create (default: lineage-virtio-sim)
#   GOFILE_FOLDER_ID   upload into an existing folder instead of creating one
#   GOFILE_TOKEN       reuse an existing account token instead of creating one
#
# What gofile's guest tier actually does (learned by testing, not from docs):
#   * Guests cannot create folders -- `createfolder` answers `error-notPremium`.
#   * `GET /contents/<id-or-code>` also answers `error-notPremium` for guests, so
#     there is NO way to look up, list, or validate content after uploading.
#   * The `downloadPage` field of the upload response is the *parent folder*
#     code, and those pages do not reliably resolve for anonymous visitors.
#   * The storage URL (https://<server>.gofile.io/download/web/<id>/<name>)
#     302-redirects to https://gofile.io/d/<file-uuid>, so the file's UUID is the
#     canonical page id. That is what this script prints.
#   * Every /d/ page answers 200 with the same JS shell whether or not the id
#     exists, so an HTTP status proves nothing. The upload response does carry
#     the file's md5 and size, which are printed so a download can be checked
#     with `md5sum`.

set -uo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <file> [file...]" >&2
    exit 1
fi

FOLDER_NAME="${GOFILE_FOLDER:-lineage-virtio-sim}"
API_BASE="https://api.gofile.io"
UPLOAD_URL="https://upload.gofile.io/uploadfile"

json_field() {
    # json_field <file> <key> : best-effort extraction without jq.
    # Looks inside "data" first, then at the top level (error responses put
    # "status" next to an empty "data").
    python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    payload = json.load(f)
key = sys.argv[2]
value = payload.get("data", {}).get(key, None)
if value in (None, ""):
    value = payload.get(key, "")
print(value)
' "$1" "$2"
}

# upload_info <response.json> : tab-separated
#   <file-page> <folder-page> <name> <size> <md5> <server> <code>
upload_info() {
    python3 -c '
import json, sys
with open(sys.argv[1]) as f:
    d = json.load(f)["data"]
uid = d.get("id", "")
print("%s\t%s\t%s\t%s\t%s\t%s\t%s" % (
    "https://gofile.io/d/%s" % uid if uid else "",
    d.get("downloadPage", ""),
    d.get("name", ""),
    d.get("size", ""),
    d.get("md5", ""),
    (d.get("servers") or [""])[0],
    d.get("code", ""),
))
' "$1"
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -z "${GOFILE_TOKEN:-}" ]; then
    echo "upload-gofile: creating account token"
    curl -sS -X POST "$API_BASE/accounts" -o "$TMP/account.json" || exit 1
    GOFILE_TOKEN="$(json_field "$TMP/account.json" token)"
fi

if [ -z "$GOFILE_TOKEN" ]; then
    echo "upload-gofile: failed to obtain a token" >&2
    exit 1
fi

FOLDER_ID="${GOFILE_FOLDER_ID:-}"
if [ -z "$FOLDER_ID" ]; then
    # Folders are a premium feature; a guest account answers `error-notPremium`.
    # That is not fatal -- uploads then land in the account's root folder -- so
    # only warn, and never promise a folder link we do not have.
    curl -sS -G "$API_BASE/contents/createfolder" \
        --data-urlencode "folderName=$FOLDER_NAME" \
        -H "Authorization: Bearer $GOFILE_TOKEN" \
        -o "$TMP/folder.json" || true
    FOLDER_ID="$(json_field "$TMP/folder.json" id)"
    if [ -n "$FOLDER_ID" ]; then
        echo "upload-gofile: created folder '$FOLDER_NAME'"
    else
        echo "upload-gofile: cannot create folder '$FOLDER_NAME' ($(json_field "$TMP/folder.json" status));" \
             "uploading to the account root instead" >&2
    fi
fi

UPLOADS=()   # "<name>\t<file-page>\t<folder-page>\t<md5>\t<size>"
FAILED=0
for file in "$@"; do
    if [ ! -f "$file" ]; then
        echo "upload-gofile: skipping missing file: $file" >&2
        FAILED=1
        continue
    fi
    echo "upload-gofile: uploading $file ($(du -h "$file" | cut -f1))"
    args=(-F "file=@$file")
    [ -n "$FOLDER_ID" ] && args+=(-F "folderId=$FOLDER_ID")
    if ! curl -sS "${args[@]}" \
            -H "Authorization: Bearer $GOFILE_TOKEN" \
            "$UPLOAD_URL" -o "$TMP/upload.json"; then
        echo "upload-gofile: upload request failed for $file" >&2
        FAILED=1
        continue
    fi

    IFS=$'\t' read -r file_page folder_page name size md5 server code \
        < <(upload_info "$TMP/upload.json")
    if [ -z "$file_page" ]; then
        echo "upload-gofile: unexpected upload response for $file: $(head -c 300 "$TMP/upload.json")" >&2
        FAILED=1
        continue
    fi

    printf 'upload-gofile: stored as %s (%s bytes, md5 %s, server %s)\n' \
        "$name" "$size" "$md5" "$server"
    UPLOADS+=("$name"$'\t'"$file_page"$'\t'"$folder_page"$'\t'"$md5"$'\t'"$size")
done

echo
if [ "${#UPLOADS[@]}" -gt 0 ]; then
    echo "upload-gofile: page links (canonical id from gofile's own redirect):"
    for u in "${UPLOADS[@]}"; do
        IFS=$'\t' read -r name file_page folder_page md5 size <<< "$u"
        printf '  %s  (%s bytes)\n    %s\n' "$name" "$size" "$file_page"
        printf '    md5 %s\n' "$md5"
        if [ -n "$folder_page" ] && [ "$folder_page" != "$file_page" ]; then
            printf '    fallback: %s\n' "$folder_page"
        fi
    done
    echo
    echo "upload-gofile: gofile cannot be queried for guest uploads, so verify a"
    echo "  download with:  md5sum <file>   and compare to the md5 printed above."
fi

exit "$FAILED"
