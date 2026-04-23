# AccessKit Linux Integration Plan

## Summary

- Use an in-repo Rust `staticlib` shim under `WDL/swell/rust/accesskit_shim`.
- Keep SWELL C++ responsible for discovering UI state from `HWND__` and routing actions back into SWELL.
- Move AccessKit object ownership, cached `TreeUpdate` cloning, Unix adapter lifecycle, and cross-thread action queuing into Rust.
- Use `WDL/swell/sample_project` as the proof of concept with a label, default button, checkbox, edit field, and slider.

## Reproducibility

- Baseline sibling dependency for phase 2: `../accesskit` branch `swell-unix-activation-fix`
- Baseline sibling dependency commit: `f4778b696747628ea213d10f57c078c23ca0ae90`

## Implementation

- Keep Linux plumbing in `swell-accesskit-generic.cpp`, but make it talk only to a narrow C ABI in `swell-accesskit-shim.h`.
- Expose a snapshot-oriented ABI from Rust:
  - host create/free
  - root window bounds updates
  - focus state updates
  - full-tree commit
  - action polling
  - debug dump string
- Do not mirror raw `accesskit-c` setters. C++ should submit a plain POD snapshot in one call, and Rust should build/cache `TreeUpdate` internally.
- Keep one shim host per top-level SWELL window.
- Keep the first update strategy simple: regenerate and commit the full tree for each dirty top-level window.
- Let Rust cache the last full `TreeUpdate` and clone it for activation and updates.
- Keep AccessKit callback threads away from SWELL state by letting Rust queue actions internally. C++ drains them from the existing SWELL message-loop pump.
- Mark the owning top-level window dirty when accessibility-relevant state changes: focus, show/hide, enable/disable, move/resize, text updates, checkbox state changes, and slider position changes.
- Map the initial SWELL controls to AccessKit roles:
  - top-level window -> `WINDOW`
  - static text -> `LABEL`
  - push button -> `BUTTON` / `DEFAULT_BUTTON`
  - checkbox -> `CHECK_BOX`
  - radio button -> `RADIO_BUTTON`
  - edit -> `TEXT_INPUT` / `MULTILINE_TEXT_INPUT`
  - slider -> `SLIDER`
  - progress -> `PROGRESS_INDICATOR`
  - group box -> `GROUP`
  - combo box -> `COMBO_BOX`
- Handle only a small initial action set:
  - focus -> `SetFocus`
  - click -> reuse SWELL's existing button keyboard activation path
  - set value -> `SetWindowText` for edit controls
  - increment/decrement -> update SWELL trackbar position and send the existing scroll notifications

## PoC

- Extend `sample_project` instead of creating a separate harness.
- Expose a dialog with one status label, one editable text field, one default button, one checkbox, one slider, and Quit.
- Make the button copy the edit text into the status label so accessibility name/value changes are visible.
- Make the checkbox and slider update the same status label so Orca announcements are easy to verify.
- Keep an environment-variable-based debug dump path for the exported cached tree.

## Validation

- Build the in-repo Rust shim with `cargo build --release`.
- Build `WDL/swell/sample_project` against the generated static library.
- Validate manually with AT-SPI enabled, Orca, and the debug dump.

## Assumptions

- Current Linux target is SWELL's existing GDK/X11 backend.
- Wayland-native support is not part of the first PoC because SWELL's host layer is still X11-specific.
- Initial Linux builds are default-on for AccessKit, with `NOACCESSKIT=1` as the escape hatch.
