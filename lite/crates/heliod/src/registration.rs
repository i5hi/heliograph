//! One-time registration against the backend `/register` endpoint. On success we
//! persist the returned icecast credentials + snapshot target into the session.

use crate::state::AppState;
use serde_json::Value;
use std::sync::Arc;

pub async fn register(
    state: Arc<AppState>,
    invite: String,
    artist: String,
    server: Option<String>,
) -> Result<(), String> {
    let server = server
        .unwrap_or_else(|| state.session.lock().unwrap().registration.server.clone())
        .trim_end_matches('/')
        .to_string();

    let client = reqwest::Client::new();
    let resp = client
        .post(format!("{server}/register"))
        .json(&serde_json::json!({ "inviteCode": invite, "artist": artist }))
        .send()
        .await
        .map_err(|e| format!("request failed: {e}"))?;

    let status = resp.status();
    let v: Value = resp
        .json()
        .await
        .map_err(|e| format!("bad response: {e}"))?;

    if !status.is_success() {
        return Err(v
            .get("error")
            .and_then(Value::as_str)
            .unwrap_or("registration failed")
            .to_string());
    }

    let channel_id = v.get("channelId").and_then(Value::as_str).unwrap_or_default().to_string();
    let ice = v.get("icecast").cloned().unwrap_or_default();
    let get = |k: &str| ice.get(k).and_then(Value::as_str).unwrap_or_default().to_string();

    {
        let mut s = state.session.lock().unwrap();
        s.artist = artist.clone();
        s.registration.server = server;
        s.registration.channel_id = channel_id.clone();
        s.registration.registered = true;
        s.broadcast.ice_host = {
            let h = get("host");
            if h.is_empty() { s.registration.server.clone() } else { h }
        };
        s.broadcast.ice_port = ice.get("port").and_then(Value::as_u64).unwrap_or(8000) as u16;
        s.broadcast.ice_mount = {
            let m = get("mount");
            if m.is_empty() { channel_id.clone() } else { m }
        };
        s.broadcast.ice_password = get("password");
        s.broadcast.snapshot_url = v.get("snapshotUrl").and_then(Value::as_str).unwrap_or_default().to_string();
        s.broadcast.snapshot_token = v.get("snapshotToken").and_then(Value::as_str).unwrap_or_default().to_string();
        if s.channel.channel.is_empty() {
            s.channel.channel = artist;
        }
    }
    state.save();
    Ok(())
}
