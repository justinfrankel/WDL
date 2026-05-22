# AccessKit Linux Integration Plan

## Current Status

- The Linux AccessKit PoC baseline is committed on this branch.
- The in-repo Rust shim lives at `WDL/swell/rust/accesskit_shim` and builds as a `staticlib`.
- The sibling AccessKit dependency is still expected at `../accesskit`.
- Phase 2 text semantics and AT-SPI keyboard event forwarding have been implemented.
- Menu, combo-box, list/listview, tree, tab, and label-relation AccessKit modeling has been committed.
- The production AccessKit path now also includes collection/focus hardening through `87085b58`: stale-focus guards, visible-range retention around active rows, active-descendant list focus, listbox export, report-row labels, report-grid cell focus with remembered columns, temp-file debug dumps, shim transition logging, stale-root protection, combo selection cleanup, one-based collection metadata normalization, tree refreshes on expansion, and snapshot reference validation/fallbacks.
- Provider-owned live announcements, persistent live-region export, duplicate-announcement handling, hidden internal popup windows, and multiline text-run export are implemented on the current branch.
- SWELL menu-bar keyboard ownership now handles and swallows F10/menu traversal before application dispatch on the GDK backend.
- The sibling AccessKit dependency now reports non-menu widget mnemonics to AT-SPI with the conventional Alt modifier while preserving bare posted-menu mnemonics.
- SWELL dialog access-key dispatch now handles Alt+mnemonic labels and buttons on Linux, with static-label mnemonics limited to their intended labelled target.
- Generic SWELL trackbars now support keyboard value changes for arrow, page, Home, and End keys.
- Provider-owned AccessKit custom nodes can now be registered for owner-drawn SWELL child windows through `SWELL_ExtendedAPI`, enabling REAPER-side semantic children for custom toolbar/transport/master-surface regions without changing keyboard tab order.
- Build artifacts are ignored by the repo root `.gitignore`.
- A REAPER missing-control audit has been captured in `reaper-accesskit-missing-control-audit.md`. The highest-priority finding is that several REAPER dialogs expose expected AT-SPI nodes but render blank and report zero-sized child extents.

## Reproducibility

- WDL baseline commit: `21c7a3f5` (`swell: add linux AccessKit PoC with Rust shim`)
- Text/key event implementation commit: `02b3c1fb` (`various changes`)
- Ignore/debug tooling commit: `9d7c1974` (`added .gitignore and debugging tool for at-spi events`)
- Menu/combo coverage commit: `12234282` (`add AccessKit menu and combo coverage`)
- Menu, label, list/tree/tab, and debug-build improvements commit: `4ff08a4e` (`Improve SWELL AccessKit menus and labels`)
- Pre-dispatch menu-bar key handling commit: `7d312add` (`Swallow menu bar navigation keys in SWELL`)
- Collection/focus hardening commits:
  - `3eb94421` (`Keep AccessKit collection focus exported`)
  - `786e2677` (`Use active descendant focus for AccessKit lists`)
  - `9ae40211` (`Log AccessKit debug snapshots to temp file`)
  - `a671308e` (`Keep adjacent AccessKit list rows exported`)
  - `059193c7` (`Log AccessKit shim update transitions`)
  - `57dd77cf` (`Export SWELL list boxes to AccessKit`)
  - `33ef8704` (`Label AccessKit grid rows`)
  - `783b48b2` (`Focus AccessKit grid cells in report lists`)
  - `8681beee` (`Track AccessKit grid cell focus columns`)
- Current follow-up commits:
  - `555bd507` (`add AccessKit announcement extension API`)
  - `cdcf6c22` (`Keep AccessKit live region node persistent`)
  - `4ab2bc89` (`Export live announcement text as label value`)
  - `cf3cac18` (`Force repeated live announcements to change`)
  - `d2fd6475` (`Hide internal popup menu windows from AccessKit`)
  - `12fd0288` (`Expose multiline edit text as separate AccessKit runs`)
  - `17e8235f` (`swell: avoid dereferencing stale AccessKit roots`)
  - `0ee422c4` (`swell: keep unfocused combo selections quiet`)
  - `2bedced7` (`swell: preserve collapsed combo selection state`)
  - `b2357349` (`accesskit shim: normalize one-based collection metadata`)
  - `5c62f243` (`swell: refresh tree accessibility on expansion`)
  - `87085b58` (`swell: validate AccessKit snapshot references`)
  - `7a13d435` (`swell: handle dialog access keys`)
  - `42e712e5` (`swell: keep dialog mnemonics on intended targets`)
  - `bc8e1a24` (`Add keyboard control for generic trackbars`)
  - `72400c4c` (`swell: log AccessKit zero geometry diagnostics`)
  - `5c4a69e7` (`swell: fix initial dialog accessibility geometry`)
  - `1d6b38fc` (`swell: fix AccessKit report cell bounds`)
