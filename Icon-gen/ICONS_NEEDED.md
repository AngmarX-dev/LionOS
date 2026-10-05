# 🦁 LionOS Icon Generation Inventory

This folder contains the master list of icons that LionOS may need for its desktop, window manager, applications, files, settings, hardware, and system states.

## Icon design rules

- **Style:** clean modern system icons, consistent LionOS visual language.
- **Shape:** simple silhouettes with rounded geometry and strong recognition at small sizes.
- **Default size:** 64×64 px source artwork.
- **Export sizes:** 16×16, 24×24, 32×32, 48×48, 64×64, 128×128, 256×256.
- **Background:** transparent PNG.
- **Variants:** normal, hover/active where useful, disabled where useful.
- **Pixel clarity:** icons must remain recognizable at 16×16 and 24×24.
- **No text inside icons** unless the icon specifically represents a letter/document type.
- **Consistent stroke/visual weight** across the complete icon set.

## Priority

- 🔴 **P0:** required for the first polished desktop
- 🟠 **P1:** important for applications and daily usability
- 🟡 **P2:** useful system/application coverage
- ⚪ **P3:** future/optional

---

## 1. 🖥️ Desktop & Shell — P0

| ID | Icon | Intended use |
|---|---|---|
| DESK-001 | LionOS logo | Boot/session branding, system identity |
| DESK-002 | Home | Home directory / desktop shortcut |
| DESK-003 | Computer / This PC | System and storage overview |
| DESK-004 | Applications grid | Application launcher |
| DESK-005 | Search | Launcher/system search |
| DESK-006 | Terminal | Terminal application |
| DESK-007 | Files | File manager |
| DESK-008 | Settings | System settings |
| DESK-009 | Browser | Web browser |
| DESK-010 | Notepad / Text Editor | Text editor |
| DESK-011 | Trash | Trash/recycle destination |
| DESK-012 | Folder | Generic directory |
| DESK-013 | Folder open | Open directory state |
| DESK-014 | New folder | Folder creation |
| DESK-015 | Desktop | Desktop/home screen |
| DESK-016 | Shutdown | System power menu |
| DESK-017 | Restart | Restart action |
| DESK-018 | Sleep | Sleep/power-saving action |
| DESK-019 | Lock | Lock screen/session |
| DESK-020 | Logout | End desktop session |

## 2. 🪟 Window Manager — P0

| ID | Icon | Intended use |
|---|---|---|
| WIN-001 | Close | Close window |
| WIN-002 | Minimize | Minimize window |
| WIN-003 | Maximize | Maximize window |
| WIN-004 | Restore | Restore window size |
| WIN-005 | Fullscreen | Enter fullscreen |
| WIN-006 | Exit fullscreen | Leave fullscreen |
| WIN-007 | Pin / always-on-top | Keep window above others |
| WIN-008 | Unpin | Disable always-on-top |
| WIN-009 | Resize | Window resizing |
| WIN-010 | Move | Window movement |
| WIN-011 | Snap left | Snap window left |
| WIN-012 | Snap right | Snap window right |
| WIN-013 | Snap top | Snap/maximize to top |
| WIN-014 | Snap bottom | Snap to bottom |
| WIN-015 | Split view | Tiled window layout |

## 3. 🧭 Navigation & Common Actions — P0

| ID | Icon | Intended use |
|---|---|---|
| NAV-001 | Back | Previous page/location |
| NAV-002 | Forward | Next page/location |
| NAV-003 | Up | Parent directory |
| NAV-004 | Home | Home location |
| NAV-005 | Refresh | Reload/refresh |
| NAV-006 | Reload hard | Forced reload |
| NAV-007 | Menu | Main/context menu |
| NAV-008 | More | Overflow actions |
| NAV-009 | Arrow down | Dropdown/download |
| NAV-010 | Arrow up | Upload/expand |
| NAV-011 | Chevron right | Enter/next |
| NAV-012 | Chevron left | Previous |
| NAV-013 | Check | Confirm/success |
| NAV-014 | X | Cancel/remove |
| NAV-015 | Plus | Add/create |
| NAV-016 | Minus | Remove/decrease |
| NAV-017 | Edit | Edit item |
| NAV-018 | Save | Save changes |
| NAV-019 | Copy | Copy item |
| NAV-020 | Cut | Cut item |
| NAV-021 | Paste | Paste item |
| NAV-022 | Delete | Delete item |
| NAV-023 | Download | Download file |
| NAV-024 | Upload | Upload file |
| NAV-025 | Share | Share item |
| NAV-026 | Link | Link/URL |
| NAV-027 | External link | Open externally |
| NAV-028 | Open | Open item |
| NAV-029 | New window | Open in new window |
| NAV-030 | Sort | Sorting controls |

