use accesskit::{
    Action, ActionData, ActionHandler, ActionRequest, ActivationHandler, DeactivationHandler,
    HasPopup, Live, Node, NodeId, Orientation, Rect, Role, TextDirection, TextPosition,
    TextSelection, Toggled, Tree, TreeId, TreeUpdate,
};
use accesskit_unix::Adapter;
use std::collections::VecDeque;
use std::env;
use std::ffi::{c_char, CString};
use std::fs::OpenOptions;
use std::io::Write;
use std::ptr;
use std::slice;
use std::sync::{Arc, Mutex, OnceLock};
use zbus::blocking::{connection::Builder as ConnectionBuilder, Connection, Proxy};

const ROLE_WINDOW: u32 = 1;
const ROLE_LABEL: u32 = 2;
const ROLE_BUTTON: u32 = 3;
const ROLE_DEFAULT_BUTTON: u32 = 4;
const ROLE_CHECK_BOX: u32 = 5;
const ROLE_RADIO_BUTTON: u32 = 6;
const ROLE_TEXT_INPUT: u32 = 7;
const ROLE_MULTILINE_TEXT_INPUT: u32 = 8;
const ROLE_SLIDER: u32 = 9;
const ROLE_PROGRESS_INDICATOR: u32 = 10;
const ROLE_GROUP: u32 = 11;
const ROLE_COMBO_BOX: u32 = 12;
const ROLE_TEXT_RUN: u32 = 13;
const ROLE_MENU_BAR: u32 = 14;
const ROLE_MENU: u32 = 15;
const ROLE_MENU_ITEM: u32 = 16;
const ROLE_MENU_ITEM_CHECK_BOX: u32 = 17;
const ROLE_MENU_ITEM_RADIO: u32 = 18;
const ROLE_MENU_LIST_POPUP: u32 = 19;
const ROLE_MENU_LIST_OPTION: u32 = 20;
const ROLE_EDITABLE_COMBO_BOX: u32 = 21;
const ROLE_LIST_BOX: u32 = 22;
const ROLE_LIST_BOX_OPTION: u32 = 23;
const ROLE_LIST: u32 = 24;
const ROLE_LIST_ITEM: u32 = 25;
const ROLE_GRID: u32 = 26;
const ROLE_ROW: u32 = 27;
const ROLE_GRID_CELL: u32 = 28;
const ROLE_COLUMN_HEADER: u32 = 29;
const ROLE_TREE: u32 = 30;
const ROLE_TREE_ITEM: u32 = 31;
const ROLE_TAB_LIST: u32 = 32;
const ROLE_TAB: u32 = 33;

const ACTION_FOCUS_MASK: u32 = 1u32 << 0;
const ACTION_CLICK_MASK: u32 = 1u32 << 1;
const ACTION_SET_VALUE_MASK: u32 = 1u32 << 2;
const ACTION_INCREMENT_MASK: u32 = 1u32 << 3;
const ACTION_DECREMENT_MASK: u32 = 1u32 << 4;
const ACTION_SET_TEXT_SELECTION_MASK: u32 = 1u32 << 5;
const ACTION_EXPAND_MASK: u32 = 1u32 << 6;
const ACTION_COLLAPSE_MASK: u32 = 1u32 << 7;
const ACTION_SCROLL_INTO_VIEW_MASK: u32 = 1u32 << 8;

const NODE_FLAG_DISABLED: u32 = 1u32 << 0;
const NODE_FLAG_READ_ONLY: u32 = 1u32 << 1;
const NODE_FLAG_HAS_NUMERIC_VALUE: u32 = 1u32 << 2;
const NODE_FLAG_HAS_MIN_NUMERIC_VALUE: u32 = 1u32 << 3;
const NODE_FLAG_HAS_MAX_NUMERIC_VALUE: u32 = 1u32 << 4;
const NODE_FLAG_HAS_NUMERIC_VALUE_STEP: u32 = 1u32 << 5;
const NODE_FLAG_HAS_EXPANDED: u32 = 1u32 << 6;
const NODE_FLAG_EXPANDED: u32 = 1u32 << 7;
const NODE_FLAG_HAS_SELECTED: u32 = 1u32 << 8;
const NODE_FLAG_SELECTED: u32 = 1u32 << 9;
const NODE_FLAG_MULTISELECTABLE: u32 = 1u32 << 10;

const HAS_POPUP_MENU: u32 = 1;
const HAS_POPUP_LISTBOX: u32 = 2;

const TOGGLED_FALSE: u32 = 1;
const TOGGLED_TRUE: u32 = 2;
const TOGGLED_MIXED: u32 = 3;

const ORIENTATION_HORIZONTAL: u32 = 1;
const ORIENTATION_VERTICAL: u32 = 2;

const LIVE_POLITE: u32 = 1;
const LIVE_ASSERTIVE: u32 = 2;

const ACTION_NONE: u32 = 0;
const ACTION_FOCUS: u32 = 1;
const ACTION_CLICK: u32 = 2;
const ACTION_SET_VALUE: u32 = 3;
const ACTION_INCREMENT: u32 = 4;
const ACTION_DECREMENT: u32 = 5;
const ACTION_SET_TEXT_SELECTION: u32 = 6;
const ACTION_EXPAND: u32 = 7;
const ACTION_COLLAPSE: u32 = 8;
const ACTION_SCROLL_INTO_VIEW: u32 = 9;

