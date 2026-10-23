//! Terminal.app focusing through in-process AppleScript.
//!
//! Every Apple Event to Terminal costs ~20 ms, so the hot path sends as few as
//! possible: a cached tty -> (window id, tab) map turns a focus into "raise window"
//! (+ "select tab" when the window has several, + "activate" when Terminal isn't
//! already frontmost). Scripts run on one dedicated thread (NSAppleScript isn't
//! thread-safe) and stay compiled between calls.

use std::collections::HashMap;
use std::sync::mpsc;
use std::sync::Mutex;
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail};
use objc2::rc::{autoreleasepool, Retained};
use objc2::runtime::AnyObject;
use objc2::{msg_send, AllocAnyThread};
use objc2_app_kit::NSRunningApplication;
use objc2_foundation::{NSAppleEventDescriptor, NSAppleScript, NSDictionary, NSString};

use super::Focuser;

const TERMINAL_BUNDLE: &str = "com.apple.Terminal";
/// Re-read the window map when idle this long, to catch tabs that were moved around.
const IDLE_REFRESH: Duration = Duration::from_secs(10);

/// Lists every tab as "window-id <TAB> tab-index <TAB> tab-count <TAB> tty" lines,
/// with two Apple Events in total.
const MAP_SCRIPT: &str = r#"
tell application "Terminal" to set {ids, ttys} to {id, tty of tabs} of every window
set out to ""
repeat with i from 1 to count of ids
  set tl to item i of ttys
  repeat with j from 1 to count of tl
    set out to out & (item i of ids) & tab & j & tab & (count of tl) & tab & (item j of tl) & linefeed
  end repeat
end repeat
return out
"#;

type Reply = mpsc::Sender<anyhow::Result<bool>>;
type Listener = Box<dyn Fn(Option<String>) + Send>;

enum Job {
    Focus(String, Reply),
    Listen(Listener),
}

/// How often the focused window is checked. Listing windows costs ~1 ms and no Apple Events.
const WATCH_EVERY: Duration = Duration::from_millis(100);
/// Windows with several tabs need an Apple Event to see which tab is selected; do it less often.
const TAB_POLL_EVERY: Duration = Duration::from_millis(300);

pub struct TerminalApp {
    jobs: Mutex<mpsc::Sender<Job>>,
}

impl TerminalApp {
    pub fn new() -> Self {
        let (tx, rx) = mpsc::channel::<Job>();
        std::thread::Builder::new()
            .name("terminal-focus".into())
            .spawn(move || {
                let mut w = Worker::new();
                // Warm up so the first turn of the knob is as fast as the rest.
                let _ = autoreleasepool(|_| w.refresh());
                let mut listener: Option<Listener> = None;
                let mut last_watch = Instant::now();
                loop {
                    match rx.recv_timeout(WATCH_EVERY) {
                        Ok(Job::Focus(tty, reply)) => {
                            // A panic must not take window switching down for good: report it
                            // and keep serving with a fresh cache.
                            let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                                autoreleasepool(|_| w.focus(&tty))
                            }))
                            .unwrap_or_else(|_| {
                                w.tabs.clear();
                                Err(anyhow!("focus panicked"))
                            });
                            let _ = reply.send(r);
                        }
                        Ok(Job::Listen(f)) => listener = Some(f),
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                        Err(mpsc::RecvTimeoutError::Disconnected) => return,
                    }
                    if last_watch.elapsed() >= WATCH_EVERY {
                        last_watch = Instant::now();
                        if let Some(f) = &listener {
                            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                                autoreleasepool(|_| w.watch(f))
                            }));
                        }
                    }
                    if w.refreshed_at.elapsed() >= IDLE_REFRESH {
                        let _ = autoreleasepool(|_| w.refresh());
                    }
                }
            })
            .expect("spawn focus thread");
        TerminalApp { jobs: Mutex::new(tx) }
    }
}

impl Focuser for TerminalApp {
    fn focus(&self, tty: &str) -> anyhow::Result<bool> {
        let (tx, rx) = mpsc::channel();
        self.jobs.lock().unwrap().send(Job::Focus(tty.to_owned(), tx)).map_err(|_| anyhow!("focus thread died"))?;
        rx.recv().map_err(|_| anyhow!("focus thread died"))?
    }

    fn on_focus_change(&self, listener: Box<dyn Fn(Option<String>) + Send>) {
        let _ = self.jobs.lock().unwrap().send(Job::Listen(listener));
    }
}

