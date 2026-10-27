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

        for required in ("@meta:name", "@meta:description"):
            if required not in key_map:
                errors.append(f"metadata key missing from catalog: {required}")
        try:
            body, _, _ = _settings_body(source_text)
        except ValueError:
            errors.append("Windhawk settings block markers are missing")
        else:
            for chunk in parse_settings_chunks(body):
                setting_key = _setting_key(chunk["header"])
                if not setting_key:
                    continue
                for suffix in ("name", "description"):
                    required = f"@meta:setting.{setting_key}.{suffix}"
                    if required not in key_map:
                        errors.append(
                            f"metadata key missing from catalog: {required}"
                        )
                if setting_key != "language":
                    for option_key, _ in chunk["options"]:
                        required = f"@meta:option.{setting_key}.{option_key}"
                        if required not in key_map:
                            errors.append(
                                f"metadata key missing from catalog: {required}"
                            )
                elif "@meta:language.auto" not in key_map:
                    errors.append(
                        "metadata key missing from catalog: @meta:language.auto"
                    )

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
    # Metadata keys are used by the Windhawk settings block only; keeping
    # them out of the runtime blobs saves a large amount of mod size.
    keys = [
        entry
        for entry in data.get("keys", [])
        if not str(entry.get("key", "")).startswith("@meta:")
    ]
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
    return generate_windhawk_metadata_block(data)


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
    body, body_start, body_end = _settings_body(source_text)
    generated_body = generate_windhawk_settings_body(data, body)
    source_text = (
        source_text[: body_start - 2]
        + "/*\n"
        + generated_body
        + "\n*/"
        + source_text[body_end + 2 :]
    )
    return source_text



