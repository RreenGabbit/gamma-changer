//! GPU-side brightness sampling through DXGI Desktop Duplication. The desktop
//! frame is cropped and box-filtered down by the GPU (GenerateMips); only a
//! tiny image is read back. Anything unsupported reports `Unavailable` so the
//! caller can report that capture is unsupported.
//!
//! Readback is asynchronous: `submit` queues the GPU work and returns at once,
//! and `collect` later picks up the finished sample without ever waiting on the
//! GPU, so the game's own rendering is never stalled by us. Frame acquisition
//! uses a zero timeout for the same reason.

use std::time::{Duration, Instant};

use windows::core::Interface;
use windows::Win32::Foundation::{HMODULE, RECT};
use windows::Win32::Graphics::Direct3D::D3D_DRIVER_TYPE_UNKNOWN;
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_UNSPECIFIED,
    DXGI_SAMPLE_DESC,
};
use windows::Win32::Graphics::Dxgi::*;

use super::from_wide;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SubmitResult {
    /// A sample is in flight; poll `collect`.
    Submitted,
    /// Nothing was presented since the last sample of this region.
    Unchanged,
    /// Both staging buffers are still in flight, or no frame is ready yet.
    Busy,
    /// GPU capture is not possible right now.
    Unavailable,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CollectResult {
    /// The image holds the newest finished sample.
    Ready,
    /// Nothing finished yet (or nothing in flight).
    None,
    /// The readback failed; capture was reset.
    Failed,
}

#[derive(Default)]
struct Slot {
    texture: Option<ID3D11Texture2D>,
    pending: bool,
    submitted_at: Option<Instant>,
    sequence: u64,
}

#[derive(Default)]
pub struct DesktopDuplicationSampler {
    device: Option<ID3D11Device>,
    context: Option<ID3D11DeviceContext>,
    duplication: Option<IDXGIOutputDuplication>,
    crop: Option<ID3D11Texture2D>,
    crop_view: Option<ID3D11ShaderResourceView>,
    // Two staging buffers let one sample finish while the next is queued.
    slots: [Slot; 2],
    sequence: u64,
    output_name: String,
    desktop: RECT,
    last_region: RECT,
    texture_width: u32,
    texture_height: u32,
    crop_width: i32,
    crop_height: i32,
    mip: u32,
    staging_width: i32,
    staging_height: i32,
    have_frame: bool,
    retry_after: Option<Instant>,
    pixels: Vec<u8>,
}

fn rects_equal(a: &RECT, b: &RECT) -> bool {
    a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom
}

fn clamp(value: i32, low: i32, high: i32) -> i32 {
    value.clamp(low, high)
}

impl DesktopDuplicationSampler {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn reset(&mut self) {
        for slot in &mut self.slots {
            slot.texture = None;
            slot.pending = false;
        }
        self.crop_view = None;
        self.crop = None;
        self.duplication = None;
        self.context = None;
        self.device = None;
        self.output_name.clear();
        self.crop_width = 0;
        self.crop_height = 0;
        self.have_frame = false;
    }

    pub fn has_pending(&self) -> bool {
        self.slots.iter().any(|slot| slot.pending)
    }

    /// Width, height and tightly packed BGRA bytes of the newest sample.
    pub fn image(&self) -> (i32, i32, &[u8]) {
        (self.staging_width, self.staging_height, &self.pixels)
    }

    fn back_off(&mut self, now: Instant, milliseconds: u64) {
        self.retry_after = Some(now + Duration::from_millis(milliseconds));
    }

    pub fn submit(&mut self, device_name: &str, screen_rect: RECT) -> SubmitResult {
        let now = Instant::now();
        let name_matches =
            self.duplication.is_some() && self.output_name.eq_ignore_ascii_case(device_name);
        if !name_matches {
            self.reset();
            if device_name.is_empty() || self.retry_after.is_some_and(|t| now < t) {
                return SubmitResult::Unavailable;
            }
            if !self.initialize(device_name) {
                self.reset();
                self.back_off(now, 5000);
                return SubmitResult::Unavailable;
            }
        }

        let mut region = RECT {
            left: screen_rect.left - self.desktop.left,
            top: screen_rect.top - self.desktop.top,
            right: screen_rect.right - self.desktop.left,
            bottom: screen_rect.bottom - self.desktop.top,
        };
        region.left = clamp(region.left, 0, self.texture_width as i32);
        region.right = clamp(region.right, 0, self.texture_width as i32);
        region.top = clamp(region.top, 0, self.texture_height as i32);
        region.bottom = clamp(region.bottom, 0, self.texture_height as i32);
        let width = region.right - region.left;
        let height = region.bottom - region.top;
        if width < 160 || height < 120 {
            return SubmitResult::Unavailable;
        }
        if !self.ensure_resources(width, height) {
            self.reset();
            self.back_off(now, 5000);
            return SubmitResult::Unavailable;
        }

        let Some(target) = self.slots.iter().position(|slot| !slot.pending) else {
            return SubmitResult::Busy;
        };

        let (Some(duplication), Some(context)) = (self.duplication.clone(), self.context.clone())
        else {
            return SubmitResult::Unavailable;
        };

        let mut info = DXGI_OUTDUPL_FRAME_INFO::default();
        let mut resource: Option<IDXGIResource> = None;
        // Never block: AcquireNextFrame's timeout is a real wait on the UI thread.
        if let Err(error) = unsafe { duplication.AcquireNextFrame(0, &mut info, &mut resource) } {
            if error.code() == DXGI_ERROR_WAIT_TIMEOUT {
                let same = self.have_frame && rects_equal(&region, &self.last_region);
                // An unchanged region keeps its last measurement; a first or
                // moved region just has to wait for the next presented frame.
                return if same { SubmitResult::Unchanged } else { SubmitResult::Busy };
            }
            // Typically DXGI_ERROR_ACCESS_LOST after a mode change or desktop switch.
            self.reset();
            self.back_off(now, 1000);
            return SubmitResult::Unavailable;
        }

        let copied = (|| -> Option<()> {
            let frame: ID3D11Texture2D = resource.as_ref()?.cast().ok()?;
            let bounds = D3D11_BOX {
                left: region.left as u32,
                top: region.top as u32,
                front: 0,
                right: region.right as u32,
                bottom: region.bottom as u32,
                back: 1,
            };
            unsafe {
                context.CopySubresourceRegion(
                    self.crop.as_ref()?,
                    0,
                    0,
                    0,
                    0,
                    &frame,
                    0,
                    Some(&bounds),
                );
            }
            Some(())
        })()
        .is_some();
        drop(resource);
        unsafe {
            let _ = duplication.ReleaseFrame();
        }
        if !copied {
            self.reset();
            self.back_off(now, 5000);
            return SubmitResult::Unavailable;
        }

        unsafe {
            if let Some(view) = &self.crop_view {
                context.GenerateMips(view);
            }
            if let (Some(staging), Some(crop)) = (&self.slots[target].texture, &self.crop) {
                context.CopySubresourceRegion(staging, 0, 0, 0, 0, crop, self.mip, None);
            }
            context.Flush(); // hand the work to the GPU now; do not wait for it
        }

        self.sequence += 1;
        let slot = &mut self.slots[target];
        slot.pending = true;
        slot.submitted_at = Some(now);
        slot.sequence = self.sequence;
        self.last_region = region;
        self.have_frame = true;
        SubmitResult::Submitted
    }

    /// Never blocks: a sample the GPU has not finished is simply left for later.
    pub fn collect(&mut self) -> CollectResult {
        let Some(context) = self.context.clone() else {
            return CollectResult::None;
        };
        let now = Instant::now();
        let mut ready = false;
        loop {
            let Some(oldest) = self
                .slots
                .iter()
                .enumerate()
                .filter(|(_, slot)| slot.pending)
                .min_by_key(|(_, slot)| slot.sequence)
                .map(|(index, _)| index)
            else {
                break;
            };
            let Some(texture) = self.slots[oldest].texture.clone() else {
                self.slots[oldest].pending = false;
                continue;
            };

            let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
            let mapping = unsafe {
                context.Map(
                    &texture,
                    0,
                    D3D11_MAP_READ,
                    D3D11_MAP_FLAG_DO_NOT_WAIT.0 as u32,
                    Some(&mut mapped),
                )
            };
            if let Err(error) = mapping {
                if error.code() == DXGI_ERROR_WAS_STILL_DRAWING {
                    // Give up on a sample that never completes rather than waiting forever.
                    let stale = self.slots[oldest]
                        .submitted_at
                        .is_some_and(|t| now.duration_since(t) > Duration::from_millis(250));
                    if stale {
                        self.slots[oldest].pending = false;
                        continue;
                    }
                    break;
                }
                self.reset();
                self.back_off(now, 1000);
                return CollectResult::Failed;
            }

            let row_bytes = self.staging_width as usize * 4;
            self.pixels.resize(row_bytes * self.staging_height as usize, 0);
            for row in 0..self.staging_height as usize {
                unsafe {
                    let source = (mapped.pData as *const u8).add(mapped.RowPitch as usize * row);
                    std::ptr::copy_nonoverlapping(
                        source,
                        self.pixels.as_mut_ptr().add(row_bytes * row),
                        row_bytes,
                    );
                }
            }
            unsafe { context.Unmap(&texture, 0) };
            self.slots[oldest].pending = false;
            ready = true; // keep draining so the newest finished sample wins
        }
        if ready {
            CollectResult::Ready
        } else {
            CollectResult::None
        }
    }

    fn initialize(&mut self, device_name: &str) -> bool {
        let Ok(factory) = (unsafe { CreateDXGIFactory1::<IDXGIFactory1>() }) else {
            return false;
        };

        let mut adapter_index = 0;
        while let Ok(adapter) = unsafe { factory.EnumAdapters1(adapter_index) } {
            adapter_index += 1;
            let mut output_index = 0;
            while let Ok(output) = unsafe { adapter.EnumOutputs(output_index) } {
                output_index += 1;
                let Ok(description) = (unsafe { output.GetDesc() }) else {
                    continue;
                };
                if !from_wide(&description.DeviceName).eq_ignore_ascii_case(device_name) {
                    continue;
                }
                // Rotated outputs would need the sample region transformed.
                if description.Rotation != DXGI_MODE_ROTATION_IDENTITY
                    && description.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED
                {
                    return false;
                }

                let Ok(base_adapter) = adapter.cast::<IDXGIAdapter>() else {
                    return false;
                };
                let mut device = None;
                let mut context = None;
                let created = unsafe {
                    D3D11CreateDevice(
                        &base_adapter,
                        D3D_DRIVER_TYPE_UNKNOWN,
                        HMODULE::default(),
                        D3D11_CREATE_DEVICE_FLAG(0),
                        None,
                        D3D11_SDK_VERSION,
                        Some(&mut device),
                        None,
                        Some(&mut context),
                    )
                };
                if created.is_err() {
                    return false;
                }
                let (Some(device), Some(context)) = (device, context) else {
                    return false;
                };
                let Ok(output1) = output.cast::<IDXGIOutput1>() else {
                    return false;
                };
                let Ok(duplication) = (unsafe { output1.DuplicateOutput(&device) }) else {
                    return false;
                };
                let duplication_description = unsafe { duplication.GetDesc() };
                // HDR / wide-gamut desktops use a float format; capture is unsupported there.
                if duplication_description.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM {
                    return false;
                }
                self.desktop = description.DesktopCoordinates;
                self.texture_width = duplication_description.ModeDesc.Width;
                self.texture_height = duplication_description.ModeDesc.Height;
                self.output_name = device_name.to_string();
                self.device = Some(device);
                self.context = Some(context);
                self.duplication = Some(duplication);
                return true;
            }
        }
        false
    }

    fn ensure_resources(&mut self, width: i32, height: i32) -> bool {
        if self.crop.is_some() && width == self.crop_width && height == self.crop_height {
            return true;
        }
        for slot in &mut self.slots {
            slot.texture = None;
            slot.pending = false;
        }
        self.crop_view = None;
        self.crop = None;
        self.have_frame = false;
        let Some(device) = self.device.clone() else {
            return false;
        };

        let crop_description = D3D11_TEXTURE2D_DESC {
            Width: width as u32,
            Height: height as u32,
            MipLevels: 0, // full chain
            ArraySize: 1,
            Format: DXGI_FORMAT_B8G8R8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: (D3D11_BIND_SHADER_RESOURCE.0 | D3D11_BIND_RENDER_TARGET.0) as u32,
            CPUAccessFlags: 0,
            MiscFlags: D3D11_RESOURCE_MISC_GENERATE_MIPS.0 as u32,
        };
        let mut crop = None;
        if unsafe { device.CreateTexture2D(&crop_description, None, Some(&mut crop)) }.is_err() {
            return false;
        }
        let Some(crop) = crop else { return false };
        let mut view = None;
        if unsafe { device.CreateShaderResourceView(&crop, None, Some(&mut view)) }.is_err() {
            return false;
        }

        // Smallest mip that is still at least 80x60: plenty for a median.
        let mut mip = 0u32;
        while (width >> (mip + 1)) >= 80 && (height >> (mip + 1)) >= 60 {
            mip += 1;
        }
        self.mip = mip;
        self.staging_width = width >> mip;
        self.staging_height = height >> mip;

        let staging_description = D3D11_TEXTURE2D_DESC {
            Width: self.staging_width as u32,
            Height: self.staging_height as u32,
            MipLevels: 1,
            ArraySize: 1,
            Format: DXGI_FORMAT_B8G8R8A8_UNORM,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Usage: D3D11_USAGE_STAGING,
            BindFlags: 0,
            CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
            MiscFlags: 0,
        };
        for slot in &mut self.slots {
            let mut texture = None;
            if unsafe { device.CreateTexture2D(&staging_description, None, Some(&mut texture)) }
                .is_err()
            {
                return false;
            }
            slot.texture = texture;
        }
        self.crop = Some(crop);
        self.crop_view = view;
        self.crop_width = width;
        self.crop_height = height;
        true
    }
}
