# Context Menu Overhaul — Configuration Guide

For maintainers and anyone extending `menu.ini`. The user-facing summary lives
in the README; this document explains how the configuration pipeline works and
exactly what to touch when you add something.

## The pipeline

```
file bytes
  → DecodeConfigBytes        UTF-8 BOM / UTF-16 BOM / BOM-less UTF-8 / BOM-less UTF-16
  → ParseRulesConfig         INI parse; line-numbered errors; last good snapshot kept
  → ApplyAppearanceValue     schema gate + hand-written setters (Appearance)
  → ConfigStore snapshot     immutable RulesConfig + revision, swapped atomically
  → ResolveAppearance        picks base / light / dark
  → ResolveLayoutMetrics     DPI scaling and resolved defaults
  → DrawPanel / HMENU path   renderer or classic menu
```

Lifecycle: `ConfigStore::EnsureLoaded` runs on the first menu open and creates
`menu.ini` from `GenerateDefaultConfigText` when missing; `RefreshIfChanged`
re-reads only when the file's size or last-write time changed. Nothing runs
while menus are closed. After a successful parse, a file older than
`kConfigSchemaVersion` is migrated with `AppendMissingSchemaKeys`.

Key symbols (all in `mod.wh.cpp`, namespace `cmo`):

- `ConfigSchemaEntry kAppearanceSchema[]` — the single source of truth for flat
  settings.
- `ApplyAppearanceValue` — the setters; the schema gate rejects unknown keys.
- `GenerateDefaultConfigText` — first-run file, built from the schema.
- `ReadSchemaVersion` / `AppendMissingSchemaKeys` — migration.
- `RulesConfig` — the parsed snapshot (`appearance`, `rules`, `commands`,
  `submenus`, `overrides`, `schemaVersion`, `revision`).
- `ResolveAppearance` / `ResolveLayoutMetrics` — theme and DPI view.
- `ApplyRulesConfigToModel` — rules → overrides → custom items at open time.

## Adding a flat appearance setting

1. Add the field and its default to `struct Appearance`.
2. Add a row to `kAppearanceSchema` (section, key, type, default,
   valid values, min/max, description, `sinceVersion`) and bump
   `kConfigSchemaVersion`.
3. Add the setter branch in `ApplyAppearanceValue`. The gate already rejects
   keys without a row, so a branch without a row is dead code.
4. If the renderer needs it, scale it in `ResolveLayoutMetrics` (and use
   `metrics.*` in `DrawPanel`, not `appearance.*`).
5. Run `bash tests/run.sh`. The drift test fails if a row's default is not
   accepted, and the generated-file test fails if the key is missing from the
   default file.
6. Add the key to the reference table below — the guide-coverage test fails if
   it is missing.

## Adding a structured feature

1. Add an entry struct (see `Rule`, `CustomCommand`, `CustomSubmenu`,
   `ItemOverride`) and a vector on `RulesConfig`.
2. Parse it in `ParseRulesConfig`: extend the `Section` enum, the section
   header branch, and a key branch. Use `ParseBoundedInt` for integers and the
   schema gate for anything appearance-like.
3. Apply it in the open path: `ApplyRulesToModel` (rules),
   `ApplyItemOverrides` (overrides), or `InsertCustomItems` (commands,
   submenus, headers, separators, built-ins).
4. Add a commented example block to `GenerateDefaultConfigText`.
5. Test the parser, the apply path, and the migration behavior; document the
   keys in this guide.

## Key reference

`[appearance]` — base look. Colors are `#RRGGBB` or `#AARRGGBB` (alpha first).