## 4. 📁 File Types & File Manager — P0/P1

| ID | Icon | Intended use |
|---|---|---|
| FILE-001 | Generic file | Unknown/general file |
| FILE-002 | Text file | .txt/text documents |
| FILE-003 | Source code | C/C++/ASM/source files |
| FILE-004 | Header file | .h/header files |
| FILE-005 | Script | Shell/script files |
| FILE-006 | Binary / executable | ELF/program files |
| FILE-007 | Disk image | ISO/img/disk image |
| FILE-008 | Archive | ZIP/TAR/GZIP/etc. |
| FILE-009 | Configuration | Config files |
| FILE-010 | Log file | Logs/debug output |
| FILE-011 | Image | PNG/JPG/BMP/etc. |
| FILE-012 | Audio | Audio files |
| FILE-013 | Video | Video files |
| FILE-014 | PDF/document | Documents |
| FILE-015 | Folder | Directory |
| FILE-016 | Shared folder | Shared directory |
| FILE-017 | Hidden file | Hidden-file state |
| FILE-018 | Read-only file | Read-only state |
| FILE-019 | Locked file | Access-restricted file |
| FILE-020 | Symlink | Symbolic link |

## 5. ⚙️ Settings & System — P0/P1

| ID | Icon | Intended use |
|---|---|---|
| SYS-001 | Gear | General settings |
| SYS-002 | Display | Display settings |
| SYS-003 | Brightness | Display brightness |
| SYS-004 | Night light | Night-light mode |
| SYS-005 | Keyboard | Keyboard settings |
| SYS-006 | Mouse | Mouse settings |
| SYS-007 | Touchpad | Touchpad settings |
| SYS-008 | Sound | Audio settings |
| SYS-009 | Microphone | Microphone settings |
| SYS-010 | Network | Network settings |
| SYS-011 | Wi-Fi | Wi-Fi settings |
| SYS-012 | Ethernet | Wired network |
| SYS-013 | Bluetooth | Bluetooth |
| SYS-014 | Battery | Battery/power |
| SYS-015 | Power | Power management |
| SYS-016 | Security | Security settings |
| SYS-017 | Users | User/account management |
| SYS-018 | Accessibility | Accessibility settings |
| SYS-019 | Date & time | Clock/time settings |
| SYS-020 | Language | Language/keyboard layout |
| SYS-021 | Updates | System updates |
| SYS-022 | About | System information |
| SYS-023 | Storage | Storage overview |
| SYS-024 | Developer | Developer settings |
| SYS-025 | Performance | Performance monitor/settings |

## 6. 🔊 Status Bar / Tray — P0

| ID | Icon | Intended use |
|---|---|---|
| TRAY-001 | Volume high | Audio on |
| TRAY-002 | Volume medium | Medium volume |
| TRAY-003 | Volume low | Low volume |
| TRAY-004 | Volume muted | Muted |
| TRAY-005 | Microphone active | Microphone in use |
| TRAY-006 | Network connected | Network connected |
| TRAY-007 | Network disconnected | Network disconnected |
| TRAY-008 | Wi-Fi signal full | Strong signal |
| TRAY-009 | Wi-Fi signal medium | Medium signal |
| TRAY-010 | Wi-Fi signal low | Weak signal |
| TRAY-011 | Wi-Fi disabled | Wi-Fi disabled |
| TRAY-012 | Battery full | Full battery |
| TRAY-013 | Battery charging | Charging |
| TRAY-014 | Battery low | Low battery |
| TRAY-015 | Battery critical | Critical battery |
| TRAY-016 | AC power | External power |
| TRAY-017 | Bluetooth connected | Bluetooth active |
| TRAY-018 | USB device | USB device connected |
| TRAY-019 | Notifications | Notification center |
| TRAY-020 | Clock | System clock |
| TRAY-021 | Keyboard layout | Current input layout |

