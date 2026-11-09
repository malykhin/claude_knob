//! Wire formats. Everything is newline-delimited JSON.
//!
//! * Hook / CLI → daemon: [`Ctl`] over the unix socket.
//! * Daemon ↔ knob: [`ToKnob`] / [`FromKnob`] over USB serial (or an `attach`ed socket for the simulator).

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Clone, Copy, PartialEq, Eq, Debug)]
#[serde(rename_all = "lowercase")]
pub enum State {
    Idle,
    Running,
    Pending,
    Done,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Item {
    pub id: String,
    pub name: String,
    pub state: State,
}

fn no_focus() -> i32 {
    -1
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(tag = "t", rename_all = "lowercase")]
pub enum ToKnob {
    /// Full snapshot. `sel` is the index the knob should point at (-1 when empty);
    /// `focus` is the session whose terminal is in front, or -1 when the user is
    /// looking at something else.
    Sessions {
        items: Vec<Item>,
        sel: i32,
        #[serde(default = "no_focus")]
        focus: i32,
    },
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(tag = "t", rename_all = "lowercase")]
pub enum FromKnob {
    Hello {
        #[serde(default)]
        fw: String,
    },
    Select {
        idx: usize,
        #[serde(default)]
        id: Option<String>,
    },
    Press {
        idx: usize,
        #[serde(default)]
        id: Option<String>,
    },
    /// Keepalive, every 2 s while the knob sees the host. A knob that has sent one and
    /// then goes quiet has a stuck USB transmit (see `run_link`).
    Alive,
}

#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct HookMsg {
    pub event: String,
    pub session_id: String,
    #[serde(default)]
    pub cwd: String,
    #[serde(default)]
    pub tty: Option<String>,
    #[serde(default)]
    pub pid: Option<i32>,
    #[serde(default)]
    pub notification_type: Option<String>,
}

#[derive(Serialize, Deserialize, Debug)]
#[serde(tag = "t", rename_all = "lowercase")]
pub enum Ctl {
    Hook(HookMsg),
    List,
    Focus {
        idx: usize,
    },
    /// Turns the connection into a knob link (used by `knobd sim`).
    Attach,
    /// Forwards one raw JSON line to the knob (board-specific tuning/debug commands).
    Knob {
        line: String,
    },
}

#[derive(Serialize, Deserialize, Debug)]
pub struct SessionInfo {
    pub id: String,
    pub name: String,
    pub cwd: String,
    pub tty: Option<String>,
    pub pid: Option<i32>,
    pub state: State,
    #[serde(default)]
    pub focused: bool,
}
