//! The entire "visual" surface of the appliance: one still image with a handful of
//! transforms, composited over the channel accent colour and encoded to PNG for the
//! periodic snapshot. No GL, no GPU — pure CPU, sized for a Pi Zero.

use image::{imageops, DynamicImage, Rgba, RgbaImage};
use serde::{Deserialize, Serialize};
use std::io::Cursor;

/// Snapshot resolution. 720p is plenty for a still and keeps encode + upload cheap.
pub const SNAP_W: u32 = 1280;
pub const SNAP_H: u32 = 720;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct ImageSettings {
    /// Absolute path to the source image ("" = none → solid accent frame).
    pub path: String,
    /// Horizontal pan, -1.0 (left) .. 1.0 (right).
    pub pan_x: f32,
    /// Vertical pan, -1.0 (up) .. 1.0 (down).
    pub pan_y: f32,
    /// Overall opacity, 0.0 .. 1.0.
    pub opacity: f32,
    /// Gaussian blur sigma in pixels (0 = sharp).
    pub blur: f32,
    /// Scale multiplier applied on top of a cover-fit (1.0 = fill the frame).
    pub scale: f32,
}

impl Default for ImageSettings {
    fn default() -> Self {
        Self {
            path: String::new(),
            pan_x: 0.0,
            pan_y: 0.0,
            opacity: 1.0,
            blur: 0.0,
            scale: 1.0,
        }
    }
}

/// Parse `#rrggbb` (or `rrggbb`) into an opaque RGBA pixel; falls back to black.
fn parse_hex(hex: &str) -> Rgba<u8> {
    let h = hex.trim_start_matches('#');
    if h.len() == 6 {
        if let Ok(v) = u32::from_str_radix(h, 16) {
            return Rgba([(v >> 16) as u8, (v >> 8) as u8, v as u8, 255]);
        }
    }
    Rgba([0, 0, 0, 255])
}

impl ImageSettings {
    /// Render the current settings to a PNG byte buffer over `background_hex`.
    /// Never fails on a bad/missing image — it degrades to the solid background so the
    /// broadcast keeps a valid frame (uptime first).
    pub fn render_png(&self, w: u32, h: u32, background_hex: &str) -> anyhow::Result<Vec<u8>> {
        let mut canvas = RgbaImage::from_pixel(w, h, parse_hex(background_hex));

        if !self.path.is_empty() {
            if let Ok(src) = image::open(&self.path) {
                self.composite(&mut canvas, src.to_rgba8(), w, h);
            }
            // On load failure we intentionally keep just the background frame.
        }

        let mut out = Cursor::new(Vec::new());
        DynamicImage::ImageRgba8(canvas)
            .write_to(&mut out, image::ImageFormat::Png)?;
        Ok(out.into_inner())
    }

    /// Cover-fit → scale → blur → opacity → pan → overlay.
    fn composite(&self, canvas: &mut RgbaImage, mut img: RgbaImage, w: u32, h: u32) {
        let (iw, ih) = (img.width().max(1), img.height().max(1));
        // Cover the frame, then apply the user scale.
        let cover = (w as f32 / iw as f32).max(h as f32 / ih as f32);
        let s = (cover * self.scale.clamp(0.05, 8.0)).max(0.001);
        let (sw, sh) = (((iw as f32 * s) as u32).max(1), ((ih as f32 * s) as u32).max(1));
        img = imageops::resize(&img, sw, sh, imageops::FilterType::Triangle);

        if self.blur > 0.0 {
            img = imageops::blur(&img, self.blur);
        }

        let op = self.opacity.clamp(0.0, 1.0);
        if op < 1.0 {
            for p in img.pixels_mut() {
                p.0[3] = (p.0[3] as f32 * op) as u8;
            }
        }

        // Centre, then pan by up to half a frame in each axis.
        let x = (w as f32 - sw as f32) * 0.5 + self.pan_x.clamp(-1.0, 1.0) * (w as f32 * 0.5);
        let y = (h as f32 - sh as f32) * 0.5 + self.pan_y.clamp(-1.0, 1.0) * (h as f32 * 0.5);
        imageops::overlay(canvas, &img, x as i64, y as i64);
    }
}
