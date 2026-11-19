# 🦁 LionOS

<p align="center">
  <strong>یک سیستم‌عامل آزمایشی ۳۲ بیتی x86 که از پایه توسعه داده می‌شود</strong><br>
  Kernel Development · Memory Management · SMP · Drivers · Networking · Desktop
</p>

<p align="center">
  <a href="https://github.com/AngmarX-dev/LionOS/actions/workflows/build.yml"><img src="https://github.com/AngmarX-dev/LionOS/actions/workflows/build.yml/badge.svg" alt="LionOS Build"></a>
  <a href="https://github.com/AngmarX-dev/LionOS/actions/workflows/xhci-mouse.yml"><img src="https://github.com/AngmarX-dev/LionOS/actions/workflows/xhci-mouse.yml/badge.svg" alt="xHCI Mouse Test"></a>
  <a href="https://img.shields.io/github/license/AngmarX-dev/LionOS"><img src="https://img.shields.io/github/license/AngmarX-dev/LionOS" alt="License"></a>
</p>

<p align="center">
  <a href="#معرفی">معرفی</a> · <a href="#قابلیتهای-فعلی">قابلیت‌ها</a> ·
  <a href="#معماری">معماری</a> · <a href="#ساخت-و-اجرا">ساخت و اجرا</a> ·
  <a href="#نقشهراه">نقشه‌راه</a>
</p>

---

## معرفی

**LionOS** یک سیستم‌عامل آموزشی و آزمایشی است که با تمرکز بر توسعه واقعی Kernel و زیرسیستم‌های سطح پایین ساخته می‌شود. هدف پروژه، ساخت تدریجی یک سیستم‌عامل ماژولار با مدیریت حافظه، پردازش، فایل‌سیستم، ورودی/خروجی، شبکه و محیط گرافیکی است.

LionOS هنوز یک سیستم‌عامل در حال توسعه است و جایگزین آماده‌ای برای Windows یا Linux محسوب نمی‌شود. در این سند، قابلیت‌های پیاده‌سازی‌شده از قابلیت‌های آزمایشی و برنامه‌های آینده جدا شده‌اند.

### اصول توسعه

- زبان اصلی: C freestanding؛ استفاده محدود از NASM در بخش‌های ضروری.
- اولویت با صحت، پایداری، کیفیت کد و سازگاری سخت‌افزاری.
- حفظ مسیرهای سالم و جلوگیری از Regression در Kernel، حافظه، ورودی، USB و گرافیک.
- آزمون در QEMU و CI پیش از اعلام تکمیل هر مرحله.
- تفکیک وضعیت شناسایی سخت‌افزار، راه‌اندازی، بارگذاری Driver و عملکرد واقعاً آزمایش‌شده.

### راهنمای وضعیت

| نشان | معنی |
|---|---|
| ✅ | پیاده‌سازی موجود و آزمون نرم‌افزاری/CI مرتبط موفق |
| 🟡 | پیاده‌سازی جزئی، آزمایشی یا دارای محدودیت شناخته‌شده |
| ⬜ | پیاده‌سازی نشده یا در نقشه‌راه قرار دارد |

موفقیت CI به‌تنهایی تضمین‌کننده عملکرد روی تمام سخت‌افزارهای فیزیکی نیست.

---

## قابلیت‌های فعلی

### Kernel، CPU و Interrupts

| قابلیت | وضعیت |
|---|:---:|
| راه‌اندازی GRUB و Multiboot2 | ✅ |
| GDT / IDT / TSS و Protected Mode | ✅ |
| CPU Exceptions و مسیر Kernel Panic | ✅ |
| PIC / PIT و مدیریت پایه وقفه‌ها | ✅ |
| CPUID و شناسایی قابلیت‌های CPU | ✅ |
| ACPI MADT و شناسایی Local APICها | ✅ |
| راه‌اندازی APها با INIT / SIPI و handshake | ✅ |
| Per-CPU TSS / IDT و Local APIC Timer | ✅ |
| SMP bring-up و آزمون تا ۱۶ vCPU در QEMU | ✅ |
| Snapshot و اعتبارسنجی پایه MTRR / PAT | 🟡 |
| Interrupt Routing کامل برای سخت‌افزارهای متنوع | ⬜ |
| Lazy FPU | ⬜ |