const ACTION_DATA_NONE: u32 = 0;
const ACTION_DATA_STRING: u32 = 1;
const ACTION_DATA_NUMERIC: u32 = 2;
const ACTION_DATA_TEXT_SELECTION: u32 = 3;

const ATSPI_REGISTRY_BUS_NAME: &str = "org.a11y.atspi.Registry";
const ATSPI_DEVICE_EVENT_CONTROLLER_PATH: &str = "/org/a11y/atspi/registry/deviceeventcontroller";
const ATSPI_DEVICE_EVENT_CONTROLLER_INTERFACE: &str = "org.a11y.atspi.DeviceEventController";

static ATSPI_CONNECTION: OnceLock<Option<Connection>> = OnceLock::new();

#[repr(C)]
pub struct swell_accesskit_rect {
    x0: f64,
    y0: f64,
    x1: f64,
    y1: f64,
}

#[repr(C)]
pub struct swell_accesskit_string_ref {
    ptr: *const c_char,
    len: usize,
}

#[repr(C)]
pub struct swell_accesskit_node {
    id: u64,
    role: u32,
    bounds: swell_accesskit_rect,
    flags: u32,
    action_mask: u32,
    toggled: u32,
    orientation: u32,
    numeric_value: f64,
    min_numeric_value: f64,
    max_numeric_value: f64,
    numeric_value_step: f64,
    child_count: usize,
    children: *const u64,
    text_selection_node: u64,
    text_selection_anchor: usize,
    text_selection_focus: usize,
    character_length_count: usize,
    character_lengths: *const u8,
    character_position_count: usize,
    character_positions: *const f32,
    character_width_count: usize,
    character_widths: *const f32,
    label: swell_accesskit_string_ref,
    value: swell_accesskit_string_ref,
    has_popup: u32,
    active_descendant: u64,
    position_in_set: usize,
    size_of_set: usize,
    access_key: swell_accesskit_string_ref,
    keyboard_shortcut: swell_accesskit_string_ref,
    labelled_by_count: usize,
    labelled_by: *const u64,
    row_count: usize,
    column_count: usize,
    row_index: usize,
    column_index: usize,
    level: usize,
    scroll_x: f64,
    scroll_x_min: f64,
    scroll_x_max: f64,
    scroll_y: f64,
    scroll_y_min: f64,
    scroll_y_max: f64,
    child_action_mask: u32,
    text_selection_anchor_node: u64,
    text_selection_focus_node: u64,
    live: u32,
}

#[repr(C)]
pub struct swell_accesskit_tree_snapshot {
    root_id: u64,
    focus_id: u64,
    node_count: usize,
    nodes: *const swell_accesskit_node,
}

#[repr(C)]
pub struct swell_accesskit_action_request {
    target_node: u64,
    action: u32,
    data_kind: u32,
    string_value: *mut c_char,
    numeric_value: f64,
    text_selection_anchor: usize,
    text_selection_focus: usize,
    text_selection_anchor_node: u64,
    text_selection_focus_node: u64,
}

#[derive(Clone, Debug)]
struct QueuedAction {
    target_node: u64,
    action: u32,
    data_kind: u32,
    string_value: Option<String>,
    numeric_value: f64,
    text_selection_anchor: usize,
    text_selection_focus: usize,
    text_selection_anchor_node: u64,
    text_selection_focus_node: u64,
}

#[derive(Default)]
struct SharedState {
    cached_update: Mutex<Option<TreeUpdate>>,
    actions: Mutex<VecDeque<QueuedAction>>,
}

struct InitialTreeHandler {
    shared: Arc<SharedState>,
}

impl ActivationHandler for InitialTreeHandler {
    fn request_initial_tree(&mut self) -> Option<TreeUpdate> {
        self.shared.cached_update.lock().ok()?.clone()
    }
}

struct ActionQueueHandler {
    shared: Arc<SharedState>,
}

impl ActionHandler for ActionQueueHandler {
    fn do_action(&mut self, request: ActionRequest) {
        let queued = QueuedAction::from_request(request);
        if let Ok(mut actions) = self.shared.actions.lock() {
            actions.push_back(queued);
        }
    }
}

struct NoopDeactivationHandler;

impl DeactivationHandler for NoopDeactivationHandler {
    fn deactivate_accessibility(&mut self) {}
}

#[allow(non_camel_case_types)]
pub struct swell_accesskit_host {
    adapter: Adapter,
    shared: Arc<SharedState>,
    tree_id: TreeId,
}

impl QueuedAction {
    fn from_request(request: ActionRequest) -> Self {
        let mut queued = Self {
            target_node: request.target_node.0,
            action: map_action(request.action),
            data_kind: ACTION_DATA_NONE,
            string_value: None,
            numeric_value: 0.0,
            text_selection_anchor: 0,
            text_selection_focus: 0,
            text_selection_anchor_node: 0,
            text_selection_focus_node: 0,
        };

        if let Some(data) = request.data {
            match data {
                ActionData::Value(value) => {
                    queued.data_kind = ACTION_DATA_STRING;
                    queued.string_value = Some(value.into_string());
                }
                ActionData::NumericValue(value) => {
                    queued.data_kind = ACTION_DATA_NUMERIC;
                    queued.numeric_value = value;
                }
                ActionData::SetTextSelection(selection) => {
                    queued.data_kind = ACTION_DATA_TEXT_SELECTION;
                    queued.text_selection_anchor = selection.anchor.character_index;
                    queued.text_selection_focus = selection.focus.character_index;
                    queued.text_selection_anchor_node = selection.anchor.node.0;
                    queued.text_selection_focus_node = selection.focus.node.0;
                }
                _ => {}
            }
        }

        queued
    }
}

