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

## Options

| Kconfig | Default | |
|---|---|---|
| `ZMK_STUDIO_LOG_PRINTK` | `y` | Capture `printk()`. Replaces any other printk hook, such as a UART console. |
| `ZMK_STUDIO_LOG_BACKEND` | `y` if `LOG` | Capture Zephyr logging as text. |
| `ZMK_STUDIO_LOG_USB_ONLY` | `y` | Never stream over Bluetooth, where log traffic competes with key reports. |
| `ZMK_STUDIO_LOG_HISTORY` | `32` | Lines kept for replay when the stream starts; `0` disables replay. |
| `ZMK_STUDIO_LOG_QUEUE` | `16` | Lines buffered while streaming; when full, new lines are dropped. |
| `ZMK_STUDIO_LOG_LINE_MAX` | `120` | Longer lines are split. |

Measured on nice!nano + Corne left with Studio over USB, compared with the
patch alone: about +1.5 KB flash and +6 KB RAM with printk only (default sizes).

## Protocol

Frames use Studio's normal SOF `0xAB` / ESC `0xAC` / EOF `0xAD` framing. Their
first byte is a prefix whose protobuf wire type (`prefix & 7` is 3, 4, 6 or 7)
proto3 never produces, so they can't be mistaken for RPC messages.

| Direction | Payload | Meaning |
|---|---|---|
| host → keyboard | `C` `1` | Stream on: hello line, then history, then live lines |
| host → keyboard | `C` `0` | Stream off; the keyboard answers `L zmk_studio_log off` |
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
