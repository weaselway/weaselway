//! Simulated input: an absolute pointer on uinput, moved in a circle. To the
//! compositor it is an ordinary evdev device that libinput picks up, which is
//! how the real presenter will deliver the Windows pointer.

use std::f64::consts::TAU;
use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

use evdev::uinput::VirtualDevice;
use evdev::{AbsInfo, AbsoluteAxisCode, AttributeSet, EventType, InputEvent, KeyCode, UinputAbsSetup};

const ABS_MAX: i32 = 65535;

fn create() -> io::Result<VirtualDevice> {
    // Absolute axes plus mouse buttons and nothing touch-like is what udev
    // and libinput classify as an absolute pointer, like a VM's tablet.
    let mut buttons = AttributeSet::<KeyCode>::new();
    buttons.insert(KeyCode::BTN_LEFT);
    buttons.insert(KeyCode::BTN_RIGHT);
    buttons.insert(KeyCode::BTN_MIDDLE);

    let axis = AbsInfo::new(0, 0, ABS_MAX, 0, 0, 0);

    VirtualDevice::builder()?
        .name("Weaselway spike pointer")
        .with_keys(&buttons)?
        .with_absolute_axis(&UinputAbsSetup::new(AbsoluteAxisCode::ABS_X, axis))?
        .with_absolute_axis(&UinputAbsSetup::new(AbsoluteAxisCode::ABS_Y, axis))?
        .build()
}

/// Start the circling pointer. `None`, with a message, if uinput is not
/// usable; the presenter works without it. The device goes away when the
/// thread ends, which it does once `quit` is set.
pub fn spawn(quit: Arc<AtomicBool>) -> Option<JoinHandle<()>> {
    let mut device = match create() {
        Ok(device) => device,
        Err(error) => {
            eprintln!(
                "presenter: cannot create the uinput pointer ({error}) -- no simulated input. \
                 Is the uinput module loaded and /dev/uinput writable?"
            );
            return None;
        }
    };
    eprintln!("presenter: uinput pointer created, circling");

    Some(thread::spawn(move || {
        let mut angle = 0.0f64;

        while !quit.load(Ordering::Relaxed) {
            // A circle around the centre, a quarter of the height in radius
            // on a 16:9 screen, once every four seconds.
            let x = 0.5 + 0.25 * angle.cos() * 9.0 / 16.0;
            let y = 0.5 + 0.25 * angle.sin();

            let events = [
                InputEvent::new(EventType::ABSOLUTE.0, AbsoluteAxisCode::ABS_X.0, (x * ABS_MAX as f64) as i32),
                InputEvent::new(EventType::ABSOLUTE.0, AbsoluteAxisCode::ABS_Y.0, (y * ABS_MAX as f64) as i32),
            ];
            // emit() appends the SYN_REPORT.
            if device.emit(&events).is_err() {
                break;
            }

            angle = (angle + TAU / (4.0 * 60.0)) % TAU;
            thread::sleep(Duration::from_micros(1_000_000 / 60));
        }
    }))
}
