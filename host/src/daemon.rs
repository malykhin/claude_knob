use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Context;
use tokio::io::{AsyncBufRead, AsyncBufReadExt, AsyncWrite, AsyncWriteExt, BufReader};
use tokio::net::{UnixListener, UnixStream};
use tokio::sync::watch;
use tokio::task::JoinHandle;

use crate::focus::{self, Focuser};
use crate::protocol::{Ctl, FromKnob, SessionInfo, ToKnob};
use crate::session::Registry;

const PRUNE_EVERY: Duration = Duration::from_secs(5);
/// The knob treats a link silent for 8 s as lost, so re-send the snapshot regularly.
const HEARTBEAT: Duration = Duration::from_secs(3);

pub fn state_dir() -> PathBuf {
    PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| ".".into())).join(".claude-knob")
}

/// `KNOBD_SOCKET` overrides the default `~/.claude-knob/knobd.sock`.
pub fn socket_path() -> PathBuf {
    std::env::var_os("KNOBD_SOCKET").map(PathBuf::from).unwrap_or_else(|| state_dir().join("knobd.sock"))
}

fn sessions_path() -> PathBuf {
    state_dir().join("sessions.json")
}

fn load_registry() -> Registry {
    std::fs::read_to_string(sessions_path())
        .ok()
        .and_then(|s| serde_json::from_str(&s).ok())
        .map(Registry::from_infos)
        .unwrap_or_default()
}

#[derive(Default)]
struct FocusQueue {
    busy: bool,
    next: Option<String>,
}

pub struct Shared {
    reg: Mutex<Registry>,
    changed: watch::Sender<u64>,
    focuser: Box<dyn Focuser>,
    /// Focus coalescing: while one focus runs, only the latest requested session waits.
    focus_queue: Mutex<FocusQueue>,
    /// Serial port of the connected knob, if any.
    knob_port: Mutex<Option<String>>,
    /// Raw lines for the knob, sent by every active link.
    raw_tx: tokio::sync::broadcast::Sender<String>,
}

impl Shared {
    /// Publishes a change and saves the sessions, so a restarted daemon picks up where it left off.
    fn notify(&self) {
        let infos = self.reg.lock().unwrap().infos();
        if let Ok(json) = serde_json::to_vec(&infos) {
            let tmp = sessions_path().with_extension("json.tmp");
            if std::fs::write(&tmp, json).is_ok() {
                let _ = std::fs::rename(&tmp, sessions_path());
            }
        }
        self.changed.send_modify(|v| *v += 1);
    }

    /// Fires whenever sessions or the knob connection change.
    #[cfg_attr(not(feature = "tray"), allow(dead_code))]
    pub fn subscribe(&self) -> watch::Receiver<u64> {
        self.changed.subscribe()
    }

    #[cfg_attr(not(feature = "tray"), allow(dead_code))]
    pub fn sessions(&self) -> Vec<SessionInfo> {
        self.reg.lock().unwrap().infos()
    }

    #[cfg_attr(not(feature = "tray"), allow(dead_code))]
    pub fn knob_port(&self) -> Option<String> {
        self.knob_port.lock().unwrap().clone()
    }

    /// From the platform's focus watcher: the user is now looking at `tty`.
    fn on_terminal_focus(&self, tty: Option<String>) {
        let mut reg = self.reg.lock().unwrap();
        if reg.set_focused_tty(tty.clone()) {
            let idx = reg.focus_index();
            drop(reg);
            eprintln!("watch: front window {} -> focus {idx}", tty.as_deref().unwrap_or("(not a terminal)"));
            self.notify();
        }
    }

    pub fn set_knob_port(&self, port: Option<String>) {
        *self.knob_port.lock().unwrap() = port;
        self.notify();
    }

    fn snapshot(&self) -> ToKnob {
        let reg = self.reg.lock().unwrap();
        let (items, sel) = reg.snapshot();
        ToKnob::Sessions { items, sel, focus: reg.focus_index() }
    }

