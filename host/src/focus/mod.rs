//! Bringing a session's terminal to the front. One implementation per platform.

#[cfg(target_os = "macos")]
mod macos;

pub trait Focuser: Send + Sync {
    /// Focuses the terminal tab attached to `tty` (e.g. `/dev/ttys003`).
    /// Returns Ok(false) when no tab owns that tty.
    fn focus(&self, tty: &str) -> anyhow::Result<bool>;

    /// Calls `listener` with the tty of the terminal the user is looking at (None when it's
    /// not a terminal), whenever that changes. Platforms that can't tell never call it.
    fn on_focus_change(&self, _listener: Box<dyn Fn(Option<String>) + Send>) {}
}

pub fn platform() -> Box<dyn Focuser> {
    #[cfg(target_os = "macos")]
    return Box::new(macos::TerminalApp::new());
    #[cfg(not(target_os = "macos"))]
    return Box::new(Unsupported);
}

#[cfg(not(target_os = "macos"))]
struct Unsupported;

#[cfg(not(target_os = "macos"))]
impl Focuser for Unsupported {
    fn focus(&self, _tty: &str) -> anyhow::Result<bool> {
        anyhow::bail!("window focusing is not implemented on this platform yet")
    }
}