**محدودیت SMP:** زیرساخت راه‌اندازی AP و Per-CPU وجود دارد؛ اما Scheduler را نباید یک Scheduler کاملاً همزمان با مهاجرت آزاد پردازش‌ها بین تمام CPUها دانست. Work-stealing و مهاجرت امن Task هنوز تکمیل نشده‌اند.

### مدیریت حافظه

| قابلیت | وضعیت |
|---|:---:|
| Physical Page Allocator | ✅ |
| Paging و فضای آدرس مجازی Kernel | ✅ |
| Kernel Heap | ✅ |
| Address Space مجزا برای Processها | ✅ |
| جداسازی User و Kernel | ✅ |
| شمارش Reference صفحات فیزیکی | ✅ |
| Copy-on-Write برای fork و رسیدگی به Write Fault | ✅ |
| پشتیبانی از آدرس‌های فیزیکی بالاتر | 🟡 |
| NX و کنترل مجوز صفحات | 🟡 |
| mmap و Anonymous Mapping | ⬜ |
| Demand Paging و Lazy Allocation | ⬜ |
| Slab / Object Allocator | ⬜ |
| Memory Accounting و Guard Pages | ⬜ |
| Swap و Memory Pressure Management | ⬜ |

### Process، Thread، Scheduler و IPC

| قابلیت | وضعیت |
|---|:---:|
| اجرای برنامه در Ring 3 | ✅ |
| Context Switching و Preemptive Scheduling | ✅ |
| fork / exec / exit / waitpid | ✅ |
| Kernel Thread و User Thread | ✅ |
| PID / TID و چرخه عمر Thread | ✅ |
| زمان‌بندی اولویت‌محور و تعاملی | ✅ |
| Per-CPU Scheduler State و CPU ownership | ✅ |
| FPU/SSE Context Switching | ✅ |
| Pipe و Blocking Pipe I/O | ✅ |
| Process Group و Session پایه | ✅ |
| waitpid با WNOHANG / WUNTRACED / WCONTINUED | ✅ |
| Foreground / Background و Controlling TTY کامل | 🟡 |
| Signal delivery و handling کامل در User Space | ⬜ |
| Shared Memory و Unix Domain Socket | ⬜ |
| مهاجرت امن Task و Load Balancing واقعی | ⬜ |

### Storage، VFS و Filesystem

| قابلیت | وضعیت |
|---|:---:|
| VFS و File Descriptorها | ✅ |
| RAMFS | ✅ |
| LionFS پایدار | ✅ |
| ATA PIO | ✅ |
| ELF32 Loader | ✅ |
| Ext2 Read-only | 🟡 |
| Directory و عملیات کامل مدیریت فایل در LionFS | ⬜ |
| Permission / Ownership / Timestamp | ⬜ |
| Mount / Unmount و Filesystem Driver API کامل | ⬜ |
| FAT32 | ⬜ |
| AHCI / SATA | ⬜ |
| NVMe | ⬜ |
| Buffer Cache، Journaling و Recovery | ⬜ |

### ورودی، USB و Device Drivers

| قابلیت | وضعیت |
|---|:---:|
| PS/2 Keyboard و Mouse | ✅ |
| PCI Device Enumeration پایه | 🟡 |
| xHCI USB Controller Path | ✅ |
| USB HID Mouse | ✅ |
| USB HID Keyboard و HID Report Parsing | 🟡 |
| USB Composite Device و چند Interface | 🟡 |
| HID Boot و برخی Non-boot Reportها | 🟡 |
| Cursor Smoothing | ✅ |
| Generic Input Event API | ⬜ |
| I²C-HID برای صفحه‌کلید و تاچ‌پد داخلی لپ‌تاپ | ⬜ |
| USB Mass Storage و USB Gamepad | ⬜ |
| Hot-plug عمومی و Device Manager | ⬜ |
| UHCI / OHCI / EHCI | ⬜ |

**نکته سخت‌افزاری:** بسیاری از لپ‌تاپ‌های جدید صفحه‌کلید و تاچ‌پد داخلی را از طریق I²C-HID ارائه می‌کنند. پشتیبانی PS/2 یا USB HID به معنی پشتیبانی از این دستگاه‌ها نیست. آزمون xHCI در CI با USB مجازی QEMU انجام می‌شود و جایگزین تست روی دستگاه فیزیکی نیست.

