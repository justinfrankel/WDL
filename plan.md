# AccessKit Linux Integration Plan

## Current Status

- The Linux AccessKit PoC baseline is committed on this branch.
- The in-repo Rust shim lives at `WDL/swell/rust/accesskit_shim` and builds as a `staticlib`.
- The sibling AccessKit dependency is still expected at `../accesskit`.
- Phase 2 text semantics and AT-SPI keyboard event forwarding have been implemented.
- Menu and combo-box AccessKit modeling has been implemented in the working tree.
- Build artifacts are ignored by the repo root `.gitignore`.

## Reproducibility

- WDL baseline commit: `21c7a3f5` (`swell: add linux AccessKit PoC with Rust shim`)
- Text/key event implementation commit: `02b3c1fb` (`various changes`)
- Ignore/debug tooling commit: `9d7c1974` (`added .gitignore and debugging tool for at-spi events`)
- Sibling dependency branch: `../accesskit` branch `swell-unix-activation-fix`
- Sibling dependency commit: `f4778b696747628ea213d10f57c078c23ca0ae90`

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

## Text Semantics

- Single-line SWELL `Edit` controls now expose a `TextInput` node plus a synthetic stable `TextRun` child.
- The `TextInput` node carries focusability, value semantics, `SetTextSelection`, and current text selection.
- The `TextRun` node carries:
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

## Control Coverage

- Top-level window -> `WINDOW`
- Static text -> `LABEL`
- Push button -> `BUTTON` / `DEFAULT_BUTTON`
- Checkbox -> `CHECK_BOX`
- Radio button -> `RADIO_BUTTON`
- Single-line edit -> `TEXT_INPUT` with synthetic `TEXT_RUN`
- Multiline edit -> `MULTILINE_TEXT_INPUT` without multiline-specific text geometry in this phase
- Slider -> `SLIDER`
- Progress -> `PROGRESS_INDICATOR`
- Group box -> `GROUP`
- Dropdown-list combo box -> `COMBO_BOX`
- Editable combo box -> `EDITABLE_COMBO_BOX` with synthetic `TEXT_RUN`
- Window menu bar -> synthetic `MENU_BAR`
- Top-level menu item -> synthetic `MENU_ITEM`
- Popup menu -> synthetic `MENU`
- Popup menu item -> synthetic `MENU_ITEM`, `MENU_ITEM_CHECK_BOX`, or `MENU_ITEM_RADIO`
- Combo dropdown popup -> synthetic `MENU_LIST_POPUP`
- Combo dropdown option -> synthetic `MENU_LIST_OPTION`

## Deferred Work

- Multiline edit text geometry and line navigation semantics.
- Rich text structure.
- Incremental AccessKit tree diffs.
- Slider arrow-key behavior as a separate SWELL widget/keyboard task.
- AT-SPI cache interface support, if the recurring `/org/a11y/atspi/cache` warning proves behaviorally significant.
- A real Linux desktop pass to confirm the Wayland/WSL key-forwarding gate does not duplicate native Orca key events.
- Manual AT-SPI/Orca verification of menu-bar keyboard traversal, submenu traversal, and combo dropdown announcement.
- Generic listbox/listview/tree coverage outside combo popup menus.

## Validation

- Rust shim build:

```sh
cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml
```

- Sample app build:

```sh
make -C WDL/swell/sample_project
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
- Slider accessibility export remains value-focused; slider keyboard behavior is intentionally deferred.
- Rust shim build passed after the menu/combo ABI extension.
- Sample app build passed after adding menu and combo coverage.
- `cargo fmt --check` passed for the Rust shim.
- `git diff --check` passed; Git warned that `WDL/swell/sample_project/res.rc` will be CRLF-normalized when Git touches it.

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

## Assumptions

- Current Linux target is SWELL's existing GDK backend.
- The sibling `../accesskit` checkout remains the active dependency for this branch.
- Single-line `Edit` controls and editable combo boxes are the fully modeled text input targets in this phase.
- `NOACCESSKIT=1` remains the escape hatch for Linux builds that should exclude AccessKit.