fn map_role(value: u32) -> Role {
    match value {
        ROLE_WINDOW => Role::Window,
        ROLE_LABEL => Role::Label,
        ROLE_BUTTON => Role::Button,
        ROLE_DEFAULT_BUTTON => Role::DefaultButton,
        ROLE_CHECK_BOX => Role::CheckBox,
        ROLE_RADIO_BUTTON => Role::RadioButton,
        ROLE_TEXT_INPUT => Role::TextInput,
        ROLE_MULTILINE_TEXT_INPUT => Role::MultilineTextInput,
        ROLE_SLIDER => Role::Slider,
        ROLE_PROGRESS_INDICATOR => Role::ProgressIndicator,
        ROLE_GROUP => Role::Group,
        ROLE_COMBO_BOX => Role::ComboBox,
        ROLE_TEXT_RUN => Role::TextRun,
        ROLE_MENU_BAR => Role::MenuBar,
        ROLE_MENU => Role::Menu,
        ROLE_MENU_ITEM => Role::MenuItem,
        ROLE_MENU_ITEM_CHECK_BOX => Role::MenuItemCheckBox,
        ROLE_MENU_ITEM_RADIO => Role::MenuItemRadio,
        ROLE_MENU_LIST_POPUP => Role::MenuListPopup,
        ROLE_MENU_LIST_OPTION => Role::MenuListOption,
        ROLE_EDITABLE_COMBO_BOX => Role::EditableComboBox,
        ROLE_LIST_BOX => Role::ListBox,
        ROLE_LIST_BOX_OPTION => Role::ListBoxOption,
        ROLE_LIST => Role::List,
        ROLE_LIST_ITEM => Role::ListItem,
        ROLE_GRID => Role::Grid,
        ROLE_ROW => Role::Row,
        ROLE_GRID_CELL => Role::GridCell,
        ROLE_COLUMN_HEADER => Role::ColumnHeader,
        ROLE_TREE => Role::Tree,
        ROLE_TREE_ITEM => Role::TreeItem,
        ROLE_TAB_LIST => Role::TabList,
        ROLE_TAB => Role::Tab,
        _ => Role::Unknown,
    }
}

fn map_effective_role(value: u32, has_children: bool) -> Role {
    if value == 0 && has_children {
        Role::Group
    } else {
        map_role(value)
    }
}

fn map_action(value: Action) -> u32 {
    match value {
        Action::Focus => ACTION_FOCUS,
        Action::Click => ACTION_CLICK,
        Action::SetValue => ACTION_SET_VALUE,
        Action::Increment => ACTION_INCREMENT,
        Action::Decrement => ACTION_DECREMENT,
        Action::SetTextSelection => ACTION_SET_TEXT_SELECTION,
        Action::Expand => ACTION_EXPAND,
        Action::Collapse => ACTION_COLLAPSE,
        Action::ScrollIntoView => ACTION_SCROLL_INTO_VIEW,
        _ => ACTION_NONE,
    }
}

fn apply_action_mask(node: &mut Node, action_mask: u32) {
    if (action_mask & ACTION_FOCUS_MASK) != 0 {
        node.add_action(Action::Focus);
    }
    if (action_mask & ACTION_CLICK_MASK) != 0 {
        node.add_action(Action::Click);
    }
    if (action_mask & ACTION_SET_VALUE_MASK) != 0 {
        node.add_action(Action::SetValue);
    }
    if (action_mask & ACTION_INCREMENT_MASK) != 0 {
        node.add_action(Action::Increment);
    }
    if (action_mask & ACTION_DECREMENT_MASK) != 0 {
        node.add_action(Action::Decrement);
    }
    if (action_mask & ACTION_SET_TEXT_SELECTION_MASK) != 0 {
        node.add_action(Action::SetTextSelection);
    }
    if (action_mask & ACTION_EXPAND_MASK) != 0 {
        node.add_action(Action::Expand);
    }
    if (action_mask & ACTION_COLLAPSE_MASK) != 0 {
        node.add_action(Action::Collapse);
    }
    if (action_mask & ACTION_SCROLL_INTO_VIEW_MASK) != 0 {
        node.add_action(Action::ScrollIntoView);
    }
}

fn rect_from_ffi(rect: &swell_accesskit_rect) -> Rect {
    Rect {
        x0: rect.x0,
        y0: rect.y0,
        x1: rect.x1,
        y1: rect.y1,
    }
}

unsafe fn string_from_ffi(raw: &swell_accesskit_string_ref) -> Option<String> {
    if raw.ptr.is_null() {
        return None;
    }
    let bytes = slice::from_raw_parts(raw.ptr.cast::<u8>(), raw.len);
    Some(String::from_utf8_lossy(bytes).into_owned())
}

unsafe fn children_from_ffi(raw: &swell_accesskit_node) -> Option<Vec<NodeId>> {
    if raw.child_count == 0 {
        return Some(Vec::new());
    }
    if raw.children.is_null() {
        return None;
    }
    let children = slice::from_raw_parts(raw.children, raw.child_count);
    Some(children.iter().copied().map(NodeId).collect())
}

