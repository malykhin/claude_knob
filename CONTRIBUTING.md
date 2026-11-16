# Contributing

Bug reports, fixes and support for other boards or platforms are welcome. Open an issue first for anything bigger than a small fix, so we can agree on the approach before you write the code.

## Reporting a bug

Use the bug report form on the **Issues** tab. Include the lines from `~/.claude-knob/knobd.log` around the time it happened. That log has every knob click and every window change the daemon saw, and it usually shows where things went wrong.

For security problems, follow [SECURITY.md](SECURITY.md) instead of opening a public issue.

## Building and testing

You need [Rust](https://rustup.rs) and [PlatformIO](https://platformio.org).

```sh
cd host
cargo fmt --check && cargo clippy --all-targets -- -D warnings && cargo test
cd ../firmware/t-knob && pio run          # and firmware/t-encoder-pro
```

CI runs these checks on every push and pull request. Without a knob, `knobd run --no-serial` and `knobd sim` let you drive the daemon from the keyboard.

The firmware has no unit tests. If you change it, say in the pull request which board you tested on. Only the T-Knob has been tested on hardware so far; the T-Encoder Pro firmware builds but hasn't run on a real board.

## Pull requests

- One change per pull request.
- Keep macOS window-focusing code behind the `Focuser` trait in `host/src/focus/`, so other platforms can be added later.
- If you change the protocol between the daemon and the knob, update `firmware/lib/knoblink/` and the protocol section of the README together.
- Commit messages: a short line saying what changed, in plain words.

## Writing style

README text, UI strings, error messages and comments follow the same rules:

- Plain words: "use", "show", "help". No "leverage", "seamless", "robust" or similar.
- Sentence case in headings.
- Bold only for UI labels the reader will look for, like **Start at login**.
- No emoji and no em dashes.
- Error messages say what happened and what to do next.

## Code of conduct

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).
