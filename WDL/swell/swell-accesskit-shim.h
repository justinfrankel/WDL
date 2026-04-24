#ifndef _SWELL_ACCESSKIT_SHIM_H_
#define _SWELL_ACCESSKIT_SHIM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
  SWELL_ACCESSKIT_ROLE_UNKNOWN = 0,
  SWELL_ACCESSKIT_ROLE_WINDOW = 1,
  SWELL_ACCESSKIT_ROLE_LABEL = 2,
  SWELL_ACCESSKIT_ROLE_BUTTON = 3,
  SWELL_ACCESSKIT_ROLE_DEFAULT_BUTTON = 4,
  SWELL_ACCESSKIT_ROLE_CHECK_BOX = 5,
  SWELL_ACCESSKIT_ROLE_RADIO_BUTTON = 6,
  SWELL_ACCESSKIT_ROLE_TEXT_INPUT = 7,
  SWELL_ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT = 8,
  SWELL_ACCESSKIT_ROLE_SLIDER = 9,
  SWELL_ACCESSKIT_ROLE_PROGRESS_INDICATOR = 10,
  SWELL_ACCESSKIT_ROLE_GROUP = 11,
  SWELL_ACCESSKIT_ROLE_COMBO_BOX = 12,
  SWELL_ACCESSKIT_ROLE_TEXT_RUN = 13,
};

enum
{
  SWELL_ACCESSKIT_ACTION_FOCUS_MASK = 1u << 0,
  SWELL_ACCESSKIT_ACTION_CLICK_MASK = 1u << 1,
  SWELL_ACCESSKIT_ACTION_SET_VALUE_MASK = 1u << 2,
  SWELL_ACCESSKIT_ACTION_INCREMENT_MASK = 1u << 3,
  SWELL_ACCESSKIT_ACTION_DECREMENT_MASK = 1u << 4,
  SWELL_ACCESSKIT_ACTION_SET_TEXT_SELECTION_MASK = 1u << 5,
};

enum
{
  SWELL_ACCESSKIT_NODE_FLAG_DISABLED = 1u << 0,
  SWELL_ACCESSKIT_NODE_FLAG_READ_ONLY = 1u << 1,
  SWELL_ACCESSKIT_NODE_FLAG_HAS_NUMERIC_VALUE = 1u << 2,
  SWELL_ACCESSKIT_NODE_FLAG_HAS_MIN_NUMERIC_VALUE = 1u << 3,
  SWELL_ACCESSKIT_NODE_FLAG_HAS_MAX_NUMERIC_VALUE = 1u << 4,
  SWELL_ACCESSKIT_NODE_FLAG_HAS_NUMERIC_VALUE_STEP = 1u << 5,
};

enum
{
  SWELL_ACCESSKIT_TOGGLED_NONE = 0,
  SWELL_ACCESSKIT_TOGGLED_FALSE = 1,
  SWELL_ACCESSKIT_TOGGLED_TRUE = 2,
  SWELL_ACCESSKIT_TOGGLED_MIXED = 3,
};

enum
{
  SWELL_ACCESSKIT_ORIENTATION_NONE = 0,
  SWELL_ACCESSKIT_ORIENTATION_HORIZONTAL = 1,
  SWELL_ACCESSKIT_ORIENTATION_VERTICAL = 2,
};

enum
{
  SWELL_ACCESSKIT_ACTION_NONE = 0,
  SWELL_ACCESSKIT_ACTION_FOCUS = 1,
  SWELL_ACCESSKIT_ACTION_CLICK = 2,
  SWELL_ACCESSKIT_ACTION_SET_VALUE = 3,
  SWELL_ACCESSKIT_ACTION_INCREMENT = 4,
  SWELL_ACCESSKIT_ACTION_DECREMENT = 5,
  SWELL_ACCESSKIT_ACTION_SET_TEXT_SELECTION = 6,
};

enum
{
  SWELL_ACCESSKIT_ACTION_DATA_NONE = 0,
  SWELL_ACCESSKIT_ACTION_DATA_STRING = 1,
  SWELL_ACCESSKIT_ACTION_DATA_NUMERIC = 2,
  SWELL_ACCESSKIT_ACTION_DATA_TEXT_SELECTION = 3,
};

typedef struct swell_accesskit_rect
{
  double x0;
  double y0;
  double x1;
  double y1;
} swell_accesskit_rect;

typedef struct swell_accesskit_string_ref
{
  const char *ptr;
  size_t len;
} swell_accesskit_string_ref;

typedef struct swell_accesskit_node
{
  uint64_t id;
  uint32_t role;
  swell_accesskit_rect bounds;
  uint32_t flags;
  uint32_t action_mask;
  uint32_t toggled;
  uint32_t orientation;
  double numeric_value;
  double min_numeric_value;
  double max_numeric_value;
  double numeric_value_step;
  size_t child_count;
  const uint64_t *children;
  uint64_t text_selection_node;
  size_t text_selection_anchor;
  size_t text_selection_focus;
  size_t character_length_count;
  const uint8_t *character_lengths;
  size_t character_position_count;
  const float *character_positions;
  size_t character_width_count;
  const float *character_widths;
  swell_accesskit_string_ref label;
  swell_accesskit_string_ref value;
} swell_accesskit_node;

typedef struct swell_accesskit_tree_snapshot
{
  uint64_t root_id;
  uint64_t focus_id;
  size_t node_count;
  const swell_accesskit_node *nodes;
} swell_accesskit_tree_snapshot;

typedef struct swell_accesskit_action_request
{
  uint64_t target_node;
  uint32_t action;
  uint32_t data_kind;
  char *string_value;
  double numeric_value;
  size_t text_selection_anchor;
  size_t text_selection_focus;
} swell_accesskit_action_request;

struct swell_accesskit_host;

struct swell_accesskit_host *swell_accesskit_host_new(const uint8_t *tree_id_bytes);
void swell_accesskit_host_free(struct swell_accesskit_host *host);
void swell_accesskit_host_set_root_window_bounds(
    struct swell_accesskit_host *host,
    const swell_accesskit_rect *outer,
    const swell_accesskit_rect *inner);
void swell_accesskit_host_update_window_focus_state(struct swell_accesskit_host *host, int is_focused);
int swell_accesskit_host_commit_full_tree(
    struct swell_accesskit_host *host,
    const swell_accesskit_tree_snapshot *snapshot);
int swell_accesskit_host_pop_action(
    struct swell_accesskit_host *host,
    swell_accesskit_action_request *out_action);
char *swell_accesskit_host_debug(const struct swell_accesskit_host *host);
void swell_accesskit_string_free(char *string_value);
void swell_accesskit_notify_keyboard_event(
    uint32_t event_type,
    uint32_t keyval,
    uint32_t hardware_keycode,
    uint32_t modifiers,
    int32_t timestamp,
    const char *event_string,
    int is_text);

#ifdef __cplusplus
}
#endif

#endif
