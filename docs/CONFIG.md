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
`kConfigSchemaVersion` is rewritten with `CanonicalizeConfig`.

Key symbols (all in `mod.wh.cpp`, namespace `cmo`):

- `ConfigSchemaEntry kAppearanceSchema[]` — the single source of truth for flat
  settings.
- `ApplyAppearanceValue` — the setters; the schema gate rejects unknown keys.
- `GenerateDefaultConfigText` — first-run file, built from the schema.
- `ReadSchemaVersion` / `CanonicalizeConfig` — canonical rewrite on schema updates.
- `RulesConfig` — the parsed snapshot (`appearance`, `rules`, `commands`,
  `submenus`, `overrides`, `schemaVersion`, `revision`).
- `ResolveAppearance` / `ResolveLayoutMetrics` — theme and DPI view.
- `ApplyRulesConfigToModel` — rules → overrides → custom items at open time.

## Adding a flat appearance setting

1. Add the field and its default to `struct Appearance`.
2. Add a row to `kAppearanceSchema` (section, key, type, default,
   group, valid values, min/max, description, `sinceVersion`, `unset`) and bump
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

`[appearance]` — base look. Colors are `R, G, B, A` decimals (0–255 each;
alpha optional, default 255). `#RRGGBB`/`#AARRGGBB` are also accepted on read.

The generated file is a **canonical settings list**: one `[appearance]` section
with group comment headers, almost every setting active at its default, and a blank
line between groups. Four keys have "unset" semantics (`itemPadding`,
`cornerRadii`, `markerColor`, `headerColor`) and are emitted commented with a
note, because activating them would pin derived behavior. When the schema
version grows, the file is rewritten into this layout with your values kept;
structured sections are preserved verbatim. A current-version file is never
rewritten.

| Key | Type | Default | Values / range |
|---|---|---|---|
| background | color | `#F01E1E1E` | panel background and blur tint |
| blur | bool | `true` | blur the screen behind the menu |
| blurStrength | int | `12` | 0–64 |
| cornerRadius | int | `8` | 0–256 px |
| border | color | `#22FFFFFF` | border color |
| borderWidth | int | `1` | 0–64 px (0 hides it) |
| shadow | bool | `true` | draw the drop shadow |
| shadowSize | int | `12` | 0–64 px spread |
| font | font | `Segoe UI, 9` | face, size |
| itemHeight | int | `28` | 1–256 px |
| iconSize | int | `16` | 1–256 px |
| padding | int | `6` | 0–256 px base padding |
| separator | color | `#18FFFFFF` | separator line |
| hoverBackground | color | `#14FFFFFF` | hovered item |
| pressedBackground | color | `#22FFFFFF` | pressed item |
| textColor | color | `#FFFFFFFF` | item text |
| disabledTextColor | color | `#66FFFFFF` | disabled item text |
| submenuArrow | color | `#99FFFFFF` | submenu arrow |
| animation | enum | `none` | none, fade, slide |
| animationDuration | int | `120` | 0–10000 ms |
| verticalPadding | int | `4` | 0–256 px above/below items |
| minWidth | int | `0` | 0–4096 px (0 = automatic) |
| maxWidth | int | `0` | 0–4096 px (0 = unlimited) |
| itemPadding | int | `6` | 0–256 px; defaults to padding |
| separatorSpacing | int | `0` | 0–256 px above/below separators |
| markerWidth | int | `14` | 0–256 px marker column |
| fontWeight | enum | `normal` | normal, semibold, bold |
| fontStyle | enum | `normal` | normal, italic |
| cornerRadii | int list | `2, 4, 6, 8` | tl,tr,br,bl; overrides cornerradius |
| shadowOpacity | int | `120` | 0–255 |
| shadowBlur | int | `12` | 0–64 px |
| marker | enum | `dot` | dot, check, bar, none |
| markerColor | color | `#FFFFFFFF` | defaults to textcolor |
| headerColor | color | `#66FFFFFF` | defaults to disabledtextcolor |
| showAccelerators | enum | `underline` | underline, strip, raw |

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
- On a schema update the file is rewritten into the canonical layout: one
  `[appearance]`, your values carried over, colors normalized to `R, G, B, A`,
  structured sections preserved verbatim, and `[meta] schemaVersion` updated.
  Comments inside `[appearance]` are regenerated; comments in structured
  sections are preserved. A file that fails to parse is never written to.

## Testing checklist

- `bash tests/run.sh` from the repository root.
- Drift: every schema default is accepted; unknown keys are rejected.
- Generated file: every key, default, `[meta]`, and the example blocks.
- Migration: canonical rewrite, values preserved, one `[appearance]`,
  structured blocks kept, malformed file untouched.
- Guide: this file mentions every schema key.