## 7. 🖱️ Input & Pointer — P0/P1

| ID | Icon | Intended use |
|---|---|---|
| INPUT-001 | Mouse pointer | Default cursor |
| INPUT-002 | Pointer link | Link cursor |
| INPUT-003 | Text cursor | Text insertion cursor |
| INPUT-004 | Busy pointer | Busy/loading cursor |
| INPUT-005 | Move cursor | Move operation |
| INPUT-006 | Resize horizontal | Horizontal resize |
| INPUT-007 | Resize vertical | Vertical resize |
| INPUT-008 | Resize diagonal NW-SE | Diagonal resize |
| INPUT-009 | Resize diagonal NE-SW | Diagonal resize |
| INPUT-010 | Grab | Draggable item |
| INPUT-011 | Grabbing | Active drag |
| INPUT-012 | Forbidden | Operation unavailable |

## 8. 🌐 Browser — P1

| ID | Icon | Intended use |
|---|---|---|
| WEB-001 | Globe | Browser/app identity |
| WEB-002 | Secure lock | HTTPS/security |
| WEB-003 | Unsecured page | HTTP/non-secure page |
| WEB-004 | Bookmark | Save page |
| WEB-005 | Bookmark filled | Saved bookmark |
| WEB-006 | New tab | New browser tab |
| WEB-007 | Close tab | Close tab |
| WEB-008 | Downloads | Download manager |
| WEB-009 | History | Browser history |
| WEB-010 | Home page | Browser home |
| WEB-011 | Search web | Web search |
| WEB-012 | URL/link | Address bar |
| WEB-013 | Stop loading | Stop page load |
| WEB-014 | Private/incognito | Private browsing |
| WEB-015 | Certificate | TLS certificate |
| WEB-016 | Page error | Web/page error |

## 9. 📝 Notepad / Text Editor — P1

| ID | Icon | Intended use |
|---|---|---|
| EDIT-001 | Document | New/open document |
| EDIT-002 | New document | Create document |
| EDIT-003 | Open document | Open document |
| EDIT-004 | Save document | Save document |
| EDIT-005 | Save as | Save under new name |
| EDIT-006 | Undo | Undo edit |
| EDIT-007 | Redo | Redo edit |
| EDIT-008 | Find | Search text |
| EDIT-009 | Find next | Next search result |
| EDIT-010 | Replace | Replace text |
| EDIT-011 | Select all | Select all text |
| EDIT-012 | Cut text | Cut selection |
| EDIT-013 | Copy text | Copy selection |
| EDIT-014 | Paste text | Paste selection |

## 10. 💻 Terminal / Shell — P1

| ID | Icon | Intended use |
|---|---|---|
| TERM-001 | Terminal | Terminal app |
| TERM-002 | Shell prompt | Prompt indicator |
| TERM-003 | Command | Command execution |
| TERM-004 | Process | Running process |
| TERM-005 | Stop process | Terminate process |
| TERM-006 | Run | Execute program |
| TERM-007 | Environment | Environment variables |
| TERM-008 | Pipes | Shell pipe |
| TERM-009 | Redirect input | stdin redirection |
| TERM-010 | Redirect output | stdout redirection |
| TERM-011 | Output | Command output |
| TERM-012 | Warning | Shell warning |
| TERM-013 | Error | Shell error |
| TERM-014 | Success | Successful command |

## 11. 🧠 System Monitoring — P1/P2

| ID | Icon | Intended use |
|---|---|---|
| MON-001 | CPU | CPU usage |
| MON-002 | CPU core | Per-core usage |
| MON-003 | Memory/RAM | RAM usage |
| MON-004 | Swap | Swap/paging |
| MON-005 | Disk | Disk activity |
| MON-006 | Network traffic | Network throughput |
| MON-007 | Process list | Process manager |
| MON-008 | Scheduler | Scheduler state |
| MON-009 | Kernel | Kernel information |
| MON-010 | Temperature | Hardware temperature |
| MON-011 | Performance graph | Performance history |
| MON-012 | Activity | Generic activity |

## 12. 💽 Storage & Devices — P1

