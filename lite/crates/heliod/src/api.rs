//! HTTP control API (loopback). `helio-cli` — and later a GUI/web UI — are stateless
//! clients over these routes.

use crate::state::{AppState, AudioCmd};
use crate::{broadcast, registration};
use axum::extract::State;
use axum::routing::{get, post};
use axum::{Json, Router};
use helio_core::api::{
    Ack, DeviceInfo, DevicesResponse, ImageUpdate, MetaUpdate, RegisterRequest, SelectDeviceRequest,
    Status,
};
use serde::Deserialize;
use std::sync::atomic::Ordering;
use std::sync::Arc;

pub fn router(state: Arc<AppState>) -> Router {
    Router::new()
        .route("/status", get(status))
        .route("/devices", get(devices))
        .route("/device", post(select_device))
        .route("/register", post(register))
        .route("/broadcast/start", post(start_broadcast))
        .route("/broadcast/stop", post(stop_broadcast))
        .route("/presence", post(presence))
        .route("/meta", post(meta))
        .route("/image", post(image))
        .with_state(state)
}

async fn status(State(st): State<Arc<AppState>>) -> Json<Status> {
    Json(st.status())
}

async fn devices(State(st): State<Arc<AppState>>) -> Json<DevicesResponse> {
    // Enumerate the system's inputs live, so this works in every capture mode
    // (including stdin mode, where the cpal thread isn't running).
    let (names, default) = tokio::task::spawn_blocking(crate::audio::enumerate)
        .await
        .unwrap_or_default();
    let selected = st.session.lock().unwrap().input_device.clone();
    let active = st.devices.lock().unwrap().active.clone();
    let devices = names
        .iter()
        .map(|name| DeviceInfo {
            name: name.clone(),
            is_default: *name == default,
            is_current: *name == active,
        })
        .collect();
    Json(DevicesResponse { devices, selected })
}

async fn select_device(
    State(st): State<Arc<AppState>>,
    Json(req): Json<SelectDeviceRequest>,
) -> Json<Ack> {
    let dev = if req.device == "default" { String::new() } else { req.device };
    st.session.lock().unwrap().input_device = dev.clone();
    st.save();
    let _ = dev;
    // Ask the audio thread to re-open against the new selection (read from the session).
    if let Err(e) = st.cmd_tx.lock().unwrap().send(AudioCmd::Open) {
        return Json(Ack::err(format!("audio thread unavailable: {e}")));
    }
    Json(Ack::ok())
}

async fn register(
    State(st): State<Arc<AppState>>,
    Json(req): Json<RegisterRequest>,
) -> Json<Ack> {
    match registration::register(st.clone(), req.invite_code, req.artist, req.server).await {
        Ok(()) => Json(Ack::ok()),
        Err(e) => Json(Ack::err(e)),
    }
}

async fn start_broadcast(State(st): State<Arc<AppState>>) -> Json<Ack> {
    match broadcast::start(&st) {
        Ok(()) => Json(Ack::ok()),
        Err(e) => Json(Ack::err(e)),
    }
}

async fn stop_broadcast(State(st): State<Arc<AppState>>) -> Json<Ack> {
    broadcast::stop(&st);
    Json(Ack::ok())
}

#[derive(Deserialize)]
struct Presence {
    away: bool,
}

async fn presence(State(st): State<Arc<AppState>>, Json(p): Json<Presence>) -> Json<Ack> {
    st.away.store(p.away, Ordering::Relaxed);
    st.session.lock().unwrap().away = p.away; // persist across restarts
    st.save();
    st.meta_notify.notify_one(); // push over WS immediately
    Json(Ack::ok())
}

async fn meta(State(st): State<Arc<AppState>>, Json(u): Json<MetaUpdate>) -> Json<Ack> {
    {
        let mut s = st.session.lock().unwrap();
        let c = &mut s.channel;
        if let Some(v) = u.channel { c.channel = v; }
        if let Some(v) = u.title { c.title = v; }
        if let Some(v) = u.note { c.note = v; }
        if let Some(v) = u.date { c.date = v; }
        if let Some(v) = u.episode { c.episode = v; }
        if let Some(v) = u.font { c.font = v; }
        if let Some(v) = u.accent { c.accent = v; }
        if let Some(v) = u.btc { c.btc = v; }
        if let Some(v) = u.ln { c.ln = v; }
        if let Some(v) = u.lq { c.lq = v; }
    }
    st.save();
    st.meta_notify.notify_one();
    Json(Ack::ok())
}

async fn image(State(st): State<Arc<AppState>>, Json(u): Json<ImageUpdate>) -> Json<Ack> {
    {
        let mut s = st.session.lock().unwrap();
        u.apply(&mut s.image);
    }
    st.save();
    st.snap_notify.notify_one();
    Json(Ack::ok())
}
