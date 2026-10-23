use std::path::PathBuf;
use std::process::Command;

use anyhow::Context;
use serde_json::{json, Value};

use crate::daemon::state_dir;

const EVENTS: &[&str] = &[
    "SessionStart",
    "SessionEnd",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "PermissionRequest",
    "Notification",
    "Stop",
];
const TOOL_EVENTS: &[&str] = &["PreToolUse", "PostToolUse", "PermissionRequest"];
const LAUNCHD_LABEL: &str = "com.claudeknob.knobd";

fn home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").expect("HOME is set"))
}

fn settings_path() -> PathBuf {
    home().join(".claude").join("settings.json")
}

fn exe() -> anyhow::Result<String> {
    Ok(std::env::current_exe()?.canonicalize()?.to_string_lossy().into_owned())
}

fn is_ours(entry: &Value) -> bool {
    entry["hooks"]
        .as_array()
        .is_some_and(|hs| hs.iter().any(|h| h["command"].as_str().is_some_and(|c| c.contains("knobd") && c.ends_with(" hook"))))
}

fn load_settings() -> anyhow::Result<Value> {
    let path = settings_path();
    match std::fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).with_context(|| format!("parsing {}", path.display())),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(json!({})),
        Err(e) => Err(e.into()),
    }
}

fn save_settings(v: &Value) -> anyhow::Result<()> {
    let path = settings_path();
    if path.exists() {
        std::fs::copy(&path, path.with_extension("json.knobd.bak"))?;
    }
    std::fs::create_dir_all(path.parent().unwrap())?;
    std::fs::write(&path, serde_json::to_string_pretty(v)? + "\n")?;
    Ok(())
}

/// True when every tracked event has a knobd hook.
#[cfg_attr(not(feature = "tray"), allow(dead_code))]
pub fn hooks_installed() -> bool {
    let Ok(v) = load_settings() else { return false };
    EVENTS.iter().all(|ev| {
        v["hooks"][*ev].as_array().is_some_and(|list| list.iter().any(is_ours))
    })
}

/// Adds a `knobd hook` entry for every event we track, leaving existing hooks alone.
pub fn install_hooks() -> anyhow::Result<()> {
    let mut v = load_settings()?;
    let command = format!("'{}' hook", exe()?);
    let hooks = v
        .as_object_mut()
        .context("settings.json is not an object")?
        .entry("hooks")
        .or_insert_with(|| json!({}));
    for ev in EVENTS {
        let list = hooks
            .as_object_mut()
            .context("\"hooks\" is not an object")?
            .entry(*ev)
            .or_insert_with(|| json!([]))
            .as_array_mut()
            .with_context(|| format!("hooks.{ev} is not an array"))?;
        list.retain(|e| !is_ours(e));
        let mut entry = json!({ "hooks": [{ "type": "command", "command": command, "timeout": 5 }] });
        if TOOL_EVENTS.contains(ev) {
            entry["matcher"] = json!("*");
        }
        list.push(entry);
    }
    save_settings(&v)?;
    println!("Installed knobd hooks into {} (backup: settings.json.knobd.bak)", settings_path().display());
    println!("Restart running Claude Code sessions to pick them up.");
    Ok(())
}

pub fn uninstall_hooks() -> anyhow::Result<()> {
    let mut v = load_settings()?;
    if let Some(hooks) = v.get_mut("hooks").and_then(Value::as_object_mut) {
        for list in hooks.values_mut().filter_map(Value::as_array_mut) {
            list.retain(|e| !is_ours(e));
        }
        hooks.retain(|_, list| list.as_array().map_or(true, |a| !a.is_empty()));
        if hooks.is_empty() {
            v.as_object_mut().unwrap().remove("hooks");
        }
    }
    save_settings(&v)?;
    println!("Removed knobd hooks from {}", settings_path().display());
    Ok(())
}

fn plist_path() -> PathBuf {
    home().join("Library/LaunchAgents").join(format!("{LAUNCHD_LABEL}.plist"))
}

/// Writes the LaunchAgent that starts `knobd run` at login. It restarts knobd
/// after a crash, but not after Quit (a clean exit).
fn write_plist() -> anyhow::Result<PathBuf> {
    let plist = plist_path();
    let log = state_dir().join("knobd.log");
    std::fs::create_dir_all(state_dir())?;
    std::fs::create_dir_all(plist.parent().unwrap())?;
    let body = format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>{LAUNCHD_LABEL}</string>
  <key>ProgramArguments</key>
  <array><string>{exe}</string><string>run</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key>
  <dict><key>SuccessfulExit</key><false/></dict>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardErrorPath</key><string>{log}</string>
  <key>StandardOutPath</key><string>{log}</string>
</dict>
</plist>
"#,
        exe = exe()?,
        log = log.display()
    );
    std::fs::write(&plist, body)?;
    Ok(plist)
}

#[cfg_attr(not(feature = "tray"), allow(dead_code))]
pub fn login_item_enabled() -> bool {
    plist_path().exists()
}

/// Used by the menu bar. Only writes or removes the plist: launchd picks it up
/// at the next login, and the knobd that's already running keeps running.
#[cfg_attr(not(feature = "tray"), allow(dead_code))]
pub fn set_login_item(enabled: bool) -> anyhow::Result<()> {
    if enabled {
        write_plist()?;
    } else if let Err(e) = std::fs::remove_file(plist_path()) {
        if e.kind() != std::io::ErrorKind::NotFound {
            return Err(e.into());
        }
    }
    Ok(())
}

/// Installs and loads a LaunchAgent so the daemon runs at login.
pub fn install_launchd() -> anyhow::Result<()> {
    let plist = write_plist()?;
    let domain = format!("gui/{}", unsafe { libc::getuid() });
    let _ = Command::new("launchctl").args(["bootout", &domain, &plist.to_string_lossy()]).output();
    let st = Command::new("launchctl").args(["bootstrap", &domain, &plist.to_string_lossy()]).status()?;
    anyhow::ensure!(st.success(), "launchctl bootstrap failed");
    println!("Installed {} and started knobd (log: {})", plist.display(), state_dir().join("knobd.log").display());
    Ok(())
}
