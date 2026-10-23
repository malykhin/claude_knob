use crate::protocol::{HookMsg, Item, SessionInfo, State};

const MAX_NAME: usize = 24;

#[derive(Debug, Clone)]
pub struct Session {
    pub id: String,
    pub cwd: String,
    pub tty: Option<String>,
    pub pid: Option<i32>,
    pub state: State,
}

impl Session {
    pub fn name(&self) -> String {
        let base = std::path::Path::new(&self.cwd)
            .file_name()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| self.cwd.clone());
        let base = if base.is_empty() { "claude".to_string() } else { base };
        base.chars().take(MAX_NAME).collect()
    }
}

/// Sessions in the order they were first seen, so knob positions stay stable.
#[derive(Default)]
pub struct Registry {
    sessions: Vec<Session>,
    /// Session the knob is currently pointing at.
    pub sel_id: Option<String>,
    /// tty of the terminal tab the user is looking at (None: not a terminal / unknown).
    focused_tty: Option<String>,
}

fn state_for(msg: &HookMsg, current: Option<State>) -> Option<State> {
    match msg.event.as_str() {
        "SessionStart" => Some(State::Idle),
        "UserPromptSubmit" | "PreToolUse" | "PostToolUse" | "PreCompact" | "SubagentStop" => {
            Some(State::Running)
        }
        "PermissionRequest" => Some(State::Pending),
        // idle_prompt fires after Claude has been waiting a while; it doesn't change anything.
        "Notification" => match msg.notification_type.as_deref() {
            Some("idle_prompt") => current,
            _ => Some(State::Pending),
        },
        "Stop" => Some(State::Done),
        _ => current,
    }
}

impl Registry {
    /// Rebuilds a registry saved with [`Registry::infos`], dropping sessions whose process is gone.
    pub fn from_infos(infos: Vec<SessionInfo>) -> Self {
        let mut r = Registry {
            sessions: infos
                .into_iter()
                .map(|i| Session { id: i.id, cwd: i.cwd, tty: i.tty, pid: i.pid, state: i.state })
                .collect(),
            sel_id: None,
            focused_tty: None,
        };
        r.prune();
        r
    }

    /// Applies a hook event. Returns true when anything visible changed.
    pub fn apply(&mut self, msg: &HookMsg) -> bool {
        if msg.event == "SessionEnd" {
            return self.remove(&msg.session_id);
        }

        let Some(pos) = self.sessions.iter().position(|s| s.id == msg.session_id) else {
            // A new session on a tty replaces whatever ran there before (e.g. after /clear).
            if let Some(tty) = &msg.tty {
                self.sessions.retain(|s| s.tty.as_ref() != Some(tty));
            }
            self.sessions.push(Session {
                id: msg.session_id.clone(),
                cwd: msg.cwd.clone(),
                tty: msg.tty.clone(),
                pid: msg.pid,
                state: state_for(msg, None).unwrap_or(State::Idle),
            });
            return true;
        };

        let s = &mut self.sessions[pos];
        let before = (s.state, s.cwd.clone());
        // Keep the directory the session started in: later events report wherever
        // the shell has cd'd to, and the knob label shouldn't change under the user.
        if s.cwd.is_empty() {
            s.cwd = msg.cwd.clone();
        }
        if msg.tty.is_some() {
            s.tty = msg.tty.clone();
        }
        if msg.pid.is_some() {
            s.pid = msg.pid;
        }
        if let Some(st) = state_for(msg, Some(s.state)) {
            s.state = st;
        }
        before != (s.state, s.cwd.clone())
    }

    fn remove(&mut self, id: &str) -> bool {
        let n = self.sessions.len();
        self.sessions.retain(|s| s.id != id);
        n != self.sessions.len()
    }

    /// Drops sessions whose claude process is gone.
    pub fn prune(&mut self) -> bool {
        let n = self.sessions.len();
        self.sessions.retain(|s| s.pid.map_or(true, pid_alive));
        n != self.sessions.len()
    }

    /// Clears the "done" highlight once the user has looked at the session.
    pub fn ack(&mut self, id: &str) -> bool {
        match self.sessions.iter_mut().find(|s| s.id == id) {
            Some(s) if s.state == State::Done => {
                s.state = State::Idle;
                true
            }
            _ => false,
        }
    }

    pub fn get(&self, idx: usize) -> Option<&Session> {
        self.sessions.get(idx)
    }

    pub fn find(&self, id: &str) -> Option<&Session> {
        self.sessions.iter().find(|s| s.id == id)
    }

    /// Resolves a knob event: the id wins when it still exists, else the index.
    pub fn resolve(&self, idx: usize, id: Option<&str>) -> Option<&Session> {
        id.and_then(|id| self.find(id)).or_else(|| self.get(idx))
    }

    /// Index of the session whose terminal is in front, or -1.
    pub fn focus_index(&self) -> i32 {
        let Some(tty) = &self.focused_tty else { return -1 };
        self.sessions.iter().position(|s| s.tty.as_ref() == Some(tty)).map_or(-1, |i| i as i32)
    }

    /// Records which terminal is in front. Focusing a session selects it, and counts as
    /// having seen it (clears "done"). Returns true when anything visible changed.
    pub fn set_focused_tty(&mut self, tty: Option<String>) -> bool {
        if self.focused_tty == tty {
            return false;
        }
        self.focused_tty = tty;
        if let Some(i) = usize::try_from(self.focus_index()).ok() {
            let id = self.sessions[i].id.clone();
            self.ack(&id);
            self.sel_id = Some(id);
        }
        true
    }