- Sibling dependency branch: `../accesskit` branch `swell-fixes`
- Sibling dependency commit: `8791d5f7ec4597637172699d95c5fd78c720b7a5`

## Implemented Model

- SWELL C++ still discovers UI state from `HWND__` and routes incoming actions back through SWELL.
- Rust owns AccessKit object construction, cached `TreeUpdate` cloning, Unix adapter lifecycle, and cross-thread action queuing.
- The SWELL-to-Rust ABI remains snapshot-oriented:
  - host create/free
  - root window bounds updates
  - focus state updates
  - full-tree commit
  - action polling
  - debug dump string
  - text selection action data
  - AT-SPI keyboard event forwarding
  - appended menu/combo roles and optional node metadata
- The update strategy is still a full-tree rebuild for each dirty top-level window.
- One AccessKit host is kept per top-level SWELL window.
- Actions from AccessKit callbacks are queued in Rust and drained from the SWELL message-loop pump.
- The ABI has been extended append-only with:
  - `MENU_BAR`, `MENU`, `MENU_ITEM`, `MENU_ITEM_CHECK_BOX`, `MENU_ITEM_RADIO`
  - `MENU_LIST_POPUP`, `MENU_LIST_OPTION`, `EDITABLE_COMBO_BOX`
  - `expanded`, `selected`, `has_popup`, `active_descendant`
  - `position_in_set`, `size_of_set`, `access_key`, `keyboard_shortcut`
  - `labelled_by`, collection metadata, scroll metadata, and hierarchy metadata for list/tree/grid/tab nodes

## Text Semantics

- Single-line SWELL `Edit` controls now expose a `TextInput` node plus a synthetic stable `TextRun` child.
- Multiline SWELL `Edit` controls now expose a `MultilineTextInput` node plus one synthetic `TextRun` child per rendered line.
- The `TextInput` node carries focusability, value semantics, `SetTextSelection`, and current text selection.
- `TextRun` nodes carry:
  - text content
  - UTF-8 character lengths
  - character positions
  - character widths
  - coarse text bounds
  - left-to-right text direction
- Selection/caret state comes from existing SWELL edit internals: `cursor_pos`, `sel1`, `sel2`, and `scroll_x`.
- Incoming AccessKit `SetTextSelection` actions are routed to the existing edit selection path.
- `SetValue` for edit controls still maps to `SetWindowText`.
- Dirty scheduling covers focus, text changes, selection changes, keyboard edit mutations, and mouse caret/selection changes.

## Keyboard Event Forwarding

- Orca on WSL/Wayland receives AT-SPI text and caret events from AccessKit, but it also needs AT-SPI key watcher context to decide what to speak.
- GTK testing showed AT-SPI key watcher events are emitted for all focused-window key events, not only edit controls.
- SWELL now mirrors all GDK key press/release events for focused SWELL windows into AT-SPI `DeviceEventController.NotifyListenersAsync`.
- Printable keys include their UTF-8 text; non-text keys use the GDK key name.
- This path is gated by `WAYLAND_DISPLAY` or `SWELL_ACCESSKIT_NOTIFY_KEYS` to reduce duplicate key streams on platforms where Orca already receives native/global key events.
- `SWELL_ACCESSKIT_DEBUG=1` logs forwarded key events and AT-SPI bus/notification failures.

## Menus And Combo Boxes

