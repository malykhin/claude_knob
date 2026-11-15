//! Finds Claude Code sessions that were already running when the daemon started
//! (or that haven't fired a hook since). Claude Code keeps one
//! `~/.claude/sessions/<pid>.json` per live session, with its id and start dir.

use std::path::PathBuf;
use std::process::Command;

use serde::Deserialize;

use crate::protocol::State;
use crate::session::Session;

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct SessionFile {
    pid: i32,
    session_id: String,
    #[serde(default)]
    cwd: String,
    #[serde(default)]
    status: String,
}

fn sessions_dir() -> PathBuf {
    crate::install::claude_config_dir().join("sessions")
}

/// Live sessions not in `known` (by id), with their terminal tty filled in.
pub fn find_new(known: impl Fn(&str) -> bool) -> Vec<Session> {
    let Ok(dir) = std::fs::read_dir(sessions_dir()) else { return Vec::new() };
    let files: Vec<SessionFile> = dir
        .flatten()
        .filter(|e| e.path().extension().is_some_and(|x| x == "json"))
        .filter_map(|e| serde_json::from_slice(&std::fs::read(e.path()).ok()?).ok())
        .filter(|f: &SessionFile| !known(&f.session_id))
        .collect();
    if files.is_empty() {
        return Vec::new();
    }

    // Only sessions whose process is still alive have a tty in `ps`; stale files drop out here.
    let ttys = ttys_by_pid();
    files
        .into_iter()
        .filter_map(|f| {
            let tty = ttys.iter().find(|(pid, _)| *pid == f.pid)?.1.clone();
            Some(Session {
                id: f.session_id,
                cwd: f.cwd,
                tty: Some(tty),
                pid: Some(f.pid),
                state: if f.status == "busy" { State::Running } else { State::Idle },
            })
        })
        .collect()
}

fn ttys_by_pid() -> Vec<(i32, String)> {
    let Ok(out) = Command::new("ps").args(["-A", "-o", "pid=,tty="]).output() else {
        return Vec::new();
    };
    String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|l| {
            let mut it = l.split_whitespace();
            let pid = it.next()?.parse().ok()?;
            let tty = it.next()?;
            (tty != "??" && tty != "?").then(|| (pid, format!("/dev/{tty}")))
        })
        .collect()
}
