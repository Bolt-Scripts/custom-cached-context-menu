# Context Menu Overhaul

A [Windhawk](https://windhawk.net/) mod that replaces the Windows Explorer file
and desktop context menu with an instantly-opening cached menu, then discovers
real shell extension items asynchronously in the background. It can render the
menu itself (DirectComposition/Direct2D, fully configurable via `menu.ini`) or
keep the classic owner-drawn menu as a fallback mode.

## Why

The native context menu is slow because every registered shell extension is
loaded synchronously before the menu can be shown — often hundreds of
milliseconds with a handful of handlers installed. This mod removes that work
from the interactive path:

1. Explorer's menu population call is intercepted and deferred.
2. A cached menu model is shown immediately (target: under 10 ms).
3. The real population runs afterwards, on the same UI thread, and refreshes
   the cache — so the next open already has full third-party handler parity.
4. Common contexts are pre-built in the background shortly after Explorer
   starts.

Hold **Shift** while right-clicking (or use **Show classic menu**) to get the
untouched native menu, including extended verbs.

## Install

1. Install [Windhawk](https://windhawk.net/).
2. Create a new mod and paste `mod.wh.cpp`, or install a published build from
   the Windhawk mod collection once available.
3. Compile and enable the mod for `explorer.exe`.

## Settings

| Setting | Default | Description |
|---|---|---|
| Shift bypass | on | Hold Shift while right-clicking for the native menu. |
| Menu mode | 0 (custom) | 0 = self-rendered menu (automatic fallback to the classic menu after 3 consecutive failures); 1 = classic owner-drawn menu. |
| Show classic menu item | on | Adds a "Show classic menu" entry at the bottom of the menu. |
| Warm-up extensions | common list | File types pre-built at Explorer startup. |
| Warm-up delay | 5 s | Delay before background warm-up starts. |
| Clear cache | off | Turn on to delete cached models; they rebuild on next use. |
| Debug logging | off | Logs timings and diagnostics. |
| Instant menu open | on | Temporarily disables system menu animation (fade and slide) while this mod's menu opens, so it appears instantly. Session-only; restored immediately. |
| Submenu open delay | 150 ms | Hover delay before a submenu opens while the replacement menu is shown; restored afterwards. 0 = instant, -1 = keep the Windows setting. |
| More options submenu | on | Moves Windows extras and third-party shell extension entries into one submenu. |
| More options submenu label | `More options` | Label of that submenu. |
| Windows items to move | Share, Add to Favorites, … | Comma-separated labels or verbs of Windows items to move into the submenu. |

## Menu configuration (`menu.ini`)

In custom mode the menu appearance, item rules, and custom commands are read
from `menu.ini` in the mod's storage directory (created with commented defaults
on first run). The file is written as UTF-8 with a BOM; UTF-8 without a BOM and
UTF-16 (with or without a BOM) are also read. It is checked **when a menu
opens** — nothing runs in the background. Errors are logged as
`menu.ini:<line>: <message>` and the last good configuration stays in effect.
Colors are `#RRGGBB` or `#AARRGGBB`; comments start with `;` (including after a
value).

```ini
[appearance]                ; base appearance
background = #1E1E1EF0      ; also used as the blur tint
blur = true                 ; blur the screen behind the menu
cornerRadius = 8            ; or cornerRadii = tl, tr, br, bl
border = #FFFFFF22
shadow = true
shadowOpacity = 120
shadowBlur = 12
font = Segoe UI, 9
fontWeight = normal         ; normal | semibold | bold
fontStyle = normal          ; normal | italic
itemHeight = 28
iconSize = 16
padding = 6
verticalPadding = 4
minWidth = 0                ; 0 = automatic
maxWidth = 0                ; 0 = unlimited
marker = dot                ; dot | check | bar | none
markerColor = #FFFFFF
markerWidth = 14
separatorSpacing = 0
hoverBackground = #FFFFFF14
textColor = #FFFFFF
headerColor = #66FFFFFF
showAccelerators = underline ; underline | strip | raw
animation = none            ; none | fade | slide

[appearance.light]          ; overrides when light theme is active
background = #F5F5F5F2
textColor = #202020

[rules]
hide = label:"Cast to Device"
keep = label:Share
move = thirdParty -> "More options"

[item "TortoiseSVN*"]       ; per-item overrides
label = SVN
icon = C:\Tools\svn.ico,0
marker = bar

[command "Open in VS Code"]
command = code.exe "%1"
workingDir = %dir%
match.ext = .cs, .cpp
menu = Tools

[command "---"]             ; separator item
type = separator

[command "Copy path"]       ; built-in actions
action = copypath           ; run | copypath | opennewwindow | properties

[submenu "Tools"]
icon = @glyph:E712
position = top
```

- **Predicates**: `label:` (glob with `*`, `&`/ellipsis-insensitive), `verb:`,
  `ext:`, `scope:` (`files`, `folders`, `background`, `desktop`, `drive`),
  `multi`, `thirdParty`. Combine with `and`.
- **Rules**: `hide` removes matching items (never the classic-menu fallback),
  `keep` protects items from `move`, `move` sends matching top-level items into
  a submenu (created automatically; the built-in "More options" grouping steps
  aside when move rules exist). Precedence: hide > keep > move.
- **Commands**: `command`, `workingDir`, `icon`, `menu`, `match.*`, `runAs`
  (`none`/`admin`), `showWindow`, `separator`, `type`
  (`command`/`separator`/`header`), `action`
  (`run`/`copypath`/`opennewwindow`/`properties`). Placeholders: `%1`, `%*`,
  `%dir%`, plus environment variables. Command `match.*` supports the context
  predicates (`ext:`, `scope:`, `multi`, `thirdParty`); label/verb predicates
  apply to rules, not to commands. Rules run before custom items are inserted,
  so `hide` does not remove custom commands or submenus.
- **Separators and headers**: `type = separator` draws a line; `type = header`
  draws a non-selectable section label (colored with `headerColor`). Both honor
  `menu` and `match.*` and keep their declaration order inside the menu.
- **Built-in actions**: `action = copypath` copies the selection's paths,
  `opennewwindow` opens the selected folder (or the current folder) in a new
  Explorer window, and `properties` shows shell properties. `run` (the default)
  executes `command`.
- **Per-item overrides** (`[item "Label glob"]`): `label` changes what is drawn
  (rules still match the original shell label), `icon` replaces the icon, and
  `marker` (`dot`/`check`/`bar`/`none`) changes the selection marker. Overrides
  apply recursively to submenu items; when several sections match, the last one
  wins per key.
- **Appearance**: `cornerRadii` sets per-corner radii, `minWidth`/`maxWidth`
  bound the panel (never narrower than the widest item), `verticalPadding` and
  `separatorSpacing` tune spacing, `marker`/`markerColor`/`markerWidth` control
  the selection marker column, `fontWeight`/`fontStyle` style the text, and
  `shadowOpacity`/`shadowBlur` tune the blurred shadow. `showAccelerators`
  chooses whether shell mnemonics are underlined (`underline`), stripped
  (`strip`), or left raw (`raw`).
- **Submenus**: `icon`, `position` (`top`, `bottom`, `after:"Label"`,
  `before:"Label"`), `match.*`. Nesting comes from `menu = A/B` (up to 3
  levels).

## Known limitations

- Menus inside other applications' file dialogs and third-party file managers
  are untouched; the mod targets `explorer.exe`.
- Navigation-pane menus (pinned items, Quick access) are replaced using the
  captured shell menu rather than a prebuilt core model, so warm-up does not
  prebuild them and their first open pays the shell's population cost.
- Exotic shell namespaces (zip folders, Recycle Bin, network locations) use the
  native fallback.
- On Windows 11 the modern XAML menu is suppressed; the classic menu (and this
  replacement) is always used.
- Icons: core actions (Cut/Copy/Rename/Delete/Properties/Open with/Paste/
  Refresh) use built-in icon-font glyphs; extension entries use the bitmap the
  shell provides, a bitmap the extension attaches with `SetMenuItemBitmaps`
  (recorded as it is set — the API has no getter), an icon captured by asking
  the extension to draw the item (owner-draw), the verb's registry `Icon`
  value (matched by verb or display label), or — as a last resort — icon 0
  from a handler DLL matched by key name, file name, or version-info
  company/product name. Everything is composited over the themed menu
  background; checked items keep the checkmark gutter. New templates use the
  shell's file-type icons, and the View modes use glyphs.
- Warm-up models are provisional: they are shown instantly but never overwrite
  a live model, and are refreshed from the real context on first use. Warm-up
  runs once per cache state: processes load the shared `menu-cache.bin` instead
  of re-warming, a session-wide mutex serializes the first cold warm-up across
  Explorer processes, and the start is jittered so many processes do not
  populate at once.
- The first open of an uncached context waits for the shell to populate and
  then shows the full menu — the same first-open cost as the native menu.
  Everything after that is instant, and warm-up keeps common types instant
  from the start. (An instant placeholder menu was tried, but the shell's
  population blocks the UI thread and left it blank.)
- With the **More options submenu** on, discovered items whose verb is not a
  known Windows verb (i.e. third-party shell extensions, however they are
  registered) and the configured Windows extras are grouped into one submenu
  just above the native fallback; separators left dangling are collapsed.
  Inside the submenu, Windows extras come first and third-party handlers last,
  split by a separator, with each group keeping the shell's relative order.
  Labels are matched with `&` accelerators and trailing ellipses stripped, so
  what you type matches what you see. Windows items are additionally detected
  by handler registration (key name, DLL name, or version-info
  company/product, ignoring generic vendor words).
- Full per-item discovery dumps and handler-candidate listings require
  **Debug logging**.
- Core menu labels are English; cached extension labels come from the shell and
  are localized.
- "Paste shortcut" falls back to the untouched native menu on builds where
  the shell object rejects that verb. Sort by and Group by are real submenus
  implemented with the documented `IFolderView2` sort/group APIs. New is a
  real submenu
  built from the registry's ShellNew templates (Folder, Shortcut, and file
  types with the shell's own type icons), created directly without involving
  the shell's own New handler. The standard Windows types (Text Document,
  Bitmap image, Rich Text Document) and Compressed (zipped) Folder are added
  when the registry provides no template — Windows 11 registers them through
  the AppX/MRT system rather than ShellNew.