- Top-level SWELL window menus now expose a synthetic `MENU_BAR` child under the window.
- Menu bar children are synthetic actionable menu item nodes with labels stripped of `&` mnemonics.
- Menu item metadata includes access keys, tab-delimited keyboard shortcuts, disabled state, submenu popups, set position, and set size.
- Checked menu items are exposed as `MENU_ITEM_CHECK_BOX`; checked radio-style menu items are exposed as `MENU_ITEM_RADIO`.
- Separators remain omitted from the accessibility tree.
- Active generic popup menu HWNDs are exposed as synthetic `MENU` roots with menu item children.
- Active combo popup HWNDs are exposed as synthetic `MENU_LIST_POPUP` roots with `MENU_LIST_OPTION` children.
- Popup roots expose active descendants when SWELL has a visible selected menu row.
- Combo boxes expose `has_popup=listbox` and collapsed/expanded state.
- Dropdown-list combos keep value semantics on the combo node and expose option selection through the popup tree.
- Editable combos use `EDITABLE_COMBO_BOX`, keep value semantics, and expose a synthetic `TEXT_RUN` child with text selection geometry.
- Synthetic AccessKit actions can focus/select popup rows and activate popup/menu items through existing SWELL menu activation paths.
- Popup menu node IDs include a per-popup serial so reopening the same SWELL menu creates a distinct accessibility subtree.
- Popup menus derive a label from the owning menu-bar or submenu item where possible.
- F10 focuses the menu bar without opening a popup; Left/Right move among top-level menu headings; Down/Enter/Space open or activate the focused heading; Escape exits menu-bar focus.
- GDK pre-dispatch handling swallows those menu-bar navigation keys before `SWELLAppMain(SWELLAPP_PROCESSMESSAGE)` so the host application does not receive them.

## Labels And Dialog Relations

- SWELL now exports AccessKit `labelled_by` relations for common labelled controls.
- The preferred label source is the preceding visible text `Static` sibling in dialog child order, matching the convention used by Win32 dialog resources and screen readers.
- Geometry-based label matching remains as a constrained fallback and only considers preceding static-text siblings from the same parent.
- List/tree/grid/tab controls are no longer labelled through broad nearby-label heuristics, avoiding incorrect labels such as a later combo label being attached to a dialog tree.

## Control Coverage

- Top-level window -> `WINDOW`
- Static text -> `LABEL`
- Push button -> `BUTTON` / `DEFAULT_BUTTON`
- Checkbox -> `CHECK_BOX`
- Radio button -> `RADIO_BUTTON`
- Single-line edit -> `TEXT_INPUT` with synthetic `TEXT_RUN`
- Multiline edit -> `MULTILINE_TEXT_INPUT` with synthetic per-line `TEXT_RUN` children
- Slider -> `SLIDER`
- Progress -> `PROGRESS_INDICATOR`
- Group box -> `GROUP`
- Provider custom node -> `LABEL`, `BUTTON`, `DEFAULT_BUTTON`, `CHECK_BOX`, `RADIO_BUTTON`, `SLIDER`, `PROGRESS_INDICATOR`, or `GROUP`
- Dropdown-list combo box -> `COMBO_BOX`
- Editable combo box -> `EDITABLE_COMBO_BOX` with synthetic `TEXT_RUN`
- Window menu bar -> synthetic `MENU_BAR`
- Top-level menu item -> synthetic `MENU_ITEM`
- Popup menu -> synthetic `MENU`
- Popup menu item -> synthetic `MENU_ITEM`, `MENU_ITEM_CHECK_BOX`, or `MENU_ITEM_RADIO`
- Combo dropdown popup -> synthetic `MENU_LIST_POPUP`
- Combo dropdown option -> synthetic `MENU_LIST_OPTION`
- List box -> `LIST_BOX`
- List box option -> synthetic `LIST_BOX_OPTION`
- List view -> `LIST`
- List view item -> synthetic `LIST_ITEM`
- Report list view -> `GRID`
- Report row -> synthetic `ROW`
- Report cell -> synthetic `GRID_CELL`
- Report column header -> synthetic `COLUMN_HEADER`
- Tree view -> `TREE`
- Tree item -> synthetic `TREE_ITEM`
- Tab control -> `TAB_LIST`
- Tab item -> synthetic `TAB`

## Completed Milestone: Manual Collection Edge Validation

- Keep this tranche on the existing ABI surface: `selected`, `multiselectable`, `active_descendant`, collection metadata, scroll metadata, and row/column metadata are already the compatibility boundary.
- Preserve the current focus contract:
  - list/listbox/tree/tab containers expose usable descendants;
  - report lists focus grid cells and retain the intended report column;
  - active rows stay exported even when large or owner-data collections use ranged export;
  - snapshots must never reference a synthetic node that is absent from the same snapshot.
