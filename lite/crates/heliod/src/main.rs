//! heliod — the broadcast appliance daemon.
//!
//! Owns all state + the capture→Opus→Icecast pipeline and runs regardless of any UI.
//! Exposes an HTTP control API on loopback for `helio-cli` (and later GUI/web clients).

mod api;
mod audio;
mod audio_stdin;
mod broadcast;
#[cfg(target_os = "linux")]
mod audio_alsa;
mod encode;
mod icecast;
mod metalink;
mod registration;
mod state;

use crate::state::{AppState, AudioCmd, BroadcastHandle, DeviceSnapshot, Shared};
use helio_core::{session_path, Session};
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex};
use tokio::sync::Notify;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "heliod=info,helio_core=info".into()),
        )
        .init();

    let home = helio_core::home();
    std::fs::create_dir_all(&home)?;
    let session = Session::load(&session_path(&home))?;
    let initial_away = session.away;
    let auto_air = session.is_registered() && session.on_air; // resume broadcast after reboot
    tracing::info!(
        "heliod {} — home={}, registered={}, resume_on_air={}",
        env!("CARGO_PKG_VERSION"),
        home.display(),
        session.is_registered(),
        auto_air
    );

    let (cmd_tx, cmd_rx) = std::sync::mpsc::channel::<AudioCmd>();
    let state = Arc::new(AppState {
        home,
        session: Mutex::new(session),
        shared: Shared::default(),
        broadcast: BroadcastHandle::default(),
        away: AtomicBool::new(initial_away),
        cmd_tx: Mutex::new(cmd_tx),
        devices: Mutex::new(DeviceSnapshot::default()),
        snap_notify: Notify::new(),
        meta_notify: Notify::new(),
    });

    // Audio capture on its own thread. All backends are device-agnostic to heliod — the OS
    // owns hardware specifics (drivers, asound.conf routing):
    //   • Linux   → native ALSA (RW), opening the selected device (default "heliomaster").
    //   • "stdin" → raw S16LE/48k/stereo piped in (fallback).
    //   • other   → cpal (macOS/dev).
    let stdin_audio = std::env::var("HELIO_AUDIO").map(|v| v == "stdin").unwrap_or(false)
        || state.session.lock().unwrap().input_device == "stdin";
    {
        let st = state.clone();
        if stdin_audio {
            std::thread::Builder::new()
                .name("heliod-audio-stdin".into())
                .spawn(move || audio_stdin::run(st))?;
        } else {
            #[cfg(target_os = "linux")]
            {
                let _ = &cmd_rx; // cpal command channel is unused with the native ALSA backend
                std::thread::Builder::new()
                    .name("heliod-audio-alsa".into())
                    .spawn(move || audio_alsa::run(st))?;
            }
            #[cfg(not(target_os = "linux"))]
            {
                std::thread::Builder::new()
                    .name("heliod-audio".into())
                    .spawn(move || audio::run(st, cmd_rx))?;
            }
        }
    }

    // Background async worker: the metalink pushes metadata + the image over WS, and its
    // heartbeat is the liveness signal. (No periodic snapshot POST — the image goes over WS
    // only when it changes.)
    tokio::spawn(metalink::run(state.clone()));

    // Appliance behaviour: if the session was ON AIR (e.g. before a reboot), resume broadcasting
    // automatically — no manual `helio start` needed. Presence (AWAY/ON DECK) is already restored.
    if auto_air {
        match broadcast::start(&state) {
            Ok(()) => tracing::info!("auto-resume: broadcasting (session was ON AIR)"),
            Err(e) => tracing::warn!("auto-resume failed: {e}"),
        }
    }

    let bind = std::env::var("HELIO_BIND").unwrap_or_else(|_| "127.0.0.1:4777".into());
    let listener = tokio::net::TcpListener::bind(&bind).await?;
    tracing::info!("control API listening on http://{bind}");

    axum::serve(listener, api::router(state.clone()))
        .with_graceful_shutdown(shutdown(state.clone()))
        .await?;

    Ok(())
}

/// On Ctrl-C / SIGTERM: stop the broadcast cleanly and tell the audio thread to exit.
async fn shutdown(state: Arc<AppState>) {
    let _ = tokio::signal::ctrl_c().await;
    tracing::info!("shutdown: stopping broadcast");
    broadcast::stop(&state);
    let _ = state.cmd_tx.lock().unwrap().send(AudioCmd::Shutdown);
}
