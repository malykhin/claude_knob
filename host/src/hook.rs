//! `knobd hook`: invoked by Claude Code hooks. Reads the hook JSON on stdin,
//! adds the terminal tty, and forwards it to the daemon. Must stay fast, never
//! print to stdout (some hooks feed stdout back to Claude), and never fail.

use std::io::{Read, Write};
use std::os::unix::net::UnixStream;
use std::process::Command;
use std::time::Duration;

use crate::daemon::socket_path;
use crate::protocol::{Ctl, HookMsg};

pub fn run(event_arg: Option<String>) {
    if let Err(e) = try_run(event_arg) {
        if std::env::var_os("KNOBD_DEBUG").is_some() {
            eprintln!("knobd hook: {e:#}");
        }
    }
}

fn try_run(event_arg: Option<String>) -> anyhow::Result<()> {
    let mut input = String::new();
    std::io::stdin().read_to_string(&mut input)?;
    let v: serde_json::Value = serde_json::from_str(&input).unwrap_or_default();
    let s = |k: &str| v.get(k).and_then(|x| x.as_str()).map(str::to_owned);

    let event = s("hook_event_name").or(event_arg).ok_or_else(|| anyhow::anyhow!("no event name"))?;
    let session_id = s("session_id").ok_or_else(|| anyhow::anyhow!("no session_id"))?;
    let (tty, pid) = find_terminal();

    let msg = Ctl::Hook(HookMsg {
        event,
        session_id,
        cwd: s("cwd").unwrap_or_default(),
        tty,
        pid,
        notification_type: s("notification_type"),
    });

    let mut stream = UnixStream::connect(socket_path())?;
    stream.set_write_timeout(Some(Duration::from_millis(500)))?;
    let mut buf = serde_json::to_vec(&msg)?;
    buf.push(b'\n');
    stream.write_all(&buf)?;
    Ok(())
}

/// Walks up the process tree to find the terminal tty and the claude process pid.
fn find_terminal() -> (Option<String>, Option<i32>) {
    let Ok(out) = Command::new("ps").args(["-A", "-o", "pid=,ppid=,tty=,comm="]).output() else {
        return (None, None);
    };
    let table: Vec<(i32, i32, String, String)> = String::from_utf8_lossy(&out.stdout)
        .lines()
        .filter_map(|l| {
            let mut it = l.split_whitespace();
            let pid = it.next()?.parse().ok()?;
            let ppid = it.next()?.parse().ok()?;
            let tty = it.next()?.to_owned();
            let comm = it.collect::<Vec<_>>().join(" ");
            Some((pid, ppid, tty, comm))
        })
        .collect();

    let mut tty = None;
    let mut claude_pid = None;
    let mut cur = std::os::unix::process::parent_id() as i32;
    for _ in 0..16 {
        let Some((pid, ppid, t, comm)) = table.iter().find(|r| r.0 == cur) else { break };
        if tty.is_none() && t != "??" && t != "?" {
            tty = Some(format!("/dev/{t}"));
        }
        let base = comm.rsplit('/').next().unwrap_or(comm);
        if claude_pid.is_none() && (base == "claude" || base == "node") {
            claude_pid = Some(*pid);
        }
        if *ppid <= 1 {
            break;
        }
        cur = *ppid;
    }
    (tty, claude_pid)
}
