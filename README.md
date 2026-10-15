# zmk-studio-log

Stream a ZMK keyboard's log to a configurator over the **same USB connection
ZMK Studio already uses**. No second serial port, no `zmk-usb-logging`
snippet, and no separate terminal app.

It captures:

- `printk()` output, and
- Zephyr logging (`LOG_INF`, `LOG_WRN`, ...) when `CONFIG_LOG=y`.

The configurator turns the stream on and off. While it is off, nothing is
sent, but the last 32 lines are kept so boot messages show up when you connect.

## Requirements

1. **ZMK with Studio raw frames.** Upstream ZMK does not have them yet. Apply
   [`patches/0001-studio-raw-frames.patch`](patches/0001-studio-raw-frames.patch)
   to your ZMK tree. It applies cleanly to ZMK `main` (2026-10-09, `4dd95d78`)
   and `v0.3.0`. Without it, the build stops with an error that names this patch.
2. **ZMK Studio over USB**: `CONFIG_ZMK_STUDIO=y` and the `studio-rpc-usb-uart`
   snippet.

## Setup

### 1. Use a ZMK that has the patch

Push the patch to a branch of your own ZMK fork, then point `config/west.yml`
at it:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: you
      url-base: https://github.com/<your-github-user>
  projects:
    - name: zmk
      remote: you
      revision: studio-raw-frames # branch with the patch applied
      import: app/west.yml
    - name: zmk-studio-log
      remote: you
      revision: main
  self:
    path: config
```

### 2. Enable it

In `config/<keyboard>.conf`:

```ini
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_STUDIO_LOG=y
```

In `build.yaml`, on the half that talks to the computer (the central half of a split):

```yaml
include:
  - board: nice_nano
    shield: corne_left
    snippet: studio-rpc-usb-uart
```

Optional: also capture Zephyr logging.

```ini
CONFIG_LOG=y
CONFIG_ZMK_LOG_LEVEL_INF=y
```

### 3. Open the log

Connect the configurator over USB and click **Enable log** in the keyboard log
panel. The first line is `zmk_studio_log v1 ...`, followed by the replayed
history and then live lines.

## Restart diagnostics (opt-in)

For tracking down unexplained restarts:

```ini
CONFIG_ZMK_STUDIO_LOG_RESTART_DIAG=y
```

This turns on three things. Each can be switched off on its own.

- **Reset reason** (`ZMK_STUDIO_LOG_RESET_REASON`). At every boot it prints
  `zmk_diag boot #<n> reset=<causes>`. The causes are `power_on`, `reset_pin`,
  `software`, `watchdog` (the firmware hung), `brownout` (the supply voltage
  dropped), `wake_from_sleep` and a few rarer ones. `#n` counts boots since
  the last power loss.
- **Crash capture** (`ZMK_STUDIO_LOG_FAULT_CAPTURE`). Replaces Zephyr's fatal
  error handler. On a crash it saves the reason, program counter, link
  register and thread, then **reboots instead of halting**. The next boot
  prints `zmk_diag fault reason=... pc=0x...`. To find the source line, run
  `arm-zephyr-eabi-addr2line -e zmk.elf <pc>` against the exact build that
  crashed. Every boot without a crash prints `zmk_diag fault none`.
- **Log history across restarts** (`ZMK_STUDIO_LOG_RETAIN_HISTORY`). The last
  `ZMK_STUDIO_LOG_HISTORY` lines survive the restart. Enabling the log
  afterwards shows what happened just before it.

All three keep their data in RAM that isn't cleared at boot. That survives a
crash, watchdog or reset-button restart, but **not a power loss**. Each record
is checked at boot, so RAM that wasn't kept reads as empty rather than as garbage.

## Black box (opt-in)

```ini
CONFIG_ZMK_STUDIO_LOG_BLACK_BOX=y
```

The black box is a flight recorder that survives power loss. It records:

- boots (with the reset cause) and crashes
- USB state and output changes (USB or Bluetooth)
- Bluetooth profile changes, connections, failed connections and disconnections (with the reason)
- split link up and down
- battery level
- idle and sleep

Events go into a RAM ring (64 by default) that also survives non-power-loss
restarts. They are saved to flash in blocks of 16 that rotate through 8
slots, so the newest 128 events persist.

