mod daemon;
mod discover;
mod focus;
mod hook;
mod install;
mod protocol;
mod serial;
mod session;
mod sim;
#[cfg(feature = "tray")]
mod tray;

use clap::{Parser, Subcommand};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixStream;

use protocol::{Ctl, SessionInfo};

#[derive(Parser)]
#[command(version, about = "Claude Knob desktop daemon")]
struct Cli {
    /// Defaults to `run`, so launching the .app starts the daemon.
    #[command(subcommand)]
    cmd: Option<Cmd>,
}

#[derive(Subcommand)]
enum Cmd {
    /// Run the daemon (socket server + knob link).
    Run {
        /// Serial port to use instead of auto-detecting the knob.
        #[arg(long)]
        port: Option<String>,
        /// Don't look for a USB knob (use with `knobd sim`).
        #[arg(long)]
        no_serial: bool,
        /// Don't show the menu-bar icon.
        #[arg(long)]
        no_tray: bool,
    },
    /// Hook entry point for Claude Code (reads hook JSON on stdin).
    Hook { event: Option<String> },
    /// List tracked sessions.
    Sessions,
    /// Focus the terminal of session number IDX (1-based, as shown by `sessions`).
    Focus { idx: usize },
    /// Send a raw JSON command to the knob, e.g. '{"t":"tune","peak":1.5}' (see firmware main.cpp).
    Knob { json: String },
    /// Simulated knob driven from the keyboard.
    Sim,
    /// List serial ports that look like the knob.
    Ports,
    /// Add knobd hooks to ~/.claude/settings.json.
    InstallHooks,
    /// Remove knobd hooks from ~/.claude/settings.json.
    UninstallHooks,
    /// Start knobd at login via a LaunchAgent (macOS).
    InstallLaunchd,
}

async fn request(ctl: &Ctl) -> anyhow::Result<String> {
    let mut s = UnixStream::connect(daemon::socket_path())
        .await
        .map_err(|e| anyhow::anyhow!("cannot reach knobd ({e}); is `knobd run` running?"))?;
    let mut buf = serde_json::to_vec(ctl)?;
    buf.push(b'\n');
    s.write_all(&buf).await?;
    let mut line = String::new();
    BufReader::new(s).read_line(&mut line).await?;
    Ok(line)
}

fn main() -> anyhow::Result<()> {
    let cmd = Cli::parse().cmd.unwrap_or(Cmd::Run { port: None, no_serial: false, no_tray: false });
    if let Cmd::Run { port, no_serial, no_tray } = cmd {
        return run_daemon(port, no_serial, no_tray);
    }
    tokio::runtime::Runtime::new()?.block_on(run_cli(cmd))
}

/// When started from Finder or launchd there's no terminal, so keep a log file
/// (the menu's "Open log" shows it).
fn log_to_file_if_detached() {
    use std::os::fd::AsRawFd;
    if unsafe { libc::isatty(2) } == 1 {
        return;
    }
    let _ = std::fs::create_dir_all(daemon::state_dir());
    let path = daemon::state_dir().join("knobd.log");
    if let Ok(f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        unsafe {
            libc::dup2(f.as_raw_fd(), 1);
            libc::dup2(f.as_raw_fd(), 2);
        }
    }
}

fn run_daemon(port: Option<String>, no_serial: bool, no_tray: bool) -> anyhow::Result<()> {
    log_to_file_if_detached();
    let rt = tokio::runtime::Runtime::new()?;
    let (shared, accept) = match rt.block_on(daemon::start(port, no_serial)) {
        // Exit cleanly so launchd doesn't keep retrying when one is already up.
        Err(e) if e.is::<daemon::AlreadyRunning>() => {
            eprintln!("{e}");
            return Ok(());
        }
        r => r?,
    };

    #[cfg(feature = "tray")]
    if !no_tray {
        tray::run(shared, rt);
    }
    #[cfg(not(feature = "tray"))]
    let _ = no_tray;

    drop(shared);
    let res = rt.block_on(accept);
    daemon::cleanup();
    res?
}

async fn run_cli(cmd: Cmd) -> anyhow::Result<()> {
    match cmd {
        Cmd::Run { .. } => unreachable!("handled in main"),
        Cmd::Hook { event } => hook::run(event),
        Cmd::Sessions => {
            let list: Vec<SessionInfo> = serde_json::from_str(&request(&Ctl::List).await?)?;
            if list.is_empty() {
                println!("no sessions");
            }
            for (i, s) in list.iter().enumerate() {
                let state = serde_json::to_string(&s.state)?;
                println!(
                    "{} {:>2}. {:<24} {:<10} {:<14} pid {:<7} {}",
                    if s.focused { "▶" } else { " " },
                    i + 1,
                    s.name,
                    state.trim_matches('"'),
                    s.tty.as_deref().unwrap_or("-"),
                    s.pid.map_or("-".into(), |p| p.to_string()),
                    s.id
                );
            }
        }
        Cmd::Focus { idx } => {
            anyhow::ensure!(idx >= 1, "IDX is 1-based");
            let reply: String = serde_json::from_str(&request(&Ctl::Focus { idx: idx - 1 }).await?)?;
            println!("{reply}");
        }
        Cmd::Knob { json } => {
            serde_json::from_str::<serde_json::Value>(&json).map_err(|e| anyhow::anyhow!("not valid JSON: {e}"))?;
            let reply: String = serde_json::from_str(&request(&Ctl::Knob { line: json }).await?)?;
            println!("{reply}");
        }
        Cmd::Sim => sim::run().await?,
        Cmd::Ports => match serial::find_port() {
            Some(p) => println!("{p}"),
            None => println!("no Espressif USB serial device found"),
        },
        Cmd::InstallHooks => install::install_hooks()?,
        Cmd::UninstallHooks => install::uninstall_hooks()?,
        Cmd::InstallLaunchd => install::install_launchd()?,
    }
    Ok(())
}
