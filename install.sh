#!/bin/sh
# Claude Knob installer (macOS).
#
#   curl -fsSL https://raw.githubusercontent.com/malykhin/claude_knob/main/install.sh | sh
#
# Installs "Claude Knob.app" for the current user only (~/Applications), adds the
# Claude Code hooks to this user's Claude config and starts knobd at login.
# Downloads the latest release build; falls back to building from source (needs Rust).
#
# Environment:
#   KNOB_FROM_SOURCE=1   always build from source instead of downloading a release
#   KNOB_REF=<ref>       git branch/tag to build from source (default: main)
#   KNOB_APP_DIR=<dir>   where to put the app (default: ~/Applications)
#   KNOB_NO_HOOKS=1      don't touch Claude Code's settings.json
#   CLAUDE_CONFIG_DIR    Claude Code's config dir, if not ~/.claude (hooks go there)
#   KNOB_NO_LOGIN=1      don't start knobd at login (just open the app once)
#
# Uninstall:  curl -fsSL .../install.sh | sh -s -- uninstall

set -eu

REPO="malykhin/claude_knob"
APP_NAME="Claude Knob.app"
ASSET="Claude-Knob-macos.zip"
LABEL="com.claudeknob.knobd"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

LEGACY_APP="/Applications/$APP_NAME"  # where older installers put it
CLAUDE_DIR="${CLAUDE_CONFIG_DIR:-$HOME/.claude}"

app_dir() {
  echo "${KNOB_APP_DIR:-$HOME/Applications}"
}

stop_running() {
  launchctl bootout "gui/$(id -u)" "$PLIST" >/dev/null 2>&1 || true
  # Only the daemon (`knobd run`, or the app with no arguments), not one-off CLI calls.
  # This user's only: other users run their own knobd.
  pkill -U "$(id -u)" -f '/knobd run$' >/dev/null 2>&1 || true
  pkill -U "$(id -u)" -f '/Contents/MacOS/knobd$' >/dev/null 2>&1 || true
}

fetch_release() {
  # $1 = work dir. Succeeds only if a release asset was downloaded and unpacked.
  url="https://github.com/$REPO/releases/latest/download/$ASSET"
  say "Downloading $url"
  curl -fsSL -o "$1/$ASSET" "$url" 2>/dev/null || return 1
  ditto -x -k "$1/$ASSET" "$1/unpacked" || return 1
  [ -d "$1/unpacked/$APP_NAME" ] || return 1
  mv "$1/unpacked/$APP_NAME" "$1/$APP_NAME"
}

build_from_source() {
  # $1 = work dir.
  ref="${KNOB_REF:-main}"
  command -v cargo >/dev/null 2>&1 ||
    die "Rust is needed to build from source. Install it from https://rustup.rs and re-run."
  if ! cargo --version >/dev/null 2>&1; then
    # rustup without a default toolchain: use stable for this build only.
    command -v rustup >/dev/null 2>&1 ||
      die "cargo doesn't work. Fix your Rust install (https://rustup.rs) and re-run."
    say "No default Rust toolchain; using stable for this build"
    rustup toolchain install stable --profile minimal >/dev/null
    RUSTUP_TOOLCHAIN=stable
    export RUSTUP_TOOLCHAIN
  fi
  say "Building from source ($REPO@$ref)"
  if command -v git >/dev/null 2>&1; then
    git clone --quiet --depth 1 --branch "$ref" "https://github.com/$REPO.git" "$1/src"
  else
    mkdir -p "$1/src"
    curl -fsSL "https://github.com/$REPO/archive/$ref.tar.gz" | tar -xz -C "$1/src" --strip-components 1
  fi
  "$1/src/host/scripts/bundle-macos.sh" >/dev/null
  mv "$1/src/host/target/release/$APP_NAME" "$1/$APP_NAME"
}

install() {
  dest_dir=$(app_dir)
  dest="$dest_dir/$APP_NAME"
  knobd="$dest/Contents/MacOS/knobd"

  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT INT TERM

  if [ "${KNOB_FROM_SOURCE:-0}" = 1 ]; then
    build_from_source "$work"
  elif ! fetch_release "$work"; then
    warn "no release build available; building from source instead"
    build_from_source "$work"
  fi

  stop_running
  say "Installing to $dest"
  mkdir -p "$dest_dir"
  rm -rf "$dest"
  mv "$work/$APP_NAME" "$dest"
  xattr -dr com.apple.quarantine "$dest" 2>/dev/null || true

  if [ "${KNOB_NO_HOOKS:-0}" != 1 ]; then
    say "Installing Claude Code hooks into $CLAUDE_DIR/settings.json"
    "$knobd" install-hooks
  fi

  if [ "${KNOB_NO_LOGIN:-0}" != 1 ]; then
    say "Starting knobd at login"
    "$knobd" install-launchd
  else
    open "$dest"
  fi

  if [ "$dest" != "$LEGACY_APP" ] && [ -d "$LEGACY_APP" ]; then
    warn "an older install is still in $LEGACY_APP. This user's hooks now use $dest."
    echo "    Remove the old copy once no other user on this Mac needs it."
  fi

  echo
  say "Done. Restart any running 'claude' sessions so they pick up the hooks."
  echo "    The first time the knob focuses a window, macOS asks whether Claude Knob may"
  echo "    control Terminal: allow it. Log: ~/.claude-knob/knobd.log"
  echo "    Firmware is flashed separately: see https://github.com/$REPO#2-firmware-t-knob"
}

uninstall() {
  dest="$(app_dir)/$APP_NAME"
  if [ ! -d "$dest" ] && [ -z "${KNOB_APP_DIR:-}" ] && [ -O "$LEGACY_APP" ]; then
    dest="$LEGACY_APP"  # installed by an older installer, by this user
  fi
  knobd="$dest/Contents/MacOS/knobd"
  if [ -x "$knobd" ]; then
    say "Removing Claude Code hooks"
    "$knobd" uninstall-hooks || warn "couldn't remove hooks"
  else
    warn "$dest not found; remove knobd hooks from $CLAUDE_DIR/settings.json by hand if any are left"
  fi
  say "Stopping knobd"
  stop_running
  rm -f "$PLIST"
  rm -rf "$dest"
  say "Uninstalled. Session state and the log are left in ~/.claude-knob/."
}

main() {
  [ "$(uname -s)" = Darwin ] || die "Claude Knob's menu-bar app is macOS-only for now."
  case "${1:-install}" in
    install) install ;;
    uninstall) uninstall ;;
    *) die "usage: install.sh [install|uninstall]" ;;
  esac
}

# Everything runs from main, so a truncated download can't run half a script.
main "$@"
