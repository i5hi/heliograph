//! Swappable pipeline backends.
//!
//! The broadcast loop is written entirely against these traits, so the concrete
//! codec/transport/input can be replaced without touching orchestration:
//!
//! ```text
//!   AudioInput  →  Encoder  →  Transport
//!   (cpal)         (Opus)      (Icecast)     ← today
//!                  (MP3)       (SRT/MoQ/WHIP) ← later, drop-in
//! ```
//!
//! Kept dependency-light (plain PCM/byte types only) so `helio-core` stays neutral;
//! the cpal/opus/network-heavy impls live in `heliod`.

use std::fmt;

/// Metadata a [`Transport`] may advertise to the server on connect.
#[derive(Debug, Clone, Default)]
pub struct StreamMeta {
    pub name: String,
    pub description: String,
    pub genre: String,
    pub url: String,
    /// True for a publicly-listed stream.
    pub public: bool,
}

/// Audio format flowing through the pipeline: interleaved 16-bit PCM.
#[derive(Debug, Clone, Copy)]
pub struct AudioFormat {
    pub sample_rate: u32,
    pub channels: u16,
}

/// Errors a [`Transport`] can raise. The distinction matters for the reconnect
/// policy: auth failures are fatal (stop), everything else is retryable (keep trying
/// while ON AIR) — mirroring the hard-won heliograph behaviour.
#[derive(Debug)]
pub enum TransportError {
    /// Bad/rejected credentials — do NOT retry.
    Auth(String),
    /// Any transient/network/server error — retry with backoff.
    Transient(String),
}

impl TransportError {
    pub fn is_auth(&self) -> bool {
        matches!(self, TransportError::Auth(_))
    }
}

impl fmt::Display for TransportError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            TransportError::Auth(m) => write!(f, "auth: {m}"),
            TransportError::Transient(m) => write!(f, "transient: {m}"),
        }
    }
}
impl std::error::Error for TransportError {}

/// A codec that turns PCM into a self-contained container bytestream (e.g. Ogg/Opus).
pub trait Encoder: Send {
    /// MIME type of the produced stream (e.g. `application/ogg`).
    fn content_type(&self) -> &'static str;
    /// Bytes to emit once at the START of a (re)connection — container + codec headers.
    fn stream_headers(&mut self) -> anyhow::Result<Vec<u8>>;
    /// Encode one chunk of interleaved i16 PCM into container bytes (may be empty until a
    /// full frame accumulates). Implementations buffer internally to the codec frame size.
    fn encode(&mut self, interleaved: &[i16]) -> anyhow::Result<Vec<u8>>;
    /// Flush any buffered audio + write the end-of-stream marker.
    fn trailer(&mut self) -> Vec<u8> {
        Vec::new()
    }
    /// Reset internal state for a fresh logical stream (called on reconnect).
    fn reset(&mut self) {}
}

/// A byte sink to the server. `connect` opens the source session; `send` streams
/// encoded bytes; `close` tears it down.
pub trait Transport: Send {
    fn connect(&mut self, content_type: &str, meta: &StreamMeta) -> Result<(), TransportError>;
    fn send(&mut self, data: &[u8]) -> Result<(), TransportError>;
    fn close(&mut self);
}

/// A capture source that pushes interleaved i16 PCM to a sink. Implementations own the
/// OS audio callback and are responsible for resampling/format to [`AudioFormat`].
pub trait AudioInput: Send {
    /// Begin capture, delivering frames via `sink`. Returns once the stream is running.
    fn start(&mut self, format: AudioFormat, sink: PcmSink) -> anyhow::Result<()>;
    fn stop(&mut self);
    /// Human-readable name of the device actually opened.
    fn device_name(&self) -> String;
}

/// Thread-safe handoff of PCM from the capture callback to the encode loop.
pub type PcmSink = std::sync::Arc<dyn Fn(&[i16]) + Send + Sync>;
