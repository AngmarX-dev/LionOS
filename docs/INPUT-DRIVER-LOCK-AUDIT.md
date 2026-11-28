# Input-Driver Lock Call-Site Audit

## Scope and status

This is a bounded source audit of the keyboard, mouse, and TTY input paths
at parent commit `078e2fb52c2af757f4e5a536d7d52899cf443ed0` on `feature/graphics-engine-v2`.

Inspected files:

- `src/keyboard.c`
- `src/keyboard_tty.c`
- `src/mouse.c`
- `src/tty.c`
- `src/xhci.c` (USB report dispatch path only)
- `tests/keyboard_tty_test.c`

This document records observed lock acquisition and callback boundaries. It
is not a complete driver or interrupt-context audit, does not prove that all
xHCI state is safe for concurrent callers, and does not authorize changing
the process or input lock scopes. No runtime code is changed by this audit.

## Observed lock edges

| Observed nested edge | Source path | Handoff / limitation |
|---|---|---|
| `keyboard_buffer_lock → process_lock` | `keyboard_read_blocking()` calls `process_sleep_on()` while the keyboard ring lock is held | The central wait handoff publishes WAITING before releasing the caller lock |
| `mouse_event_lock → process_lock` | `mouse_read_event()` calls `process_sleep_on()` while the event-ring lock is held | The event producer releases the ring lock before waking readers |
| `console_tty.input_lock → console_tty.lock` | Ctrl-C foreground snapshot in `tty_input_byte()` | Both locks are released before the process signal callback |
| `console_tty.input_lock → process_lock` | Blocking TTY read | Same central check-to-sleep handoff documented in the process wait audit |

## Producer and callback boundaries

### Keyboard

- `push_char()` calls `keyboard_tty_forward_byte()` before acquiring
  `keyboard_buffer_lock`. TTY input therefore does not execute while the
  keyboard ring lock is held.
- The character is inserted and the ring indices are updated under
  `keyboard_buffer_lock`.
- The lock is released before `process_wakeup()` is called.
- `keyboard_read_blocking()` is the deliberate exception to the no-callback
  rule: it enters `process_sleep_on()` with the ring lock held so the empty
  check and WAITING publication remain atomic.
- USB keyboard reports reach the same `push_char()` path through
  `keyboard_handle_usb_report()`. The inspected xHCI poll dispatches the
  report without acquiring a spinlock in `src/xhci.c`; concurrent access to
  the broader xHCI state is not established by this observation.

### Mouse

- `mouse_event_push()` updates the bounded event ring and drop counter under
  `mouse_event_lock`, then releases the lock before waking readers.
- `mouse_read_event()` uses the central wait handoff while retaining the
  event-ring lock; `mouse_try_read_event()` and the availability/drop-count
  accessors release the lock before returning.
- The PS/2 decoder reaches the event producer from
  `mouse_handle_ps2_byte()`. USB reports are consumed by the foreground
  `mouse_poll()` path and then published through the same event ring.
- No process callback is made from the inspected mouse event producer while
  `mouse_event_lock` is held, other than the reader's intentional
  `process_sleep_on()` handoff.

### TTY integration

- `tty_input_byte()` takes `input_lock`; the Ctrl-C path briefly nests
  `console_tty.lock` to read the foreground process group.
- The Ctrl-C path releases `console_tty.lock` and `input_lock` before
  calling `process_tty_signal_foreground()`.
- Normal data and completed-line paths release `input_lock` before calling
  `process_wakeup()`.
- Blocking reads preserve `input_lock → process_lock` through
  `process_sleep_on()`. The process-side foreground checks take
  `process_lock → console_tty.lock` before the read acquires `input_lock`;
  the inspected TTY callbacks do not retain the state lock while entering
  process APIs.
- TTY echo flushing uses `output_lock → input_lock`, releases the input
  lock, and may then take `console_tty.lock` while retaining output lock.
  Session release uses `output_lock → input_lock → console_tty.lock`.

## Existing test coverage and gaps

- `tests/keyboard_tty_test.c` validates byte forwarding, canonical line
  editing, Ctrl-C forwarding, and rejection when no TTY session owns input.
- That test does not exercise concurrent keyboard ring producers/consumers,
  interrupt-context lock contention, or process wait handoff.
- No dedicated host mouse event-ring concurrency test was found in the
  inspected repository tree.
- QEMU xHCI mouse integration exercises USB input delivery, but it is not a
  physical-controller test and does not prove arbitrary concurrent access to
  xHCI state.

## Review boundary

The observed input wait edges extend the partial process lock graph. They do
not prove deadlock freedom or a complete global lock order. In particular,
all remaining driver locks, interrupt paths, console/framebuffer callbacks,
networking, storage, and device teardown still need inspection. Keep
`process_lock` and the input locks unchanged until the complete graph,
stable process-reference protocol, and production acceptance tests are
available.
