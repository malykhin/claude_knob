//! `knobd sim`: a stand-in for the hardware. Attaches to the daemon over the
//! socket, speaks the same protocol as the firmware, and is driven from stdin.

use std::sync::{Arc, Mutex};

use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixStream;

use crate::daemon::socket_path;
use crate::protocol::{FromKnob, Item, State, ToKnob};

#[derive(Default)]
struct Knob {
    items: Vec<Item>,
    sel: usize,
    focus: i32,
}

impl Knob {
    fn render(&self) {
        if self.items.is_empty() {
            println!("  (no Claude sessions)");
            return;
        }
        for (i, it) in self.items.iter().enumerate() {
            let mark = if i as i32 == self.focus {
                "▶"
            } else if i == self.sel {
                "▷"
            } else {
                " "
            };
            let st = match it.state {
                State::Idle => "idle",
                State::Running => "RUNNING",
                State::Pending => "PENDING ⚠",
                State::Done => "done ✓",
            };
            println!("{mark} {}. {:<24} {st}", i + 1, it.name);
        }
    }

    fn event(&self, press: bool) -> Option<FromKnob> {
        let it = self.items.get(self.sel)?;
        let (idx, id) = (self.sel, Some(it.id.clone()));
        Some(if press { FromKnob::Press { idx, id } } else { FromKnob::Select { idx, id } })
    }
}

pub async fn run() -> anyhow::Result<()> {
    let stream = UnixStream::connect(socket_path()).await?;
    let (r, mut w) = stream.into_split();
    w.write_all(b"{\"t\":\"attach\"}\n{\"t\":\"hello\",\"fw\":\"sim\"}\n").await?;
    println!("Simulated knob. Commands: j/+ = rotate right, k/- = rotate left, p/<enter> = press, q = quit");

    let knob = Arc::new(Mutex::new(Knob::default()));
    {
        let knob = knob.clone();
        tokio::spawn(async move {
            let mut lines = BufReader::new(r).lines();
            let mut last = String::new();
            while let Ok(Some(l)) = lines.next_line().await {
                // Skip heartbeats that repeat the previous snapshot.
                if l == last {
                    continue;
                }
                last = l.clone();
                let Ok(ToKnob::Sessions { items, sel, focus }) = serde_json::from_str(&l) else { continue };
                let mut k = knob.lock().unwrap();
                k.items = items;
                k.sel = sel.max(0) as usize;
                k.focus = focus;
                println!("--");
                k.render();
            }
            println!("daemon closed the connection");
            std::process::exit(0);
        });
    }

    let mut stdin = BufReader::new(tokio::io::stdin()).lines();
    while let Some(cmd) = stdin.next_line().await? {
        let ev = {
            let mut k = knob.lock().unwrap();
            let n = k.items.len();
            match cmd.trim() {
                "q" => break,
                "j" | "+" if n > 0 => {
                    k.sel = (k.sel + 1) % n;
                    k.event(false)
                }
                "k" | "-" if n > 0 => {
                    k.sel = (k.sel + n - 1) % n;
                    k.event(false)
                }
                "p" | "" => k.event(true),
                _ => None,
            }
        };
        if let Some(ev) = ev {
            knob.lock().unwrap().render();
            let mut buf = serde_json::to_vec(&ev)?;
            buf.push(b'\n');
            w.write_all(&buf).await?;
        }
    }
    Ok(())
}
