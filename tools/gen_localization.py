#!/usr/bin/env python3
"""Generate and validate the localization catalog embedded in the mod.

The source of truth is localization/strings.json. The mod is a single file, so
the generator writes the runtime key table, one UTF-8 value blob per language,
and (later) the localized Windhawk metadata block between stable markers.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO_ROOT / "localization" / "strings.json"
DEFAULT_MOD = REPO_ROOT / "custom-cached-context-menu.wh.cpp"

RUNTIME_START = "// [CMO:LocalizationData]"
RUNTIME_END = "// [/CMO:LocalizationData]"
METADATA_START = "// [CMO:WindhawkMetadata]"
METADATA_END = "// [/CMO:WindhawkMetadata]"

FULL_LANGUAGE_TAGS = [
    "ar",
    "az",
    "bn",
    "cs",
    "da",
    "de",
    "el",
    "en",
    "es",
    "fa",
    "fr",
    "hi",
    "hr",
    "ht",
    "hu",
    "id",
    "it",
    "ja",
    "ko",
    "nb",
    "nl",
    "or",
    "pl",
    "pt-BR",
    "pt-PT",
    "ro",
    "ru",
    "sv",
    "ta",
    "th",
    "tr",
    "uk",
    "uz",
    "vi",
    "zh-CN",
    "zh-TW",
]

PLACEHOLDER_RE = re.compile(r"%(?:s|d)")

_CPP_ESCAPE_RE = re.compile(r"\\(?:u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|x[0-9a-fA-F]+|.)")


def load_source(path: pathlib.Path = DEFAULT_SOURCE) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _language_tags(data: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(language["tag"]): language
        for language in data.get("languages", [])
        if "tag" in language
    }


def _key_entries(data: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(entry["key"]): entry
        for entry in data.get("keys", [])
        if "key" in entry
    }


def decode_cpp_wide_literal(text: str) -> str:
    """Decode the escapes we use in wide string literal keys."""

    def decode(match: re.Match[str]) -> str:
        escape = match.group(0)[1:]
        simple = {
            "n": "\n",
            "r": "\r",
            "t": "\t",
            "0": "\0",
            "\\": "\\",
            '"': '"',
            "'": "'",
        }
        if escape in simple:
            return simple[escape]
        if escape.startswith("u") and len(escape) == 5:
            return chr(int(escape[1:], 16))
        if escape.startswith("U") and len(escape) == 9:
            return chr(int(escape[1:], 16))
        if escape.startswith("x") and len(escape) > 1:
            return chr(int(escape[1:], 16))
        return escape

    return _CPP_ESCAPE_RE.sub(decode, text)


def scan_source_keys(source_text: str) -> set[str]:
    """Keys the generator can see literally in the mod source."""
    keys: set[str] = set()

    for pattern in (
        r'\b(?:Tr|FormatTr)\(\s*L"((?:[^"\\]|\\.)*)"',
        r'\b(?:addCommand|addSubmenu|addViewAction|makeCommand|makeViewAction|MakeIntSliderRow)\(\s*L"((?:[^"\\]|\\.)*)"\s*[,)]',
        r'\.displayName\s*=\s*L"((?:[^"\\]|\\.)*)"',
    ):
        for match in re.finditer(pattern, source_text):
            key = decode_cpp_wide_literal(match.group(1))
            if key:
                keys.add(key)

    for match in re.finditer(
        r'\bMakeSettingsItem\(\s*[^,()\n]+,\s*L"((?:[^"\\]|\\.)*)"\s*[,)]',
        source_text,
    ):
        key = decode_cpp_wide_literal(match.group(1))
        if key:
            keys.add(key)

    for match in re.finditer(
        r'\bTrOption\(\s*L"((?:[^"\\]|\\.)*)"\s*,\s*L"((?:[^"\\]|\\.)*)"',
        source_text,
    ):
        keys.add(
            "@option:"
            + decode_cpp_wide_literal(match.group(1))
            + ":"
            + decode_cpp_wide_literal(match.group(2))
        )

    for name, prefix in (
        ("TrEffect", "@effect:"),
        ("TrOverlay", "@overlay:"),
        ("TrTheme", "@theme:"),
    ):
        for match in re.finditer(
            rf'\b{name}\(\s*L"((?:[^"\\]|\\.)*)"', source_text
        ):
            keys.add(prefix + decode_cpp_wide_literal(match.group(1)))

    return keys


def validate_source(
    data: dict[str, Any],
    source_text: str | None = None,
    allow_missing: bool = False,
    require_full_catalog: bool = False,
) -> list[str]:
    errors: list[str] = []
    language_map = _language_tags(data)
    key_map = _key_entries(data)

    languages = data.get("languages", [])
    tags = [language.get("tag") for language in languages]
    if len(tags) != len(set(tags)):
        errors.append("duplicate language tag")
    for language in languages:
        if not language.get("tag"):
            errors.append("language without a tag")
        if not language.get("windhawk"):
            errors.append(f"language {language.get('tag')!r} has no windhawk tag")

    if require_full_catalog:
        if set(tags) != set(FULL_LANGUAGE_TAGS):
            missing = sorted(set(FULL_LANGUAGE_TAGS) - set(tags))
            extra = sorted(set(tags) - set(FULL_LANGUAGE_TAGS))
            if missing:
                errors.append("missing language tags: " + ", ".join(missing))
            if extra:
                errors.append("unknown language tags: " + ", ".join(extra))

    keys = data.get("keys", [])
    seen_keys: set[str] = set()
    for entry in keys:
        key = str(entry.get("key", ""))
        english = str(entry.get("english", ""))
        if not key:
            errors.append("key without a name")
            continue
        if key in seen_keys:
            errors.append(f"duplicate key: {key}")
        seen_keys.add(key)
        if not english:
            errors.append(f"empty English value for key: {key}")

    translations = data.get("translations", {})
    for tag, values in translations.items():
        if tag not in language_map:
            errors.append(f"translations for unknown language: {tag}")
            continue
        if not isinstance(values, dict):
            errors.append(f"translations for {tag} are not an object")
            continue
        for key, value in values.items():
            entry = key_map.get(key)
            if not entry:
                errors.append(f"translation for unknown key: {tag}/{key}")
                continue
            if entry.get("identity"):
                if value != entry.get("english"):
                    errors.append(f"identity key changed: {tag}/{key}")
                continue
            expected = len(PLACEHOLDER_RE.findall(str(entry.get("english", ""))))
            actual = len(PLACEHOLDER_RE.findall(str(value)))
            if expected != actual:
                errors.append(
                    f"placeholder mismatch: {tag}/{key} "
                    f"({expected} expected, {actual} found)"
                )

    if not allow_missing:
        for tag, language in language_map.items():
            if tag == "en":
                continue
            values = translations.get(tag, {})
            for entry in keys:
                key = entry.get("key")
                if entry.get("identity"):
                    continue
                if not values.get(key):
                    errors.append(f"missing translation: {tag}/{key}")

    if source_text is not None:
        for key in sorted(scan_source_keys(source_text)):
            if key not in key_map:
                errors.append(f"runtime key missing from catalog: {key}")

    return errors


def _escape_cpp(value: str) -> str:
    return (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def _language_blob(
    data: dict[str, Any], tag: str, keys: list[dict[str, Any]]
) -> str:
    translations = data.get("translations", {}).get(tag, {})
    entries: list[str] = []
    for entry in keys:
        key = str(entry.get("key", ""))
        if tag == "en" or entry.get("identity"):
            value = str(entry.get("english", ""))
        else:
            value = str(translations.get(key, ""))
        entries.append(value)

    while entries and not entries[-1]:
        entries.pop()
    if not entries:
        return ""
    return "\\037".join(_escape_cpp(value) for value in entries)


def generate_localization_block(data: dict[str, Any]) -> str:
    keys = data.get("keys", [])
    languages = data.get("languages", [])
    lines = ["const LocalizationKey kLocalizationKeys[] = {"]
    for entry in keys:
        lines.append(
            '    {L"' + _escape_cpp(str(entry["key"])) + '", L"'
            + _escape_cpp(str(entry["english"])) + '"},'
        )
    lines.append("};")
    lines.append("")
    lines.append("const LocalizationLanguage kLocalizationLanguages[] = {")
    for language in languages:
        tag = str(language["tag"])
        rtl = "true" if language.get("rtl") else "false"
        blob = _language_blob(data, tag, keys)
        lines.append(f'    {{L"{_escape_cpp(tag)}", {rtl}, "{blob}"}},')
    lines.append("};")
    return "\n".join(lines)


def generate_metadata_block(data: dict[str, Any]) -> str:
    # Filled in by Task 8. The marker stays valid in the meantime.
    return (
        "// Localized @name/@description and Windhawk setting annotations are\n"
        "// generated by tools/gen_localization.py."
    )


def replace_region(text: str, start: str, end: str, body: str) -> str:
    start_at = text.find(start)
    end_at = text.find(end, start_at + len(start))
    if start_at < 0 or end_at < 0:
        raise ValueError(f"missing generated markers: {start} / {end}")
    start_at += len(start)
    return text[:start_at] + "\n" + body.rstrip() + "\n" + text[end_at:]


def generate_mod_text(source_text: str, data: dict[str, Any]) -> str:
    source_text = replace_region(
        source_text, RUNTIME_START, RUNTIME_END,
        generate_localization_block(data),
    )
    if METADATA_START in source_text and METADATA_END in source_text:
        source_text = replace_region(
            source_text, METADATA_START, METADATA_END,
            generate_metadata_block(data),
        )
    return source_text


def _main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, default=DEFAULT_SOURCE)
    parser.add_argument("--mod", type=pathlib.Path, default=DEFAULT_MOD)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--allow-missing-translations", action="store_true")
    args = parser.parse_args(argv)

    data = load_source(args.source)
    source_text = args.mod.read_text(encoding="utf-8")
    errors = validate_source(
        data,
        source_text=source_text,
        allow_missing=args.allow_missing_translations,
        require_full_catalog=True,
    )
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    generated = generate_mod_text(source_text, data)
    if args.check:
        if generated != source_text:
            print(
                "error: generated localization data is stale; "
                "run python3 tools/gen_localization.py",
                file=sys.stderr,
            )
            return 1
        return 0

    args.mod.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