- Basic list/listbox navigation appears stable enough to treat as regression coverage rather than redesign work.
- Manual validation covered the still-risky collection paths before widening the model:
  - multiselect collection state/event fidelity;
  - large owner-data report lists that force ranged export and offscreen-focus handling;
  - row/cell action routing;
  - safe focus fallback when an intended synthetic target cannot be exported.
- `87085b58` adds a debug-only integrity pass before each tree commit. It verifies focus IDs, active descendants, children, and labelled-by references, omits invalid child/label references, and falls back invalid focus/active-descendant targets to exported widget nodes.
- No source change was needed from this pass: the sample harness exported valid collection references, AT-SPI action routing succeeded for representative collection nodes, and debug logs showed no missing-reference or fallback diagnostics.
- Treat the direct AT-SPI branch as concluded research, not the implementation path to extend.

## Deferred Work

- Richer multiline edit navigation semantics beyond the current per-line text-run export.
- Rich text structure.
- Incremental AccessKit tree diffs.
- AT-SPI cache interface support, if the recurring `/org/a11y/atspi/cache` warning proves behaviorally significant.
- A real Linux desktop pass to confirm the Wayland/WSL key-forwarding gate does not duplicate native Orca key events.
- Manual auditory Orca verification of live announcements, menu-bar keyboard traversal, submenu traversal, combo dropdown announcement, list/listview/tree navigation, and tab announcement.
- Richer selection metadata beyond the current ABI, if validation shows it is needed after this collection-hardening pass.

## REAPER Missing-Control Audit

- Audit document: `reaper-accesskit-missing-control-audit.md`
- Audit date: 2026-05-22
- Main-screen tab focusability was not used as a coverage source and should not be expanded merely to satisfy accessibility exposure.
- Main menus and popup menus are broadly exposed with useful labels/actions.
- Main-window custom regions still need semantic children for visible controls such as toolbar buttons, transport controls, master controls, faders, meters, and status/rate/time displays.
- Preferences, Actions, and FX browser exposed expected accessible controls but displayed blank dialog bodies and exported zero-sized or invalid child extents. The initial SWELL dialog geometry collapse and AccessKit screen-coordinate export issue have been fixed by `5c4a69e7`.
- Action List report headers and cells now export nonzero per-column bounds, including when REAPER leaves the underlying SWELL column widths at zero; empty-label cells no longer expose click actions as of `1d6b38fc`.
- AccessKit debug validation did not report missing-reference, stale-target, invalid-reference, fallback, failure, or panic diagnostics during the pass.

## Plan For Implementation

1. Reproduce the blank-dialog/zero-extent bug with a minimal debug run.
   - Use Preferences, Actions, and FX browser as the first targets because they all expose AT-SPI children while their dialog bodies render blank.
   - Capture each affected top-level `HWND__`, native window rectangle, client rectangle, SWELL child rectangle, and exported AccessKit bounds.
   - Confirm whether the bad geometry starts in SWELL layout, native/GDK window geometry, or the AccessKit coordinate conversion layer.
   - Completed in this chunk: the failing Preferences run showed zero SWELL child `m_position` rectangles before AccessKit export, followed by a separate AccessKit root-coordinate mismatch after SWELL layout was corrected.

2. Fix dialog body geometry before adding new semantics.
   - Patch the smallest SWELL geometry/layout path that explains the blank rendered body and zero child extents.
   - Verify that existing dialog controls keep their current roles, labels, actions, text/value interfaces, list/tree/table metadata, and focus behavior.
   - Re-run Preferences, Actions, and FX browser under `SWELL_ACCESSKIT_DEBUG=1` and confirm visible controls have non-zero extents.
   - Completed in this chunk: Preferences, Actions, and Browse FX now expose visible child controls with nonzero extents and no missing-reference/fallback diagnostics in the debug logs.

3. Clean up list/table semantics after geometry is trustworthy.
   - Recheck the Action List table row/cell export with valid extents.
   - Remove or suppress actionable empty-label cells where they represent empty/decorative columns rather than invokable content.
   - Verify FX browser tree/list counts, visible ranges, selection, active descendant, and action target IDs.
   - Partially completed in this chunk: Action List report cells now have stable per-column bounds and empty-label cells do not advertise click actions. Browse FX tree/list extents are nonzero; its 248-row plugin list still uses the existing under-1000 full-export policy.

