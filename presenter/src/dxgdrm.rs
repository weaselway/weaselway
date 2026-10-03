//! The presenter side of dxgdrm's uapi (dxgdrm_drm.h in the dxgdrm repo).
//! Keep the structs in step with that header.

use std::fs::{File, OpenOptions};
use std::io;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::os::unix::fs::OpenOptionsExt;

pub const MAX_DAMAGE_RECTS: usize = 16;

pub const FRAME_PRIMARY: u32 = 1 << 0;
pub const FRAME_SHARED: u32 = 1 << 1;
pub const FRAME_DAMAGE_FULL: u32 = 1 << 3;
pub const FRAME_CURSOR: u32 = 1 << 4;

#[derive(Clone, Copy, Debug)]
pub enum Plane {
    Primary = 0,
    Cursor = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Rect {
    pub x1: i32,
    pub y1: i32,
    pub x2: i32,
    pub y2: i32,
}

#[repr(C)]
#[derive(Default)]
struct GetFrame {
    seq: u64,
    timeout_ms: u32,
    flags: u32,
    buffer_id: u64,
    primary_seq: u64,
    cursor_seq: u64,
    width: u32,
    height: u32,
    format: u32,
    pitch: u32,
    fd: i32,
    num_damage: u32,
    damage: [Rect; MAX_DAMAGE_RECTS],
    cursor_x: i32,
    cursor_y: i32,
    cursor_width: u32,
    cursor_height: u32,
}

#[repr(C)]
#[derive(Default)]
struct ReadPixels {
    plane: u32,
    size: u32,
    data: u64,
    width: u32,
    height: u32,
    format: u32,
    pitch: u32,
}

/// _IOWR('d', DRM_COMMAND_BASE + nr, T)
const fn drm_iowr<T>(nr: u64) -> u64 {
    const DRM_COMMAND_BASE: u64 = 0x40;
    (3 << 30) | ((std::mem::size_of::<T>() as u64) << 16) | ((b'd' as u64) << 8) | (DRM_COMMAND_BASE + nr)
}

// What the C header's structs come to; a mismatch means they drifted apart.
const _: () = assert!(std::mem::size_of::<GetFrame>() == 336);
const _: () = assert!(std::mem::size_of::<ReadPixels>() == 32);

const IOCTL_GET_FRAME: u64 = drm_iowr::<GetFrame>(0x01);
const IOCTL_READ_PIXELS: u64 = drm_iowr::<ReadPixels>(0x02);

/// One commit, as the kernel reports it.
pub struct Frame {
    pub seq: u64,
    pub flags: u32,
    pub buffer_id: u64,
    pub primary_seq: u64,
    pub cursor_seq: u64,
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub pitch: u32,
    /// The D3D12 shared handle behind the primary plane, if it is one.
    /// Closed when the frame is dropped.
    pub shared: Option<OwnedFd>,
    pub damage: Vec<Rect>,
    pub cursor_x: i32,
    pub cursor_y: i32,
    pub cursor_width: u32,
    pub cursor_height: u32,
}

impl Frame {
    pub fn has(&self, flag: u32) -> bool {
        self.flags & flag != 0
    }
}

/// Pixels of a dumb buffer, copied out of the kernel.
pub struct Pixels {
    pub width: u32,
    pub height: u32,
    pub pitch: u32,
    pub data: Vec<u8>,
}

pub struct Device {
    file: File,
}

impl Device {
    /// The dxgdrm render node, matched on the driver name as Mesa does.
    pub fn open() -> io::Result<Self> {
        for minor in 128..144 {
            let driver = std::fs::read_link(format!("/sys/class/drm/renderD{minor}/device/driver"));
            let is_dxgdrm = driver
                .ok()
                .and_then(|path| path.file_name().map(|name| name == "dxgdrm"))
                .unwrap_or(false);
            if !is_dxgdrm {
                continue;
            }

            let path = format!("/dev/dri/renderD{minor}");
            let file = OpenOptions::new()
                .read(true)
                .write(true)
                .custom_flags(libc::O_CLOEXEC)
                .open(&path)?;
            eprintln!("presenter: using {path}");
            return Ok(Self { file });
        }

        Err(io::Error::new(
            io::ErrorKind::NotFound,
            "no dxgdrm render node -- is the module loaded?",
        ))
    }

    pub fn as_raw_fd(&self) -> i32 {
        self.file.as_raw_fd()
    }

    /// Wait for a commit newer than `seen`. `Ok(None)` on timeout or signal.
    pub fn get_frame(&self, seen: u64, timeout_ms: u32) -> io::Result<Option<Frame>> {
        let mut args = GetFrame {
            seq: seen,
            timeout_ms,
            fd: -1,
            ..Default::default()
        };

        // SAFETY: `args` is the struct this ioctl number was built from, and
        // it outlives the call.
        let ret = unsafe { libc::ioctl(self.file.as_raw_fd(), IOCTL_GET_FRAME as _, &mut args) };
        if ret != 0 {
            let error = io::Error::last_os_error();
            return match error.raw_os_error() {
                Some(libc::ETIME) | Some(libc::EINTR) => Ok(None),
                _ => Err(error),
            };
        }

        // SAFETY: on success the kernel installed a new fd for us, or left -1.
        let shared = (args.fd >= 0).then(|| unsafe { OwnedFd::from_raw_fd(args.fd) });
        let num_damage = (args.num_damage as usize).min(MAX_DAMAGE_RECTS);

        Ok(Some(Frame {
            seq: args.seq,
            flags: args.flags,
            buffer_id: args.buffer_id,
            primary_seq: args.primary_seq,
            cursor_seq: args.cursor_seq,
            width: args.width,
            height: args.height,
            format: args.format,
            pitch: args.pitch,
            shared,
            damage: args.damage[..num_damage].to_vec(),
            cursor_x: args.cursor_x,
            cursor_y: args.cursor_y,
            cursor_width: args.cursor_width,
            cursor_height: args.cursor_height,
        }))
    }

    /// Copy a plane's dumb buffer out. `capacity` is what the caller expects
    /// it to need (pitch * height); a larger buffer fails with ENOSPC.
    pub fn read_pixels(&self, plane: Plane, capacity: usize) -> io::Result<Pixels> {
        let mut data = vec![0u8; capacity];
        let mut args = ReadPixels {
            plane: plane as u32,
            size: u32::try_from(capacity).map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?,
            data: data.as_mut_ptr() as u64,
            ..Default::default()
        };

        // SAFETY: `args.data` points at `args.size` writable bytes, which is
        // all the kernel writes; both outlive the call.
        let ret = unsafe { libc::ioctl(self.file.as_raw_fd(), IOCTL_READ_PIXELS as _, &mut args) };
        if ret != 0 {
            return Err(io::Error::last_os_error());
        }

        // Callers walk the data in rows of `pitch`.
        if args.pitch < args.width.saturating_mul(4) || args.pitch == 0 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "dumb buffer with a short pitch"));
        }
        data.truncate(args.pitch as usize * args.height as usize);
        Ok(Pixels {
            width: args.width,
            height: args.height,
            pitch: args.pitch,
            data,
        })
    }
}
