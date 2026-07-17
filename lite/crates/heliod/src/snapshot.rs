//! Periodic snapshot: render the image + transforms to a PNG and PUT it to the
//! server with the transmission metadata (`X-Transmission`). This is the liveness
//! signal (the server marks a channel live on fresh frames) AND the metadata floor
//! (a copy rides here even if the WS fast-path is down).

use crate::state::AppState;
use base64::Engine;
use helio_core::image::{SNAP_H, SNAP_W};
use std::sync::Arc;
use std::time::Duration;

/// Snapshot cadence — comfortably under the server's 15 s liveness window.
const INTERVAL: Duration = Duration::from_secs(12);

pub async fn run(state: Arc<AppState>) {
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(20))
        .build()
        .expect("http client");

    loop {
        tokio::select! {
            _ = state.snap_notify.notified() => {}
            _ = tokio::time::sleep(INTERVAL) => {}
        }
        if !state.on_air() {
            continue;
        }

        let (url, token, img) = {
            let s = state.session.lock().unwrap();
            (
                s.broadcast.snapshot_url.clone(),
                s.broadcast.snapshot_token.clone(),
                s.image.clone(),
            )
        };
        if url.is_empty() || token.is_empty() {
            continue;
        }
        let tx = state.transmission();

        // Render off the async runtime (CPU-bound). Background is always BLACK — the accent is a
        // UI/tint colour on the client, never the frame backdrop (deep-space look, no image = black).
        let png = match tokio::task::spawn_blocking(move || img.render_png(SNAP_W, SNAP_H, "#000000")).await {
            Ok(Ok(p)) => p,
            Ok(Err(e)) => {
                tracing::warn!("snapshot render failed: {e}");
                continue;
            }
            Err(e) => {
                tracing::warn!("snapshot render task failed: {e}");
                continue;
            }
        };

        let meta_b64 = base64::engine::general_purpose::STANDARD
            .encode(serde_json::to_vec(&tx).unwrap_or_default());

        match client
            .put(&url)
            .header("Authorization", format!("Bearer {token}"))
            .header("X-Transmission", meta_b64)
            .header("Content-Type", "image/png")
            .body(png)
            .send()
            .await
        {
            Ok(r) if r.status().is_success() => tracing::debug!("snapshot ok ({} B)", r.content_length().unwrap_or(0)),
            Ok(r) => tracing::warn!("snapshot rejected: HTTP {}", r.status()),
            Err(e) => tracing::warn!("snapshot post failed: {e}"),
        }
    }
}
