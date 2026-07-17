//! Native ALSA capture (Linux) — reads an ALSA device directly in-process, in **RW mode**.
//!
//! This replaces the `arecord | heliod` pipe. cpal's capture uses mmap, which returns
//! silence with the experimental zoom driver; RW interleaved reads (what `arecord` does)
//! work fine, so we do the same natively. heliod stays device-agnostic: it opens whatever
//! ALSA device name the session selects (default `heliomaster`, an OS-side asound.conf route
//! that maps the L-8 master ch1-2 → clean stereo). All hardware specifics live in the OS.

use crate::state::{load_f32, store_f32, AppState, PCM_CAP, SAMPLE_RATE};
use alsa::pcm::{Access, Format, HwParams, PCM};
use alsa::{Direction, ValueOr};
use std::sync::Arc;
use std::time::Duration;

/// Resolve the selected device, defaulting to the `heliomaster` OS route.
fn selected(state: &AppState) -> String {
    let s = state.session.lock().unwrap().input_device.clone();
    if s.is_empty() || s == "default" { "heliomaster".to_string() } else { s }
}

pub fn run(state: Arc<AppState>) {
    loop {
        let dev = selected(&state);
        match capture(&state, &dev) {
            Ok(reason) => tracing::info!("audio: reopening capture ({reason})"),
            Err(e) => {
                tracing::warn!("audio: '{dev}' unavailable ({e}); streaming silence, retry in 2s");
                let mut d = state.devices.lock().unwrap();
                d.active = String::new();
                d.present = false;
            }
        }
        std::thread::sleep(Duration::from_secs(2));
    }
}

fn capture(state: &Arc<AppState>, dev: &str) -> anyhow::Result<&'static str> {
    let pcm = PCM::new(dev, Direction::Capture, false).map_err(|e| anyhow::anyhow!("open: {e}"))?;
    {
        let hwp = HwParams::any(&pcm)?;
        hwp.set_channels(2)?;
        hwp.set_rate(SAMPLE_RATE, ValueOr::Nearest)?;
        hwp.set_format(Format::s16())?;
        hwp.set_access(Access::RWInterleaved)?;
        hwp.set_period_size_near(1024, ValueOr::Nearest)?;
        pcm.hw_params(&hwp)?;
    }
    pcm.prepare()?;
    let io = pcm.io_i16()?;
    {
        let mut d = state.devices.lock().unwrap();
        d.active = dev.to_string();
        d.present = true;
    }
    tracing::info!("audio: capturing '{dev}' @ 48k/stereo/S16 (native ALSA, RW)");

    let mut buf = vec![0i16; 2048]; // 1024 frames × 2ch
    loop {
        if selected(state) != dev {
            return Ok("device changed");
        }
        match io.readi(&mut buf) {
            Ok(frames) => {
                let n = frames * 2;
                let mut q = state.shared.pcm.lock().unwrap();
                let (mut peak, mut sumsq, mut cnt) = (0f32, 0f64, 0usize);
                let mut i = 0;
                while i + 1 < n {
                    let (l, r) = (buf[i], buf[i + 1]);
                    while q.len() + 2 > PCM_CAP {
                        q.pop_front();
                        q.pop_front();
                    }
                    q.push_back(l);
                    q.push_back(r);
                    let (lf, rf) = (l as f32 / 32768.0, r as f32 / 32768.0);
                    peak = peak.max(lf.abs()).max(rf.abs());
                    sumsq += (lf * lf + rf * rf) as f64;
                    cnt += 2;
                    i += 2;
                }
                drop(q);
                if cnt > 0 {
                    store_f32(&state.shared.level, ((sumsq / cnt as f64).sqrt() as f32).min(1.0));
                }
                let decayed = (load_f32(&state.shared.peak) * 0.92).max(peak).min(1.0);
                store_f32(&state.shared.peak, decayed);
                if frames > 0 {
                    state.shared.mark_audio(); // device delivering data (even silence) → meters live
                }
            }
            Err(e) => {
                // Recover from xruns/suspend; if unrecoverable, reopen the device.
                if pcm.try_recover(e, true).is_err() {
                    return Err(anyhow::anyhow!("read: {e}"));
                }
            }
        }
    }
}
