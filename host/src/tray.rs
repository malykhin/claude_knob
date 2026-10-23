//! Menu-bar icon for `knobd run`. The UI event loop must own the main thread on
//! macOS, so the tokio runtime keeps running on its worker threads alongside it.

use std::collections::HashMap;
use std::sync::Arc;

use tao::event::{Event, StartCause};
use tao::event_loop::{ControlFlow, EventLoopBuilder};
#[cfg(target_os = "macos")]
use tao::platform::macos::{ActivationPolicy, EventLoopExtMacOS};
use tray_icon::menu::{CheckMenuItem, Menu, MenuEvent, MenuId, MenuItem, PredefinedMenuItem};
use tray_icon::{Icon, TrayIcon, TrayIconBuilder};

use crate::daemon::{self, state_dir, Shared};
use crate::install;
use crate::protocol::State;

enum UiEvent {
    Changed,
    Menu(MenuId),
}

enum Action {
    Focus(String),
    ToggleHooks,
    ToggleLogin,
    OpenLog,
    Quit,
}

pub fn run(shared: Arc<Shared>, rt: tokio::runtime::Runtime) -> ! {
    #[allow(unused_mut)]
    let mut event_loop = EventLoopBuilder::<UiEvent>::with_user_event().build();
    // Menu bar only: no Dock icon, no app menu.
    #[cfg(target_os = "macos")]
    event_loop.set_activation_policy(ActivationPolicy::Accessory);

    let proxy = event_loop.create_proxy();
    {
        let proxy = proxy.clone();
        MenuEvent::set_event_handler(Some(move |e: MenuEvent| {
            let _ = proxy.send_event(UiEvent::Menu(e.id));
        }));
    }
    {
        let mut rx = shared.subscribe();
        rt.spawn(async move {
            while rx.changed().await.is_ok() {
                if proxy.send_event(UiEvent::Changed).is_err() {
                    break;
                }
            }
        });
    }

    let mut ui = Ui { shared, rt, tray: None, actions: HashMap::new(), alert: None };
    event_loop.run(move |event, _, flow| {
        *flow = ControlFlow::Wait;
        match event {
            // The tray has to be created once the event loop is running.
            Event::NewEvents(StartCause::Init) => {
                match TrayIconBuilder::new().with_icon_templated(knob_icon(None)).build() {
                    Ok(t) => ui.tray = Some(t),
                    Err(e) => eprintln!("tray icon: {e}"),
                }
                ui.refresh();
            }
            Event::UserEvent(UiEvent::Changed) => ui.refresh(),
            Event::UserEvent(UiEvent::Menu(id)) => ui.handle(&id),
            _ => {}
        }
    })
}

struct Ui {
    shared: Arc<Shared>,
    rt: tokio::runtime::Runtime,
    tray: Option<TrayIcon>,
    actions: HashMap<MenuId, Action>,
    /// Whether the icon currently shows the "needs you" variant.
    alert: Option<bool>,
}

impl Ui {
    fn handle(&mut self, id: &MenuId) {
        let Some(action) = self.actions.get(id) else { return };
        let res = match action {
            Action::Focus(sid) => {
                let (shared, sid) = (self.shared.clone(), sid.clone());
                self.rt.spawn(async move { shared.focus_id(sid).await });
                Ok(())
            }
            Action::ToggleHooks if install::hooks_installed() => install::uninstall_hooks(),
            Action::ToggleHooks => install::install_hooks(),
            Action::ToggleLogin => install::set_login_item(!install::login_item_enabled()),
            Action::OpenLog => {
                std::process::Command::new("open").arg(log_path()).status().map(|_| ()).map_err(Into::into)
            }
            Action::Quit => {
                daemon::cleanup();
                std::process::exit(0);
            }
        };
        if let Err(e) = res {
            eprintln!("menu action: {e:#}");
        }
        // Check marks reflect what's on disk, so rebuild after every action.
        self.refresh();
    }