| ID | Icon | Intended use |
|---|---|---|
| DEV-001 | Hard disk | HDD |
| DEV-002 | SSD | Solid-state drive |
| DEV-003 | NVMe | NVMe storage |
| DEV-004 | USB drive | USB storage |
| DEV-005 | CD/DVD | Optical media |
| DEV-006 | Partition | Disk partition |
| DEV-007 | Mounted drive | Mounted filesystem |
| DEV-008 | Unmounted drive | Unmounted filesystem |
| DEV-009 | Network drive | Network storage |
| DEV-010 | Eject | Eject/removable media |
| DEV-011 | Printer | Printer device |
| DEV-012 | Camera | Camera device |
| DEV-013 | Monitor | Display device |
| DEV-014 | Laptop | Laptop system |
| DEV-015 | Keyboard device | Keyboard hardware |
| DEV-016 | Mouse device | Mouse hardware |
| DEV-017 | Gamepad | Game controller |
| DEV-018 | USB hub | USB hub |

## 13. 🛡️ Security & Permissions — P2

| ID | Icon | Intended use |
|---|---|---|
| SEC-001 | Shield | Security |
| SEC-002 | Shield check | Protected/verified |
| SEC-003 | Shield warning | Security warning |
| SEC-004 | Lock | Locked/private |
| SEC-005 | Unlock | Unlocked |
| SEC-006 | Key | Credential/permission |
| SEC-007 | User | User identity |
| SEC-008 | User admin | Administrator/root |
| SEC-009 | Group | User group |
| SEC-010 | Permission | Access permissions |
| SEC-011 | Read permission | Read access |
| SEC-012 | Write permission | Write access |
| SEC-013 | Execute permission | Execute access |
| SEC-014 | Secure boot | Boot security |

## 14. ⚠️ Notifications & System States — P0/P1

| ID | Icon | Intended use |
|---|---|---|
| STATE-001 | Information | Informational message |
| STATE-002 | Question | Help/confirmation |
| STATE-003 | Success | Successful operation |
| STATE-004 | Warning | Warning |
| STATE-005 | Error | Error |
| STATE-006 | Critical error | Severe error |
| STATE-007 | Loading | Loading state |
| STATE-008 | Sync | Synchronization |
| STATE-009 | Downloading | Download in progress |
| STATE-010 | Uploading | Upload in progress |
| STATE-011 | Offline | Offline mode |
| STATE-012 | Connected | Connected |
| STATE-013 | Disconnected | Disconnected |
| STATE-014 | Disabled | Feature disabled |

## 15. 🦁 LionOS Branding / System — P0/P2

| ID | Icon | Intended use |
|---|---|---|
| BRAND-001 | LionOS primary mark | Main branding |
| BRAND-002 | LionOS monochrome mark | Small UI/toolbar |
| BRAND-003 | LionOS outline mark | Light/dark backgrounds |
| BRAND-004 | Lion head | System identity |
| BRAND-005 | LionOS installer | Future installer |
| BRAND-006 | LionOS boot | Boot splash |
| BRAND-007 | LionOS recovery | Recovery environment |
| BRAND-008 | LionOS developer | Developer tools |
| BRAND-009 | LionOS server | Future server profile |

## 16. 🧰 Developer Tools — P2

| ID | Icon | Intended use |
|---|---|---|
| DEVTOOLS-001 | Code | Code editor |
| DEVTOOLS-002 | Compiler | Compiler/toolchain |
| DEVTOOLS-003 | Build | Build system |
| DEVTOOLS-004 | Debug | Debugger |
| DEVTOOLS-005 | Bug | Bug/issues |
| DEVTOOLS-006 | Git/source control | Version control |
| DEVTOOLS-007 | Branch | Git branch |
| DEVTOOLS-008 | Commit | Git commit |
| DEVTOOLS-009 | Terminal console | Development console |
| DEVTOOLS-010 | Package | Package/application format |
| DEVTOOLS-011 | API | API/interface |
| DEVTOOLS-012 | Documentation | Documentation |

## 17. 📦 Packages & Applications — P2/P3

| ID | Icon | Intended use |
|---|---|---|
| APP-001 | Package | Generic package |
| APP-002 | Install | Install application |
| APP-003 | Uninstall | Remove application |
| APP-004 | Update | Update application |
| APP-005 | Verified package | Trusted package |
| APP-006 | App store | Future application manager |
| APP-007 | Plugin | Plugin/extension |
| APP-008 | Service | System service |
| APP-009 | Startup | Startup application |
| APP-010 | Background service | Background daemon |