| Key | Type | Default | Values / range |
|---|---|---|---|
| background | color | `#F01E1E1E` | panel background and blur tint |
| blur | bool | `true` | blur the screen behind the menu |
| blurstrength | int | `12` | 0–64 |
| cornerradius | int | `8` | 0–256 px |
| border | color | `#22FFFFFF` | border color |
| borderwidth | int | `1` | 0–64 px (0 hides it) |
| shadow | bool | `true` | draw the drop shadow |
| shadowsize | int | `12` | 0–64 px spread |
| font | font | `Segoe UI, 9` | face, size |
| itemheight | int | `28` | 1–256 px |
| iconsize | int | `16` | 1–256 px |
| padding | int | `6` | 0–256 px base padding |
| separator | color | `#18FFFFFF` | separator line |
| hoverbackground | color | `#14FFFFFF` | hovered item |
| pressedbackground | color | `#22FFFFFF` | pressed item |
| textcolor | color | `#FFFFFFFF` | item text |
| disabledtextcolor | color | `#66FFFFFF` | disabled item text |
| submenuarrow | color | `#99FFFFFF` | submenu arrow |
| animation | enum | `none` | none, fade, slide |
| animationduration | int | `120` | 0–10000 ms |
| verticalpadding | int | `4` | 0–256 px above/below items |
| minwidth | int | `0` | 0–4096 px (0 = automatic) |
| maxwidth | int | `0` | 0–4096 px (0 = unlimited) |
| itempadding | int | `6` | 0–256 px; defaults to padding |
| separatorspacing | int | `0` | 0–256 px above/below separators |
| markerwidth | int | `14` | 0–256 px marker column |
| fontweight | enum | `normal` | normal, semibold, bold |
| fontstyle | enum | `normal` | normal, italic |
| cornerradii | int list | `2, 4, 6, 8` | tl,tr,br,bl; overrides cornerradius |
| shadowopacity | int | `120` | 0–255 |
| shadowblur | int | `12` | 0–64 px |
| marker | enum | `dot` | dot, check, bar, none |
| markercolor | color | `#FFFFFFFF` | defaults to textcolor |
| headercolor | color | `#66FFFFFF` | defaults to disabledtextcolor |
| showaccelerators | enum | `underline` | underline, strip, raw |

`[appearance.light]` and `[appearance.dark]` accept the same keys and override
the base section when that theme is active; keys not listed inherit from
`[appearance]`.

## Structured sections

- `[rules]` — `hide`, `keep`, `move = <predicate> -> "Destination"`.
  Precedence hide > keep > move; the classic-menu fallback is never hidden.
- `[command "Label"]` — `command`, `workingDir`, `icon`, `menu`, `match.*`,
  `runAs` (none/admin), `showWindow`, `separator`, `type`
  (command/separator/header), `action` (run/copypath/opennewwindow/properties).
  Placeholders `%1`, `%*`, `%dir%` and environment variables.
- `[submenu "Name"]` — `icon`, `position` (top/bottom/after:"Label"/
  before:"Label"), `match.*`. Nesting via `menu = A/B` (max 3 levels).
- `[item "Label glob"]` — `match.*`, `icon`, `label`, `marker`. Applied after
  rules; the last matching section wins per key.
- `[meta]` — `schemaVersion`. Managed automatically; do not hand-edit.

## Predicates

`label:` (glob with `*`; `&` accelerators and trailing ellipses ignored),
`verb:`, `ext:`, `scope:` (files, folders, background, desktop, drive),
`multi`, `thirdParty`; combine with `and`. Commands evaluate context
predicates; rules evaluate item predicates.

## Errors and migration

- Errors are logged as `menu.ini:<line>: <message>` and the last good
  configuration stays in effect. Messages name the valid values.
- A file that fails to parse is never written to.
- Migration appends a marked block of commented defaults for keys that do not
  appear anywhere in the file and rewrites only the `schemaVersion` line.
  Existing lines are never modified or reordered, and commented-out keys count
  as present.

## Testing checklist

- `bash tests/run.sh` from the repository root.
- Drift: every schema default is accepted; unknown keys are rejected.
- Generated file: every key, default, `[meta]`, and the example blocks.
- Migration: append-only, no duplicates, version line only, malformed file
  untouched.
- Guide: this file mentions every schema key.
