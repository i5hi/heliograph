//! Metadata fast-path — a resilient WebSocket from the daemon to the server.
//!
//! Pushes the current [`Transmission`] the instant metadata/presence changes (and a
//! light periodic refresh for level/uptime). This is a **best-effort fast-path**: if
//! the socket is down it silently reconnects with backoff, and the periodic snapshot
//! POST still carries a copy of the metadata as the guaranteed floor. It never blocks
//! or affects the audio broadcast.

use crate::state::AppState;
use base64::Engine;
use futures_util::{SinkExt, StreamExt};
use helio_core::image::{SNAP_H, SNAP_W};
use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Duration;
use tokio_tungstenite::tungstenite::Message;

/// Render the current (transformed) image to a PNG over black and base64-encode it.
async fn render_image_b64(state: &Arc<AppState>) -> Option<String> {
    let img = state.session.lock().unwrap().image.clone();
    let png = tokio::task::spawn_blocking(move || img.render_png(SNAP_W, SNAP_H, "#000000"))
        .await
        .ok()?
        .ok()?;
    Some(base64::engine::general_purpose::STANDARD.encode(png))
}

/// Derive the `ws(s)://…/ingest?channel=…&token=…` URL from the registered server.
fn ingest_url(server: &str, channel_id: &str, token: &str) -> String {
    let base = server.trim_end_matches('/');
    let ws = if let Some(rest) = base.strip_prefix("https://") {
        format!("wss://{rest}")
    } else if let Some(rest) = base.strip_prefix("http://") {
        format!("ws://{rest}")
    } else {
        format!("wss://{base}")
    };
    format!(
        "{ws}/ingest?channel={}&token={}",
        urlencode(channel_id),
        urlencode(token)
    )
}

fn urlencode(s: &str) -> String {
    s.bytes()
        .map(|b| match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => (b as char).to_string(),
            _ => format!("%{b:02X}"),
        })
        .collect()
}

pub async fn run(state: Arc<AppState>) {
    let mut backoff = 1u64;
    loop {
        // Only connect once registered.
        let (url, ready) = {
            let s = state.session.lock().unwrap();
            (
                ingest_url(
                    &s.registration.server,
                    &s.registration.channel_id,
                    &s.broadcast.snapshot_token,
                ),
                s.is_registered(),
            )
        };
        if !ready {
            tokio::time::sleep(Duration::from_secs(3)).await;
            continue;
        }

        match tokio_tungstenite::connect_async(&url).await {
            Ok((mut ws, _)) => {
                tracing::info!("metalink: connected");
                backoff = 1;
                let mut tick = tokio::time::interval(Duration::from_secs(3));
                // On connect: push metadata + the current image once. Thereafter meta on change
                // (or heartbeat), image ONLY on change. Image is never re-sent periodically.
                let mut want_meta = true;
                let mut want_image = true;
                loop {
                    if want_meta {
                        want_meta = false;
                        let payload = serde_json::json!({
                            "type": "meta",
                            "live": state.on_air(),
                            "away": state.away.load(Ordering::Relaxed),
                            "transmission": state.transmission(),
                        });
                        if ws.send(Message::Text(payload.to_string())).await.is_err() {
                            break;
                        }
                    }
                    if want_image {
                        want_image = false;
                        if let Some(b64) = render_image_b64(&state).await {
                            let payload = serde_json::json!({ "type": "image", "png": b64 });
                            if ws.send(Message::Text(payload.to_string())).await.is_err() {
                                break;
                            }
                            tracing::info!("metalink: image pushed");
                        }
                    }
                    tokio::select! {
                        _ = state.meta_notify.notified() => want_meta = true,   // metadata/presence changed
                        _ = state.snap_notify.notified() => want_image = true,  // image changed
                        _ = tick.tick() => want_meta = true,                    // liveness heartbeat
                        msg = ws.next() => match msg {
                            Some(Ok(_)) => {}                                   // ignore inbound
                            _ => break,                                         // closed/errored → reconnect
                        },
                    }
                }
                tracing::warn!("metalink: disconnected, will reconnect");
            }
            Err(e) => {
                tracing::debug!("metalink: connect failed ({e}); backoff {backoff}s");
            }
        }
        tokio::time::sleep(Duration::from_secs(backoff)).await;
        backoff = (backoff * 2).min(30);
    }
}