4. Add REAPER main-window custom control semantics without changing tab order.
   - Model visible toolbar and transport buttons as semantic actionable nodes only when their action target can be resolved.
   - Expose master controls, faders, knobs, meters, rate/time/status labels, and similar custom controls with roles/value metadata that match their behavior.
   - Keep decorative panels, separators, and empty arrange/background regions non-focusable and non-actionable.
   - Completed in this chunk: SWELL now exposes a provider-owned custom accessibility node API and routes provider actions. The sample app exports a custom group with button, checkbox, slider, and status label nodes, and an AT-SPI action probe invoked the sample custom button successfully. REAPER still needs app-side provider callbacks for its main-window custom surfaces.

5. Validate with the current integrity checks and a REAPER smoke pass.
   - Build with `make -C WDL/swell DEBUG=1 -j2`.
   - Run REAPER with `SWELL_ACCESSKIT_DEBUG=1`.
   - Dump AT-SPI trees with `tools/atspi_dump.py`.
   - Confirm logs contain no missing-reference, stale-target, invalid-reference, fallback, warning, failure, or panic diagnostics.
   - Confirm no fix requires adding main-screen controls to keyboard tab order.

## Validation

- Rust shim build:

```sh
cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml
```

- Sample app build:

```sh
make -C WDL/swell/sample_project
```

- SWELL debug build:

```sh
make -C WDL/swell DEBUG=1 -j2
```

- Whitespace check:

```sh
git diff --check
```

- Rust formatting check:

```sh
cargo fmt --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml --check
```

- AT-SPI inspection tools:
  - `tools/atspi_events.py` for object event tracing
  - `SWELL_ACCESSKIT_DEBUG=1` for exported tree and key forwarding diagnostics

## Known Validation Results

- `myapp` appears on the AT-SPI bus when accessibility is enabled.
- Orca announces the edit field on focus.
- Orca now speaks arrow-key caret movement in the edit field.
- Orca receives all forwarded SWELL key events through the AT-SPI key watcher path.
- Backspace/Delete receive key watcher context, so Orca can classify text deletion events correctly.
- Checkbox behavior remained stable during text work.
- Slider accessibility export remains value-focused; generic trackbars now handle Left/Down, Right/Up, PageDown/PageUp, Home, and End keyboard value changes.
- 2026-05-20 non-audible AT-SPI/Orca regression pass:
  - `make -C WDL/swell DEBUG=1 -j2` passed with existing debug-symbol objects already up to date.
  - `make -C WDL/swell/sample_project DEBUG=1` passed; the sample target is `WDL/swell/sample_project/myapp`.
  - `cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml` passed.
  - `SWELL_ACCESSKIT_DEBUG=1 ./myapp` launched successfully, exported the sample tree, and wrote a fresh `/tmp/swell-accesskit-debug.log`.
  - The debug log contained no missing, invalid, fallback, stale, warning, failure, or panic diagnostics from the AccessKit snapshot/reference path.
  - `python3 tools/atspi_events.py --all-object-events --seconds 7` captured the sample on the AT-SPI bus, including root child registration, focus, bounds changes, and child additions. The helper was run through `python3` because this environment has `dbus_next` installed but does not have `uv`, and the script file is not executable.
  - A `pyatspi` tree query found the live `myapp` application and verified AT-SPI exposure for the sample frame, tab list/tabs, labelled text entries, dropdown and editable combos, slider, list boxes, report tables, owner-data table, tree, menu bar, and top-level menu items.
  - AT-SPI component `grabFocus()` succeeded for representative tabs, the slider, the single-line edit, listbox, and tree nodes, and focus changes stayed inside exported AccessKit nodes.
  - The only warnings seen during `pyatspi` probing were the existing desktop AT-SPI cache-interface warnings for `/org/a11y/atspi/cache`; these are still tracked as deferred cache-interface work and were not emitted by the SWELL debug integrity pass.
  - Auditory Orca announcement checks and full key-driven interaction coverage remain manual: this execution channel cannot verify spoken output, and it lacks a key-injection tool such as `xdotool`/`xte`.