unsafe fn labelled_by_from_ffi(raw: &swell_accesskit_node) -> Option<Vec<NodeId>> {
    if raw.labelled_by_count == 0 {
        return Some(Vec::new());
    }
    if raw.labelled_by.is_null() {
        return None;
    }
    let labelled_by = slice::from_raw_parts(raw.labelled_by, raw.labelled_by_count);
    Some(labelled_by.iter().copied().map(NodeId).collect())
}

unsafe fn character_lengths_from_ffi(raw: &swell_accesskit_node) -> Option<Vec<u8>> {
    if raw.character_length_count == 0 {
        return Some(Vec::new());
    }
    if raw.character_lengths.is_null() {
        return None;
    }
    Some(slice::from_raw_parts(raw.character_lengths, raw.character_length_count).to_vec())
}

unsafe fn character_positions_from_ffi(raw: &swell_accesskit_node) -> Option<Vec<f32>> {
    if raw.character_position_count == 0 {
        return Some(Vec::new());
    }
    if raw.character_positions.is_null() {
        return None;
    }
    Some(slice::from_raw_parts(raw.character_positions, raw.character_position_count).to_vec())
}

unsafe fn character_widths_from_ffi(raw: &swell_accesskit_node) -> Option<Vec<f32>> {
    if raw.character_width_count == 0 {
        return Some(Vec::new());
    }
    if raw.character_widths.is_null() {
        return None;
    }
    Some(slice::from_raw_parts(raw.character_widths, raw.character_width_count).to_vec())
}

unsafe fn build_tree_update(
    tree_id: TreeId,
    snapshot: &swell_accesskit_tree_snapshot,
) -> Option<TreeUpdate> {
    if snapshot.node_count == 0 || snapshot.nodes.is_null() {
        return None;
    }

    let raw_nodes = slice::from_raw_parts(snapshot.nodes, snapshot.node_count);
    let mut nodes = Vec::with_capacity(raw_nodes.len());

    for raw in raw_nodes {
        let child_ids = children_from_ffi(raw)?;
        let mut node = Node::new(map_effective_role(raw.role, !child_ids.is_empty()));
        if let Some(label) = string_from_ffi(&raw.label) {
            node.set_label(label);
        }
        if let Some(value) = string_from_ffi(&raw.value) {
            node.set_value(value);
        }
        if let Some(access_key) = string_from_ffi(&raw.access_key) {
            node.set_access_key(access_key);
        }
        if let Some(keyboard_shortcut) = string_from_ffi(&raw.keyboard_shortcut) {
            node.set_keyboard_shortcut(keyboard_shortcut);
        }
        let labelled_by = labelled_by_from_ffi(raw)?;
        if !labelled_by.is_empty() {
            node.set_labelled_by(labelled_by);
        }
        if (raw.flags & NODE_FLAG_DISABLED) != 0 {
            node.set_disabled();
        }
        if (raw.flags & NODE_FLAG_READ_ONLY) != 0 {
            node.set_read_only();
        }
        if (raw.flags & NODE_FLAG_HAS_EXPANDED) != 0 {
            node.set_expanded((raw.flags & NODE_FLAG_EXPANDED) != 0);
        }
        if (raw.flags & NODE_FLAG_HAS_SELECTED) != 0 {
            node.set_selected((raw.flags & NODE_FLAG_SELECTED) != 0);
        }
        if (raw.flags & NODE_FLAG_MULTISELECTABLE) != 0 {
            node.set_multiselectable();
        }
        match raw.has_popup {
            HAS_POPUP_MENU => node.set_has_popup(HasPopup::Menu),
            HAS_POPUP_LISTBOX => node.set_has_popup(HasPopup::Listbox),
            _ => {}
        }
        if raw.active_descendant != 0 {
            node.set_active_descendant(NodeId(raw.active_descendant));
        }
        if raw.position_in_set != 0 {
            node.set_position_in_set(raw.position_in_set - 1);
        }
        if raw.size_of_set != 0 {
            node.set_size_of_set(raw.size_of_set);
        }
        if raw.row_count != 0 {
            node.set_row_count(raw.row_count);
        }
        if raw.column_count != 0 {
            node.set_column_count(raw.column_count);
        }
        if raw.row_index != 0 {
            node.set_row_index(raw.row_index);
        }
        if raw.column_index != 0 {
            node.set_column_index(raw.column_index);
        }
        if raw.level != 0 {
            node.set_level(raw.level - 1);
        }
        if raw.scroll_x_max > raw.scroll_x_min {
            node.set_scroll_x(raw.scroll_x);
            node.set_scroll_x_min(raw.scroll_x_min);
            node.set_scroll_x_max(raw.scroll_x_max);
        }
        if raw.scroll_y_max > raw.scroll_y_min {
            node.set_scroll_y(raw.scroll_y);
            node.set_scroll_y_min(raw.scroll_y_min);
            node.set_scroll_y_max(raw.scroll_y_max);
        }
        match raw.toggled {
            TOGGLED_FALSE => node.set_toggled(Toggled::False),
            TOGGLED_TRUE => node.set_toggled(Toggled::True),
            TOGGLED_MIXED => node.set_toggled(Toggled::Mixed),
            _ => {}
        }
        match raw.orientation {
            ORIENTATION_HORIZONTAL => node.set_orientation(Orientation::Horizontal),
            ORIENTATION_VERTICAL => node.set_orientation(Orientation::Vertical),
            _ => {}
        }
        match raw.live {
            LIVE_POLITE => node.set_live(Live::Polite),
            LIVE_ASSERTIVE => node.set_live(Live::Assertive),
            _ => {}
        }
        if (raw.flags & NODE_FLAG_HAS_NUMERIC_VALUE) != 0 {
            node.set_numeric_value(raw.numeric_value);
        }
        if (raw.flags & NODE_FLAG_HAS_MIN_NUMERIC_VALUE) != 0 {
            node.set_min_numeric_value(raw.min_numeric_value);
        }
        if (raw.flags & NODE_FLAG_HAS_MAX_NUMERIC_VALUE) != 0 {
            node.set_max_numeric_value(raw.max_numeric_value);
        }
        if (raw.flags & NODE_FLAG_HAS_NUMERIC_VALUE_STEP) != 0 {
            node.set_numeric_value_step(raw.numeric_value_step);
        }
        if raw.text_selection_node != 0 {
            let anchor_node = if raw.text_selection_anchor_node != 0 {
                raw.text_selection_anchor_node
            } else {
                raw.text_selection_node
            };
            let focus_node = if raw.text_selection_focus_node != 0 {
                raw.text_selection_focus_node
            } else {
                raw.text_selection_node
            };
            node.set_text_selection(TextSelection {
                anchor: TextPosition {
                    node: NodeId(anchor_node),
                    character_index: raw.text_selection_anchor,
                },
                focus: TextPosition {
                    node: NodeId(focus_node),
                    character_index: raw.text_selection_focus,
                },
            });
        }
        if raw.role == ROLE_TEXT_RUN {
            node.set_character_lengths(character_lengths_from_ffi(raw)?);
            node.set_character_positions(character_positions_from_ffi(raw)?);
            node.set_character_widths(character_widths_from_ffi(raw)?);
            node.set_text_direction(TextDirection::LeftToRight);
        }
        apply_action_mask(&mut node, raw.action_mask);
        if (raw.child_action_mask & ACTION_SCROLL_INTO_VIEW_MASK) != 0 {
            node.add_child_action(Action::ScrollIntoView);
        }

        if !child_ids.is_empty() {
            node.set_children(child_ids);
        }

        node.set_bounds(rect_from_ffi(&raw.bounds));
        nodes.push((NodeId(raw.id), node));
    }

    let root = NodeId(snapshot.root_id);
    let focus = if snapshot.focus_id == 0 {
        root
    } else {
        NodeId(snapshot.focus_id)
    };
    Some(TreeUpdate {
        nodes,
        tree: Some(Tree {
            root,
            toolkit_name: Some("SWELL".into()),
            toolkit_version: None,
        }),
        tree_id,
        focus,
    })
}

