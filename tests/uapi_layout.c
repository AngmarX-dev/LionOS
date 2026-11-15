#include <stdint.h>
#include "uapi.h"

typedef char lion_stat_size_must_be_12[
    sizeof(struct lion_stat) == 12u ? 1 : -1
];
typedef char lion_stat_backend_offset_must_be_4[
    __builtin_offsetof(struct lion_stat, backend) == 4u ? 1 : -1
];
typedef char lion_stat_flags_offset_must_be_8[
    __builtin_offsetof(struct lion_stat, flags) == 8u ? 1 : -1
];
typedef char lion_mouse_event_size_must_be_12[
    sizeof(struct lion_mouse_event) == 12u ? 1 : -1
];
typedef char lion_mouse_event_buttons_offset_must_be_8[
    __builtin_offsetof(struct lion_mouse_event, buttons) == 8u ? 1 : -1
];
typedef char lion_mouse_event_source_offset_must_be_9[
    __builtin_offsetof(struct lion_mouse_event, source) == 9u ? 1 : -1
];
