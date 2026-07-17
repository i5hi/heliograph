//! Control-API DTOs shared by `heliod` (server) and `helio-cli` (client).
//!
//! One source of truth for the wire types keeps the CLI and daemon in lockstep.
//! All update requests use `Option` fields for PATCH semantics — omitted fields are
//! left unchanged.

use crate::image::ImageSettings;
use serde::{Deserialize, Serialize};

/// High-level daemon state machine, surfaced to clients.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum State {
    /// No account yet — must `register`.
    Unregistered,
    /// Registered, idle (not broadcasting).
    Ready,
    /// Broadcasting.
    OnAir,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RegisterRequest {
    pub invite_code: String,
    pub artist: String,
    /// Backend URL; omit to use the current/default server.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub server: Option<String>,
}

/// Full status snapshot.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Status {
    pub state: State,
    pub artist: String,
    pub channel: String,
    pub episode: u32,
    pub server: String,
    pub channel_id: String,
    /// True while ON AIR.
    pub on_air: bool,
    /// Presence: false = ON DECK, true = AWAY.
    pub away: bool,
    pub uptime_s: u64,
    pub reconnects: u64,
    /// 0..1 current audio level + short-term peak.
    pub level: f32,
    pub peak: f32,
    /// Selected input ("" = system default) and whether it's currently present.
    pub input_device: String,
    pub input_present: bool,
    /// Name of the device actually captured from right now (may differ from the
    /// selection when following the system default).
    pub active_input: String,
}

/// One audio input device the daemon can see.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceInfo {
    pub name: String,
    /// This is the OS default input.
    pub is_default: bool,
    /// This is the device we're currently capturing from.
    pub is_current: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DevicesResponse {
    pub devices: Vec<DeviceInfo>,
    /// The persisted selection ("" = follow system default).
    pub selected: String,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct SelectDeviceRequest {
    /// Device name, or "" / "default" to follow the system default.
    pub device: String,
}

/// PATCH for channel/track metadata. Omitted = unchanged.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct MetaUpdate {
    pub channel: Option<String>,
    pub title: Option<String>,
    pub note: Option<String>,
    pub date: Option<String>,
    pub episode: Option<u32>,
    pub font: Option<String>,
    pub accent: Option<String>,
    pub btc: Option<String>,
    pub ln: Option<String>,
    pub lq: Option<String>,
}

/// PATCH for the image + its transforms. `path` swaps the image; `clear` removes it.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct ImageUpdate {
    pub path: Option<String>,
    pub clear: Option<bool>,
    pub pan_x: Option<f32>,
    pub pan_y: Option<f32>,
    pub opacity: Option<f32>,
    pub blur: Option<f32>,
    pub scale: Option<f32>,
}

impl ImageUpdate {
    /// Apply this PATCH onto live settings.
    pub fn apply(&self, s: &mut ImageSettings) {
        if let Some(ref p) = self.path {
            s.path = p.clone();
        }
        if self.clear == Some(true) {
            s.path.clear();
        }
        if let Some(v) = self.pan_x {
            s.pan_x = v.clamp(-1.0, 1.0);
        }
        if let Some(v) = self.pan_y {
            s.pan_y = v.clamp(-1.0, 1.0);
        }
        if let Some(v) = self.opacity {
            s.opacity = v.clamp(0.0, 1.0);
        }
        if let Some(v) = self.blur {
            s.blur = v.max(0.0);
        }
        if let Some(v) = self.scale {
            s.scale = v.clamp(0.05, 8.0);
        }
    }
}

/// Generic OK/err envelope for mutating calls.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Ack {
    pub ok: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

impl Ack {
    pub fn ok() -> Self {
        Self { ok: true, error: None }
    }
    pub fn err(msg: impl Into<String>) -> Self {
        Self { ok: false, error: Some(msg.into()) }
    }
}
