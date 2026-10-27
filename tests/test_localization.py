import json
import pathlib
import re
import unittest

from tools import gen_localization

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
STRINGS_PATH = REPO_ROOT / "localization" / "strings.json"


class LocalizationSourceTests(unittest.TestCase):
    def test_language_list_is_complete(self):
        source = gen_localization.load_source(STRINGS_PATH)
        tags = [language["tag"] for language in source["languages"]]
        self.assertEqual(len(tags), 36)
        self.assertEqual(len(set(tags)), 36)
        self.assertIn("fr", tags)
        self.assertIn("ar", tags)
        self.assertIn("fa", tags)
        self.assertIn("zh-CN", tags)
        self.assertIn("zh-TW", tags)
        by_tag = {language["tag"]: language for language in source["languages"]}
        self.assertTrue(by_tag["ar"]["rtl"])
        self.assertTrue(by_tag["fa"]["rtl"])
        self.assertFalse(by_tag["fr"]["rtl"])
        self.assertEqual(by_tag["fr"]["windhawk"], "fr-FR")

    def test_duplicate_key_rejected(self):
        data = {
            "languages": [{"tag": "en", "windhawk": "en", "rtl": False}],
            "keys": [
                {"key": "Open", "english": "Open"},
                {"key": "Open", "english": "Open again"},
            ],
            "translations": {},
        }
        errors = gen_localization.validate_source(data)
        self.assertTrue(any("duplicate" in error.lower() for error in errors))

    def test_empty_english_rejected(self):
        data = {
            "languages": [{"tag": "en", "windhawk": "en", "rtl": False}],
            "keys": [{"key": "Open", "english": ""}],
            "translations": {},
        }
        errors = gen_localization.validate_source(data)
        self.assertTrue(any("english" in error.lower() for error in errors))

    def test_placeholder_mismatch_rejected(self):
        data = {
            "languages": [
                {"tag": "en", "windhawk": "en", "rtl": False},
                {"tag": "fr", "windhawk": "fr-FR", "rtl": False},
            ],
            "keys": [{"key": "Reset %s now", "english": "Reset %s now"}],
            "translations": {"fr": {"Reset %s now": "Réinitialiser maintenant"}},
        }
        errors = gen_localization.validate_source(data)
        self.assertTrue(any("placeholder" in error.lower() for error in errors))

    def test_missing_runtime_key_detected(self):
        source = 'Tr(L"Open"); FormatTr(L"Reset %s…", {});'
        data = {
            "languages": [{"tag": "en", "windhawk": "en", "rtl": False}],
            "keys": [],
            "translations": {},
        }
        errors = gen_localization.validate_source(data, source_text=source)
        self.assertTrue(any("Open" in error for error in errors))
        self.assertTrue(any("Reset" in error for error in errors))

    def test_generated_block_has_one_entry_per_key(self):
        data = {
            "languages": [
                {"tag": "en", "windhawk": "en", "rtl": False},
                {"tag": "fr", "windhawk": "fr-FR", "rtl": False},
            ],
            "keys": [
                {"key": "Open", "english": "Open"},
                {"key": "Cut", "english": "Cut"},
            ],
            "translations": {"fr": {"Open": "Ouvrir", "Cut": "Couper"}},
        }
        block = gen_localization.generate_localization_block(data)
        self.assertIn('{L"Open", L"Open"}', block)
        self.assertIn('{L"Cut", L"Cut"}', block)
        self.assertIn("Ouvrir\\037Couper", block)
        self.assertIn('{L"fr", false,', block)

    def test_settings_parser_preserves_settings(self):
        source = gen_localization.load_source(STRINGS_PATH)
        mod_text = (REPO_ROOT / "custom-cached-context-menu.wh.cpp").read_text(
            encoding="utf-8"
        )
        body, _, _ = gen_localization._settings_body(mod_text)
        chunks = gen_localization.parse_settings_chunks(body)
        self.assertGreaterEqual(len(chunks), 19)
        keys = {gen_localization._setting_key(chunk["header"]) for chunk in chunks}
        self.assertIn("theme", keys)
        self.assertIn("language", keys)
        self.assertIn("warmup", keys)

    def test_schema_labels_and_options_are_in_catalog(self):
        source = gen_localization.load_source(STRINGS_PATH)
        keys = {entry["key"] for entry in source["keys"]}
        mod_text = (REPO_ROOT / "custom-cached-context-menu.wh.cpp").read_text(
            encoding="utf-8"
        )
        start = mod_text.index("const ConfigSchemaEntry kAppearanceSchema[] = {")
        end = mod_text.index("};", start)
        body = mod_text[start:end]

        overrides_start = mod_text.index(
            "static const LabelOverride kOverrides[] = {"
        )
        overrides_end = mod_text.index("};", overrides_start)
        overrides = dict(
            re.findall(
                r'\{L"([^"]+)", L"([^"]+)"\}',
                mod_text[overrides_start:overrides_end],
            )
        )

        for match in re.finditer(
            r'\{L"appearance", L"([^"]+)", L"([^"]+)", '
            r"SettingType::(Enum|EffectList),[^}]*?, L\"([^\"]*)\",",
            body,
        ):
            key, _group, setting_type, values = match.groups()
            for value in values.split("|") if values else []:
                if setting_type == "Enum":
                    catalog_key = f"@option:{key}:{value}"
                elif key == "overlay":
                    catalog_key = f"@overlay:{value}"
                else:
                    catalog_key = f"@effect:{value}"
                self.assertIn(catalog_key, keys)

        for match in re.finditer(r'\{L"appearance", L"([^"]+)"', body):
            key = match.group(1)
            lower = key.lower()
            if lower in overrides:
                label = overrides[lower]
            else:
                label = "".join(
                    (" " + char if index and char.isupper() else char)
                    for index, char in enumerate(key)
                )
                label = label[0].upper() + label[1:]
            self.assertIn(label, keys)

    def test_metadata_block_is_localized(self):
        source = gen_localization.load_source(STRINGS_PATH)
        metadata = gen_localization.generate_windhawk_metadata_block(source)
        self.assertIn("// @name:fr-FR", metadata)
        self.assertIn("// @description:fr-FR", metadata)
        mod_text = (REPO_ROOT / "custom-cached-context-menu.wh.cpp").read_text(
            encoding="utf-8"
        )
        body, _, _ = gen_localization._settings_body(mod_text)
        settings = gen_localization.generate_windhawk_settings_body(source, body)
        self.assertIn("$name:fr-FR: Thème", settings)
        self.assertIn("$options:fr-FR:", settings)
        self.assertIn("Langue", settings)

    def test_proper_names_may_be_identity(self):
        data = {
            "languages": [
                {"tag": "en", "windhawk": "en", "rtl": False},
                {"tag": "fr", "windhawk": "fr-FR", "rtl": False},
            ],
            "keys": [
                {"key": "@theme:Nord", "english": "Nord", "identity": True},
            ],
            "translations": {},
        }
        errors = gen_localization.validate_source(data, allow_missing=False)
        self.assertEqual(errors, [])


if __name__ == "__main__":
    unittest.main()
