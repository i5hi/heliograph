//! Transmission metadata — the JSON we base64 into the `X-Transmission` header on
//! each snapshot POST. Kept key-compatible with what heliograph emits, so the
//! snapshot-api stores/returns it unchanged and the existing web player renders it.

use serde::{Deserialize, Serialize};

/// Mutable per-broadcast branding + track metadata (persisted in the session).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct ChannelMeta {
    /// Display name of the channel (defaults to the artist name on the server side).
    pub channel: String,
    pub title: String,
    pub note: String,
    /// Free-form date string as shown to listeners, e.g. "2026.07.16".
    pub date: String,
    pub episode: u32,
    /// UI font key + accent colour the player uses for this channel.
    pub font: String,
    pub accent: String,
    /// Optional donation addresses.
    pub btc: String,
    pub ln: String,
    pub lq: String,
}

impl Default for ChannelMeta {
    fn default() -> Self {
        Self {
            channel: String::new(),
            title: String::new(),
            note: String::new(),
            date: String::new(),
            episode: 1,
            font: "aldrich".to_string(),
            accent: "#b0e233".to_string(),
            btc: String::new(),
            ln: String::new(),
            lq: String::new(),
        }
    }
}

/// The full transmission object sent to the server. Built from [`ChannelMeta`] plus
/// live runtime fields (status, level, peak, uptime).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Transmission {
    pub channel: String,
    pub artist: String,
    pub title: String,
    pub note: String,
    pub date: String,
    pub episode: u32,
    pub font: String,
    pub accent: String,
    /// Presence, matching heliograph: "ON DECK" | "AWAY". Listening is implied when live.
    pub status: String,
    /// 0..1 audio level + peak (server feeds the client's meter).
    pub level: f32,
    pub peak: f32,
    /// Seconds on air.
    pub uptime: u64,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub btc: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub ln: String,
    #[serde(skip_serializing_if = "String::is_empty")]
    pub lq: String,
}

impl Transmission {
    pub fn build(
        artist: &str,
        meta: &ChannelMeta,
        away: bool,
        level: f32,
        peak: f32,
        uptime: u64,
    ) -> Self {
        let channel = if meta.channel.is_empty() {
            artist.to_string()
        } else {
            meta.channel.clone()
        };
        Self {
            channel,
            artist: artist.to_string(),
            title: meta.title.clone(),
            note: meta.note.clone(),
            date: meta.date.clone(),
            episode: meta.episode,
            font: meta.font.clone(),
            accent: meta.accent.clone(),
            status: if away { "AWAY" } else { "ON DECK" }.to_string(),
            level,
            peak,
            uptime,
            btc: meta.btc.clone(),
            ln: meta.ln.clone(),
            lq: meta.lq.clone(),
        }
    }
}
