use accesskit::{
    Action, ActionData, ActionHandler, ActionRequest, ActivationHandler, DeactivationHandler, Node,
    NodeId, Orientation, Rect, Role, TextDirection, TextPosition, TextSelection, Toggled, Tree,
    TreeId, TreeUpdate,
};
use accesskit_unix::Adapter;
use std::collections::VecDeque;
use std::env;
use std::ffi::{c_char, CString};
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

const ACTION_FOCUS_MASK: u32 = 1u32 << 0;
const ACTION_CLICK_MASK: u32 = 1u32 << 1;
const ACTION_SET_VALUE_MASK: u32 = 1u32 << 2;
const ACTION_INCREMENT_MASK: u32 = 1u32 << 3;
const ACTION_DECREMENT_MASK: u32 = 1u32 << 4;
const ACTION_SET_TEXT_SELECTION_MASK: u32 = 1u32 << 5;

const NODE_FLAG_DISABLED: u32 = 1u32 << 0;
const NODE_FLAG_READ_ONLY: u32 = 1u32 << 1;
const NODE_FLAG_HAS_NUMERIC_VALUE: u32 = 1u32 << 2;
const NODE_FLAG_HAS_MIN_NUMERIC_VALUE: u32 = 1u32 << 3;
const NODE_FLAG_HAS_MAX_NUMERIC_VALUE: u32 = 1u32 << 4;
const NODE_FLAG_HAS_NUMERIC_VALUE_STEP: u32 = 1u32 << 5;

const TOGGLED_FALSE: u32 = 1;
const TOGGLED_TRUE: u32 = 2;
const TOGGLED_MIXED: u32 = 3;

const ORIENTATION_HORIZONTAL: u32 = 1;
const ORIENTATION_VERTICAL: u32 = 2;

const ACTION_NONE: u32 = 0;
const ACTION_FOCUS: u32 = 1;
const ACTION_CLICK: u32 = 2;
const ACTION_SET_VALUE: u32 = 3;
const ACTION_INCREMENT: u32 = 4;
const ACTION_DECREMENT: u32 = 5;
const ACTION_SET_TEXT_SELECTION: u32 = 6;

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
        _ => Role::Unknown,
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
        let mut node = Node::new(map_role(raw.role));
        if let Some(label) = string_from_ffi(&raw.label) {
            node.set_label(label);
        }
        if let Some(value) = string_from_ffi(&raw.value) {
            node.set_value(value);
        }
        if (raw.flags & NODE_FLAG_DISABLED) != 0 {
            node.set_disabled();
        }
        if (raw.flags & NODE_FLAG_READ_ONLY) != 0 {
            node.set_read_only();
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
            node.set_text_selection(TextSelection {
                anchor: TextPosition {
                    node: NodeId(raw.text_selection_node),
                    character_index: raw.text_selection_anchor,
                },
                focus: TextPosition {
                    node: NodeId(raw.text_selection_node),
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

        let child_ids = children_from_ffi(raw)?;
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