def _yaml_unquote(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        try:
            return json.loads(value)
        except json.JSONDecodeError:
            return value[1:-1]
    if len(value) >= 2 and value[0] == "'" and value[-1] == "'":
        return value[1:-1].replace("''", "'")
    return value


def _yaml_quote(value: str) -> str:
    value = str(value)
    if value == "":
        return '""'
    needs_quote = (
        value != value.strip()
        or ": " in value
        or " #" in value
        or value[0] in "-?:,[]{}#&*!|>'\"%@`"
        or "\n" in value
        or "\r" in value
        or "\t" in value
    )
    if not needs_quote:
        return value
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


def _settings_body(source_text: str) -> tuple[str, int, int]:
    start = source_text.index("// ==WindhawkModSettings==")
    body_start = source_text.index("/*", start) + 2
    body_end = source_text.index("*/", body_start)
    return source_text[body_start:body_end], body_start, body_end


def parse_settings_chunks(body: str) -> list[dict[str, Any]]:
    chunks: list[dict[str, Any]] = []
    current: dict[str, Any] | None = None
    skip_localized_options = False
    in_english_options = False
    for line in body.splitlines():
        if line.startswith("- "):
            if current is not None:
                chunks.append(current)
            current = {
                "header": line,
                "name": "",
                "description": "",
                "options": [],
                "extra": [],
            }
            skip_localized_options = False
            in_english_options = False
            continue
        if current is None:
            continue

        localized_name = re.match(r"^\s*\$name:[A-Za-z-]+:\s*(.*)$", line)
        localized_desc = re.match(
            r"^\s*\$description:[A-Za-z-]+:\s*(.*)$", line
        )
        localized_options = re.match(r"^\s*\$options:[A-Za-z-]+:\s*$", line)
        if localized_name or localized_desc:
            skip_localized_options = False
            in_english_options = False
            continue
        if localized_options:
            skip_localized_options = True
            in_english_options = False
            continue
        if skip_localized_options:
            if re.match(r"^\s*-\s+", line):
                continue
            skip_localized_options = False

        english_name = re.match(r"^\s*\$name:\s*(.*)$", line)
        english_desc = re.match(r"^\s*\$description:\s*(.*)$", line)
        if english_name:
            current["name"] = _yaml_unquote(english_name.group(1))
            in_english_options = False
            continue
        if english_desc:
            current["description"] = _yaml_unquote(english_desc.group(1))
            in_english_options = False
            continue
        if re.match(r"^\s*\$options:\s*$", line):
            in_english_options = True
            continue
        if in_english_options:
            option = re.match(r"^\s*-\s+(.*?):\s*(.*)$", line)
            if option:
                current["options"].append(
                    (_yaml_unquote(option.group(1)), _yaml_unquote(option.group(2)))
                )
                continue
            in_english_options = False
        current["extra"].append(line)
    if current is not None:
        chunks.append(current)
    return chunks


def _setting_key(header: str) -> str:
    match = re.match(r"^-\s+([A-Za-z0-9_]+):", header)
    return match.group(1) if match else ""


def _translation(
    data: dict[str, Any], tag: str, key: str, fallback: str
) -> str:
    if tag == "en":
        return fallback
    translations = data.get("translations", {}).get(tag, {})
    value = translations.get(key)
    if value:
        return str(value)
    entry = _key_entries(data).get(key)
    if entry and entry.get("identity"):
        return fallback
    return fallback


def _has_translation(data: dict[str, Any], tag: str, key: str) -> bool:
    if tag == "en":
        return False
    return bool(data.get("translations", {}).get(tag, {}).get(key))


def generate_windhawk_metadata_block(data: dict[str, Any]) -> str:
    lines: list[str] = []
    base_name = _key_entries(data).get("@meta:name", {}).get(
        "english", "Custom Cached Context Menu"
    )
    base_desc = _key_entries(data).get("@meta:description", {}).get("english", "")
    for language in data.get("languages", []):
        tag = str(language["tag"])
        if tag == "en":
            continue
        name = _translation(data, tag, "@meta:name", base_name)
        desc = _translation(data, tag, "@meta:description", base_desc)
        if name != base_name:
            lines.append(
                f"// @name:{language['windhawk']} "
                + name.replace("\n", " ").replace("\r", " ")
            )
        if desc != base_desc:
            lines.append(
                f"// @description:{language['windhawk']} "
                + desc.replace("\n", " ").replace("\r", " ")
            )
    return "\n".join(lines) if lines else "// Localized metadata is generated here."


def generate_windhawk_settings_body(
    data: dict[str, Any], body: str
) -> str:
    chunks = parse_settings_chunks(body)
    languages = data.get("languages", [])
    native_names = {
        str(language["tag"]): str(language.get("native", language["tag"]))
        for language in languages
    }
    lines: list[str] = []
    for chunk in chunks:
        key = _setting_key(chunk["header"])
        lines.append(chunk["header"])
        name_key = f"@meta:setting.{key}.name"
        desc_key = f"@meta:setting.{key}.description"
        english_name = chunk["name"]
        english_desc = chunk["description"]
        if english_name:
            lines.append(f"  $name: {_yaml_quote(english_name)}")
            for language in languages:
                tag = str(language["tag"])
                translated = _translation(data, tag, name_key, english_name)
                if translated != english_name:
                    lines.append(
                        f"  $name:{language['windhawk']}: "
                        + _yaml_quote(translated)
                    )
        if english_desc:
            lines.append(f"  $description: {_yaml_quote(english_desc)}")
            for language in languages:
                tag = str(language["tag"])
                translated = _translation(data, tag, desc_key, english_desc)
                if translated != english_desc:
                    lines.append(
                        f"  $description:{language['windhawk']}: "
                        + _yaml_quote(translated)
                    )
        if chunk["options"]:
            lines.append("  $options:")
            for option_key, option_label in chunk["options"]:
                lines.append(
                    f"  - {_yaml_quote(option_key)}: "
                    + _yaml_quote(option_label)
                )
            for language in languages:
                tag = str(language["tag"])
                if tag == "en":
                    continue
                # The language picker shows native names; only "auto" is
                # translated.
                if key == "language":
                    auto = _translation(
                        data, tag, "@meta:language.auto", "Automatic (match Windows)"
                    )
                    if not _has_translation(data, tag, "@meta:language.auto"):
                        continue
                    lines.append(f"  $options:{language['windhawk']}:")
                    for option_key, option_label in chunk["options"]:
                        label = (
                            auto
                            if option_key == "auto"
                            else native_names.get(option_key, option_label)
                        )
                        lines.append(
                            f"  - {_yaml_quote(option_key)}: "
                            + _yaml_quote(label)
                        )
                    continue
                option_keys = [
                    f"@meta:option.{key}.{option_key}"
                    for option_key, _ in chunk["options"]
                ]
                if not any(_has_translation(data, tag, option_key)
                           for option_key in option_keys):
                    continue
                lines.append(f"  $options:{language['windhawk']}:")
                for option_key, option_label in chunk["options"]:
                    translated = _translation(
                        data,
                        tag,
                        f"@meta:option.{key}.{option_key}",
                        option_label,
                    )
                    lines.append(
                        f"  - {_yaml_quote(option_key)}: "
                        + _yaml_quote(translated)
                    )
        for extra in chunk["extra"]:
            lines.append(extra)
    return "\n".join(lines)

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