    fn refresh(&mut self) {
        let Some(tray) = &self.tray else { return };
        let sessions = self.shared.sessions();
        let pending = sessions.iter().filter(|s| s.state == State::Pending).count();

        self.actions.clear();
        let menu = Menu::new();
        let status = match self.shared.knob_port() {
            Some(port) => format!("Knob connected ({})", port.trim_start_matches("/dev/")),
            None => "Knob not connected".to_string(),
        };
        let _ = menu.append(&MenuItem::new(status, false, None));
        let _ = menu.append(&PredefinedMenuItem::separator());

        if sessions.is_empty() {
            let _ = menu.append(&MenuItem::new("No Claude sessions", false, None));
        }
        for s in &sessions {
            let (glyph, label) = match s.state {
                State::Idle => ("○", "Idle"),
                State::Running => ("●", "Running"),
                State::Pending => ("⚠︎", "Needs you"),
                State::Done => ("✓", "Done"),
            };
            let item = MenuItem::new(format!("{glyph}  {}  —  {label}", s.name), s.tty.is_some(), None);
            self.actions.insert(item.id().clone(), Action::Focus(s.id.clone()));
            let _ = menu.append(&item);
        }
        let _ = menu.append(&PredefinedMenuItem::separator());

        let hooks = CheckMenuItem::new("Claude Code hooks installed", true, install::hooks_installed(), None);
        self.actions.insert(hooks.id().clone(), Action::ToggleHooks);
        let login = CheckMenuItem::new("Start at login", true, install::login_item_enabled(), None);
        self.actions.insert(login.id().clone(), Action::ToggleLogin);
        let log = MenuItem::new("Open log", log_path().exists(), None);
        self.actions.insert(log.id().clone(), Action::OpenLog);
        let quit = MenuItem::new("Quit Claude Knob", true, None);
        self.actions.insert(quit.id().clone(), Action::Quit);
        let _ = menu.append_items(&[&hooks, &login, &log, &PredefinedMenuItem::separator(), &quit]);

        tray.set_menu(Some(Box::new(menu)));
        let _ = tray.set_tooltip(Some(format!("Claude Knob: {} session(s)", sessions.len())));

        let alert = pending > 0;
        if self.alert != Some(alert) {
            self.alert = Some(alert);
            let res = if alert {
                tray.set_icon(Some(knob_icon(Some([0xF5, 0x9E, 0x0B]))))
            } else {
                tray.set_icon_templated(Some(knob_icon(None)))
            };
            if let Err(e) = res {
                eprintln!("tray icon: {e}");
            }
        }
        tray.set_title(if alert { Some(pending.to_string()) } else { None });
    }
}

fn log_path() -> std::path::PathBuf {
    state_dir().join("knobd.log")
}

/// Draws the menu-bar icon: a ring with a pointer, like a knob seen from above.
/// `None` gives a black template image that macOS tints for light/dark menu bars.
fn knob_icon(color: Option<[u8; 3]>) -> Icon {
    const N: usize = 36; // 18pt at 2x
    const SS: usize = 4; // supersampling per axis
    let c = N as f32 / 2.0;
    let inside = |x: f32, y: f32| {
        let d = ((x - c).powi(2) + (y - c).powi(2)).sqrt();
        let ring = (12.5..=16.5).contains(&d);
        // Pointer: a capsule from the center up toward 12 o'clock.
        let (top, bottom, half_w) = (7.0, c, 1.9);
        let dy = if y < top {
            top - y
        } else if y > bottom {
            y - bottom
        } else {
            0.0
        };
        let pointer = (x - c).powi(2) + dy.powi(2) <= half_w * half_w;
        ring || pointer
    };
    let [r, g, b] = color.unwrap_or([0, 0, 0]);
    let mut rgba = Vec::with_capacity(N * N * 4);
    for py in 0..N {
        for px in 0..N {
            let mut hits = 0;
            for sy in 0..SS {
                for sx in 0..SS {
                    let x = px as f32 + (sx as f32 + 0.5) / SS as f32;
                    let y = py as f32 + (sy as f32 + 0.5) / SS as f32;
                    hits += inside(x, y) as usize;
                }
            }
            rgba.extend_from_slice(&[r, g, b, (hits * 255 / (SS * SS)) as u8]);
        }
    }
    Icon::from_rgba(rgba, N as u32, N as u32).expect("valid icon")
}