/// The topmost normal on-screen window: (owner pid, CGWindowID). CGWindowIDs are the
/// numbers Terminal's AppleScript calls `id of window`.
///
/// This asks the window server, so it's always current and needs no permissions. Don't
/// use NSWorkspace.frontmostApplication for this: it's cached in-process, and after a
/// switch to another app it stayed stale for minutes, so the knob stopped following.
fn top_window() -> Option<(i64, i64)> {
    use core_foundation::base::{CFType, TCFType};
    use core_foundation::dictionary::CFDictionary;
    use core_foundation::number::CFNumber;
    use core_foundation::string::CFString;
    use core_graphics::window::{
        copy_window_info, kCGNullWindowID, kCGWindowAlpha, kCGWindowLayer, kCGWindowListExcludeDesktopElements,
        kCGWindowListOptionOnScreenOnly, kCGWindowNumber, kCGWindowOwnerPID,
    };

    let list = copy_window_info(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID)?;
    for item in list.iter() {
        let dict: CFDictionary<CFString, CFType> =
            unsafe { CFDictionary::wrap_under_get_rule(*item as core_foundation::dictionary::CFDictionaryRef) };
        let num = |key| {
            dict.find(unsafe { CFString::wrap_under_get_rule(key) })
                .and_then(|v| v.downcast::<CFNumber>())
        };
        if num(unsafe { kCGWindowLayer }).and_then(|n| n.to_i64()) != Some(0) {
            continue;
        }
        if num(unsafe { kCGWindowAlpha }).and_then(|n| n.to_f64()).is_some_and(|a| a <= 0.0) {
            continue;
        }
        let owner = num(unsafe { kCGWindowOwnerPID }).and_then(|n| n.to_i64())?;
        let window = num(unsafe { kCGWindowNumber }).and_then(|n| n.to_i64())?;
        return Some((owner, window));
    }
    None
}

/// The front Terminal window, if Terminal's window is the topmost one on screen.
fn front_terminal_window() -> Option<i64> {
    let pid = terminal_pid()? as i64;
    top_window().filter(|(owner, _)| *owner == pid).map(|(_, w)| w)
}

fn terminal_pid() -> Option<i32> {
    let apps = NSRunningApplication::runningApplicationsWithBundleIdentifier(&NSString::from_str(TERMINAL_BUNDLE));
    apps.firstObject().map(|a| a.processIdentifier())
}

#[derive(Clone, Copy)]
struct TabRef {
    window: i64,
    index: usize,
    count: usize,
}

fn error_message(info: &NSDictionary<NSString, AnyObject>) -> String {
    info.objectForKey(&NSString::from_str("NSAppleScriptErrorMessage"))
        .and_then(|m| m.downcast::<NSString>().ok())
        .map_or_else(|| format!("{info:?}"), |m| m.to_string())
}

fn focus_source(t: TabRef, activate: bool) -> String {
    let select = if t.count > 1 { format!("set selected of tab {} of w to true\n", t.index) } else { String::new() };
    let activate = if activate { "activate\n" } else { "" };
    format!("tell application \"Terminal\"\nset w to window id {}\n{select}set index of w to 1\n{activate}end tell", t.window)
}

struct Worker {
    scripts: HashMap<String, Retained<NSAppleScript>>,
    tabs: HashMap<String, TabRef>,
    refreshed_at: Instant,
    /// Last focused tty reported to the listener.
    reported: Option<Option<String>>,
    /// Selected-tab cache for multi-tab windows: (window, tty, when).
    tab_seen: Option<(i64, String, Instant)>,
}

impl Worker {
    fn new() -> Self {
        Worker {
            scripts: HashMap::new(),
            tabs: HashMap::new(),
            refreshed_at: Instant::now(),
            reported: None,
            tab_seen: None,
        }
    }

    /// The tty of the Terminal tab the user is looking at, if Terminal is in front.
    fn focused_tty(&mut self) -> Option<String> {
        let window = front_terminal_window()?;
        let mut in_window: Vec<(String, TabRef)> = self.tabs_in(window);
        if in_window.is_empty() && self.refreshed_at.elapsed() > Duration::from_secs(1) {
            // A window we haven't seen yet.
            let _ = self.refresh();
            in_window = self.tabs_in(window);
        }
        match in_window.len() {
            0 => None,
            1 => Some(in_window.remove(0).0),
            _ => {
                if let Some((w, tty, at)) = &self.tab_seen {
                    if *w == window && at.elapsed() < TAB_POLL_EVERY {
                        return Some(tty.clone());
                    }
                }
                let tty = self.run(&format!("tell application \"Terminal\" to get tty of selected tab of window id {window}")).ok()?;
                self.tab_seen = Some((window, tty.clone(), Instant::now()));
                Some(tty)
            }
        }
    }