fn make_c_string(value: &str) -> *mut c_char {
    let sanitized = value.replace('\0', " ");
    CString::new(sanitized).map_or(ptr::null_mut(), CString::into_raw)
}

fn debug_enabled() -> bool {
    env::var_os("SWELL_ACCESSKIT_DEBUG").is_some()
}

fn debug_node_summary(id: NodeId, node: &Node) -> String {
    let label = node.label().unwrap_or("");
    format!(
        "{:?} role={:?} row={:?} pos={:?} selected={:?} active_descendant={:?} label={:?}",
        id,
        node.role(),
        node.row_index(),
        node.position_in_set(),
        node.is_selected(),
        node.active_descendant(),
        label
    )
}

fn debug_find_node<'a>(update: &'a TreeUpdate, id: NodeId) -> Option<&'a Node> {
    update
        .nodes
        .iter()
        .find_map(|(node_id, node)| (*node_id == id).then_some(node))
}

fn debug_update_has_node(update: &TreeUpdate, id: NodeId) -> bool {
    update.nodes.iter().any(|(node_id, _)| *node_id == id)
}

fn debug_log_update_transition(old: Option<&TreeUpdate>, new: &TreeUpdate) {
    if !debug_enabled() {
        return;
    }

    let Ok(mut file) = OpenOptions::new()
        .create(true)
        .append(true)
        .open("/tmp/swell-accesskit-debug.log")
    else {
        return;
    };

    let _ = writeln!(file, "\n----- AccessKit shim transition -----");
    match old {
        Some(old) => {
            if old.focus != new.focus {
                let _ = writeln!(file, "focus {:?} -> {:?}", old.focus, new.focus);
            } else {
                let _ = writeln!(file, "focus unchanged {:?}", new.focus);
            }

            let mut added = 0;
            for (id, node) in &new.nodes {
                if !debug_update_has_node(old, *id) {
                    if added < 32 {
                        let _ = writeln!(file, "added {}", debug_node_summary(*id, node));
                    }
                    added += 1;
                }
            }
            if added > 32 {
                let _ = writeln!(file, "added ... {} more", added - 32);
            }

            let mut removed = 0;
            for (id, node) in &old.nodes {
                if !debug_update_has_node(new, *id) {
                    if removed < 32 {
                        let _ = writeln!(file, "removed {}", debug_node_summary(*id, node));
                    }
                    removed += 1;
                }
            }
            if removed > 32 {
                let _ = writeln!(file, "removed ... {} more", removed - 32);
            }

            for (id, new_node) in &new.nodes {
                let old_node = debug_find_node(old, *id);
                let old_active = old_node.and_then(|node| node.active_descendant());
                let new_active = new_node.active_descendant();
                if old_active != new_active {
                    let _ = writeln!(
                        file,
                        "active-descendant on {:?}: {:?} -> {:?} ({})",
                        id,
                        old_active,
                        new_active,
                        debug_node_summary(*id, new_node)
                    );
                }

                let old_selected = old_node.and_then(|node| node.is_selected());
                let new_selected = new_node.is_selected();
                if old_selected != new_selected {
                    let _ = writeln!(
                        file,
                        "selection on {:?}: {:?} -> {:?} ({})",
                        id,
                        old_selected,
                        new_selected,
                        debug_node_summary(*id, new_node)
                    );
                }
            }
        }
        None => {
            let _ = writeln!(
                file,
                "initial update focus {:?} nodes={}",
                new.focus,
                new.nodes.len()
            );
        }
    }
}

