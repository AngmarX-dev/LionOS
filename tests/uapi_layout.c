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
typedef char lion_dirent_size_must_be_264[
    sizeof(struct lion_dirent) == 264u ? 1 : -1
];
typedef char lion_dirent_type_offset_must_be_4[
    __builtin_offsetof(struct lion_dirent, type) == 4u ? 1 : -1
];
typedef char lion_dirent_name_offset_must_be_5[
    __builtin_offsetof(struct lion_dirent, name) == 5u ? 1 : -1
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

typedef char lion_tty_attributes_size_must_be_12[
    sizeof(struct lion_tty_attributes) == 12u ? 1 : -1
];
typedef char lion_tty_attributes_vtime_offset_must_be_8[
    __builtin_offsetof(struct lion_tty_attributes, vtime) == 8u ? 1 : -1
];

typedef char lion_sigaction_size_must_be_8[
    sizeof(struct lion_sigaction) == 8u ? 1 : -1
];
typedef char lion_sigaction_handler_offset_must_be_0[
    __builtin_offsetof(struct lion_sigaction, handler) == 0u ? 1 : -1
];
typedef char lion_sigaction_restorer_offset_must_be_4[
    __builtin_offsetof(struct lion_sigaction, restorer) == 4u ? 1 : -1
];