### شبکه

| قابلیت | وضعیت |
|---|:---:|
| Loopback IPv4 | ✅ |
| RTL8139 Ethernet | 🟡 |
| Ethernet TX/RX | 🟡 |
| ARP / IPv4 / ICMP | 🟡 |
| Ping در User Space | 🟡 |
| Network Syscallهای پایه | 🟡 |
| TCP / UDP و Socket API | ⬜ |
| DHCP و DNS Resolver | ⬜ |
| IPv6 و Routing | ⬜ |
| VirtIO-net و Intel e1000 | ⬜ |
| ابزارهای پیکربندی و عیب‌یابی شبکه | ⬜ |

### Desktop، GUI و Browser

| قابلیت | وضعیت |
|---|:---:|
| VGA Console و Shell | ✅ |
| Firmware Framebuffer از Multiboot2 | ✅ |
| Desktop گرافیکی مبتنی بر Framebuffer | ✅ |
| Window، Taskbar و Launcher پایه | ✅ |
| Wallpaper و Assetهای گرافیکی | ✅ |
| Mouse Cursor و تعامل پایه | ✅ |
| Software Compositor | 🟡 |
| Window Manager مستقل و کامل | ⬜ |
| Double / Triple Buffering و Damage Tracking | ⬜ |
| Clipboard، Notification و چند Workspace | ⬜ |
| Font Engine و HiDPI | ⬜ |
| Browser آزمایشی با HTTP روی IPv4 و آدرس عددی | 🟡 |
| نمایش ساده متن HTML | 🟡 |
| DNS، HTTPS/TLS، JavaScript و CSS Layout | ⬜ |
| Image Rendering و Browser Storage | ⬜ |

مرورگر فعلی نمونه‌ای آزمایشی برای اتصال شبکه و نمایش ساده متن است و با مرورگرهای وب مدرن قابل مقایسه نیست.

### Graphics و GPU

مسیر نمایش فعلی LionOS از **Firmware Framebuffer** و ترکیب نرم‌افزاری تصویر استفاده می‌کند. لایه شناسایی گرافیک Intel برای آماده‌سازی Driver بومی توسعه یافته است؛ اما شناسایی GPU یا خواندن MMIO به معنی شتاب‌دهی گرافیکی نیست.

| قابلیت | وضعیت |
|---|:---:|
| حفظ و استفاده از Multiboot2 Framebuffer | ✅ |
| Software Compositor پایه | 🟡 |
| PCI Graphics Device Discovery | ✅ |
| شناسایی Intel Display Function و Device IDهای پشتیبانی‌شده | ✅ |
| تشخیص Intel UHD روی سخت‌افزار Alder Lake آزمایش‌شده | ✅ |
| بررسی اولیه BAR و محدوده MMIO | 🟡 |
| اعتبارسنجی کامل GGTT / Stolen Memory | ⬜ |
| Native Intel Display Driver و Mode Setting | ⬜ |
| Graphics Memory Manager | ⬜ |
| Hardware Cursor و VBlank | ⬜ |
| 2D BLT / Fill / Copy Acceleration | ⬜ |
| GPU Command Submission و Ring Management | ⬜ |
| اتصال شتاب‌دهی GPU به Compositor | ⬜ |
| Native Driver برای NVIDIA و AMD | ⬜ |
| EDID و Multi-display | ⬜ |

**محدودیت مهم:** در این مرحله شتاب‌دهی سخت‌افزاری Intel/NVIDIA/AMD فعال نیست و GPU Command Submission انجام نمی‌شود. مسیر Framebuffer نرم‌افزاری باید تا زمان اعتبارسنجی کامل Driver حفظ شود.

### Userland و ابزارها

| قابلیت | وضعیت |
|---|:---:|
| ELF32 User Program و User API | ✅ |
| libc پایه و تخصیص حافظه User Space | 🟡 |
| echo / cat / ls / pwd / uname | ✅ |
| rm / stat / grep / find / cp / mv | ✅ |
| ping و تست‌های VFS / COW | 🟡 |
| TTY با ورودی Blocking و حالت Canonical / Noncanonical | 🟡 |
| Shell Job Control کامل | ⬜ |
| mkdir / rmdir / chmod / chown | ⬜ |
| Process Monitor و System Monitor کامل | ⬜ |
| Package Manager و Application Format | ⬜ |
| Init و Service Manager | ⬜ |
| ابزارهای Debugging بومی | ⬜ |