    fn tabs_in(&self, window: i64) -> Vec<(String, TabRef)> {
        self.tabs.iter().filter(|(_, t)| t.window == window).map(|(k, t)| (k.clone(), *t)).collect()
    }

    fn watch(&mut self, listener: &Listener) {
        let now = self.focused_tty();
        if self.reported.as_ref() != Some(&now) {
            self.reported = Some(now.clone());
            listener(now);
        }
    }

    /// Compiling takes ~45 ms, so compiled scripts are kept. Focus scripts differ only
    /// by window/tab, so this stays small.
    fn compiled(&mut self, source: &str) -> anyhow::Result<Retained<NSAppleScript>> {
        if let Some(s) = self.scripts.get(source) {
            return Ok(s.clone());
        }
        let s = NSAppleScript::initWithSource(NSAppleScript::alloc(), &NSString::from_str(source))
            .ok_or_else(|| anyhow!("NSAppleScript init failed"))?;
        let mut err = None;
        if !unsafe { s.compileAndReturnError(Some(&mut err)) } {
            bail!("compiling AppleScript: {}", err.map_or("unknown error".into(), |e| error_message(&e)));
        }
        self.scripts.insert(source.to_owned(), s.clone());
        Ok(s)
    }

    fn run(&mut self, source: &str) -> anyhow::Result<String> {
        let script = self.compiled(source)?;
        let mut err: Option<Retained<NSDictionary<NSString, AnyObject>>> = None;
        // Not the generated binding: it declares a non-null return and panics on the
        // nil that -executeAndReturnError: gives back when the script fails
        // (e.g. a cached window that was closed).
        let out: Option<Retained<NSAppleEventDescriptor>> =
            unsafe { msg_send![&*script, executeAndReturnError: Some(&mut err)] };
        match (out, err) {
            (_, Some(e)) => bail!("AppleScript: {}", error_message(&e)),
            (None, None) => bail!("AppleScript failed"),
            (Some(out), None) => Ok(out.stringValue().map(|s| s.to_string()).unwrap_or_default()),
        }
    }

    fn refresh(&mut self) -> anyhow::Result<()> {
        let out = self.run(MAP_SCRIPT)?;
        self.tabs = out
            .lines()
            .filter_map(|l| {
                let mut f = l.split('\t');
                let window = f.next()?.parse().ok()?;
                let index = f.next()?.parse().ok()?;
                let count = f.next()?.parse().ok()?;
                Some((f.next()?.to_owned(), TabRef { window, index, count }))
            })
            .collect();
        self.refreshed_at = Instant::now();
        // Precompile focus scripts for every tab, with and without `activate`.
        let tabs: Vec<TabRef> = self.tabs.values().copied().collect();
        for t in tabs {
            for activate in [false, true] {
                self.compiled(&focus_source(t, activate))?;
            }
        }
        Ok(())
    }

    fn terminal_is_frontmost(&self) -> bool {
        front_terminal_window().is_some()
    }

    fn focus_tab(&mut self, t: TabRef) -> anyhow::Result<()> {
        let activate = !self.terminal_is_frontmost();
        self.run(&focus_source(t, activate)).map(|_| ())
    }

    fn focus(&mut self, tty: &str) -> anyhow::Result<bool> {
        if !self.tabs.contains_key(tty) {
            self.refresh()?;
        }
        let Some(&t) = self.tabs.get(tty) else { return Ok(false) };
        if self.focus_tab(t).is_ok() {
            return Ok(true);
        }
        // The window is gone or the tab moved: re-read the map and try once more.
        self.refresh()?;
        let Some(&t) = self.tabs.get(tty) else { return Ok(false) };
        self.focus_tab(t)?;
        Ok(true)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn worker() -> Worker {
        Worker::new()
    }

    #[test]
    fn script_error_is_an_error_not_a_panic() {
        let mut w = worker();
        let e = w.run("error \"boom\"").unwrap_err();
        assert!(e.to_string().contains("boom"), "{e}");
        // And the worker keeps working afterwards.
        assert_eq!(w.run("return \"ok\"").unwrap(), "ok");
    }
}
