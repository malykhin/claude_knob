use std::sync::Arc;
use std::time::Duration;

use tokio::io::BufReader;
use tokio_serial::{SerialPort, SerialPortBuilderExt, SerialPortType};

use crate::daemon::{run_link, Shared};

/// Espressif's USB vendor id (native USB CDC on the ESP32-S3).
const ESPRESSIF_VID: u16 = 0x303a;
const RETRY: Duration = Duration::from_secs(2);

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

/// Keeps a connection to the knob alive, reconnecting after unplugs.
pub async fn run(shared: Arc<Shared>, port: Option<String>) {
    let mut announced_missing = false;
    loop {
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
                    shared.set_knob_port(Some(path));
                    let (r, w) = tokio::io::split(s);
                    run_link(shared.clone(), BufReader::new(r), w).await;
                    shared.set_knob_port(None);
                    eprintln!("knob disconnected");
                }
                Err(e) => eprintln!("opening {path}: {e}"),
            },
        }
        tokio::time::sleep(RETRY).await;
    }
}
