# Claude Knob

<!-- Demo: add a 10-20 s GIF here (turning the knob switches Terminal windows; clicking a
     window turns the knob), e.g. ![Claude Knob demo](docs/demo.gif) -->

A USB knob for switching between Claude Code sessions. Each open session gets one position on the knob, and turning the knob brings that session's Terminal window to the front. It works the other way too: focus a window yourself and the knob turns to it. The knob shows each session's state (idle, running, needs you, done) on its LEDs or screen, and the menu-bar icon shows it too.

```
Claude Code ──hooks──▶ knobd hook ──unix socket──▶ knobd daemon ◀──USB serial (NDJSON)──▶ knob (ESP32-C6 / S3)
                                                       │  ▲
                              focus a tab by tty ──────┘  └────── which window is in front
                              (in-process AppleScript)            (CoreGraphics window list)
```

- `host/`: `knobd`, a Rust daemon with a menu-bar icon, plus a CLI (macOS for now; window focusing sits behind a `Focuser` trait so other platforms can be added).
- `firmware/t-knob/`: firmware for the **LilyGO T-Knob** (ESP32-C6, BLDC gimbal motor + MT6701 magnetic encoder, 4 LEDs; the buzzer footprint isn't populated). The motor creates the detents: exactly one per session, with walls past the first and last. An add-on GC9A01 round LCD on the knob's static centre shows a session dial (see `hardware/`).
- `firmware/t-encoder-pro/`: firmware for the LilyGO T-Encoder Pro (ESP32-S3, round AMOLED, mechanical encoder). Not tested on hardware.
- `firmware/lib/knoblink/`: the host protocol, shared by both boards.
- `hardware/`: display wiring and reference dimensions for building a case around the T-Knob + Waveshare 1.28" round LCD. The design files live online: [adapter PCB](https://oshwlab.com/dmalykhin/project_eatennvi) (EasyEDA) and [case](https://cad.onshape.com/documents/d5cec4e5e8bf401d6ac3d327/w/2e4dde70915e291400fd7391/e/c75e21deb758423042022dd5) (Onshape).

## What you need

- **[LilyGO T-Knob](https://github.com/Xinyuan-LilyGO/T-Knob)**: ESP32-C6 haptic knob (BLDC motor, magnetic encoder, LEDs). On its own it's a complete knob, with LEDs for session states.
- **[Waveshare 1.28inch LCD Module](https://www.waveshare.com/1.28inch-lcd-module.htm)** (optional): GC9A01 240×240 round LCD for the session dial. It connects to the knob's top PCB through the [adapter PCB](https://oshwlab.com/dmalykhin/project_eatennvi) (or hand-soldered: see the [wiring](hardware/README.md)).
- **3D-printed parts** (optional): [Onshape document](https://cad.onshape.com/documents/d5cec4e5e8bf401d6ac3d327/w/2e4dde70915e291400fd7391/e/c75e21deb758423042022dd5).
- A USB-C cable and a Mac. The host app is macOS-only for now.
- To build: [Rust](https://rustup.rs) and [PlatformIO](https://platformio.org).

## Setup

### 1. Menu-bar app

```sh
cd host
./scripts/bundle-macos.sh                          # builds target/release/Claude Knob.app (needs a default
                                                   # Rust toolchain: `rustup default stable`)
mv "target/release/Claude Knob.app" /Applications/
open "/Applications/Claude Knob.app"
```

In the knob icon's menu in the menu bar, turn on **Claude Code hooks installed** and **Start at login**, then restart any running `claude` sessions. The first time it focuses a window, macOS asks whether Claude Knob may control Terminal. Allow it.

The menu shows:
- whether the knob is connected
- every session with its state (click one to focus it)
- the hooks and login toggles
- the log (`~/.claude-knob/knobd.log`)
- Quit

When a session needs you, the icon turns amber and shows a count.

Hooks point at the binary's current path. If you move the app, turn hooks off and on again.

Without the app: `cargo build --release`, then `knobd install-hooks` and `knobd install-launchd`. Use `knobd run --no-tray` for a daemon with no menu-bar icon, or build with `--no-default-features` to leave out the UI code completely (for example on Linux).

### 2. Firmware (T-Knob)

```sh
cd firmware/t-knob
pio run -t upload        # quit Claude Knob first: it holds the serial port
```

On first boot the motor calibrates: the knob turns by itself about 50° and back. Don't touch it while that happens. The result is saved in flash. Hold BOOT while powering on to calibrate again, or run `knobd knob '{"t":"recal"}'`.

Each LED shows one session (on the page of 4 that contains the selection):
- dim = idle (brighter if it's the selected one)
- breathing = running
- fast blink = needs you
- solid = done

Turning clockwise moves to the next session. To flip that, change `KNOB_REVERSE` in `include/board.h`.

#### Round display

If the GC9A01 LCD is fitted (wiring in [`hardware/README.md`](hardware/README.md)), it shows a dial:
- **Marks:** one bar per session around the edge, spaced like the detents, starting at 12 o'clock and running clockwise like hours on a watch. Each bar has its session's colour: blue = running (pulses), amber = needs you (blinks), green = done, grey = idle. The selected session's bar is longer and wider.
- **Pointer:** a white needle that follows the wheel, so it lands on a mark at each detent.
- **Centre:** "2 / 5", the selected session's name (up to two lines) and its state. Before knobd connects it shows "Claude Knob / waiting for knobd".

The panel is mounted turned 45°, so the firmware draws everything rotated back (`LCD_ROTATE_DEG`). If the pointer turns against the wheel, flip `DISPLAY_ANGLE_SIGN`. Both are in `include/board.h`. Frames go out in 8-row strips over DMA between motor-loop passes, so drawing never stalls the haptics. The LCD takes the C6's only general-purpose SPI host, so the MT6701 is read by bit-banged SSI.

When a session starts waiting for you, the knob gives a short vibration (`nudge`, set it to 0 to turn it off). There's no sound. While waiting for knobd, all LEDs breathe slowly. With no sessions the knob spins freely.

#### Tuning the feel

The knob takes commands through the daemon, and replies go to `~/.claude-knob/knobd.log`:

```sh
knobd knob '{"t":"tune","peak":1.5,"shape":0.8,"click":0.6}' # strength (V), profile 0=crisp..1=smooth, tick (V)
knobd knob '{"t":"save"}'                                   # keep the current values across reboots ("reset" = defaults)
knobd knob '{"t":"info"}'                                   # current calibration and parameters
knobd knob '{"t":"debug","on":true,"ms":100}'               # angle/torque telemetry
```

Every parameter is described in `HapticParams` in `src/haptics.h`.

### T-Encoder Pro (untested)

```sh
cd firmware/t-encoder-pro
pio run -t upload
```

Two panel revisions of this board exist. `include/board.h` defaults to the newer CO5300 panel. If the screen stays black, switch to `PANEL_SH8601`.

## Usage

| Knob | Action |
|---|---|
| Rotate | Move to the next (clockwise) or previous session (one detent each). Its Terminal tab is focused right away (about 20–45 ms). Spinning fast skips windows instead of queueing them. |
| Press | Focus the selected session immediately. The T-Knob uses the MT6701 push flag or the BOOT button. |
| Focus a window yourself | The knob follows. Click a session's Terminal window, use ⌘\`, or anything else, and the T-Knob's motor turns the wheel to that session within about 0.1 s. It gives up if you're holding the wheel, and waits 1 s after you last turned it. |
| Switch to another app | The knob shows "no selection" (no LED is highlighted) and stays parked where it was. The next click moves to the neighboring session as usual. If it's parked at the first or last session, pushing into that end's wall selects that session (you feel a tick). |

The "done" state clears once you focus that session.

### CLI

```sh
knobd sessions          # list tracked sessions with state, tty, pid
knobd focus 2           # focus session #2
knobd ports             # show the detected knob serial port
knobd run --no-serial   # run the daemon (and menu-bar icon) without hardware…
knobd sim               # …and drive it from a keyboard "knob" (j/k rotate, p press)
knobd knob '<json>'     # send a raw command to the knob
knobd uninstall-hooks
```

`KNOBD_SOCKET` overrides the socket path (default `~/.claude-knob/knobd.sock`). `KNOBD_DEBUG=1` makes `knobd hook` print errors to stderr.

Files in `~/.claude-knob/`:
- `knobd.log`: what the daemon did. Each knob click (`knob: select`) and each window change it saw (`watch: front window …`) gets a line, which is the place to look if the knob and Terminal disagree. `knob went silent (USB transmit stuck); resetting it` means the watchdog below kicked in.
- `sessions.json`: the tracked sessions. Saved on every change, so restarting the app doesn't lose them.

## How state is tracked

| Claude Code hook | State |
|---|---|
| SessionStart | idle |
| UserPromptSubmit, PreToolUse, PostToolUse | running |
| PermissionRequest, Notification | needs you (`idle_prompt` notifications are ignored) |
| Stop | done |
| SessionEnd | removed |

`knobd hook` walks up the process tree to find the session's tty (used to find the Terminal tab) and the `claude` pid (used to drop sessions whose process has died). A new session on the same tty replaces the old one, for example after `/clear`. A session keeps the name of the folder it started in.

Every 100 ms the daemon checks which window is on top, using the window server's list. That costs about 1 ms and needs no permissions. Terminal is asked over AppleScript only when the front window has several tabs. When that window belongs to a session, the session becomes focused: the knob turns to it and its "done" state clears.

## Protocol (USB CDC, newline-delimited JSON)

- daemon → knob: `{"t":"sessions","items":[{"id":"…","name":"proj","state":"running"}],"sel":0,"focus":0}`, sent on every change and every 3 s as a heartbeat. `focus` is the session whose window is in front, or -1 when it's another app.
- knob → daemon: `{"t":"hello","fw":"0.2.0"}` (every 1 s until it hears from the daemon), `{"t":"alive"}` (every 2 s while connected), `{"t":"select","idx":1,"id":"…"}`, `{"t":"press","idx":1,"id":"…"}`.
- Watchdog: the ESP32's USB serial transmit can wedge (seen after a re-plug) while receive keeps working. The knob then follows the host, but its turns never arrive. If a knob that has sent `alive` goes quiet for 6 s, knobd resets the chip over USB (RTS with DTR low) and reconnects. Firmware that never sends `alive` is never reset.
- Board-specific commands (tuning, debug, calibration) pass through `knobd knob '<json>'`. They're listed at the top of `firmware/t-knob/src/main.cpp`.

## Development

```sh
cd host
cargo fmt --check && cargo clippy --all-targets -- -D warnings && cargo test
cd ../firmware/t-knob && pio run          # and firmware/t-encoder-pro
```

CI (`.github/workflows/ci.yml`) runs the same checks on every push: the host on macOS (fmt, clippy with and without the menu-bar feature, tests, and an app bundle build), plus a PlatformIO build of both firmwares. The firmware has no unit tests yet. On the T-Knob, `knobd knob '{"t":"shift","deg":8}'` with debug on is a quick hardware self-test: the motor should turn the knob 8° and settle without wobbling.

## License

[MIT](LICENSE), for the code and for the hardware designs (the adapter PCB and the 3D-printed parts). The fonts in `firmware/t-knob/src/fonts/` are GNU FreeFont (GPLv3 with the font exception), converted by Adafruit. The font exception is meant to let you embed fonts without the GPL covering the rest of the work.
