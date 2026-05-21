# REAPER AccessKit Missing-Control Audit

Date: 2026-05-22

## Scope

This audit ran REAPER against this checkout's debug SWELL AccessKit build and inspected the exported AT-SPI tree without treating main-window tab order as coverage. Main-screen controls that are intentionally not focusable or not in tab order should stay that way.

## Environment

- Repo: `/home/robbie/src/WDL`
- REAPER executable: `/home/robbie/reaper/reaper`
- SWELL library link: `/home/robbie/reaper/libSwell.so -> ../src/WDL/WDL/swell/libSwell.so`
- Build command: `make -C WDL/swell DEBUG=1 -j2`
- Runtime flag: `SWELL_ACCESSKIT_DEBUG=1`
- AT-SPI dump tool: `tools/atspi_dump.py`
- Captured logs:
  - `/tmp/reaper-accesskit-debug.out`
  - `/tmp/swell-accesskit-debug.log`

## Validation Summary

- The working tree was clean before and after the audit.
- The SWELL debug build was already up to date.
- REAPER appeared on the AT-SPI bus as service `:1.23`.
- Debug logs had no AccessKit missing-reference, stale-target, invalid-reference, fallback, failure, or panic diagnostics. Search noise came from REAPER changelog text containing words like "fix".
- `tools/atspi_dump.py` showed many SWELL roles as `<unknown>` because the DBus binding used by the script did not expose role-name lookup. Numeric AT-SPI roles were still valid when checked against `pyatspi` role constants.

## Real Missing Support To Patch

- The main REAPER surface exports major custom regions only as generic panels or unknown nodes. The arrange view, ruler area, TCP/MCP/master area, transport group, and main toolbar need richer semantic children.
- Visible main-window controls are not currently exported as distinct semantic controls: transport buttons, toolbar buttons, master FX/routing/mute/solo/mono controls, knobs/faders/meters, and rate/time controls.
- Preferences, Actions, and FX browser dialogs opened visually blank in this run while their AT-SPI trees contained expected controls with zero-sized or otherwise invalid extents.
- The dialog body issue is the highest-priority finding because it makes otherwise present controls unusable to both sighted inspection and coordinate-based accessibility clients.
- The FX browser exported large tree/list contents, but the body controls and items all had zero extents.

## Incomplete Semantics To Improve Later

- The Action List exports table structure, rows, cells, and headers, but row/cell extents are zero.
- Empty third-column cells in the Action List are exposed as actionable empty-label table cells.
- Main menu bar and popup menus are generally useful: menu bar, menu items, labels, actions, and popup children appeared.
- Popup/menu status spacers include at least one empty actionable menu item. This may be decorative/status spacing and should be reviewed separately for screen reader noise.
- Sliders/faders/meters on REAPER custom surfaces still need value, range, orientation, and naming coverage.

## Intentionally Non-Focusable Or Non-Actionable Main-Screen Controls

- Lack of tab focusability on main-screen REAPER controls is not a bug by itself.
- Empty arrange/project regions, background panels, and visual separators should remain non-focusable unless they gain explicit semantic behavior.
- Decorative panels may remain generic containers when they do not expose an interactive or informational contract.

## Non-Issues And Decorative Elements

- Empty arrange/background containers are acceptable as decorative or grouping nodes.
- The AT-SPI dump script's `<unknown>` role display is a tooling limitation for this environment, not proof that AccessKit exported `UNKNOWN` roles.
- The debug log did not show snapshot reference integrity failures.

## Follow-Up Targets

- Reproduce and isolate the blank-dialog/zero-extent issue first with Preferences, Actions, and FX browser.
- Compare visual window/client rectangles against SWELL `HWND__` bounds used by `swell_accesskit_populate_node`.
- Verify whether zero dialog child extents originate in SWELL window layout, GDK/native window geometry, or AccessKit coordinate conversion.
- After dialog geometry is fixed, revisit list/tree/table action routing and table-cell labels in the Action List and FX browser.
- Add semantic children for REAPER main-window custom controls without changing their tab order.