fn atspi_connection() -> Option<&'static Connection> {
    ATSPI_CONNECTION
        .get_or_init(|| {
            let session = match Connection::session() {
                Ok(session) => session,
                Err(error) => {
                    if debug_enabled() {
                        eprintln!("SWELL AccessKit AT-SPI session bus connection failed: {error}");
                    }
                    return None;
                }
            };
            let bus_proxy =
                match Proxy::new(&session, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus") {
                    Ok(proxy) => proxy,
                    Err(error) => {
                        if debug_enabled() {
                            eprintln!("SWELL AccessKit AT-SPI bus proxy creation failed: {error}");
                        }
                        return None;
                    }
                };
            let address: String = match bus_proxy.call("GetAddress", &()) {
                Ok(address) => address,
                Err(error) => {
                    if debug_enabled() {
                        eprintln!("SWELL AccessKit AT-SPI GetAddress failed: {error}");
                    }
                    return None;
                }
            };
            match ConnectionBuilder::address(address.as_str()).and_then(|builder| builder.build()) {
                Ok(connection) => Some(connection),
                Err(error) => {
                    if debug_enabled() {
                        eprintln!("SWELL AccessKit AT-SPI bus connection failed: {error}");
                    }
                    None
                }
            }
        })
        .as_ref()
}

fn notify_atspi_keyboard_event(
    event_type: u32,
    keyval: u32,
    hardware_keycode: u32,
    modifiers: u32,
    timestamp: i32,
    event_string: &str,
    is_text: bool,
) -> zbus::Result<()> {
    let Some(connection) = atspi_connection() else {
        return Ok(());
    };
    let proxy = Proxy::new(
        connection,
        ATSPI_REGISTRY_BUS_NAME,
        ATSPI_DEVICE_EVENT_CONTROLLER_PATH,
        ATSPI_DEVICE_EVENT_CONTROLLER_INTERFACE,
    )?;

    // AT-SPI's DeviceEvent DBus signature is (uiiiisb). Orca uses it to
    // classify subsequent caret events as character/word/line navigation.
    let event = (
        event_type,
        keyval as i32,
        hardware_keycode as i32,
        modifiers as i32,
        timestamp,
        event_string,
        is_text,
    );
    let _: () = proxy.call("NotifyListenersAsync", &(event,))?;
    Ok(())
}

#[no_mangle]
pub extern "C" fn swell_accesskit_host_new(tree_id_bytes: *const u8) -> *mut swell_accesskit_host {
    let _ = tree_id_bytes;
    let tree_id = TreeId::ROOT;
    let shared = Arc::new(SharedState::default());
    let adapter = Adapter::new(
        InitialTreeHandler {
            shared: Arc::clone(&shared),
        },
        ActionQueueHandler {
            shared: Arc::clone(&shared),
        },
        NoopDeactivationHandler,
    );

    Box::into_raw(Box::new(swell_accesskit_host {
        adapter,
        shared,
        tree_id,
    }))
}

#[no_mangle]
pub extern "C" fn swell_accesskit_host_free(host: *mut swell_accesskit_host) {
    if !host.is_null() {
        unsafe {
            drop(Box::from_raw(host));
        }
    }
}

#[no_mangle]
pub extern "C" fn swell_accesskit_host_set_root_window_bounds(
    host: *mut swell_accesskit_host,
    outer: *const swell_accesskit_rect,
    inner: *const swell_accesskit_rect,
) {
    if host.is_null() || outer.is_null() || inner.is_null() {
        return;
    }
    let host = unsafe { &mut *host };
    let outer = unsafe { &*outer };
    let inner = unsafe { &*inner };
    host.adapter
        .set_root_window_bounds(rect_from_ffi(outer), rect_from_ffi(inner));
}

#[no_mangle]
pub extern "C" fn swell_accesskit_host_update_window_focus_state(
    host: *mut swell_accesskit_host,
    is_focused: i32,
) {
    if host.is_null() {
        return;
    }
    let host = unsafe { &mut *host };
    host.adapter.update_window_focus_state(is_focused != 0);
}

#[no_mangle]
pub extern "C" fn swell_accesskit_host_commit_full_tree(
    host: *mut swell_accesskit_host,
    snapshot: *const swell_accesskit_tree_snapshot,
) -> i32 {
    if host.is_null() || snapshot.is_null() {
        return 0;
    }

    let host = unsafe { &mut *host };
    let snapshot = unsafe { &*snapshot };
    let Some(update) = (unsafe { build_tree_update(host.tree_id, snapshot) }) else {
        return 0;
    };

    if let Ok(mut cached) = host.shared.cached_update.lock() {
        debug_log_update_transition(cached.as_ref(), &update);
        *cached = Some(update.clone());
    }

    host.adapter.update_if_active(|| update);
    1
}

