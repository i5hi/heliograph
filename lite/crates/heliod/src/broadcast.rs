//! Broadcast controller + pipeline loop.
//!
//! The loop is written against the `Encoder`/`Transport` traits, so swapping Opus→MP3
//! or Icecast→SRT/MoQ is a new impl, not a rewrite. Reconnect policy mirrors the
//! hard-won heliograph behaviour: **retry forever while ON AIR, give up only on auth**.
//! When the input is momentarily absent it streams silence rather than dropping the
//! source — listeners stay connected (uptime first).

use crate::encode::OpusEncoder;
use crate::icecast::IcecastTransport;
use crate::state::{AppState, FRAME_INTERLEAVED};
use helio_core::pipeline::{Encoder, StreamMeta, Transport};
use std::collections::VecDeque;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// Immutable snapshot of what the pipeline needs, captured at start.
struct Config {
    host: String,
    port: u16,
    mount: String,
    password: String,
    name: String,
    description: String,
    bitrate: i32,
}

/// Begin broadcasting. Returns an error string if we can't start (e.g. not registered).
pub fn start(state: &Arc<AppState>) -> Result<(), String> {
    if state.on_air() {
        return Ok(());
    }
    let cfg = {
        let s = state.session.lock().unwrap();
        if !s.is_registered() {
            return Err("not registered — run `helio register` first".into());
        }
        let base_mount = if s.broadcast.ice_mount.is_empty() {
            s.registration.channel_id.clone()
        } else {
            s.broadcast.ice_mount.clone()
        };
        Config {
            host: s.broadcast.ice_host.clone(),
            port: if s.broadcast.ice_port == 0 { 8000 } else { s.broadcast.ice_port },
            // Opus REPLACES MP3 on the channel's mount, so the server's streamUrl
            // (/stream/<channelId>) keeps working with no server/client change.
            mount: base_mount,
            password: s.broadcast.ice_password.clone(),
            name: if s.channel.channel.is_empty() { s.artist.clone() } else { s.channel.channel.clone() },
            description: s.channel.note.clone(),
            bitrate: 192_000,
        }
    };

    let b = &state.broadcast;
    b.fatal_auth.store(false, Ordering::Relaxed);
    b.reconnects.store(0, Ordering::Relaxed);
    b.running.store(true, Ordering::Relaxed);
    *b.started.lock().unwrap() = Some(Instant::now());
    // Presence is preserved across start (persisted separately). Record ON-AIR intent so a
    // reboot auto-resumes the broadcast.
    state.session.lock().unwrap().on_air = true;
    state.save();

    let st = state.clone();
    let handle = std::thread::Builder::new()
        .name("heliod-broadcast".into())
        .spawn(move || run_loop(st, cfg))
        .map_err(|e| format!("spawn broadcast thread: {e}"))?;
    *b.join.lock().unwrap() = Some(handle);

    // Push a fresh frame + metadata immediately.
    state.snap_notify.notify_one();
    state.meta_notify.notify_one();
    tracing::info!("BROADCAST start");
    Ok(())
}

/// Stop broadcasting and join the thread.
pub fn stop(state: &Arc<AppState>) {
    if !state.on_air() {
        return;
    }
    state.broadcast.running.store(false, Ordering::Relaxed);
    state.session.lock().unwrap().on_air = false; // deliberate stop → don't auto-resume on reboot
    state.save();
    let handle = state.broadcast.join.lock().unwrap().take();
    if let Some(h) = handle {
        let _ = h.join();
    }
    *state.broadcast.started.lock().unwrap() = None;
    let n = state.broadcast.reconnects.load(Ordering::Relaxed);
    tracing::info!("BROADCAST stop (reconnects this session={n})");
}

fn run_loop(state: Arc<AppState>, cfg: Config) {
    let mut enc = match OpusEncoder::new(cfg.bitrate) {
        Ok(e) => e,
        Err(e) => {
            tracing::error!("BROADCAST: opus init failed: {e}");
            state.broadcast.running.store(false, Ordering::Relaxed);
            return;
        }
    };
    let mut tx = IcecastTransport::new(cfg.host, cfg.port, cfg.mount, cfg.password);
    let meta = StreamMeta {
        name: cfg.name,
        description: cfg.description,
        genre: "radio".into(),
        url: String::new(),
        public: false,
    };
    let running = state.broadcast.running.clone();
    let mut backoff = Duration::from_secs(1);

    while running.load(Ordering::Relaxed) {
        enc.reset();
        match tx.connect(enc.content_type(), &meta) {
            Ok(()) => {
                tracing::info!("BROADCAST link up");
                backoff = Duration::from_secs(1);
            }
            Err(e) if e.is_auth() => {
                tracing::error!("BROADCAST AUTH FAILED ({e}) — stopping. Re-register.");
                state.broadcast.fatal_auth.store(true, Ordering::Relaxed);
                running.store(false, Ordering::Relaxed);
                break;
            }
            Err(e) => {
                state.broadcast.reconnects.fetch_add(1, Ordering::Relaxed);
                tracing::warn!("BROADCAST link down ({e}) — retry in {:?}", backoff);
                sleep_interruptible(&running, backoff);
                backoff = (backoff * 2).min(Duration::from_secs(15));
                continue;
            }
        }

        // Headers for the fresh logical stream.
        if let Ok(h) = enc.stream_headers() {
            if tx.send(&h).is_err() {
                tx.close();
                continue;
            }
        }

        // Real-time paced encode/send loop.
        let mut next = Instant::now();
        while running.load(Ordering::Relaxed) {
            let frame = pull_frame(&state.shared.pcm);
            match enc.encode(&frame) {
                Ok(bytes) if !bytes.is_empty() => {
                    if tx.send(&bytes).is_err() {
                        tracing::warn!("BROADCAST source dropped — reconnecting");
                        state.broadcast.reconnects.fetch_add(1, Ordering::Relaxed);
                        break;
                    }
                }
                Ok(_) => {}
                Err(e) => {
                    tracing::error!("BROADCAST encode error: {e}");
                    break;
                }
            }
            next += Duration::from_millis(20);
            let now = Instant::now();
            if next > now {
                std::thread::sleep(next - now);
            } else {
                next = now; // fell behind (e.g. slept for reconnect) — resync
            }
        }
        tx.close();
    }
    tx.close();
    tracing::info!("broadcast loop ended");
}

/// Pull exactly one 20 ms interleaved frame; pad with silence if the buffer is short.
fn pull_frame(pcm: &Mutex<VecDeque<i16>>) -> Vec<i16> {
    let mut q = pcm.lock().unwrap();
    let mut f = Vec::with_capacity(FRAME_INTERLEAVED);
    while f.len() < FRAME_INTERLEAVED {
        match q.pop_front() {
            Some(s) => f.push(s),
            None => break,
        }
    }
    drop(q);
    f.resize(FRAME_INTERLEAVED, 0); // silence-fill the tail
    f
}

fn sleep_interruptible(running: &std::sync::atomic::AtomicBool, dur: Duration) {
    let deadline = Instant::now() + dur;
    while running.load(Ordering::Relaxed) && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(100));
    }
}
