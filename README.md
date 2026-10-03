# KONKR Pocket FIT input fix

[![CI](https://github.com/slimpdev/konkr-pocket-fit-input-fix/actions/workflows/ci.yml/badge.svg)](https://github.com/slimpdev/konkr-pocket-fit-input-fix/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-SteamOS%20ARM-1b2838)](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds)

Userspace controller glitch filter for the **KONKR Pocket FIT family** running the community SteamOS ARM build with **InputPlumber**.

It fixes short false **R3** presses and isolated **L2/R2** spikes without patching the kernel. The filter grabs the physical controller, creates a corrected virtual controller, then hands that virtual device to InputPlumber.

## Supported devices

| Device | SoC | Controller path | Status |
|---|---|---|---|
| KONKR Pocket FIT | SM8650 | XInput or DirectInput | Implemented from current SteamOS ARM/InputPlumber profiles; hardware confirmation welcome |
| KONKR Pocket FIT Elite | SM8750 | `AYANEO MCU Gamepad` | **Tested on hardware** with InputPlumber 0.81.0 |

The installer auto-detects the model. Future KONKR models are **not** automatically treated as compatible.

## What it fixes

- R3 / `BTN_THUMBR`: ignores pulses shorter than **50 ms**;
- L2/R2: requires **two consecutive non-zero frames** before a trigger press is exposed;
- real R3 presses still work after the short confirmation delay;
- trigger release is immediate;
- input capabilities and VID/PID are cloned from the physical controller;
- `FF_RUMBLE` is proxied when the evdev source supports it;
- starts before InputPlumber and survives reboot.

For the regular Pocket FIT the trigger axes are selected from the detected controller mode:

- XInput `045e:028e`: `ABS_Z` / `ABS_RZ`;
- DirectInput `4001:0428`: `ABS_BRAKE` / `ABS_GAS`; `ABS_Z` / `ABS_RZ` remain the right stick.

## Install

From a cloned repository:

```bash
git clone https://github.com/slimpdev/konkr-pocket-fit-input-fix.git
cd konkr-pocket-fit-input-fix
sudo ./install.sh
```

Or directly:

```bash
curl -fsSL https://raw.githubusercontent.com/slimpdev/konkr-pocket-fit-input-fix/main/install.sh | sudo bash
```

A working C compiler (`cc`, `gcc`, or `clang`) is required. When run through `curl`, the installer downloads `src/konkr-input-fix.c` from this repository and compiles it locally.

## Architecture

```text
Physical KONKR controller
        |
        | EVIOCGRAB
        v
  konkr-input-fix
  - R3: 50 ms debounce
  - L2/R2: 2-frame filter
        |
        v
KONKR Filtered Gamepad
        |
        v
    InputPlumber
        |
        v
SteamOS Handheld Controller
```

No kernel module is replaced and the stock controller driver remains untouched.

### Why the virtual pad reports `BUS_BLUETOOTH`

InputPlumber 0.81 skips ordinary virtual/uinput evdev nodes unless they are specifically allowed. The filtered controller therefore uses `BUS_BLUETOOTH` as a **userspace compatibility marker** while preserving the original VID/PID. This does not create or require a Bluetooth connection.

### Pocket FIT Elite

The Elite stock profile matches the raw controller by the literal name `AYANEO MCU Gamepad`. The installer adds local `/etc/inputplumber/devices.d` rules that ignore the raw source and route `KONKR Filtered Gamepad` plus `KONKR System Buttons` into the normal SteamOS controller.

### Pocket FIT

The regular FIT stock profile supports both XInput and DirectInput and identifies the main pad by VID/PID. The filter preserves those IDs, so the existing capability map continues to be selected. A small earlier-loading local InputPlumber rule ignores the raw evdev source so only the filtered copy reaches the composite controller.

## Verify

Check services and routing:

```bash
sudo ./install.sh --status
```

Or manually:

```bash
systemctl is-active konkr-input-fix inputplumber
sudo find /dev/inputplumber/by-hidden -maxdepth 1 -type l -printf '%p -> %l\n' | sort
sudo journalctl -u konkr-input-fix -u inputplumber -b --no-pager | tail -60
```

In Steam open:

**Settings → Controller → SteamOS Handheld Controller → Test Device Inputs**

Move the right stick aggressively for 30–60 seconds without clicking it. R3 should not fire. Then make a real R3 press and verify that it still registers.

## Uninstall

From a clone:

```bash
sudo ./install.sh --uninstall
```

Or without keeping the repository:

```bash
curl -fsSL https://raw.githubusercontent.com/slimpdev/konkr-pocket-fit-input-fix/main/install.sh | sudo bash -s -- --uninstall
```

The uninstaller removes the daemon, local InputPlumber rules and systemd integration, then restarts InputPlumber on its stock controller path.

## Troubleshooting

If the controller stops responding after installation, collect:

```bash
sudo ./install.sh --status
inputplumber --version
cat /sys/firmware/devicetree/base/model 2>/dev/null; echo
sudo journalctl -u konkr-input-fix -u inputplumber -b --no-pager
```

Then open an issue and include the output plus whether the device is a **Pocket FIT** or **Pocket FIT Elite**.

For the Elite, the expected chain after boot is roughly:

```text
AYANEO MCU Gamepad -> konkr-input-fix -> KONKR Filtered Gamepad -> InputPlumber
```

Event numbers such as `event6` or `event8` are dynamic and are not expected to stay fixed between boots.

## Development

Build locally:

```bash
cc -O2 -Wall -Wextra -Werror src/konkr-input-fix.c -o konkr-input-fix
```

Syntax-check the installer:

```bash
bash -n install.sh
```

## Upstream references

- [SteamOS-ARM-Handhelds](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds)
- [InputPlumber](https://github.com/ShadowBlip/InputPlumber)
- regular Pocket FIT InputPlumber profile: [`02-konkr-pocketfit.yaml`](https://github.com/hashtagbasit/SteamOS-ARM-Handhelds/blob/main/sm8650-overlay/etc/inputplumber/devices.d/02-konkr-pocketfit.yaml)

## License

MIT