#[no_mangle]
pub extern "C" fn swell_accesskit_host_pop_action(
    host: *mut swell_accesskit_host,
    out_action: *mut swell_accesskit_action_request,
) -> i32 {
    if host.is_null() || out_action.is_null() {
        return 0;
    }

    let host = unsafe { &mut *host };
    let mut actions = match host.shared.actions.lock() {
        Ok(actions) => actions,
        Err(_) => return 0,
    };
    let Some(action) = actions.pop_front() else {
        return 0;
    };

    unsafe {
        (*out_action).target_node = action.target_node;
        (*out_action).action = action.action;
        (*out_action).data_kind = action.data_kind;
        (*out_action).numeric_value = action.numeric_value;
        (*out_action).text_selection_anchor = action.text_selection_anchor;
        (*out_action).text_selection_focus = action.text_selection_focus;
        (*out_action).text_selection_anchor_node = action.text_selection_anchor_node;
        (*out_action).text_selection_focus_node = action.text_selection_focus_node;
        (*out_action).string_value = action
            .string_value
            .as_deref()
            .map_or(ptr::null_mut(), make_c_string);
    }
    1
}

#[no_mangle]
pub extern "C" fn swell_accesskit_host_debug(host: *const swell_accesskit_host) -> *mut c_char {
    if host.is_null() {
        return ptr::null_mut();
    }

    let host = unsafe { &*host };
    let queued_actions = host
        .shared
        .actions
        .lock()
        .map_or(0, |actions| actions.len());
    let description = match host.shared.cached_update.lock() {
        Ok(cached) => {
            if let Some(update) = cached.as_ref() {
                format!("queued_actions: {}\n{:#?}", queued_actions, update)
            } else {
                format!("queued_actions: {}\n<no cached tree>", queued_actions)
            }
        }
        Err(_) => String::from("<debug unavailable>"),
    };

    make_c_string(&description)
}

#[no_mangle]
pub extern "C" fn swell_accesskit_string_free(string_value: *mut c_char) {
    if !string_value.is_null() {
        unsafe {
            drop(CString::from_raw(string_value));
        }
    }
}

