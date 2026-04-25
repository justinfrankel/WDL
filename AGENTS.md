# Agent Notes

This repository is a WDL/SWELL checkout with active Linux SWELL AccessKit work. Treat it as a shared working tree: inspect local changes before editing, preserve user work, and keep fixes narrowly scoped.

## Workflow

- Commit after each completed logical change. Keep commits small enough to review independently, and do not mix unrelated fixes in one commit.
- Check `git status --short` before starting and before finishing. If files already have local changes, read the relevant diffs and work with them instead of reverting them.
- Use `rg`/`rg --files` for searching.
- Use `apply_patch` for manual source edits.
- Avoid deleting generated or untracked files unless the user explicitly asks.
- When investigating a crash, capture the exact failing ID or state transition and map it back to the SWELL/AccessKit synthetic ID helpers before patching.

## AccessKit And SWELL

- The main AccessKit bridge is `WDL/swell/swell-accesskit-generic.cpp`.
- Linux generic list view, tree view, and other widget behavior lives primarily in `WDL/swell/swell-wnd-generic.cpp`.
- Shared internal declarations are in `WDL/swell/swell-internal.h`.
- Synthetic AccessKit node IDs must always be exported in the same snapshot that references them as focus, active descendants, children, or labelled-by targets.
- Large or owner-data collections may export only a visible range. If focus or selection can move outside that range, adjust the exported range or fall back to the widget node rather than emitting a missing synthetic node ID.
- Keep C++ changes compatible with the existing style: C-like helpers, early returns, minimal abstraction, and ASCII text.

## Build And Verification

- Build SWELL with:

```bash
make -C WDL/swell -j2
```

- Build the Rust AccessKit shim directly when Rust-side changes are involved:

```bash
cargo build --release --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml
```

- Run Rust shim tests when changing Rust code:

```bash
cargo test --manifest-path WDL/swell/rust/accesskit_shim/Cargo.toml
```

- For runtime accessibility debugging, `SWELL_ACCESSKIT_DEBUG=1` prints tree snapshots and key events.
- The sibling `../accesskit` checkout is expected by the Rust shim dependency setup in this workspace.

## Current Project Context

- REAPER table/list accessibility is sensitive to focus IDs for synthetic rows and cells.
- Combo boxes, menus, edit controls, tree views, tab controls, list views, and report-style grids have custom synthetic node handling.
- Before changing node IDs, verify that the corresponding resolver/action paths still recognize the namespace.
- Before changing collection export behavior, verify count, children, focus, active descendant, and action target paths stay consistent.