    /// Focuses right away; if a focus is already running, remembers only the latest
    /// target for when it finishes. Spinning past sessions never queues up a backlog.
    fn request_focus(self: &Arc<Self>, id: String) {
        {
            let mut q = self.focus_queue.lock().unwrap();
            if q.busy {
                q.next = Some(id);
                return;
            }
            q.busy = true;
        }
        let me = self.clone();
        tokio::spawn(async move {
            let mut id = id;
            loop {
                me.focus_id(id).await;
                let mut q = me.focus_queue.lock().unwrap();
                match q.next.take() {
                    Some(next) => id = next,
                    None => {
                        q.busy = false;
                        return;
                    }
                }
            }
        });
    }

    pub async fn focus_id(self: &Arc<Self>, id: String) {
        let Some(tty) = self.reg.lock().unwrap().find(&id).and_then(|s| s.tty.clone()) else {
            eprintln!("focus: session {id} has no known tty");
            return;
        };
        let me = self.clone();
        let started = std::time::Instant::now();
        let res = tokio::task::spawn_blocking(move || me.focuser.focus(&tty)).await;
        match res {
            Ok(Ok(true)) => {
                eprintln!("focused {id} in {} ms", started.elapsed().as_millis());
                if self.reg.lock().unwrap().ack(&id) {
                    self.notify();
                }
            }
            Ok(Ok(false)) => eprintln!("focus: no terminal tab found for session {id}"),
            Ok(Err(e)) => eprintln!("focus: {e:#}"),
            Err(e) => eprintln!("focus task: {e}"),
        }
    }

    fn handle_knob(self: &Arc<Self>, ev: FromKnob) {
        match ev {
            FromKnob::Hello { fw } => eprintln!("knob connected (fw {fw})"),
            FromKnob::Select { idx, id } | FromKnob::Press { idx, id } => {
                eprintln!("knob: select {idx}");
                if let Some(id) = self.select(idx, id) {
                    self.request_focus(id);
                }
            }
        }
    }

    fn select(&self, idx: usize, id: Option<String>) -> Option<String> {
        let mut reg = self.reg.lock().unwrap();
        let id = reg.resolve(idx, id.as_deref())?.id.clone();
        reg.sel_id = Some(id.clone());
        Some(id)
    }
}

/// Runs a knob link: pushes snapshots whenever state changes, handles knob events.
/// Returns when the reader hits EOF or an I/O error.
pub async fn run_link<R, W>(shared: Arc<Shared>, reader: R, mut writer: W)
where
    R: AsyncBufRead + Unpin,
    W: AsyncWrite + Unpin,
{
    let mut rx = shared.changed.subscribe();
    let mut raw_rx = shared.raw_tx.subscribe();
    let mut lines = reader.lines();
    let mut heartbeat = tokio::time::interval(HEARTBEAT);
    loop {
        tokio::select! {
            r = rx.changed() => {
                if r.is_err() || send(&mut writer, &shared.snapshot()).await.is_err() {
                    return;
                }
                heartbeat.reset();
            }
            raw = raw_rx.recv() => {
                if let Ok(line) = raw {
                    let mut buf = line.into_bytes();
                    buf.push(b'\n');
                    if writer.write_all(&buf).await.is_err() || writer.flush().await.is_err() {
                        return;
                    }
                }
            }
            // Also fires immediately on connect.
            _ = heartbeat.tick() => {
                if send(&mut writer, &shared.snapshot()).await.is_err() {
                    return;
                }
            }
            line = lines.next_line() => {
                let line = match line {
                    Ok(Some(l)) => l,
                    _ => return,
                };
                let line = line.trim();
                if line.is_empty() {
                    continue;
                }
                match serde_json::from_str::<FromKnob>(line) {
                    Ok(FromKnob::Hello { fw }) => {
                        shared.handle_knob(FromKnob::Hello { fw });
                        if send(&mut writer, &shared.snapshot()).await.is_err() {
                            return;
                        }
                    }
                    Ok(ev) => shared.handle_knob(ev),
                    // The firmware may print debug logs on the same port.
                    Err(_) => eprintln!("knob: {line}"),
                }
            }
        }
    }
}

