//! Persisted device state — `session.json`.
//!
//! Field names in [`Registration`] and [`Broadcast`] are kept compatible with the
//! existing heliograph/`radio.stackmate.org` protocol so the backend needs no changes:
//! `/register` returns the icecast block and channelId, exactly as consumed here.

use crate::image::ImageSettings;
use crate::transmission::ChannelMeta;
use serde::{Deserialize, Serialize};
use std::path::Path;

/// Default backend if the user never sets one.
pub const DEFAULT_SERVER: &str = "https://radio.stackmate.org";

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Session {
    /// Artist identity — entered ONCE at registration, read-only thereafter.
    pub artist: String,
    /// Selected audio input device NAME. Empty = follow the system default input
    /// (recommended on the Pi, where aux-in is the default). The daemon never
    /// hard-codes hardware — it resolves this name against whatever is present.
    pub input_device: String,
    /// Server-assigned registration facts.
    pub registration: Registration,
    /// Icecast source credentials handed back by `/register`.
    pub broadcast: Broadcast,
    /// Mutable per-broadcast channel branding + metadata.
    pub channel: ChannelMeta,
    /// The single image and its transforms (the entire "visual" surface).
    pub image: ImageSettings,
    /// Presence, persisted so it survives restarts: false = ON DECK, true = AWAY.
    pub away: bool,
    /// Broadcast intent, persisted so a reboot auto-resumes ON AIR.
    pub on_air: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Registration {
    pub server: String,
    #[serde(rename = "channelId")]
    pub channel_id: String,
    pub registered: bool,
}

impl Default for Registration {
    fn default() -> Self {
        Self {
            server: DEFAULT_SERVER.to_string(),
            channel_id: String::new(),
            registered: false,
        }
    }
}

/// Icecast source connection + snapshot upload target. Mirrors the `/register` reply.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Broadcast {
    #[serde(rename = "iceHost")]
    pub ice_host: String,
    #[serde(rename = "icePort")]
    pub ice_port: u16,
    /// Mount is the channelId; we append `.opus` for the Ogg/Opus source (see heliod).
    #[serde(rename = "iceMount")]
    pub ice_mount: String,
    #[serde(rename = "icePassword")]
    pub ice_password: String,
    #[serde(rename = "snapshotUrl")]
    pub snapshot_url: String,
    #[serde(rename = "snapshotToken")]
    pub snapshot_token: String,
}

impl Session {
    /// Load from `session.json`, or return defaults if it doesn't exist yet.
    pub fn load(path: &Path) -> anyhow::Result<Self> {
        match std::fs::read(path) {
            Ok(bytes) => Ok(serde_json::from_slice(&bytes)?),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(e) => Err(e.into()),
        }
    }

    /// Persist atomically (write temp + rename) so a crash never truncates state.
    pub fn save(&self, path: &Path) -> anyhow::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        let tmp = path.with_extension("json.tmp");
        std::fs::write(&tmp, serde_json::to_vec_pretty(self)?)?;
        std::fs::rename(&tmp, path)?;
        Ok(())
    }

    pub fn is_registered(&self) -> bool {
        self.registration.registered
            && !self.registration.channel_id.is_empty()
            && !self.broadcast.ice_password.is_empty()
    }
}
