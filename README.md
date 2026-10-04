# Context Menu Overhaul

A [Windhawk](https://windhawk.net/) mod that replaces the Windows Explorer file
context menu with an instantly-opening cached menu, then discovers real shell
extension items asynchronously in the background.

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

Hold **Shift** while right-clicking (or use **Show more options**) to get the
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
| Show more options item | on | Adds a fallback entry at the bottom of the menu. |
| Warm-up extensions | common list | File types pre-built at Explorer startup. |
| Warm-up delay | 5 s | Delay before background warm-up starts. |
| Clear cache | off | Turn on to delete cached models; they rebuild on next use. |
| Debug logging | off | Logs timings and diagnostics. |
| Instant menu open | on | Temporarily disables system menu animation (fade and slide) while this mod's menu opens, so it appears instantly. Session-only; restored immediately. |

## Known limitations (v1)

- Menus inside other applications' file dialogs and third-party file managers
  are untouched; the mod targets `explorer.exe`.
- Exotic shell namespaces (zip folders, Recycle Bin, network locations) use the
  native fallback.
- On Windows 11 the modern XAML menu is suppressed; the classic menu (and this
  replacement) is always used.
- Icons: core actions (Cut/Copy/Rename/Delete/Properties/Open with/Paste/
  Refresh) use built-in icon-font glyphs; extension entries use the bitmap the
  shell provides, an icon captured by asking the extension to draw the item
  (owner-draw), or the verb's registry `Icon` value (matched by verb or display
  label). Everything is composited over the themed menu background, including
  alpha-less bitmaps where black is the transparent color key; checked items
  keep the checkmark gutter.
- Core menu labels are English; cached extension labels come from the shell and
  are localized.
- "Sort by" / "New" / "Paste shortcut" fall back to the native menu on builds
  where the shell object rejects those verbs.
- Rare entries whose labels cannot be read from the shell (dynamic or
  owner-drawn items) are hidden rather than shown blank, and submenus left
  empty are removed with them; each hidden item is logged as
  `[suspicious dN] ...` so it can be reported.
- Rename, Refresh, and the View modes use the documented `IFolderView2` /
  `IShellView` APIs; the old `FCIDM_*` view command IDs are not used.

Menu models persist to disk (`menu-cache.bin` in the mod's storage directory)
and are pre-warmed at Explorer start, so extension items survive restarts.
Handler registration changes are detected by polling, which keeps the cache
stable during normal shell activity.

## Reporting issues

Enable **Debug logging**, reproduce the problem, and include the Windhawk log
output. It records cache hit/miss, menu preparation time, population and
discovery durations, the chosen item and invocation result, and fallback
reasons.

## Development

- Design: `docs/superpowers/specs/2026-10-04-context-menu-overhaul-design.md`
- Implementation plan: `docs/superpowers/plans/2026-10-04-context-menu-overhaul.md`
- Tests: `bash tests/run.sh` (mingw-w64 cross-compile + Wine), covering
  signatures, models, cache serialization, LRU, invalidation stamps, warm-up
  type normalization, and decision logic.

## License

MIT. Techniques adapted from the `explorer-context-menu-classic` and
`remove-context-menu-items` Windhawk mods, credited in the source.