async fn send<W: AsyncWrite + Unpin>(w: &mut W, msg: &impl serde::Serialize) -> std::io::Result<()> {
    let mut buf = serde_json::to_vec(msg).expect("serializable");
    buf.push(b'\n');
    w.write_all(&buf).await?;
    w.flush().await
}

#[derive(Debug)]
pub struct AlreadyRunning;

impl std::fmt::Display for AlreadyRunning {
    fn fmt(&self, f: &mut std::fmt::Formatter) -> std::fmt::Result {
        write!(f, "another knobd is already running ({})", socket_path().display())
    }
}

impl std::error::Error for AlreadyRunning {}

/// Binds the socket and spawns the daemon's tasks on the current runtime.
/// The returned handle is the accept loop; it only finishes on error.
pub async fn start(
    port: Option<String>,
    no_serial: bool,
) -> anyhow::Result<(Arc<Shared>, JoinHandle<anyhow::Result<()>>)> {
    std::fs::create_dir_all(state_dir())?;
    let sock = socket_path();
    if UnixStream::connect(&sock).await.is_ok() {
        return Err(AlreadyRunning.into());
    }
    let _ = std::fs::remove_file(&sock);
    let listener = UnixListener::bind(&sock).with_context(|| format!("binding {}", sock.display()))?;
    eprintln!("knobd listening on {}", sock.display());

    let (changed, _) = watch::channel(0u64);
    let shared = Arc::new(Shared {
        reg: Mutex::new(load_registry()),
        changed,
        focuser: focus::platform(),
        focus_queue: Mutex::new(FocusQueue::default()),
        knob_port: Mutex::new(None),
        raw_tx: tokio::sync::broadcast::channel(16).0,
    });
    {
        // Terminal -> knob: follow whatever window the user brings to the front.
        let weak = Arc::downgrade(&shared);
        shared.focuser.on_focus_change(Box::new(move |tty| {
            if let Some(shared) = weak.upgrade() {
                shared.on_terminal_focus(tty);
            }
        }));
    }

    {
        let shared = shared.clone();
        tokio::spawn(async move {
            let mut tick = tokio::time::interval(PRUNE_EVERY);
            loop {
                tick.tick().await;
                if shared.reg.lock().unwrap().prune() {
                    shared.notify();
                }
            }
        });
    }

    if !no_serial {
        tokio::spawn(crate::serial::run(shared.clone(), port));
    }

    let accept = {
        let shared = shared.clone();
        tokio::spawn(async move {
            loop {
                let (stream, _) = listener.accept().await?;
                let shared = shared.clone();
                tokio::spawn(async move {
                    if let Err(e) = handle_conn(shared, stream).await {
                        eprintln!("connection: {e:#}");
                    }
                });
            }
        })
    };
    Ok((shared, accept))
}

/// Removes the socket so the next start doesn't have to.
pub fn cleanup() {
    let _ = std::fs::remove_file(socket_path());
}

async fn handle_conn(shared: Arc<Shared>, stream: UnixStream) -> anyhow::Result<()> {
    let (r, mut w) = stream.into_split();
    let mut reader = BufReader::new(r);
    let mut line = String::new();
    // Probes (e.g. the "already running" check) connect and hang up without a request.
    if reader.read_line(&mut line).await? == 0 {
        return Ok(());
    }
    match serde_json::from_str::<Ctl>(line.trim()).context("bad request")? {
        Ctl::Hook(msg) => {
            if shared.reg.lock().unwrap().apply(&msg) {
                shared.notify();
            }
        }
        Ctl::List => {
            let infos = shared.reg.lock().unwrap().infos();
            send(&mut w, &infos).await?;
        }
        Ctl::Focus { idx } => {
            let id = shared.reg.lock().unwrap().get(idx).map(|s| s.id.clone());
            let reply = match id {
                Some(id) => {
                    shared.focus_id(id).await;
                    "ok"
                }
                None => "no such session",
            };
            send(&mut w, &reply).await?;
        }
        Ctl::Attach => run_link(shared, reader, w).await,
        Ctl::Knob { line } => {
            let reply = match shared.raw_tx.send(line) {
                Ok(_) => "sent",
                Err(_) => "no knob connected",
            };
            send(&mut w, &reply).await?;
        }
    }
    Ok(())
}
