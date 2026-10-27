#!/usr/bin/env python3
"""Fill missing translations in localization/strings.json.

Uses a public Google Translate batch endpoint, with MyMemory as a fallback.
Tokens that must survive translation unchanged (placeholders, menu.ini,
Windhawk, paths) are replaced by sentinels first. Generated translations are
machine output; native corrections are welcome and manual values are never
overwritten.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
STRINGS_PATH = REPO_ROOT / "localization" / "strings.json"

GOOGLE = (
    "https://clients5.google.com/translate_a/t"
    "?client=dict-chrome-ex&sl=en&tl={tl}&q={q}"
)
MYMEMORY = (
    "https://api.mymemory.translated.net/get?q={q}&langpair=en|{tl}"
)

# The language picker uses native names; the generator handles it separately.
SKIP_PREFIXES = ("@meta:option.language.",)

# Single-word menu commands are ambiguous out of context ("Open" becomes the
# adjective). Ask for a menu command and strip the parenthetical.
CONTEXT_KEYS = {
    "Open",
    "New",
    "Cut",
    "Copy",
    "Paste",
    "Rename",
    "Delete",
    "Properties",
    "Refresh",
    "View",
    "Status",
    "Saved",
    "Custom",
    "Classic",
}

PROTECTED = [
    "%s",
    "%d",
    "menu.ini",
    "Windhawk",
    "Explorer",
    "Shift",
    "Ctrl",
    "TPM_NOANIMATION",
    "animationOpen/Close",
    "<storage>\\themes\\<name>.ini",
    "Windows 11",
    "Windows 10",
]


def google_code(tag: str) -> str:
    if tag == "nb":
        return "no"
    if tag == "pt-BR":
        return "pt"
    return tag


def protect(text: str) -> tuple[str, dict[str, str]]:
    mapping: dict[str, str] = {}
    result = text
    for index, token in enumerate(sorted(PROTECTED, key=len, reverse=True)):
        if token not in result:
            continue
        sentinel = f"ZXQ{index}QXZ"
        result = result.replace(token, sentinel)
        mapping[sentinel] = token
    return result, mapping


def restore(text: str, mapping: dict[str, str]) -> str | None:
    for sentinel, token in mapping.items():
        if sentinel not in text:
            return None
    for sentinel, token in mapping.items():
        text = text.replace(sentinel, token)
    return text


def _http_json(url: str) -> object:
    request = urllib.request.Request(
        url, headers={"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
    )
    with urllib.request.urlopen(request, timeout=45) as response:
        return json.loads(response.read().decode("utf-8"))


def translate_batch(texts: list[str], tag: str) -> list[str] | None:
    translated: list[str] = []
    prepared: list[str] = []
    mappings: list[dict[str, str]] = []
    for text in texts:
        if text in CONTEXT_KEYS:
            text_to_send = text + " (menu command)"
        else:
            text_to_send = text
        prepared_text, mapping = protect(text_to_send)
        # Request the context only to disambiguate; strip the translated
        # parenthetical afterwards.
        prepared.append(prepared_text)
        mappings.append(mapping)

    joined = "\n@@\n".join(prepared)
    url = GOOGLE.format(
        tl=urllib.parse.quote(google_code(tag)),
        q=urllib.parse.quote(joined),
    )
    try:
        data = _http_json(url)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return None
    if not isinstance(data, list) or not data:
        return None
    out = data[0]
    if not isinstance(out, str):
        return None
    parts = out.split("\n@@\n")
    if len(parts) != len(texts):
        return None
    for source, part, mapping in zip(texts, parts, mappings):
        restored = restore(part, mapping)
        if restored is None:
            return None
        if source in CONTEXT_KEYS:
            restored = re.sub(r"\s*\([^()]*\)\s*$", "", restored).strip()
        translated.append(restored)
    return translated


def translate_one(text: str, tag: str) -> str | None:
    if text in CONTEXT_KEYS:
        text_to_send = text + " (menu command)"
    else:
        text_to_send = text
    prepared, mapping = protect(text_to_send)
    url = MYMEMORY.format(
        q=urllib.parse.quote(prepared), tl=urllib.parse.quote(google_code(tag))
    )
    try:
        data = _http_json(url)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError):
        return None
    if not isinstance(data, dict):
        return None
    value = data.get("responseData", {}).get("translatedText")
    if not isinstance(value, str) or not value:
        return None
    restored = restore(value, mapping)
    if restored is None:
        return None
    if text in CONTEXT_KEYS:
        restored = re.sub(r"\s*\([^()]*\)\s*$", "", restored).strip()
    return restored


def pending_keys(data: dict, tag: str) -> list[dict]:
    translations = data.setdefault("translations", {}).setdefault(tag, {})
    pending = []
    for entry in data["keys"]:
        key = entry["key"]
        if entry.get("identity") or key.startswith(SKIP_PREFIXES):
            continue
        if translations.get(key):
            continue
        pending.append(entry)
    return pending


def translate_language(data: dict, tag: str, dry_run: bool) -> int:
    entries = pending_keys(data, tag)
    if not entries:
        return 0
    translations = data.setdefault("translations", {}).setdefault(tag, {})
    written = 0
    index = 0
    while index < len(entries):
        batch = entries[index : index + 12]
        texts = [entry["english"] for entry in batch]
        result = None
        for attempt in range(4):
            result = translate_batch(texts, tag)
            if result is not None:
                break
            time.sleep(2.0 * (attempt + 1))
        if result is None:
            # Fall back to individually translated values. MyMemory has a
            # much smaller limit; skip entries it refuses and leave English.
            result = []
            for text in texts:
                value = None
                for attempt in range(2):
                    value = translate_one(text, tag)
                    if value:
                        break
                    time.sleep(1.5)
                result.append(value)
        for entry, value in zip(batch, result):
            if value:
                translations[entry["key"]] = value
                written += 1
                if not dry_run:
                    print(
                        f"{tag}: {entry['key']} -> {value[:70]}",
                        file=sys.stderr,
                    )
        index += len(batch)
        time.sleep(0.7)
    return written


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--languages",
        help="comma-separated tags; default is every non-English language",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    data = json.loads(STRINGS_PATH.read_text(encoding="utf-8"))
    if args.languages:
        wanted = {tag.strip() for tag in args.languages.split(",") if tag.strip()}
    else:
        wanted = {
            str(language["tag"])
            for language in data["languages"]
            if language["tag"] != "en"
        }
    for language in data["languages"]:
        tag = str(language["tag"])
        if tag not in wanted or tag == "en":
            continue
        written = translate_language(data, tag, args.dry_run)
        print(f"{tag}: {written} translations", file=sys.stderr)
        if not args.dry_run:
            STRINGS_PATH.write_text(
                json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
