//! Reading a scanout buffer back on the GPU.
//!
//! The frame is a D3D12 shared handle. Mesa's d3d12 driver imports one as a
//! dma-buf (OpenSharedHandle underneath), and glReadPixels on it is the same
//! CopyTextureRegion + map a presenter talking to D3D12 directly would do --
//! from a second process, on a device of its own, which is the point.
//!
//! Everything GL or EGL hands out is owned by a struct here and released in
//! its Drop.

use std::ffi::c_void;
use std::os::fd::AsRawFd;
use std::rc::Rc;

use glow::HasContext;
use khronos_egl as egl;

use crate::dxgdrm::{Frame, Rect};

const EGL_PLATFORM_GBM_KHR: egl::Enum = 0x31D7;
const EGL_LINUX_DMA_BUF_EXT: egl::Enum = 0x3270;
const EGL_LINUX_DRM_FOURCC_EXT: egl::Attrib = 0x3271;
const EGL_DMA_BUF_PLANE0_FD_EXT: egl::Attrib = 0x3272;
const EGL_DMA_BUF_PLANE0_OFFSET_EXT: egl::Attrib = 0x3273;
const EGL_DMA_BUF_PLANE0_PITCH_EXT: egl::Attrib = 0x3274;

/// A compositor flips between a handful of buffers.
const MAX_IMPORTS: usize = 8;

type Egl = egl::Instance<egl::Static>;
type Error = Box<dyn std::error::Error>;

extern "C" {
    fn gbm_create_device(fd: i32) -> *mut c_void;
    fn gbm_device_destroy(device: *mut c_void);
}

struct GbmDevice(*mut c_void);

impl Drop for GbmDevice {
    fn drop(&mut self) {
        // SAFETY: created by gbm_create_device() and destroyed once, here.
        unsafe { gbm_device_destroy(self.0) };
    }
}

/// The EGL display and context. Shared by the imports, which need both to
/// release what they hold.
struct Context {
    egl: Egl,
    display: egl::Display,
    context: egl::Context,
    gl: glow::Context,
    /// glEGLImageTargetTexture2DOES, which libglvnd does not export.
    image_target_texture: unsafe extern "system" fn(u32, *mut c_void),
    // After everything above: the display lives on this device.
    _gbm: GbmDevice,
}

impl Drop for Context {
    fn drop(&mut self) {
        let _ = self.egl.make_current(self.display, None, None, None);
        let _ = self.egl.destroy_context(self.display, self.context);
        let _ = self.egl.terminate(self.display);
    }
}

/// One imported scanout buffer: EGL image, the texture bound to it, and a
/// framebuffer to read it through.
struct Import {
    context: Rc<Context>,
    buffer_id: u64,
    last_used: u64,
    image: egl::Image,
    texture: glow::Texture,
    framebuffer: glow::Framebuffer,
}

impl Drop for Import {
    fn drop(&mut self) {
        // SAFETY: the context is current on this thread for as long as it
        // exists, and these names were created on it.
        unsafe {
            self.context.gl.delete_framebuffer(self.framebuffer);
            self.context.gl.delete_texture(self.texture);
        }
        let _ = self.context.egl.destroy_image(self.context.display, self.image);
    }
}

pub struct Gpu {
    // Before `context`, so the imports go first.
    imports: Vec<Import>,
    context: Rc<Context>,
    use_counter: u64,
}

