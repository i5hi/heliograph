//! Shared daemon state + small helpers. One `Arc<AppState>` is handed to axum and to
//! the background workers (audio thread, broadcast thread, snapshot task).

use helio_core::api::{State as ApiState, Status};
use helio_core::Session;
use std::collections::VecDeque;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex};
use std::time::Instant;
use tokio::sync::Notify;

/// Fixed internal audio format. The L-8, Pi aux-in and most class-compliant ADCs run
/// 48 kHz natively; we normalise to 48 kHz stereo and Opus-encode from there.
pub const SAMPLE_RATE: u32 = 48_000;
pub const CHANNELS: u16 = 2;
/// 20 ms Opus frame per channel.
pub const FRAME_SAMPLES: usize = 960;
/// Interleaved samples per frame.
pub const FRAME_INTERLEAVED: usize = FRAME_SAMPLES * CHANNELS as usize;
/// ~2 s of capture backlog before we drop oldest (bounds memory, absorbs jitter).
pub const PCM_CAP: usize = SAMPLE_RATE as usize * CHANNELS as usize * 2;

#[inline]
pub fn store_f32(a: &AtomicU32, v: f32) {
    a.store(v.to_bits(), Ordering::Relaxed);
}
#[inline]
pub fn load_f32(a: &AtomicU32) -> f32 {
    f32::from_bits(a.load(Ordering::Relaxed))
}

/// Commands to the (single, long-lived) audio thread. The new selection is read from
/// the session, so `Open` just pokes the thread to re-resolve it.
pub enum AudioCmd {
    Open,
    Shutdown,
}

/// Latest view of the audio interfaces, refreshed by the watcher.
#[derive(Default, Clone)]
pub struct DeviceSnapshot {
    pub devices: Vec<String>,
    pub default: String,
    /// Device currently being captured from.
    pub active: String,
    /// Whether the *selected* device is present (true when following default).
    pub present: bool,
}

/// PCM + metering shared between the audio thread (producer) and broadcast thread.
#[derive(Clone)]
pub struct Shared {
    pub pcm: Arc<Mutex<VecDeque<i16>>>,
    pub level: Arc<AtomicU32>,
    pub peak: Arc<AtomicU32>,
    /// When fresh audio last arrived. If stale (device off/unplugged/no signal), the
    /// meters must read 0 rather than freezing on the last value.
    pub last_audio: Arc<Mutex<Instant>>,
}

impl Default for Shared {
    fn default() -> Self {
        Self {
            pcm: Arc::new(Mutex::new(VecDeque::with_capacity(PCM_CAP))),
            level: Arc::new(AtomicU32::new(0)),
            peak: Arc::new(AtomicU32::new(0)),
            last_audio: Arc::new(Mutex::new(Instant::now())),
        }
    }
}

impl Shared {
    /// Call whenever a fresh audio block is captured.
    pub fn mark_audio(&self) {
        *self.last_audio.lock().unwrap() = Instant::now();
    }
    /// True if audio arrived within the last second.
    pub fn audio_fresh(&self) -> bool {
        self.last_audio.lock().unwrap().elapsed() < std::time::Duration::from_millis(1000)
    }
    /// Level/peak, forced to 0 when audio is stale (input gone / silent).
    pub fn meters(&self) -> (f32, f32) {
        if self.audio_fresh() {
            (load_f32(&self.level), load_f32(&self.peak))
        } else {
            (0.0, 0.0)
        }
    }
}

/// Lifecycle + metrics for the broadcast thread.
#[derive(Default)]
pub struct BroadcastHandle {
    pub running: Arc<AtomicBool>,
    pub fatal_auth: Arc<AtomicBool>,
    pub reconnects: Arc<AtomicU64>,
    pub started: Mutex<Option<Instant>>,
    pub join: Mutex<Option<std::thread::JoinHandle<()>>>,
}

pub struct AppState {
    pub home: PathBuf,
    pub session: Mutex<Session>,
    pub shared: Shared,
    pub broadcast: BroadcastHandle,
    /// Presence: false = ON DECK, true = AWAY.
    pub away: AtomicBool,
    pub cmd_tx: Mutex<Sender<AudioCmd>>,
    pub devices: Mutex<DeviceSnapshot>,
    /// Fire to push a snapshot immediately (on image change).
    pub snap_notify: Notify,
    /// Fire to push metadata over the WS fast-path immediately (on meta/presence change).
    pub meta_notify: Notify,
}

impl AppState {
    /// Build the transmission object from current session + live metrics.
    pub fn transmission(&self) -> helio_core::Transmission {
        let s = self.session.lock().unwrap();
        let (level, peak) = self.shared.meters();
        let mut t = helio_core::Transmission::build(
            &s.artist,
            &s.channel,
            self.away.load(Ordering::Relaxed),
            level,
            peak,
            self.uptime_s(),
        );
        // Date is read from the DEVICE clock, never from user metadata.
        t.date = chrono::Local::now().format("%Y.%m.%d").to_string();
        t
    }
}

impl AppState {
    pub fn on_air(&self) -> bool {
        self.broadcast.running.load(Ordering::Relaxed)
    }

    pub fn uptime_s(&self) -> u64 {
        self.broadcast
            .started
            .lock()
            .unwrap()
            .map(|t| t.elapsed().as_secs())
            .unwrap_or(0)
    }

    /// Persist the session to disk (best-effort; logs on failure).
    pub fn save(&self) {
        let s = self.session.lock().unwrap().clone();
        if let Err(e) = s.save(&helio_core::session_path(&self.home)) {
            tracing::error!("session save failed: {e}");
        }
    }

    /// Build the public status view.
    pub fn status(&self) -> Status {
        let s = self.session.lock().unwrap();
        let dev = self.devices.lock().unwrap();
        let state = if !s.is_registered() {
            ApiState::Unregistered
        } else if self.on_air() {
            ApiState::OnAir
        } else {
            ApiState::Ready
        };
        let channel = if s.channel.channel.is_empty() {
            s.artist.clone()
        } else {
            s.channel.channel.clone()
        };
        Status {
            state,
            artist: s.artist.clone(),
            channel,
            episode: s.channel.episode,
            server: s.registration.server.clone(),
            channel_id: s.registration.channel_id.clone(),
            on_air: self.on_air(),
            away: self.away.load(Ordering::Relaxed),
            uptime_s: self.uptime_s(),
            reconnects: self.broadcast.reconnects.load(Ordering::Relaxed),
            level: self.shared.meters().0,
            peak: self.shared.meters().1,
            input_device: s.input_device.clone(),
            input_present: dev.present,
            active_input: dev.active.clone(),
        }
    }
}