A block is saved once 16 events are waiting, right after each boot, or after
5 quiet minutes. Saves never happen more than once a minute
(`ZMK_STUDIO_LOG_BLACK_BOX_SAVE_MIN_SECONDS`), and an idle keyboard never
writes. If events arrive faster than they can be saved, the oldest unsaved
ones are dropped and a `dropped=N` event records how many.

"Load retained log" in the configurator (a `D` `1` frame) prints the saved
events oldest first, then the ones not yet in flash, and then starts the live log:

```
zmk_diag dump begin
zmk_diag #41 boot=1 t=0.004s boot reset_cause=0x00000010
zmk_diag #41 boot=1 t=12.310s ble_disconnect profile=0 reason=0x08 addr=..a1b2c3
...
zmk_diag unsaved#1 boot=2 t=0.004s boot reset_cause=0x00000002
zmk_diag dump end saved=37 unsaved=1
```

The configurator explains these lines, for example `reset_cause` bits and
Bluetooth disconnect reasons. Your own code can add events with type `0x80`
or above:

```c
#include <zmk_studio_log/diag.h>

zmk_studio_log_diag_record(0x80, my_state, my_reason, my_value);
```

**Limits:** events from the moments just before a power loss may not have
been saved yet. Only the half that runs Studio, the central half of a split,
records anything.

## Options

| Kconfig | Default | |
|---|---|---|
| `ZMK_STUDIO_LOG_PRINTK` | `y` | Capture `printk()`. Replaces any other printk hook, such as a UART console. |
| `ZMK_STUDIO_LOG_BACKEND` | `y` if `LOG` | Capture Zephyr logging as text. |
| `ZMK_STUDIO_LOG_USB_ONLY` | `y` | Never stream over Bluetooth, where log traffic competes with key reports. |
| `ZMK_STUDIO_LOG_HISTORY` | `32` | Lines kept for replay when the stream starts; `0` disables replay. |
| `ZMK_STUDIO_LOG_QUEUE` | `16` | Lines buffered while streaming; when full, new lines are dropped. |
| `ZMK_STUDIO_LOG_LINE_MAX` | `120` | Longer lines are split. |
| `ZMK_STUDIO_LOG_RESTART_DIAG` | `n` | Reset reason, crash capture and retained history, as above. |
| `ZMK_STUDIO_LOG_BLACK_BOX` | `n` | Flash-backed event recorder, as above. |
| `ZMK_STUDIO_LOG_BLACK_BOX_RING` / `_BLOCK` / `_SLOTS` | `64` / `16` / `8` | Events in RAM, events per save, saved blocks kept. |
| `ZMK_STUDIO_LOG_BLACK_BOX_SAVE_MIN_SECONDS` / `_SAVE_IDLE_SECONDS` | `60` / `300` | Fastest save rate, and when a partial block is saved. |

Measured on nice!nano + Corne left with Studio over USB, compared with the
patch alone:

- log stream only: about +1.6 KB flash, +6 KB RAM
- with restart diagnostics: +1.4 KB flash, +0.5 KB RAM on top of that
- with the black box: +3.8 KB flash, +3.5 KB RAM on top of the log stream

All sizes are with default settings.

## Protocol

Frames use Studio's normal SOF `0xAB` / ESC `0xAC` / EOF `0xAD` framing. Their
first byte is a prefix whose protobuf wire type (`prefix & 7` is 3, 4, 6 or 7)
proto3 never produces, so they can't be mistaken for RPC messages.

| Direction | Payload | Meaning |
|---|---|---|
| host → keyboard | `C` `1` | Stream on: hello line, then history, then live lines |
| host → keyboard | `C` `0` | Stream off; the keyboard answers `L zmk_studio_log off` |
| host → keyboard | `D` `1` | Black box dump (`zmk_diag ...` lines), then the live log; needs `ZMK_STUDIO_LOG_BLACK_BOX` |
| keyboard → host | `L` *text* | One log line, UTF-8, no newline |

Firmware without this module ignores `C` frames. A host should therefore treat
"no `zmk_studio_log` hello after enabling" as "not supported".

Other modules can reuse the transport:

```c
#include <zmk_studio_log/studio_log.h>

zmk_studio_log_send_line("my_feature ready");       /* works even with the stream off */
zmk_studio_log_send_frame('K', data, len);          /* your own frame type */
```

They can also handle their own host requests:

```c
#include <zmk/studio/rpc.h>

static void on_frame(const uint8_t *payload, size_t len) { /* payload[0] == 'G' */ }
ZMK_RPC_RAW_FRAME_HANDLER(my_handler, 'G', on_frame);
```
