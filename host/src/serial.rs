use std::sync::Arc;
use std::time::Duration;

use tokio::io::BufReader;
use tokio_serial::{SerialPort, SerialPortBuilderExt, SerialPortType};

use crate::daemon::{run_link, LinkEnd, Shared};

/// Espressif's USB vendor id (native USB CDC on the ESP32-S3).
const ESPRESSIF_VID: u16 = 0x303a;
const RETRY: Duration = Duration::from_secs(2);
/// The firmware sends a keepalive every 2 s.
const SILENT_AFTER: Duration = Duration::from_secs(6);

/// Finds the knob's serial port by USB vendor id.
pub fn find_port() -> Option<String> {
    let ports = tokio_serial::available_ports().ok()?;
    let mut found: Vec<String> = ports
        .into_iter()
        .filter(|p| matches!(&p.port_type, SerialPortType::UsbPort(u) if u.vid == ESPRESSIF_VID))
        .map(|p| p.port_name)
        .collect();
    // On macOS every device shows up as both /dev/tty.* and /dev/cu.*; cu.* doesn't block on carrier detect.
    found.sort_by_key(|p| !p.contains("/cu."));
    found.into_iter().next().map(|p| p.replace("/dev/tty.", "/dev/cu."))
}

/// True when this user's session is the one on screen. With fast user switching
/// every user can have a knobd running, but only one can hold the knob's port, so
/// the knob follows whoever is at the screen. macOS gives `/dev/console` to that user.
#[cfg(target_os = "macos")]
fn on_screen() -> bool {
    use std::os::unix::fs::MetadataExt;
    std::fs::metadata("/dev/console").map_or(true, |m| m.uid() == unsafe { libc::getuid() })
}

#[cfg(not(target_os = "macos"))]
fn on_screen() -> bool {
    true
}

async fn until_off_screen() {
    while on_screen() {
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
}

/// Resets the ESP32 through its USB Serial/JTAG bridge: RTS high with DTR low holds the
/// chip in reset, releasing RTS lets it boot normally (DTR low = not download mode).
fn reset_chip(path: &str) {
    match tokio_serial::new(path, 115_200).open() {
        Ok(mut p) => {
            let _ = p.write_data_terminal_ready(false);
            let _ = p.write_request_to_send(true);
            std::thread::sleep(Duration::from_millis(100));
            let _ = p.write_request_to_send(false);
        }
        Err(e) => eprintln!("resetting knob on {path}: {e}"),
    }
}

/// Keeps a connection to the knob alive, reconnecting after unplugs.
pub async fn run(shared: Arc<Shared>, port: Option<String>) {
    let mut announced_missing = false;
    let mut announced_away = false;
    loop {
        if !on_screen() {
            if !announced_away {
                eprintln!("another user is on screen; leaving the knob to them");
                announced_away = true;
            }
            tokio::time::sleep(RETRY).await;
            continue;
        }
        announced_away = false;
        let path = port.clone().or_else(find_port);
        match path {
            None => {
                if !announced_missing {
                    eprintln!("waiting for knob (no Espressif USB device found)");
                    announced_missing = true;
                }
            }
            Some(path) => match tokio_serial::new(&path, 115_200).open_native_async() {
                Ok(mut s) => {
                    announced_missing = false;
                    let _ = s.write_data_terminal_ready(true);
                    eprintln!("knob port opened: {path}");
                    shared.set_knob_port(Some(path.clone()));
                    let (r, w) = tokio::io::split(s);
                    // Dropping the link closes the port, so the user on screen can open it.
                    let end = tokio::select! {
                        end = run_link(shared.clone(), BufReader::new(r), w, Some(SILENT_AFTER)) => Some(end),
                        _ = until_off_screen() => None,
                    };
                    shared.set_knob_port(None);
                    if end.is_none() {
                        eprintln!("switched to another user; released the knob");
                    } else if end == Some(LinkEnd::Silent) {
                        eprintln!("knob went silent (USB transmit stuck); resetting it");
                        reset_chip(&path);
                    } else {
                        eprintln!("knob disconnected");
                    }
                }
                Err(e) => eprintln!("opening {path}: {e}"),
            },
        }
        tokio::time::sleep(RETRY).await;
    }
}