impl Gpu {
    /// `drm_fd` is the dxgdrm node; it has to stay open as long as this does.
    pub fn new(drm_fd: i32) -> Result<Self, Error> {
        let egl = egl::Instance::new(egl::Static);

        // SAFETY: plain FFI call; a null return is handled.
        let gbm = unsafe { gbm_create_device(drm_fd) };
        if gbm.is_null() {
            return Err("gbm_create_device failed".into());
        }
        let gbm = GbmDevice(gbm);

        // SAFETY: `gbm.0` is a valid gbm_device, which is what this platform
        // takes as its native display, and it outlives the display.
        let display = unsafe { egl.get_platform_display(EGL_PLATFORM_GBM_KHR, gbm.0, &[egl::ATTRIB_NONE]) }?;
        let (major, minor) = egl.initialize(display)?;

        let extensions = egl.query_string(Some(display), egl::EXTENSIONS)?.to_string_lossy();
        for needed in [
            "EGL_EXT_image_dma_buf_import",
            "EGL_KHR_surfaceless_context",
            "EGL_KHR_no_config_context",
        ] {
            if !extensions.contains(needed) {
                let _ = egl.terminate(display);
                return Err(format!("EGL lacks {needed}").into());
            }
        }

        egl.bind_api(egl::OPENGL_ES_API)?;
        // SAFETY: EGL_NO_CONFIG_KHR is a null config, allowed by
        // EGL_KHR_no_config_context, checked above.
        let no_config = unsafe { egl::Config::from_ptr(std::ptr::null_mut()) };
        let context = egl.create_context(
            display,
            no_config,
            None,
            &[egl::CONTEXT_MAJOR_VERSION, 3, egl::NONE],
        )?;
        egl.make_current(display, None, None, Some(context))?;

        // SAFETY: the context is current, and eglGetProcAddress is the loader
        // for it.
        let gl = unsafe {
            glow::Context::from_loader_function(|name| {
                egl.get_proc_address(name)
                    .map_or(std::ptr::null(), |function| function as *const c_void)
            })
        };

        let image_target_texture = egl
            .get_proc_address("glEGLImageTargetTexture2DOES")
            .filter(|_| gl.supported_extensions().contains("GL_OES_EGL_image"))
            .ok_or("GL lacks GL_OES_EGL_image")?;
        // SAFETY: that is this entry point's signature.
        let image_target_texture: unsafe extern "system" fn(u32, *mut c_void) =
            unsafe { std::mem::transmute(image_target_texture) };

        // The spike is only worth anything on the GPU: llvmpipe cannot open a
        // D3D12 shared handle.
        // SAFETY: the context is current.
        let renderer = unsafe { gl.get_parameter_string(glow::RENDERER) };
        eprintln!("presenter: EGL {major}.{minor}, renderer: {renderer}");

        Ok(Self {
            imports: Vec::new(),
            context: Rc::new(Context {
                egl,
                display,
                context,
                gl,
                image_target_texture,
                _gbm: gbm,
            }),
            use_counter: 0,
        })
    }

    /// Import the frame's shared handle, or find the import made for it
    /// earlier. Returns its index.
    fn import(&mut self, frame: &Frame) -> Result<usize, Error> {
        self.use_counter += 1;

        if let Some(index) = self.imports.iter().position(|i| i.buffer_id == frame.buffer_id) {
            self.imports[index].last_used = self.use_counter;
            return Ok(index);
        }

        if self.imports.len() == MAX_IMPORTS {
            let oldest = self
                .imports
                .iter()
                .enumerate()
                .min_by_key(|(_, import)| import.last_used)
                .map(|(index, _)| index)
                .expect("the import cache is full, so not empty");
            self.imports.swap_remove(oldest);
        }

        let shared = frame.shared.as_ref().ok_or("frame carries no shared handle")?;
        let context = &self.context;
        let attribs = [
            egl::WIDTH as egl::Attrib,
            frame.width as egl::Attrib,
            egl::HEIGHT as egl::Attrib,
            frame.height as egl::Attrib,
            EGL_LINUX_DRM_FOURCC_EXT,
            frame.format as egl::Attrib,
            EGL_DMA_BUF_PLANE0_FD_EXT,
            shared.as_raw_fd() as egl::Attrib,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            frame.pitch as egl::Attrib,
            egl::ATTRIB_NONE,
        ];

        // SAFETY: EGL_NO_CONTEXT and a null client buffer are what the dma-buf
        // target takes; the buffer is named by the attributes.
        let (no_context, no_buffer) = unsafe {
            (
                egl::Context::from_ptr(std::ptr::null_mut()),
                egl::ClientBuffer::from_ptr(std::ptr::null_mut()),
            )
        };
        let image = context
            .egl
            .create_image(context.display, no_context, EGL_LINUX_DMA_BUF_EXT, no_buffer, &attribs)
            .map_err(|error| format!("importing buffer {} failed: {error}", frame.buffer_id))?;

        // SAFETY: the context is current. The Import built below owns the
        // three objects from here on, including on the error path.
        let import = unsafe {
            let gl = &context.gl;
            let texture = gl.create_texture()?;
            gl.bind_texture(glow::TEXTURE_2D, Some(texture));
            (context.image_target_texture)(glow::TEXTURE_2D, image.as_ptr());

            let framebuffer = gl.create_framebuffer()?;
            let import = Import {
                context: Rc::clone(context),
                buffer_id: frame.buffer_id,
                last_used: self.use_counter,
                image,
                texture,
                framebuffer,
            };

            gl.bind_framebuffer(glow::READ_FRAMEBUFFER, Some(framebuffer));
            gl.framebuffer_texture_2d(
                glow::READ_FRAMEBUFFER,
                glow::COLOR_ATTACHMENT0,
                glow::TEXTURE_2D,
                Some(texture),
                0,
            );
            if gl.check_framebuffer_status(glow::READ_FRAMEBUFFER) != glow::FRAMEBUFFER_COMPLETE {
                return Err(format!("buffer {} is not readable as a framebuffer", frame.buffer_id).into());
            }
            import
        };

        let fourcc = frame.format.to_le_bytes();
        eprintln!(
            "presenter: imported buffer {} ({}x{}, {})",
            frame.buffer_id,
            frame.width,
            frame.height,
            String::from_utf8_lossy(&fourcc)
        );

        self.imports.push(import);
        Ok(self.imports.len() - 1)
    }

