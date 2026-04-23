use accesskit::{
    Action, ActionData, ActionHandler, ActionRequest, ActivationHandler, DeactivationHandler, Node,
    NodeId, Orientation, Rect, Role, Toggled, Tree, TreeId, TreeUpdate,
};
use accesskit_unix::Adapter;
use std::collections::VecDeque;
use std::ffi::{c_char, CString};
use std::ptr;
use std::slice;
use std::sync::{Arc, Mutex};

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

const ACTION_FOCUS_MASK: u32 = 1u32 << 0;
const ACTION_CLICK_MASK: u32 = 1u32 << 1;
const ACTION_SET_VALUE_MASK: u32 = 1u32 << 2;
const ACTION_INCREMENT_MASK: u32 = 1u32 << 3;
const ACTION_DECREMENT_MASK: u32 = 1u32 << 4;

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

const ACTION_DATA_NONE: u32 = 0;
const ACTION_DATA_STRING: u32 = 1;
const ACTION_DATA_NUMERIC: u32 = 2;

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
}

#[derive(Clone, Debug)]
struct QueuedAction {
    target_node: u64,
    action: u32,
    data_kind: u32,
    string_value: Option<String>,
    numeric_value: f64,
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
    if raw.ptr.is_null() || raw.len == 0 {
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
        apply_action_mask(&mut node, raw.action_mask);

        let child_ids = children_from_ffi(raw)?;
        if !child_ids.is_empty() {
            node.set_children(child_ids);
        }

        node.set_bounds(rect_from_ffi(&raw.bounds));
        nodes.push((NodeId(raw.id), node));
    }

    let root = NodeId(snapshot.root_id);
    let focus = if snapshot.focus_id == 0 { root } else { NodeId(snapshot.focus_id) };
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
    let queued_actions = host.shared.actions.lock().map_or(0, |actions| actions.len());
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
