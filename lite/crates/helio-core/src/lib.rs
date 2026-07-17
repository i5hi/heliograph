//! helio-core — shared types for the heliod broadcast appliance.
//!
//! This crate is deliberately dependency-light and platform-neutral: it holds the
//! persisted [`Session`] schema (kept wire-compatible with the existing
//! `radio.stackmate.org` backend), the [`ImageSettings`] + snapshot renderer, the
//! [`Transmission`] metadata we push to the server, and the request/response DTOs
//! shared between `heliod` (daemon) and `helio-cli`.

pub mod api;
pub mod image;
pub mod pipeline;
pub mod session;
pub mod transmission;

pub use image::ImageSettings;
pub use pipeline::{AudioFormat, AudioInput, Encoder, PcmSink, StreamMeta, Transport, TransportError};
pub use session::{Broadcast, Registration, Session};
pub use transmission::{ChannelMeta, Transmission};

use std::path::{Path, PathBuf};

/// Resolve the heliod home directory (state lives here). Override with `HELIO_HOME`;
/// defaults to `~/.heliod`. Created on first use by the caller.
pub fn home() -> PathBuf {
    if let Ok(h) = std::env::var("HELIO_HOME") {
        return PathBuf::from(h);
    }
    dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join(".heliod")
}

/// Path to `session.json` inside the given home dir.
pub fn session_path(home: &Path) -> PathBuf {
    home.join("session.json")
}