    /// Read `rects` of the frame into `shadow` (RGBX, `frame.width` pixels per
    /// row, top row first). Returns the number of pixels read.
    pub fn read_back(&mut self, frame: &Frame, rects: &[Rect], shadow: &mut [u8]) -> Result<u64, Error> {
        let index = self.import(frame)?;
        let gl = &self.context.gl;
        let (width, height) = (frame.width as i32, frame.height as i32);
        let mut pixels = 0u64;

        // SAFETY: the context is current and the framebuffer is ours.
        unsafe {
            gl.bind_framebuffer(glow::READ_FRAMEBUFFER, Some(self.imports[index].framebuffer));
            gl.pixel_store_i32(glow::PACK_ALIGNMENT, 4);
            // Rows go straight into their place in the shadow copy.
            gl.pixel_store_i32(glow::PACK_ROW_LENGTH, width);
        }

        for rect in rects {
            let (x1, y1) = (rect.x1.clamp(0, width), rect.y1.clamp(0, height));
            let (x2, y2) = (rect.x2.clamp(0, width), rect.y2.clamp(0, height));
            if x2 <= x1 || y2 <= y1 {
                continue;
            }

            // A dma-buf's first row is the top one and GL calls the first row
            // y = 0, so nothing is flipped. The slice starts at the rect's
            // first pixel and ends with the buffer, which covers everything
            // GL writes: (rows - 1) full rows plus one rect width.
            let start = (y1 as usize * width as usize + x1 as usize) * 4;
            let target = shadow.get_mut(start..).ok_or("shadow copy is smaller than the frame")?;
            let needed = ((y2 - y1 - 1) as usize * width as usize + (x2 - x1) as usize) * 4;
            if target.len() < needed {
                return Err("shadow copy is smaller than the frame".into());
            }

            // SAFETY: as above; the destination was just checked to hold the
            // rows GL writes with this PACK_ROW_LENGTH.
            unsafe {
                gl.read_pixels(
                    x1,
                    y1,
                    x2 - x1,
                    y2 - y1,
                    glow::RGBA,
                    glow::UNSIGNED_BYTE,
                    glow::PixelPackData::Slice(Some(target)),
                );
            }
            pixels += (x2 - x1) as u64 * (y2 - y1) as u64;
        }

        // SAFETY: the context is current.
        if unsafe { gl.get_error() } != glow::NO_ERROR {
            return Err("glReadPixels failed".into());
        }

        Ok(pixels)
    }

    /// Forget every import, e.g. after the display changed size.
    pub fn clear_imports(&mut self) {
        self.imports.clear();
    }
}