    pub fn snapshot(&self) -> (Vec<Item>, i32) {
        let items = self
            .sessions
            .iter()
            .map(|s| Item { id: s.id.clone(), name: s.name(), state: s.state })
            .collect::<Vec<_>>();
        let focus = self.focus_index();
        let sel = match &self.sel_id {
            _ if items.is_empty() => -1,
            _ if focus >= 0 => focus,
            Some(id) => self.sessions.iter().position(|s| &s.id == id).unwrap_or(0) as i32,
            None => 0,
        };
        (items, sel)
    }

    pub fn infos(&self) -> Vec<SessionInfo> {
        self.sessions
            .iter()
            .map(|s| SessionInfo {
                id: s.id.clone(),
                name: s.name(),
                cwd: s.cwd.clone(),
                tty: s.tty.clone(),
                pid: s.pid,
                state: s.state,
                focused: s.tty.is_some() && s.tty == self.focused_tty,
            })
            .collect()
    }
}

fn pid_alive(pid: i32) -> bool {
    // SAFETY: signal 0 only checks for existence/permission.
    let r = unsafe { libc::kill(pid, 0) };
    r == 0 || std::io::Error::last_os_error().raw_os_error() != Some(libc::ESRCH)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn msg(event: &str, id: &str, tty: Option<&str>) -> HookMsg {
        HookMsg {
            event: event.into(),
            session_id: id.into(),
            cwd: "/x/proj".into(),
            tty: tty.map(Into::into),
            pid: None,
            notification_type: None,
        }
    }

    #[test]
    fn lifecycle() {
        let mut r = Registry::default();
        assert!(r.apply(&msg("SessionStart", "a", Some("/dev/ttys001"))));
        assert!(r.apply(&msg("SessionStart", "b", Some("/dev/ttys002"))));
        r.apply(&msg("UserPromptSubmit", "a", None));
        assert_eq!(r.get(0).unwrap().state, State::Running);
        r.apply(&msg("Notification", "a", None));
        assert_eq!(r.get(0).unwrap().state, State::Pending);
        r.apply(&msg("Stop", "a", None));
        assert_eq!(r.get(0).unwrap().state, State::Done);
        assert!(r.ack("a"));
        assert_eq!(r.get(0).unwrap().state, State::Idle);
        assert!(r.apply(&msg("SessionEnd", "a", None)));
        assert_eq!(r.snapshot().0.len(), 1);
        assert_eq!(r.get(0).unwrap().name(), "proj");
    }

    #[test]
    fn name_sticks_to_start_dir() {
        let mut r = Registry::default();
        r.apply(&msg("SessionStart", "a", None));
        let mut m = msg("PreToolUse", "a", None);
        m.cwd = "/x/proj/sub".into();
        r.apply(&m);
        assert_eq!(r.get(0).unwrap().name(), "proj");
    }

    #[test]
    fn idle_prompt_keeps_state() {
        let mut r = Registry::default();
        r.apply(&msg("Stop", "a", None));
        let mut n = msg("Notification", "a", None);
        n.notification_type = Some("idle_prompt".into());
        assert!(!r.apply(&n));
        assert_eq!(r.get(0).unwrap().state, State::Done);
    }

    #[test]
    fn same_tty_replaces_session() {
        let mut r = Registry::default();
        r.apply(&msg("SessionStart", "a", Some("/dev/ttys001")));
        r.apply(&msg("SessionStart", "b", Some("/dev/ttys001")));
        let (items, sel) = r.snapshot();
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].id, "b");
        assert_eq!(sel, 0);
    }

    #[test]
    fn round_trips_through_infos() {
        let mut r = Registry::default();
        let mut m = msg("SessionStart", "a", Some("/dev/ttys001"));
        m.pid = Some(std::process::id() as i32);
        r.apply(&m);
        m.session_id = "dead".into();
        m.tty = Some("/dev/ttys002".into());
        m.pid = Some(i32::MAX); // no such process
        r.apply(&m);
        let back = Registry::from_infos(r.infos());
        let ids: Vec<_> = back.snapshot().0.into_iter().map(|i| i.id).collect();
        assert_eq!(ids, ["a"]);
    }

    #[test]
    fn focus_selects_and_acks() {
        let mut r = Registry::default();
        r.apply(&msg("SessionStart", "a", Some("/dev/ttys001")));
        r.apply(&msg("SessionStart", "b", Some("/dev/ttys002")));
        r.apply(&msg("Stop", "b", None));
        assert_eq!(r.focus_index(), -1);
        assert!(r.set_focused_tty(Some("/dev/ttys002".into())));
        assert_eq!(r.focus_index(), 1);
        assert_eq!(r.snapshot().1, 1);
        assert_eq!(r.get(1).unwrap().state, State::Idle, "focusing counts as seen");
        assert!(!r.set_focused_tty(Some("/dev/ttys002".into())), "no change");
        // A plain shell tab or another app: nothing focused.
        assert!(r.set_focused_tty(Some("/dev/ttys009".into())));
        assert_eq!(r.focus_index(), -1);
        // A session that registers after its window was focused still resolves.
        r.apply(&msg("SessionStart", "c", Some("/dev/ttys009")));
        assert_eq!(r.focus_index(), 2);
    }

    #[test]
    fn sel_follows_id() {
        let mut r = Registry::default();
        r.apply(&msg("SessionStart", "a", None));
        r.apply(&msg("SessionStart", "b", None));
        r.sel_id = Some("b".into());
        assert_eq!(r.snapshot().1, 1);
        r.apply(&msg("SessionEnd", "a", None));
        assert_eq!(r.snapshot().1, 0);
    }
}
