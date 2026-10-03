//! weaselway-presenter: the userspace half of dxgdrm's virtual display.
//!
//! Spike. The compositor scans out to dxgdrm's KMS node; this waits for each
//! commit, reads the frame back and writes it out as a JPEG instead of sending
//! it to Windows. It answers the three open questions:
//!
//!   - can a second process open the compositor's scanout buffer on its own
//!     device and read back only what was damaged, and how long does that
//!     take (the timings it prints);
//!   - does the compositor take input from a uinput device (it moves a
//!     pointer in a circle, which shows up in the frames);
//!   - does any of this work with an unmodified compositor at all.

mod dxgdrm;
mod gpu;
mod pointer;

use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use dxgdrm::{Device, Frame, Pixels, Plane, Rect};

type Error = Box<dyn std::error::Error>;

struct Options {
    out_dir: PathBuf,
    max_frames: u32,
    quality: u8,
    pointer: bool,
}

const USAGE: &str = "\
usage: weaselway-presenter [--out DIR] [--max-frames N] [--quality Q] [--no-pointer]
  --out DIR       where the JPEGs go (default /tmp/weaselway-frames)
  --max-frames N  keep N files, then start over at frame-000000 (default 600)
  --quality Q     JPEG quality, 1-100 (default 85)
  --no-pointer    do not create the circling uinput pointer";

fn parse_args() -> Option<Options> {
    let mut options = Options {
        out_dir: PathBuf::from("/tmp/weaselway-frames"),
        max_frames: 600,
        quality: 85,
        pointer: true,
    };
    let mut args = std::env::args().skip(1);

    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--out" => options.out_dir = PathBuf::from(args.next()?),
            "--max-frames" => options.max_frames = args.next()?.parse().ok().filter(|n| *n > 0)?,
            "--quality" => options.quality = args.next()?.parse().ok().filter(|q| (1..=100).contains(q))?,
            "--no-pointer" => options.pointer = false,
            _ => return None,
        }
    }

    Some(options)
}

/// The screen as last read back, plus the cursor plane's image.
struct Screen {
    width: u32,
    height: u32,
    /// RGBX, top row first.
    shadow: Vec<u8>,
    /// The cursor plane, ARGB8888 as the compositor wrote it.
    cursor: Option<Pixels>,
    cursor_seq: u64,
    compose: Vec<u8>,
}

impl Screen {
    fn new(width: u32, height: u32) -> Self {
        let size = width as usize * height as usize * 4;
        Self {
            width,
            height,
            shadow: vec![0; size],
            cursor: None,
            cursor_seq: 0,
            compose: vec![0; size],
        }
    }

    /// A dumb-buffer frame (a compositor rendering without the GPU): all of
    /// it, converted from XRGB8888, which is B, G, R, X in memory.
    fn fill_from_dumb(&mut self, pixels: &Pixels) -> Result<u64, Error> {
        if pixels.width != self.width || pixels.height != self.height {
            return Err("the dumb buffer changed size under us".into());
        }

        let row_bytes = self.width as usize * 4;
        for (dst, src) in self
            .shadow
            .chunks_exact_mut(row_bytes)
            .zip(pixels.data.chunks(pixels.pitch as usize))
        {
            for (dst, src) in dst.chunks_exact_mut(4).zip(src.chunks_exact(4)) {
                dst.copy_from_slice(&[src[2], src[1], src[0], 0xff]);
            }
        }

        Ok(self.width as u64 * self.height as u64)
    }

    /// Shadow copy plus the cursor plane, which is what the client would draw.
    fn compose(&mut self, frame: &Frame) -> &[u8] {
        self.compose.copy_from_slice(&self.shadow);

        let Some(cursor) = self.cursor.as_ref().filter(|_| frame.has(dxgdrm::FRAME_CURSOR)) else {
            return &self.compose;
        };

        for (cy, row) in cursor.data.chunks(cursor.pitch as usize).enumerate() {
            let y = frame.cursor_y as i64 + cy as i64;
            if y < 0 || y >= self.height as i64 {
                continue;
            }

            for (cx, src) in row.chunks_exact(4).take(cursor.width as usize).enumerate() {
                let x = frame.cursor_x as i64 + cx as i64;
                if x < 0 || x >= self.width as i64 || src[3] == 0 {
                    continue;
                }

                // Premultiplied alpha.
                let offset = (y as usize * self.width as usize + x as usize) * 4;
                let inverse = 255 - src[3] as u32;
                let dst = &mut self.compose[offset..offset + 3];
                for (dst, src) in dst.iter_mut().zip([src[2], src[1], src[0]]) {
                    *dst = (src as u32 + *dst as u32 * inverse / 255).min(255) as u8;
                }
            }
        }

        &self.compose
    }
}

fn write_jpeg(dir: &Path, index: u32, quality: u8, rgbx: &[u8], width: u32, height: u32) -> Result<(), Error> {
    let path = dir.join(format!("frame-{index:06}.jpg"));
    let tmp = path.with_extension("jpg.tmp");
    let (width, height) = (u16::try_from(width)?, u16::try_from(height)?);

    jpeg_encoder::Encoder::new_file(&tmp, quality)?.encode(rgbx, width, height, jpeg_encoder::ColorType::Rgba)?;
    // So a viewer following the directory never sees half a file.
    std::fs::rename(&tmp, &path)?;
    Ok(())
}

static QUIT: AtomicBool = AtomicBool::new(false);

extern "C" fn on_signal(_signal: libc::c_int) {
    QUIT.store(true, Ordering::Relaxed);
}

