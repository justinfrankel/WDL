# AccessKit Linux Integration Plan

## Current Status

- Linux SWELL AccessKit integration is the active production path on this branch.
- The Rust shim lives at `WDL/swell/rust/accesskit_shim` and builds as a `staticlib`.
- The sibling AccessKit dependency is expected at `../accesskit` on branch `swell-fixes`.
- SWELL C++ still discovers UI state from `HWND__`, builds full AccessKit snapshots for dirty top-level windows, and routes AccessKit actions back through SWELL or provider callbacks.
- Rust owns AccessKit object construction, cached `TreeUpdate` cloning, Unix adapter lifecycle, AT-SPI keyboard forwarding, and cross-thread action queuing.
- One AccessKit host is kept per top-level SWELL window.
- `NOACCESSKIT=1` remains the escape hatch for Linux builds that should exclude AccessKit.

## Implemented Coverage

- Core controls:
  - top-level windows, labels, buttons, default buttons, checkboxes, radio buttons, group boxes
  - single-line and multiline edits with synthetic `TextRun` children
  - dropdown-list and editable combo boxes, including combo popup options
  - sliders, progress indicators, list boxes, list views, report grids, tree views, and tab controls
- Menus:
  - top-level menu bars and menu items
  - popup menus and menu items, including checked and radio-style items
  - access keys, keyboard shortcuts, submenu popup metadata, active descendants, and menu-bar keyboard ownership
- Collections:
  - list/listbox/tree/tab active descendants and action routing
  - report-grid rows, cells, headers, labels, and remembered focused columns
  - ranged export for large/owner-data collections with retained active rows
  - debug-time snapshot reference validation for focus, children, active descendants, and labelled-by targets
- Text and keyboard:
  - text value, text-run geometry, selection, caret state, and `SetTextSelection`
  - AT-SPI key watcher forwarding for focused SWELL windows, gated by Wayland or `SWELL_ACCESSKIT_NOTIFY_KEYS`
- Announcements:
  - provider-owned live announcements with persistent live-region export
  - duplicate-announcement handling
- Provider custom nodes:
  - owner-drawn SWELL child windows can register custom AccessKit nodes through `SWELL_ExtendedAPI`
  - supported provider roles are label, button, default button, checkbox, radio button, slider, progress indicator, and group
  - custom nodes support label/value text, bounds, selected/toggled state, numeric value/range/step/orientation, and click/value/increment/decrement/focus action routing
  - custom nodes do not change keyboard tab order unless the provider explicitly exposes and handles custom focus

## REAPER State

- Audit document: `reaper-accesskit-missing-control-audit.md`
- Audit date: 2026-05-22
- Dialog geometry issues found in Preferences, Actions, and FX browser have been fixed:
  - initial SWELL dialog body collapse fixed by `5c4a69e7`
  - AccessKit report cell bounds fixed by `1d6b38fc`
  - Preferences, Actions, and Browse FX now expose visible child controls with nonzero extents and no missing-reference/fallback diagnostics in debug logs
- Action List report cells now have stable per-column bounds, and empty-label cells no longer advertise click actions.
- Main menus and popup menus are broadly exposed with useful labels/actions.
- Remaining REAPER gap:
  - main-window custom regions still expose mostly generic/unknown containers
  - REAPER needs app-side custom provider callbacks for visible toolbar, transport, master controls, faders, meters, rate/time/status labels, and similar owner-drawn surfaces
  - main-screen tab focusability should not be expanded merely to satisfy accessibility exposure
  - decorative panels, separators, and empty arrange/background regions should remain non-focusable and non-actionable

## Next Work

1. Add REAPER-side custom provider callbacks for the highest-value main-window surfaces.
   - Start with Main toolbar and Transport because current dumps show labelled owner HWNDs but no semantic child controls.
   - Export only visible, user-meaningful controls whose bounds and action targets are known.
   - Use provider-local stable nonzero IDs for each semantic child.
   - Keep controls out of keyboard tab order unless REAPER already owns an equivalent focus concept.

2. Add master/control-surface semantics after toolbar/transport are stable.
   - Expose faders/knobs/meters as slider or progress nodes only when value/range/orientation metadata is reliable.
   - Expose status, rate, tempo, time, and similar read-only displays as labels.
   - Avoid exporting decorative backgrounds or layout-only panels as actionable nodes.

3. Validate with REAPER and the current integrity checks.
   - Build SWELL with debug symbols.
   - Run REAPER with `SWELL_ACCESSKIT_DEBUG=1`.
   - Dump AT-SPI trees with `tools/atspi_dump.py` or pyatspi.
   - Confirm no missing-reference, stale-target, invalid-reference, fallback, failure, or panic diagnostics.
   - Confirm no implementation requires adding main-screen controls to keyboard tab order.

## Validation

Primary commands:

```sh
make -C WDL/swell DEBUG=1 -j2
make -C WDL/swell/sample_project DEBUG=1
cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml
cargo test --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml
cargo fmt --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml --check
git diff --check
```

Runtime/debug tools:

```sh
SWELL_ACCESSKIT_DEBUG=1 ./WDL/swell/sample_project/myapp
SWELL_ACCESSKIT_DEBUG=1 /home/robbie/reaper/reaper
python3 tools/atspi_events.py --all-object-events --seconds 7
python3 tools/atspi_dump.py
```

Known recent results:

- `make -C WDL/swell DEBUG=1 -j2`, `make -C WDL/swell/sample_project DEBUG=1`, `cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml`, and `git diff --check` passed after provider-owned custom nodes were added.
- A timed `SWELL_ACCESSKIT_DEBUG=1` sample launch exported the custom group, button, checkbox, slider, and status label with nonzero bounds.
- A pyatspi probe invoked the sample custom button successfully.
- The sample debug log showed no missing, invalid, fallback, stale, failure, or panic diagnostics.
- Previous validation passed for snapshot reference validation, manual collection edge cases, dialog mnemonic targets, generic trackbar keyboard control, and REAPER dialog geometry/report-cell fixes.
- The recurring AT-SPI cache warning for `/org/a11y/atspi/cache` remains deferred unless it proves behaviorally significant.

## Sample App Coverage

- Menu bar with nested submenu, disabled item, checked item, radio-style item, and tab-delimited shortcut labels.
- Dropdown-list combo and editable combo.
- Single-line and multiline edit controls.
- Slider and progress indicator.
- Single-select and multiselect list boxes.
- Report list and 1500-row owner-data report list.
- Tree and tab controls.
- Provider-owned custom accessibility surface with button, checkbox, slider, and status label nodes.

## Reproducibility Notes

- WDL baseline: `21c7a3f5` (`swell: add linux AccessKit PoC with Rust shim`)
- Snapshot reference validation: `87085b58` (`swell: validate AccessKit snapshot references`)
- Dialog access keys: `7a13d435` and `42e712e5`
- Generic trackbar keyboard control: `bc8e1a24`
- Dialog geometry/report-cell fixes: `5c4a69e7` and `1d6b38fc`
- Provider custom nodes: `889bbbf7` (`swell: add custom AccessKit provider nodes`)
- Sibling dependency branch: `../accesskit` branch `swell-fixes`
- Last recorded sibling dependency commit: `8791d5f7ec4597637172699d95c5fd78c720b7a5`