- Rare entries whose labels cannot be read from the shell (dynamic or
  owner-drawn items) are hidden rather than shown blank, and submenus left
  empty are removed with them; each hidden item is logged as
  `[suspicious dN] ...` so it can be reported.
- Rename, Refresh, and the View modes use the documented `IFolderView2` /
  `IShellView` APIs; the old `FCIDM_*` view command IDs are not used. View,
  Sort by, and Group by show the current selection with a checkmark, and
  Group by includes `(None)`.
- Undo, "Expand/Collapse all groups", and the "More..." pickers are not
  implemented: the shell does not expose them through documented interfaces.
- **Custom mode has no UI Automation support yet** — screen readers should set
  **Menu mode** to 1 (classic menu). Custom mode also requires Direct3D 11 /
  DirectComposition; if the device cannot be created it falls back to the
  classic menu, and three consecutive custom-render failures disable custom
  mode for the session.
- Custom mode's backdrop blur samples the screen once per menu level when it
  opens (it is a blurred snapshot, not a live blur), and the drop shadow is a
  layered approximation. Tall menus do not scroll visually yet (wheel input is
  tracked but the drawing does not offset). Animations are off by default
  (`animation = none`); `fade` and `slide` are compositor-driven.

Menu models persist to disk (`menu-cache.bin` in the mod's storage directory)
and are pre-warmed at Explorer start, so extension items survive restarts.
Handler registration changes are checked when a menu is opened (debounced to
once per 5 seconds), so the mod does no background polling while Explorer is
idle; an hourly revalidation catches handler DLL updates.

## On-device checklist

- Edit `menu.ini` in a UTF-8 editor and a UTF-16 editor: changes apply on the
  next menu open; a bad line logs once and keeps the last configuration.
- Try the new appearance keys, every marker style, per-item overrides,
  separators/headers, and built-in actions.
- Click-away closes the menu; right-clicking elsewhere closes and reopens;
  leaving a submenu via any side closes it; repeated desktop right-clicks work.
- Pinned navigation-pane items show the replacement menu.
- With debug logging off, an idle Explorer session produces no recurring
  warm-up or `menu.ini` log lines.
- Open and close menus about 50 times: Explorer's handle and GPU memory stay
  stable.

## Reporting issues

Enable **Debug logging**, reproduce the problem, and include the Windhawk log
output. It records cache hit/miss, menu preparation time, population and
discovery durations, the chosen item and invocation result, fallback reasons,
and which items the More options submenu moved or kept (with their verbs).

## Development

- Design: `docs/superpowers/specs/2026-10-04-context-menu-overhaul-design.md`
- Implementation plan: `docs/superpowers/plans/2026-10-04-context-menu-overhaul.md`
- Custom renderer design: `docs/superpowers/specs/2026-10-04-custom-menu-renderer-design.md`
- Custom renderer plan: `docs/superpowers/plans/2026-10-04-custom-menu-renderer.md`
- Tests: `bash tests/run.sh` (mingw-w64 cross-compile + Wine), covering
  signatures, models, cache serialization, LRU, invalidation stamps, warm-up
  type normalization, and decision logic.

## License

MIT. Techniques adapted from the `explorer-context-menu-classic` and
`remove-context-menu-items` Windhawk mods, credited in the source.
