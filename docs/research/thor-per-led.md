# Per-LED control of the AYN Thor analog-stick RGB rings — feasibility & plan

Research only, 2026-11-16. Nothing on the device was written, remounted, flashed, or rebooted;
all adb calls were read-only queries (sysfs/getprop/pm/service/ls, plus file pulls of
`/system/bin/pservice` and the SELinux `.cil` files). No module was loaded. Disassembly was done
off-device on the already-pulled `.ko` files with Apple `llvm-objdump`.

Device recap (verified this session): AYN Thor, `ro.product.vendor.device=kalama` (SD 8 Gen 2),
Android 13, kernel vermagic `5.15.123-gafd857749d1f SMP preempt mod_unload modversions aarch64`,
build `eng.Thor.20260206.163241`. Bootloader UNLOCKED (`ro.boot.vbmeta.device_state=unlocked`),
active slot `_b`, **NOT rooted** (adb shell is `u:r:shell:s0`, no su). `veritymode=enforcing`,
`avb_version 1.2`.

---

## TL;DR

- **Per-LED colour is a pure software limit, confirmed from the datasheet-class register map and the
  module disassembly.** Each of the 12 channels per stick has its own 8-bit PWM register; the stock
  driver *chooses* to write all four LEDs of a ring the same colour.
- **It is NOT achievable without root.** There is no hidden sysfs attribute, no per-group path, and no
  extra service method. The only lower-level path (raw `/dev/i2c-*`) is blocked by SELinux for every
  non-root domain, including the `pservice` root-command service Bifrost already uses. SELinux is
  enforcing and the policy cannot be changed without root.
- **Recommended route: Magisk root by patching `init_boot_b`, then a tiny userspace i2c helper that
  writes the chip's PWM registers directly.** No kernel rebuild, no partition modification, fully
  reversible by `fastboot flash init_boot` of the stock image. Bifrost/our app would drive per-LED by
  sending `i2cset`/helper commands through the root shell (or keep using `PServerBinder` if the root
  solution also opens `/dev/i2c`).
- Effort for the per-LED feature itself, once rooted: small (a day). The cost is the owner's one-time
  root decision and a short calibration pass to learn the LED-position ordering.

---

## 1. Hardware: per-LED is a software limit (CONFIRMED)

### 1.1 The part is an IS31FL3236 / SN3236-class 36-channel constant-current PWM LED driver

The "HTR3212" (HEROIC) is register-compatible with the Lumissil/Si-En IS31FL3236 / SN3236 family.
Identified purely from the register addresses the driver pokes (disassembly of `htr3212l.ko`, offsets
below; `htr3212r.ko` is byte-for-byte the same logic with `l`→`r` strings):

| Register | Use | Evidence (htr3212l.ko) |
|---|---|---|
| `0x00` | Shutdown/normal (`0x01`=normal) | `set_enable` 0x800-0x82c writes reg `0x00`=1 last |
| `0x0D + 3*group + {0,1,2}` | **per-channel 8-bit PWM** (12 regs, `0x0D..0x18`) | `set_rgb_group_brightness` 0xc0c: `add w9,w22,#0x0d` / `#0x0e` / `#0x0f`, `w22 = group*3` |
| `0x25` | update/latch (write to apply PWM+control) | `Led_DataUpdate` 0xd74 writes reg `0x25` |
| `0x32 + 3*group + {0,1,2}` | per-channel on/off control (`0x32..0x3D`) | `turn_on_off_rgb_group` 0x8e8: `add w9,w22,#0x32` / `#0x33` / `#0x34` |
| `0x4A`, `0x4B` | global control, PWM frequency | `set_enable` 0x704-0x76c writes `0x4a`, `0x4b`=1 |

This exactly matches IS31FL3236 (PWM `0x01..0x24`, update `0x25`, LED-control `0x26..0x49`, global
`0x4A`, freq `0x4B`, reset `0x4F`). The Thor wires **12 of the 36 channels** (the block starting at
PWM `0x0D`, i.e. channels 13-24, control base `0x32 = 0x26+12`) to the four RGB LEDs of one ring.

**Each channel is an independent PWM register.** Therefore every one of the 4 LEDs × 3 colours per
stick is individually addressable at the silicon level. Per-LED radial fill is physically supported;
only software prevents it.

### 1.2 Why the stock path writes all four LEDs the same colour

`set_brightness` (sysfs `brightness` store handler, `htr3212l.ko` @0xa50) parses `"ch-R:G:B:br"`
with `strsep`/`kstrtoull`, then:

- `0xba8`: `cmp x8,#1` — **only acts when `ch == 1`** (any other `ch`, e.g. Bifrost's `"2-"`, just
  `printk`s and returns);
- `0xbb0-0xbfc`: calls `set_rgb_group_brightness(group, R, G, B)` in a fully unrolled loop for
  **group = 0, 1, 2, 3 with the same R/G/B**, then `0xc00` calls `Led_DataUpdate` (latch).

So the kernel deliberately broadcasts one colour to all four groups. The 4th field (`br`) is parsed
but never used (matches Bifrost's own comment). `set_rgb_group_brightness` itself takes a `group`
argument and writes only that group's three PWM registers — i.e. **the per-LED primitive already
exists inside the module; it is simply never exposed with a per-group argument.**

### 1.3 Physical LED position ↔ group index: UNCONFIRMED

Neither the driver nor the strings name physical positions. ROCKNIX's mainline Thor DT lists
`l1..l4`/`r1..r4` and (per prior notes) maps `l1→group4, l2→1, l3→2, l4→3`, which only hints at an
order. On the stock kernel the mapping is untestable without writing registers (root). After rooting,
a 10-minute calibration (light one group full, others off, observe which quarter of the ring lights)
fixes the order definitively. Treat the ROCKNIX order as a starting guess, not fact.

### 1.4 Bus topology (CORRECTION to prior notes)

Prior notes said "left on i2c0, right on i2c12". Verified wrong. Actual:

- **Left ring:** `htr3212l` at **i2c bus 3, address 0x3c** (`/sys/.../a80000.i2c/i2c-3/3-003c`).
- **Right ring:** `htr3212r` at **i2c bus 5, address 0x3c** (`/sys/.../890000.i2c/i2c-5/5-003c`).
- The matching char devices `/dev/i2c-3` and `/dev/i2c-5` exist (`CONFIG_I2C_CHARDEV=y`).

---

## 2. Hidden no-root paths: there are none

### 2.1 Stock module exposes exactly three sysfs attributes

`htr3212l_probe` calls `device_create_file` three times, for the three `dev_attr_*` objects in
`.data`: **`brightness`, `breath`, `enable`** (relocs `.data+0x110/0x130/0x150` →
`dev_attr_breath/brightness/enable`). Live node listing confirms only `brightness`, `breath`,
`enable` (+ `power/subsystem/uevent`) exist. All store/show handlers were disassembled:

- **`brightness`** (`set_brightness`): the broadcast-to-all-four path above. No per-LED selector.
- **`enable`** (`set_enable` @0x66c): `1` = power the ring on (GPIO + `0x4a/0x4b/0x00` init + all
  four groups on); anything else = whole-chip off. Whole-ring only.
- **`breath`** (`set_breath` @0xe14, worker `Led_Breath` @0x434): parses `"val:mask"`, runs a 100-step
  gamma breathing loop (`Gamma_L_T32` table) across all four groups. Per-group colour is taken from a
  single stored RGB; no per-LED input.

`init.qcom.rc` (lines 263-269) `chmod 0666` on `sn3112l/led/{enable,brightness,miniled_enable,
vled_enable,blackvalue}` and `sn3112r/led/{enable,brightness}`. **`miniled_enable`, `vled_enable`,
`blackvalue` do not exist** on this build (the module never creates them; the chmods fail silently) —
confirmed by the live listing. So no hidden mode/effect/per-group attribute.

### 2.2 `PServerBinder` (`/system/bin/pservice`) is a generic root-command runner, not an LED API

`pservice` runs as uid 0 in domain `u:r:pservice:s0` (ps: PID 1363). Its binder `onTransact` code 0
takes a string array `[command, token]` and executes the command via **`popen`** (strings:
`popen`, `cmd %s `, `run_cmd token`). That is its only LED-relevant capability — there is no dedicated
"set LED" method. Bifrost's `LedController.kt:213-219` just hands it
`echo "<ch>-R:G:B:br" > /sys/class/sn3112{l,r}/led/brightness`. So the framework/service layer adds
nothing beyond "run this shell string"; it cannot address single LEDs because the sysfs file it writes
cannot (§1.2). The AYN/Odin system apps present (`com.ayn.magni`, `com.odin.*`) use the same sysfs
path; none exposes a per-LED method.

### 2.3 Raw `/dev/i2c-*` from userspace is blocked by SELinux (the decisive point)

This is the only path that could reach per-LED without changing the kernel, and it is closed:

- `/dev/i2c-3` and `/dev/i2c-5` are labeled `u:object_r:device:s0` (the generic fallback `device`
  type), mode `crw-------` root-only (`/dev/i2c-5` is `root:root` too; only i2c-5 for audio? no — both
  LED buses are root-only `crw-------`).
- In the loaded policy, the broadest grant any domain gets on type `device` is
  `allow domain device (dir (search))` — **no `chr_file` read/write to `device` for any domain**, and
  there is **no `allow … device (chr_file (write))` rule at all**. `pservice` itself has essentially no
  allows beyond executing its own binary (`allow pservice pservice_exec (file …)`), is not permissive
  (zero `typepermissive` statements in the whole policy), and belongs only to attributes `domain` and
  `mlstrustedsubject`.
- `toybox i2cset/i2cget/i2cdetect` exist, but with no openable device node they are useless to
  `shell` or to `pservice`.

Even though `pservice` runs as root, SELinux (enforcing) stops it opening the i2c char device.
Granting that access = editing `sepolicy` = requires root. So **there is no no-root route to per-LED.**
(Note: `CONFIG_DEVMEM is not set` and `STRICT_DEVMEM` moot — the /dev/mem poke route is also gone.)

---

## 3. Root routes, ranked

Kernel facts that shape the options (from `/proc/config.gz`, read this session):
`CONFIG_MODULES=y`, `CONFIG_MODULE_SIG=y` but **`CONFIG_MODULE_SIG_FORCE` is NOT set** (and the live
`htr3212l/r` already show taint `(OE)` = out-of-tree + unsigned), `CONFIG_MODULE_SCMVERSION=y`,
`CONFIG_MODULES_TREE_LOOKUP=y`, `CONFIG_I2C_CHARDEV=y`, `CONFIG_KALLSYMS_ALL=y`, `CONFIG_KPROBES=y`,
`CONFIG_IKHEADERS=y`. `vendor`/`vendor_dlkm` are AVB-verified read-only logical partitions
(`veritymode=enforcing`). A/B device, active slot `_b`; `init_boot`, `boot`, `vendor_boot`, `vbmeta`,
`dtbo` are all discrete partitions with stock images recoverable via fastboot.

### Route (a) — Magisk root via patched `init_boot`, + userspace i2c helper  ★ RECOMMENDED

Android 13 GKI keeps the first-stage ramdisk in **`init_boot`**, so Magisk patches that (not `boot`,
which holds the GKI kernel). Then root opens `/dev/i2c-3`/`-5` directly and writes PWM registers.

Owner steps (all by the owner; I will not perform any of them):
1. `adb pull` or extract the **stock `init_boot_b.img`** (or the matching OTA/factory image for build
   `eng.Thor.20260206.163241` / `Thor02061722`). Keep it as the restore image.
2. Install the Magisk app, "Select and Patch a File" → the stock `init_boot.img` → get
   `magisk_patched-*.img`.
3. Reboot to bootloader; `fastboot flash init_boot_b magisk_patched.img` (flash the **active** slot,
   `_b`). Do **not** flash `--slot all`; leave `_a` stock as a fallback. Do not touch `vbmeta`
   (keeping verity on is fine — Magisk on init_boot does not require disabling it on this device
   because the modification is in the ramdisk image covered by the `boot`/`init_boot` AVB chain, which
   an unlocked bootloader does not enforce).
4. `fastboot reboot`; open Magisk, confirm root.
5. Per-LED helper: root can run `i2cset -f -y 3 0x3c <reg> <val>` for each PWM register, then latch
   with `i2cset -f -y 3 0x3c 0x25 0x00`. Registers from §1.1. The `-f` is required because the
   `htr3212l` driver holds address 0x3c (i2c-dev warns otherwise). A ~40-line C helper (open
   `/dev/i2c-3`, `ioctl(I2C_SLAVE_FORCE,0x3c)`, write the 12 PWM bytes + latch) is cleaner and faster
   than forking `i2cset` 13× per frame.

How our app would drive it: the helper runs as root (a Magisk service, or invoked via `su -c`). Bifrost
would send per-LED frames to the helper (e.g. a small local socket or a `su -c` pipe) instead of the
`echo >sysfs` broadcast. The existing `PServerBinder` path can stay for the non-per-LED effects.

Brick/recovery risk: **low.** Worst realistic case is a bootloop from a bad patch →
`fastboot flash init_boot_b stock_init_boot.img` restores it; slot `_a` is untouched as a second
fallback. No user data touched. Writing wrong i2c registers can only mis-light or blank the rings
(cosmetic, cleared by `enable 0`/reboot), not damage hardware at these currents. Downsides: tripping
root may disturb Play Integrity / anti-cheat for other apps (owner's call), and an OTA will overwrite
`init_boot` (re-patch after updates).

### Route (b) — Replacement or companion kernel module

Build a module that exposes a per-LED sysfs (e.g. `brightness_led` taking `"group-R:G:B"`), reusing
the existing `set_rgb_group_brightness` primitive. Feasible because `MODULE_SIG_FORCE` is off, so an
unsigned module loads (tainted, as the stock ones already are). But:

- It must match vermagic `5.15.123-gafd857749d1f … modversions` and the per-symbol CRCs
  (`modversions` on). That needs the Thor's GKI 5.15 source + this build's `Module.symvers`/KMI, which
  AYN has not published; you would be rebuilding against a reconstructed symver set (fragile), or
  rewriting the AYN driver from the disassembly (we have full disassembly, so this is doable but is
  real work).
- Loading: you still need root to `insmod`. You cannot replace the on-disk `.ko` in `vendor_dlkm`
  (AVB-verified, read-only); you would `insmod` your module from `/data` at boot via a Magisk
  late_start service, or ship it as a Magisk module that overlays `vendor_dlkm` (overlayfs) — both
  need Magisk anyway.
- Brick risk is higher than (a): a module that oopses in `probe` can panic the LED subsystem or wedge
  boot if loaded first-stage; recovery is the same fastboot path but the failure modes are nastier.

Verdict: **strictly worse than (a)** — it needs the same root and adds a kernel-build/ABI problem, for
no capability (a) lacks. Only pick (b) if the goal is a self-contained driver with no persistent root
helper.

### Route (c) — Raw i2c from non-root userspace

**Not possible.** Blocked by SELinux `device`-type labeling (§2.3). Becomes route (a) the moment you
have root. Listed only to close it out.

### Route (d) — KernelSU / other

KernelSU needs a modified **`boot`** (GKI kernel) image with the KSU kernel, which is a bigger, more
brick-prone change than patching `init_boot`, and requires either a prebuilt KSU kernel for this exact
build (none known) or building GKI yourself. No advantage over Magisk here. APatch (kernel-patch) is
similar. Not recommended over (a).

---

## 4. Effort & recommendation

- **Feasible without root: NO.** The hardware supports per-LED, but every userspace path to it on the
  stock, SELinux-enforcing, unrooted build is closed. No Bifrost change, plugin, preset field, or
  upstream PR can reach single LEDs on stock firmware.
- **Recommended: Route (a)** — Magisk via patched `init_boot_b` + a small root i2c helper. Lowest
  effort, no kernel/ABI work, reversible with the stock `init_boot` image, data-safe.
- **Owner must do, by hand:** obtain the stock `init_boot.img`, patch it in Magisk, `fastboot flash
  init_boot_b`, accept the root trade-offs (anti-cheat/integrity, re-patch after OTA). Then a short
  per-LED calibration pass to fix group→quarter ordering (§1.3).
- **Developer effort once rooted:** ~1 day. The i2c helper is ~40 lines (register map in §1.1 is
  fully known); the Bifrost side is a new per-LED frame encoder and a path to the helper. The radial
  stamina-wheel fill then maps the 4 groups to 4 quarters (hue/brightness per quarter, red/orange on
  low/exhaust) exactly as the owner described.
- **If the owner will not root:** per-LED is off the table; the best stock-firmware approximation
  remains the whole-ring gauge on the left stick via the existing broadcast `brightness` path (see
  `research.md` §1d.3) — one colour for the whole ring, modulated by stamina fraction, not a radial
  fill.

### Register cheat-sheet for the helper (left = bus 3, right = bus 5, addr 0x3c)

```
PWM (8-bit, 0x00..0xFF) per channel, group g in 0..3:
  R = 0x0D + 3*g,  G = 0x0E + 3*g,  B = 0x0F + 3*g      # g: 0x0D..0x18
On/off control per channel (0x00/0x01), group g:
  0x32 + 3*g, 0x33 + 3*g, 0x34 + 3*g                    # set 0x01 once at init
Latch after any PWM write:  write 0x25 = 0x00
Power on (if ring was disabled): 0x4A=0x00(global on), 0x4B=0x01(freq), 0x00=0x01(normal)
```
(R/G/B-to-channel and group-to-physical-quarter assignments are the driver's naming; verify both by
calibration after rooting.)