- Rust shim build passed after the menu/combo ABI extension.
- Sample app build passed after adding menu and combo coverage.
- `cargo fmt --check` passed for the Rust shim.
- `git diff --check` passed; Git warned that `WDL/swell/sample_project/res.rc` will be CRLF-normalized when Git touches it.
- `make -C WDL/swell DEBUG=1` passed after menu, label, list/tree/tab, and pre-dispatch key handling changes.
- `cargo test --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml` passed after the same changes.
- Live-region announcement tests passed after the provider-owned announcement work.
- Multiline text now exports one synthetic text run per rendered line.
- Tree expansion now schedules fresh accessibility snapshots, collapsed combos preserve their selected synthetic option, unfocused combos avoid noisy selected descendants, and the Rust shim normalizes one-based collection metadata before export.
- Snapshot reference validation now guards tree commits against missing focus, active-descendant, child, and labelled-by targets; stale synthetic actions are consumed safely and logged when `SWELL_ACCESSKIT_DEBUG=1`.
- `make -C WDL/swell DEBUG=1 -j2`, `cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml`, `make -C WDL/swell/sample_project`, `cargo test --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml`, and `git diff --check` passed after adding snapshot reference validation.
- Manual collection edge validation passed with `SWELL_ACCESSKIT_DEBUG=1`: multiselect listbox state changed through AT-SPI actions, report grid row/cell focus and click actions succeeded, owner-data row 1496 stayed exported and focusable inside the ranged export, tree/tab action routing stayed valid, and `/tmp/swell-accesskit-debug.log` showed no missing-reference, stale-target, invalid-reference, or fallback diagnostics.
- `make -C WDL/swell DEBUG=1 -j2`, `cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml`, `make -C WDL/swell/sample_project DEBUG=1`, `cargo test --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml`, and `git diff --check` passed after manual collection edge validation.
- REAPER loads the rebuilt debug `libSwell.so` through `/home/robbie/REAPER/libSwell.so -> /home/robbie/src/WDL/WDL/swell/libSwell.so`.
- The sample harness now covers listbox selection, multiselect listbox state, report-grid rows/cells, and a large owner-data report list intended to exercise ranged export plus offscreen active-item retention.
- `cargo test -p accesskit_atspi_common` passed in `../accesskit` after adding Alt-modified AT-SPI keybinding strings for non-menu widget mnemonics.
- `make -C WDL/swell DEBUG=1 -j2`, `make -C WDL/swell/sample_project DEBUG=1`, and `git diff --check` passed after tightening dialog mnemonic targets.
- A timed `SWELL_ACCESSKIT_DEBUG=1` sample launch confirmed the progress indicator keeps its `labelled_by` relation without advertising an unusable access key; interactive Orca keypress coverage remains part of the broader manual AT-SPI pass.
- `make -C WDL/swell DEBUG=1 -j2`, `make -C WDL/swell/sample_project DEBUG=1`, and `git diff --check` passed after the generic trackbar keyboard work. An automated AT-SPI/XTest sample pass focused the Demo slider and confirmed Right, Left, Home, End, PageDown, and PageUp changed the exposed value through `5 -> 6 -> 5 -> 0 -> 10 -> 9 -> 10`; the debug log showed no missing, invalid, fallback, or stale AccessKit diagnostics.
- `make -C WDL/swell DEBUG=1 -j2`, `make -C WDL/swell/sample_project DEBUG=1`, `cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml`, and `git diff --check` passed after adding provider-owned custom nodes. A timed `SWELL_ACCESSKIT_DEBUG=1` sample launch exported the custom group/button/checkbox/slider/status label with nonzero bounds, and a pyatspi action probe invoked the custom button; the debug log showed no missing, invalid, fallback, stale, failure, or panic diagnostics.

## Sample App Coverage

- The sample app now includes a menu bar with:
  - nested submenu coverage
  - disabled item coverage
  - checked item coverage
  - radio-style checked item coverage
  - tab-delimited shortcut labels
- The sample app now includes:
  - a dropdown-list combo with selected value
  - an editable combo with selectable/editable text
  - a provider-owned custom accessibility surface with button, checkbox, slider, and status label nodes
  - a single-select listbox
  - a multiselect listbox with independent selected rows
  - a report list
  - a 1500-row owner-data report list with initial focus near the end of the collection

## Assumptions

- Current Linux target is SWELL's existing GDK backend.
- The sibling `../accesskit` checkout remains the active dependency for this branch.
- Single-line `Edit` controls and editable combo boxes are the fully modeled text input targets in this phase.
- `NOACCESSKIT=1` remains the escape hatch for Linux builds that should exclude AccessKit.
- The AccessKit branch is the production path; direct AT-SPI work is retained as an archived experiment.