## 18. 🔌 Hardware / Driver Status — P2

| ID | Icon | Intended use |
|---|---|---|
| HW-001 | PCI device | PCI enumeration |
| HW-002 | USB | USB subsystem |
| HW-003 | USB connected | USB device online |
| HW-004 | USB disconnected | USB device removed |
| HW-005 | xHCI controller | xHCI controller |
| HW-006 | Network card | NIC |
| HW-007 | Intel graphics | Intel GPU/display |
| HW-008 | Generic GPU | Graphics device |
| HW-009 | CPU/AP | CPU core/AP state |
| HW-010 | ACPI | ACPI/platform device |
| HW-011 | Interrupt | Interrupt/IRQ |
| HW-012 | Driver | Driver manager |

## 19. 🧩 UI Components — P2

| ID | Icon | Intended use |
|---|---|---|
| UI-001 | Dropdown | Dropdown control |
| UI-002 | Expand | Expand control |
| UI-003 | Collapse | Collapse control |
| UI-004 | Checkbox checked | Checked checkbox |
| UI-005 | Checkbox unchecked | Unchecked checkbox |
| UI-006 | Radio selected | Selected radio |
| UI-007 | Radio unselected | Unselected radio |
| UI-008 | Slider | Slider control |
| UI-009 | Toggle on | Toggle enabled |
| UI-010 | Toggle off | Toggle disabled |
| UI-011 | Filter | Filtering |
| UI-012 | Sort ascending | A→Z / low→high |
| UI-013 | Sort descending | Z→A / high→low |
| UI-014 | Zoom in | Increase zoom |
| UI-015 | Zoom out | Decrease zoom |
| UI-016 | Help | Help/support |

## 20. 🔄 Window / Desktop States — P3

| ID | Icon | Intended use |
|---|---|---|
| STATEWIN-001 | Focused window | Focus indicator |
| STATEWIN-002 | Unfocused window | Unfocused state |
| STATEWIN-003 | Minimized window | Minimized task item |
| STATEWIN-004 | Attention | App needs attention |
| STATEWIN-005 | New notification | New notification |
| STATEWIN-006 | Window group | Grouped windows |
| STATEWIN-007 | Workspace | Virtual workspace |
| STATEWIN-008 | Workspace active | Active workspace |
| STATEWIN-009 | Workspace empty | Empty workspace |

---

## Required first-generation set

These should be generated first so the desktop can have a consistent visual language:

1. LionOS logo
2. Home
3. Computer
4. Applications
5. Search
6. Terminal
7. Files
8. Settings
9. Browser
10. Notepad
11. Trash
12. Folder
13. Folder open
14. New folder
15. Close
16. Minimize
17. Maximize
18. Restore
19. Back
20. Forward
21. Up
22. Refresh
23. Menu
24. More
25. Check
26. Cancel
27. Plus
28. Minus
29. Edit
30. Save
31. Copy
32. Cut
33. Paste
34. Delete
35. Download
36. Upload
37. Share
38. Globe
39. Bookmark
40. New tab
41. Volume
42. Network
43. Wi-Fi
44. Battery
45. Bluetooth
46. Notifications
47. Clock
48. Mouse
49. Keyboard
50. CPU
51. Memory
52. Disk
53. Warning
54. Error
55. Information
56. Success
57. Loading
58. Shield
59. Lock
60. Unlock

## Generation / export checklist

For every icon:

- [ ] 64×64 master generated
- [ ] Transparent PNG exported
- [ ] 16×16 export checked
- [ ] 24×24 export checked
- [ ] 32×32 export checked
- [ ] 48×48 export checked
- [ ] 64×64 export checked
- [ ] 128×128 export checked
- [ ] 256×256 export checked
- [ ] Dark-background preview checked
- [ ] Light-background preview checked
- [ ] Normal state checked
- [ ] Hover/active state checked where required
- [ ] Disabled state checked where required
- [ ] Naming matches the IDs in this document

## Suggested filename convention

`<id>_<name>.png`

Examples:

`DESK-007_files.png`
`WIN-001_close.png`
`TRAY-013_battery_charging.png`
`STATE-005_error.png`

Keep generated artwork in this directory and preserve the IDs so the GUI can map icons to stable asset names.