fn install_signal_handlers() {
    // SAFETY: the handler only stores to an atomic. No SA_RESTART, so a
    // signal gets us out of DXGDRM_GET_FRAME.
    unsafe {
        let mut action: libc::sigaction = std::mem::zeroed();
        action.sa_sigaction = on_signal as extern "C" fn(libc::c_int) as usize;
        libc::sigaction(libc::SIGINT, &action, std::ptr::null_mut());
        libc::sigaction(libc::SIGTERM, &action, std::ptr::null_mut());
    }
}

fn run(options: &Options) -> Result<u32, Error> {
    const FRAME_INTERVAL: Duration = Duration::from_micros(1_000_000 / 60);

    std::fs::create_dir_all(&options.out_dir)?;

    let device = Device::open()?;
    let mut gpu = gpu::Gpu::new(device.as_raw_fd())?;

    let mut screen: Option<Screen> = None;
    let mut have_frame = false;
    let (mut seq, mut primary_seq) = (0u64, 0u64);
    let mut frames = 0u32;
    let mut last_frame: Option<Instant> = None;

    eprintln!("presenter: waiting for the compositor's first commit");

    while !QUIT.load(Ordering::Relaxed) {
        // The cursor moving is a commit too. One frame per display refresh is
        // plenty; damage keeps accumulating in the kernel meanwhile.
        if let Some(wait) = last_frame.and_then(|at| FRAME_INTERVAL.checked_sub(at.elapsed())) {
            thread::sleep(wait);
        }

        let Some(frame) = device.get_frame(seq, 1000)? else {
            continue;
        };
        seq = frame.seq;
        let started = Instant::now();
        last_frame = Some(started);

        if !frame.has(dxgdrm::FRAME_PRIMARY) {
            if have_frame {
                eprintln!("presenter: the compositor turned the display off");
            }
            have_frame = false;
            continue;
        }

        if screen.as_ref().map(|s| (s.width, s.height)) != Some((frame.width, frame.height)) {
            screen = Some(Screen::new(frame.width, frame.height));
            gpu.clear_imports();
            have_frame = false;
        }
        let screen = screen.as_mut().expect("just created");

        let mut pixels = 0u64;
        if frame.primary_seq != primary_seq || !have_frame {
            // The shadow copy starts out empty, whatever the damage says.
            let full = [Rect { x1: 0, y1: 0, x2: frame.width as i32, y2: frame.height as i32 }];
            let rects = if frame.has(dxgdrm::FRAME_DAMAGE_FULL) || !have_frame {
                &full[..]
            } else {
                &frame.damage[..]
            };

            let read = if frame.has(dxgdrm::FRAME_SHARED) {
                gpu.read_back(&frame, rects, &mut screen.shadow)
            } else {
                device
                    .read_pixels(Plane::Primary, frame.pitch as usize * frame.height as usize)
                    .map_err(Error::from)
                    .and_then(|dumb| screen.fill_from_dumb(&dumb))
            };

            match read {
                Ok(count) => pixels = count,
                Err(error) => {
                    // Ask for everything again next time round.
                    eprintln!("presenter: reading the frame back failed: {error}");
                    have_frame = false;
                    continue;
                }
            }
        }
        primary_seq = frame.primary_seq;
        have_frame = true;
        let read_done = Instant::now();

        if frame.has(dxgdrm::FRAME_CURSOR) && (frame.cursor_seq != screen.cursor_seq || screen.cursor.is_none()) {
            let size = frame.cursor_width as usize * frame.cursor_height as usize * 4;
            screen.cursor = device
                .read_pixels(Plane::Cursor, size)
                .map_err(|error| eprintln!("presenter: reading the cursor failed: {error}"))
                .ok();
            screen.cursor_seq = frame.cursor_seq;
        }

        let (width, height) = (screen.width, screen.height);
        write_jpeg(
            &options.out_dir,
            frames % options.max_frames,
            options.quality,
            screen.compose(&frame),
            width,
            height,
        )?;
        frames += 1;

        eprintln!(
            "frame {frames}: {} {width}x{height} buffer {}, {} rect(s){}, {:.1}% read back in {:.2} ms, \
             jpeg {:.2} ms, cursor {} at {},{}",
            if frame.has(dxgdrm::FRAME_SHARED) { "d3d12" } else { "dumb" },
            frame.buffer_id,
            frame.damage.len(),
            if frame.has(dxgdrm::FRAME_DAMAGE_FULL) { " (full)" } else { "" },
            100.0 * pixels as f64 / (width as f64 * height as f64),
            (read_done - started).as_secs_f64() * 1000.0,
            read_done.elapsed().as_secs_f64() * 1000.0,
            if frame.has(dxgdrm::FRAME_CURSOR) { "plane" } else { "none" },
            frame.cursor_x,
            frame.cursor_y,
        );
    }

    Ok(frames)
}

fn main() -> ExitCode {
    let Some(options) = parse_args() else {
        eprintln!("{USAGE}");
        return ExitCode::from(2);
    };

    install_signal_handlers();

    let pointer_quit = Arc::new(AtomicBool::new(false));
    let pointer = options.pointer.then(|| pointer::spawn(Arc::clone(&pointer_quit))).flatten();

    let result = run(&options);

    pointer_quit.store(true, Ordering::Relaxed);
    if let Some(pointer) = pointer {
        let _ = pointer.join();
    }

    match result {
        Ok(frames) => {
            eprintln!("presenter: wrote {frames} frame(s) to {}", options.out_dir.display());
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("presenter: {error}");
            ExitCode::FAILURE
        }
    }
}