---

## معماری

```text
                    GRUB / Multiboot2
                           |
                           v
                  +-------------------+
                  |    LIONOS KERNEL |
                  |-------------------|
                  | CPU / Interrupts  |
                  | SMP / Scheduler  |
                  | Memory / Paging  |
                  | Syscalls / UAPI  |
                  | VFS / Filesystems |
                  | Drivers / Network|
                  | Framebuffer GUI  |
                  +---------+---------+
                            |
                 +----------+----------+
                 |                     |
                 v                     v
          +-------------+       +--------------+
          | RING-3 APPS |       |   HARDWARE   |
          | ELF32 / libc|       | ATA / RTL8139|
          | VFS / Syscall|      | PS/2 / xHCI  |
          +-------------+       +--------------+
```

### ساختار مخزن

```text
LionOS/
├── boot/                 # Multiboot2 و کد راه‌اندازی
├── include/              # Headerهای Kernel و UAPI
├── src/                  # Kernel، Driverها، VFS، GUI و SMP
├── user/                 # برنامه‌ها، libc و تست‌های User Space
├── assets/               # لوگو، آیکون‌ها و Wallpaperها
├── docs/                 # مستندات فنی و طراحی
├── scripts/              # اسکریپت‌های Build و Test
├── tests/                # تست‌های مستقل
├── .github/workflows/    # GitHub Actions
├── Makefile
├── linker.ld
└── README.md
```

## پیش‌نیازها

- سیستم میزبان Linux
- GCC با پشتیبانی از خروجی ۳۲ بیتی
- NASM، GNU Make و Binutils
- GRUB tooling و xorriso برای ساخت ISO
- QEMU برای اجرا و تست

LionOS یک Kernel از نوع freestanding است و به Runtime سیستم‌عامل میزبان وابسته نیست.

## ساخت و اجرا

```bash
git clone https://github.com/AngmarX-dev/LionOS.git
cd LionOS

make
make userland
make iso
make disk
```

اجرای QEMU:

```bash
make run
```

برای اجرای QEMU در پنجره عادی:

```bash
LIONOS_QEMU_FULLSCREEN=0 make run
```

اجرای تست‌ها:

```bash
make test
```

در صورت تفاوت Targetها، Makefile مرجع نهایی دستورات ساخت است.

## آزمون‌ها و اعتبارسنجی

آزمون‌های پروژه در چند سطح انجام می‌شوند:

- Build کردن Kernel و Userland
- اعتبارسنجی Multiboot2 و ELF32
- ساخت ISO و Disk Image
- Boot و Smoke Test در QEMU
- تست‌های حافظه، VFS و Copy-on-Write
- بررسی Persistence پس از راه‌اندازی مجدد
- آزمون SMP در پیکربندی‌های چند CPU
- تست xHCI USB Mouse در QEMU

موفقیت QEMU/CI عملکرد روی تمام BIOSها، چیپ‌ست‌ها، کنترلرهای USB، GPUها یا لپ‌تاپ‌ها را تضمین نمی‌کند. تست فیزیکی برای هر خانواده سخت‌افزاری لازم است.

## ساخت Live USB

ابتدا Image را بسازید:

```bash
make clean
make usb
```

دستگاه USB را شناسایی کنید:

```bash
lsblk
```

سپس Image را روی کل دستگاه USB بنویسید، نه پارتیشن آن:

```bash
sudo umount /dev/sdX* 2>/dev/null || true
sudo dd if=build/lionos-usb.img of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

> **هشدار:** دستور dd تمام داده‌های دستگاه مقصد را حذف می‌کند. پیش از اجرا نام دیسک را با lsblk چند بار بررسی کنید.

---

## مدل امنیتی و محدودیت‌ها

LionOS برای اجرای کد User Space از Ring 3 و بررسی ورودی‌های System Call استفاده می‌کند. کنترل محدوده حافظه User، اعتبارسنجی داده‌های ورودی و محدودیت دسترسی به برخی عملیات پیاده‌سازی شده یا در حال توسعه‌اند.

با این حال، پروژه هنوز ممیزی امنیتی مستقل، مدل کامل User/Group و Permission، ASLR، محافظت جامع در برابر آسیب‌پذیری‌های Kernel یا تضمین امنیت در برابر کد مخرب ندارد. بنابراین برای داده‌های حساس یا محیط عملیاتی مناسب نیست.

---

## نقشه‌راه

ترتیب زیر اولویت مهندسی مراحل باقی‌مانده را نشان می‌دهد؛ این فهرست تعهد زمانی یا تضمین نسخه انتشار نیست.

### مرحله A — تثبیت Kernel و SMP

- ⬜ تکمیل آزمون‌های Stress برای SMP و Synchronization
- ⬜ طراحی Run Queue مستقل برای هر CPU با مالکیت و قفل‌گذاری روشن
- ⬜ پیاده‌سازی مهاجرت امن Task و Work-stealing پس از آماده‌شدن زیرساخت
- ⬜ تکمیل Interrupt Routing و آمار Per-CPU
- ⬜ بررسی Lazy FPU در کنار مسیر فعلی FPU/SSE
- ⬜ تست طولانی‌مدت Context Switch، Timer و Process Lifecycle

### مرحله B — حافظه و قابلیت اطمینان

- ⬜ تکمیل مدیریت صفحات در تمام محدوده‌های پشتیبانی‌شده
- ⬜ mmap و Anonymous Mapping
- ⬜ Demand Paging و Lazy Allocation
- ⬜ Slab / Object Allocator
- ⬜ Guard Page و Memory Accounting
- ⬜ تست فشار حافظه و بازیابی از خطا
- ⬜ طراحی Swap در صورت نیاز معماری

### مرحله C — Driver و ورودی

- ⬜ Generic Device Model و Driver Registration API
- ⬜ تکمیل PCI Capability و BAR Resource Management
- ⬜ Generic Input Event Subsystem
- ⬜ I²C و I²C-HID برای سخت‌افزارهای جدید
- ⬜ تکمیل USB HID و تست روی چند کنترلر واقعی
- ⬜ USB Mass Storage
- ⬜ Hot-plug و مدیریت چرخه عمر Device

### مرحله D — Storage و Filesystem

- ⬜ تکمیل Directoryها و عملیات فایل LionFS
- ⬜ Permission، Ownership و Timestamp
- ⬜ Mount / Unmount و Filesystem Interface
- ⬜ FAT32 و بهبود Ext2
- ⬜ AHCI و سپس NVMe
- ⬜ Buffer Cache و Disk Cache
- ⬜ Journaling یا سازوکار Recovery مناسب
- ⬜ ابزار Filesystem Check و تست قطع ناگهانی برق

### مرحله E — Networking

- ⬜ تکمیل و سخت‌سازی RTL8139
- ⬜ UDP و TCP
- ⬜ Socket API
- ⬜ DHCP و DNS
- ⬜ Network Interface Abstraction
- ⬜ VirtIO-net و e1000
- ⬜ IPv6 و Routing
- ⬜ Packet Diagnostics و تست پایداری شبکه

### مرحله F — Graphics Engine

- ⬜ تکمیل تشخیص CPU/GPU و قابلیت‌ها بدون وابستگی به یک مدل خاص
- ⬜ اعتبارسنجی BAR، MMIO، MTRR و PAT
- ⬜ شناسایی مالکیت و محدوده GGTT و Stolen Memory
- ⬜ مدیریت امن Graphics Memory
- ⬜ پیاده‌سازی عملیات 2D محدود و قابل‌آزمون (Fill / Copy / BLT)
- ⬜ طراحی Command Submission پس از اثبات صحت Memory و Engine State
- ⬜ اتصال شتاب‌دهی به Compositor همراه Software Fallback
- ⬜ پشتیبانی Native از Intel GPUهای بیشتر
- ⬜ بررسی معماری Driver برای AMD و NVIDIA
- ⬜ EDID، Mode Setting، VBlank و Multi-display

**اصل ایمنی Graphics:** تا وقتی مالکیت حافظه گرافیکی، نوع Cache، آدرس‌دهی و وضعیت Engine اثبات نشده، نباید GGTT یا Ring Buffer دستکاری شود یا فرمان GPU ارسال گردد. Software Framebuffer باید مسیر پشتیبان باقی بماند.

### مرحله G — Desktop و User Experience

- ⬜ Window Manager مستقل
- ⬜ Compositor با Double / Triple Buffering
- ⬜ Damage Tracking و مدیریت Z-order
- ⬜ Widget Toolkit و Text Input
- ⬜ Clipboard، Notification و Dialog
- ⬜ Workspace و Session Manager
- ⬜ Font Rendering و HiDPI
- ⬜ Accessibility و مدیریت ورودی چند دستگاه

### مرحله H — Userland و ابزارهای توسعه

- ⬜ تکمیل libc و قراردادهای استاندارد فایل
- ⬜ Shell Job Control و Redirection
- ⬜ Signal API کامل
- ⬜ ابزارهای مدیریت فایل و Permission
- ⬜ Process Monitor و System Monitor
- ⬜ Editor و ابزارهای Debugging
- ⬜ Package Manager و Application Format
- ⬜ Init و Service Manager

### مرحله I — Browser و برنامه‌های شبکه‌ای

- ⬜ HTTP کامل‌تر شامل Header، Redirect و Chunked Transfer
- ⬜ DNS و HTTPS/TLS
- ⬜ HTML Parser و Document Tree
- ⬜ CSS Parser و Layout Engine
- ⬜ Image Decoder و Font Integration
- ⬜ Browser Storage، Cookie و Download
- ⬜ بررسی JavaScript Engine در مرحله‌ای مستقل

### مرحله J — Security و Release Engineering

- ⬜ User / Group و Permission Enforcement
- ⬜ Capability Model دقیق‌تر
- ⬜ Kernel Stack و User Stack Protection
- ⬜ ASLR و Hardening
- ⬜ Fuzzing برای Parserها و System Callها
- ⬜ تست‌های طولانی‌مدت و Regression در چند پیکربندی QEMU
- ⬜ ماتریس تست سخت‌افزار فیزیکی
- ⬜ Release Artifact قابل تکرار و نسخه‌بندی‌شده
- ⬜ مستندات توسعه Driver و Kernel
- ⬜ بررسی امکان Self-hosting در آینده

---

## اهداف بلندمدت

1. Kernel قابل‌اعتماد با مدیریت حافظه و Process مستقل.
2. Scheduler چندپردازنده‌ای با Synchronization و Migration امن.
3. Driver Model قابل‌گسترش برای سخت‌افزارهای متنوع.
4. Filesystem پایدار و پشتیبانی از Storage مدرن.
5. شبکه کامل با TCP/IP و ابزارهای User Space.
6. Desktop مستقل با Compositor و شتاب‌دهی در سخت‌افزارهای پشتیبانی‌شده.
7. محیط برنامه‌نویسی و ابزارهای توسعه بومی.
8. مدل امنیتی مستند، آزمون‌پذیر و قابل ممیزی.

این اهداف بلندمدت هستند و نباید با قابلیت‌های فعلی اشتباه گرفته شوند.

## وضعیت فعلی پروژه

- معماری پایه Kernel، User Space، VFS، ورودی، شبکه آزمایشی و Desktop در مخزن وجود دارد.
- مسیر نمایش فعلی Framebuffer و Software Compositor است.
- Intel GPU در سطح PCI/Bootstrap شناسایی می‌شود؛ Native Acceleration هنوز پیاده‌سازی نشده است.
- SMP bring-up در QEMU آزمون شده؛ مهاجرت آزاد Task بین CPUها تکمیل نشده است.
- اجرای فیزیکی روی دستگاه‌های متنوع همچنان به آزمون سخت‌افزاری نیاز دارد.
- پروژه برای توسعه و آزمایش است و هنوز نسخه پایدار روزمره محسوب نمی‌شود.

## مشارکت

برای پیشنهاد قابلیت، گزارش خطا یا مشارکت در توسعه، از GitHub Issues و Pull Requests استفاده کنید. هر تغییر Kernel باید همراه با توضیح محدودیت‌ها، تست‌های مرتبط و بررسی اثر آن بر زیرسیستم‌های موجود باشد.

## مجوز

این پروژه تحت مجوز MIT منتشر شده است. برای جزئیات، فایل [LICENSE](LICENSE) را ببینید.