#[no_mangle]
pub extern "C" fn swell_accesskit_notify_keyboard_event(
    event_type: u32,
    keyval: u32,
    hardware_keycode: u32,
    modifiers: u32,
    timestamp: i32,
    event_string: *const c_char,
    is_text: i32,
) {
    let event_string = if event_string.is_null() {
        String::new()
    } else {
        let mut len = 0;
        unsafe {
            while *event_string.add(len) != 0 {
                len += 1;
            }
            let bytes = slice::from_raw_parts(event_string.cast::<u8>(), len);
            String::from_utf8_lossy(bytes).into_owned()
        }
    };
    if let Err(error) = notify_atspi_keyboard_event(
        event_type,
        keyval,
        hardware_keycode,
        modifiers,
        timestamp,
        &event_string,
        is_text != 0,
    ) {
        if debug_enabled() {
            eprintln!("SWELL AccessKit AT-SPI key event notify failed: {error}");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty_string() -> swell_accesskit_string_ref {
        swell_accesskit_string_ref {
            ptr: ptr::null(),
            len: 0,
        }
    }

    fn empty_rect() -> swell_accesskit_rect {
        swell_accesskit_rect {
            x0: 0.0,
            y0: 0.0,
            x1: 10.0,
            y1: 10.0,
        }
    }

    fn empty_node(id: u64, role: u32) -> swell_accesskit_node {
        swell_accesskit_node {
            id,
            role,
            bounds: empty_rect(),
            flags: 0,
            action_mask: 0,
            toggled: 0,
            orientation: 0,
            numeric_value: 0.0,
            min_numeric_value: 0.0,
            max_numeric_value: 0.0,
            numeric_value_step: 0.0,
            child_count: 0,
            children: ptr::null(),
            text_selection_node: 0,
            text_selection_anchor: 0,
            text_selection_focus: 0,
            character_length_count: 0,
            character_lengths: ptr::null(),
            character_position_count: 0,
            character_positions: ptr::null(),
            character_width_count: 0,
            character_widths: ptr::null(),
            label: empty_string(),
            value: empty_string(),
            has_popup: 0,
            active_descendant: 0,
            position_in_set: 0,
            size_of_set: 0,
            access_key: empty_string(),
            keyboard_shortcut: empty_string(),
            labelled_by_count: 0,
            labelled_by: ptr::null(),
            row_count: 0,
            column_count: 0,
            row_index: 0,
            column_index: 0,
            level: 0,
            scroll_x: 0.0,
            scroll_x_min: 0.0,
            scroll_x_max: 0.0,
            scroll_y: 0.0,
            scroll_y_min: 0.0,
            scroll_y_max: 0.0,
            child_action_mask: 0,
            text_selection_anchor_node: 0,
            text_selection_focus_node: 0,
            live: 0,
        }
    }

    #[test]
    fn maps_collection_and_tab_roles() {
        assert_eq!(map_role(ROLE_LIST_BOX), Role::ListBox);
        assert_eq!(map_role(ROLE_LIST_BOX_OPTION), Role::ListBoxOption);
        assert_eq!(map_role(ROLE_GRID), Role::Grid);
        assert_eq!(map_role(ROLE_GRID_CELL), Role::GridCell);
        assert_eq!(map_role(ROLE_TREE), Role::Tree);
        assert_eq!(map_role(ROLE_TREE_ITEM), Role::TreeItem);
        assert_eq!(map_role(ROLE_TAB_LIST), Role::TabList);
        assert_eq!(map_role(ROLE_TAB), Role::Tab);
    }

    #[test]
    fn maps_new_actions() {
        let mut node = Node::new(Role::Unknown);
        apply_action_mask(
            &mut node,
            ACTION_EXPAND_MASK | ACTION_COLLAPSE_MASK | ACTION_SCROLL_INTO_VIEW_MASK,
        );
        assert!(node.supports_action(Action::Expand));
        assert!(node.supports_action(Action::Collapse));
        assert!(node.supports_action(Action::ScrollIntoView));
    }

    #[test]
    fn maps_unknown_containers_to_group() {
        let children = [2_u64];
        let mut root = empty_node(1, 0);
        root.child_count = children.len();
        root.children = children.as_ptr();
        let child = empty_node(2, ROLE_LABEL);
        let nodes = [root, child];
        let snapshot = swell_accesskit_tree_snapshot {
            root_id: 1,
            focus_id: 1,
            node_count: nodes.len(),
            nodes: nodes.as_ptr(),
        };
        let update = unsafe { build_tree_update(TreeId::ROOT, &snapshot) }.unwrap();
        assert_eq!(update.nodes[0].1.role(), Role::Group);
    }

    #[test]
    fn keeps_empty_unknown_nodes_unknown() {
        let root = empty_node(1, 0);
        let nodes = [root];
        let snapshot = swell_accesskit_tree_snapshot {
            root_id: 1,
            focus_id: 1,
            node_count: nodes.len(),
            nodes: nodes.as_ptr(),
        };
        let update = unsafe { build_tree_update(TreeId::ROOT, &snapshot) }.unwrap();
        assert_eq!(update.nodes[0].1.role(), Role::Unknown);
    }

    #[test]
    fn maps_metadata_and_distinct_text_selection_nodes() {
        let label_refs = [2_u64];
        let children = [3_u64];
        let mut root = empty_node(1, ROLE_GRID);
        root.child_count = children.len();
        root.children = children.as_ptr();
        root.labelled_by_count = label_refs.len();
        root.labelled_by = label_refs.as_ptr();
        root.flags = NODE_FLAG_MULTISELECTABLE;
        root.row_count = 7;
        root.column_count = 2;
        root.active_descendant = 3;
        root.child_action_mask = ACTION_SCROLL_INTO_VIEW_MASK;

        let label = empty_node(2, ROLE_LABEL);
        let mut text = empty_node(3, ROLE_MULTILINE_TEXT_INPUT);
        text.text_selection_node = 4;
        text.text_selection_anchor_node = 4;
        text.text_selection_focus_node = 5;
        text.text_selection_anchor = 1;
        text.text_selection_focus = 2;
        let run_a = empty_node(4, ROLE_TEXT_RUN);
        let run_b = empty_node(5, ROLE_TEXT_RUN);
        let nodes = [root, label, text, run_a, run_b];
        let snapshot = swell_accesskit_tree_snapshot {
            root_id: 1,
            focus_id: 3,
            node_count: nodes.len(),
            nodes: nodes.as_ptr(),
        };
        let update = unsafe { build_tree_update(TreeId::ROOT, &snapshot) }.unwrap();
        let root_node = &update.nodes[0].1;
        assert_eq!(root_node.labelled_by(), &[NodeId(2)]);
        assert!(root_node.is_multiselectable());
        assert_eq!(root_node.row_count(), Some(7));
        assert_eq!(root_node.column_count(), Some(2));
        assert_eq!(root_node.active_descendant(), Some(NodeId(3)));
        assert!(root_node.child_supports_action(Action::ScrollIntoView));

        let text_selection = update.nodes[2].1.text_selection().unwrap();
        assert_eq!(text_selection.anchor.node, NodeId(4));
        assert_eq!(text_selection.focus.node, NodeId(5));
    }

    #[test]
    fn maps_one_based_collection_metadata_to_accesskit_indices() {
        let mut item = empty_node(1, ROLE_TREE_ITEM);
        item.position_in_set = 1;
        item.size_of_set = 4;
        item.level = 1;
        let nodes = [item];
        let snapshot = swell_accesskit_tree_snapshot {
            root_id: 1,
            focus_id: 1,
            node_count: nodes.len(),
            nodes: nodes.as_ptr(),
        };
        let update = unsafe { build_tree_update(TreeId::ROOT, &snapshot) }.unwrap();
        let item = &update.nodes[0].1;
        assert_eq!(item.position_in_set(), Some(0));
        assert_eq!(item.size_of_set(), Some(4));
        assert_eq!(item.level(), Some(0));
    }

    #[test]
    fn maps_live_region_metadata() {
        let children = [2_u64, 3_u64];
        let mut root = empty_node(1, ROLE_WINDOW);
        root.child_count = children.len();
        root.children = children.as_ptr();
        let mut polite = empty_node(2, ROLE_LABEL);
        polite.live = LIVE_POLITE;
        let mut assertive = empty_node(3, ROLE_LABEL);
        assertive.live = LIVE_ASSERTIVE;
        let nodes = [root, polite, assertive];
        let snapshot = swell_accesskit_tree_snapshot {
            root_id: 1,
            focus_id: 1,
            node_count: nodes.len(),
            nodes: nodes.as_ptr(),
        };
        let update = unsafe { build_tree_update(TreeId::ROOT, &snapshot) }.unwrap();
        assert_eq!(update.nodes[1].1.live(), Some(Live::Polite));
        assert_eq!(update.nodes[2].1.live(), Some(Live::Assertive));
    }
}
