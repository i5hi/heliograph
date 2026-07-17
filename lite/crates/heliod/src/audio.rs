//! Audio capture — interface-agnostic, hotplug-aware.
//!
//! One long-lived thread owns the cpal input stream (cpal `Stream` is `!Send`, so it
//! can never leave this thread). The thread:
//!   * opens the selected device, or the system default when the selection is empty
//!     (the Pi's aux-in is the default → zero-config there);
//!   * every ~2 s re-enumerates devices (USB interfaces come and go) and, if the
//!     selected/active device changed or vanished, transparently re-opens — the
//!     broadcast keeps running on silence in the gap, so listeners never drop;
//!   * converts any device format to 48 kHz stereo i16 and feeds the shared PCM ring,
//!     updating the level/peak meters.

use crate::state::{load_f32, store_f32, AppState, AudioCmd, PCM_CAP};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{Device, SampleFormat, Stream, StreamConfig};
use std::sync::mpsc::{Receiver, RecvTimeoutError};
use std::sync::Arc;
use std::time::Duration;

/// List input device names + the default device name.
///
/// On Linux we prefer the friendly sound-card names from `/proc/asound/cards`
/// (e.g. "ZOOM L-8 · hw:CARD=L8") — including devices currently in use — instead of
/// cpal's cryptic PCM hints (`default:CARD=…`) that also drop busy devices. Falls back
/// to cpal elsewhere.
pub fn enumerate() -> (Vec<String>, String) {
    #[cfg(target_os = "linux")]
    if let Some(cards) = linux_cards() {
        return cards;
    }
    let host = cpal::default_host();
    let mut names = Vec::new();
    if let Ok(devs) = host.input_devices() {
        for d in devs {
            if let Ok(n) = d.name() {
                names.push(n);
            }
        }
    }
    let default = host
        .default_input_device()
        .and_then(|d| d.name().ok())
        .unwrap_or_default();
    (names, default)
}

/// Parse `/proc/asound/cards`, keep capture-capable cards, return friendly labels.
#[cfg(target_os = "linux")]
fn linux_cards() -> Option<(Vec<String>, String)> {
    let text = std::fs::read_to_string("/proc/asound/cards").ok()?;
    let mut out = Vec::new();
    for line in text.lines() {
        let l = line.trim_start();
        // Header lines start with the card index; the indented description line doesn't.
        let idx: u32 = match l.split_whitespace().next().and_then(|t| t.parse().ok()) {
            Some(n) => n,
            None => continue,
        };
        // capture-capable? (has a pcm*c node)
        let has_capture = std::fs::read_dir(format!("/proc/asound/card{idx}"))
            .map(|rd| {
                rd.flatten().any(|e| {
                    let n = e.file_name();
                    let n = n.to_string_lossy();
                    n.starts_with("pcm") && n.ends_with('c')
                })
            })
            .unwrap_or(false);
        if !has_capture {
            continue;
        }
        let id = l
            .split('[')
            .nth(1)
            .and_then(|s| s.split(']').next())
            .map(|s| s.trim().to_string())
            .unwrap_or_default();
        let name = l
            .splitn(2, " - ")
            .nth(1)
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| id.clone());
        out.push(format!("{name}  ·  hw:CARD={id}"));
    }
    if out.is_empty() {
        None
    } else {
        Some((out, String::new()))
    }
}

/// The capture thread entrypoint.
pub fn run(state: Arc<AppState>, cmd_rx: Receiver<AudioCmd>) {
    let host = cpal::default_host();
    let mut stream: Option<Stream> = None;
    let mut open_name = String::new();

    // Resolve + open whatever the session currently selects.
    let reopen = |host: &cpal::Host, stream: &mut Option<Stream>, open_name: &mut String| {
        let sel = state.session.lock().unwrap().input_device.clone();
        *stream = None; // stop the old stream first (drop releases the device)
        match open_device(host, &sel, &state) {
            Ok((s, name)) => {
                let _ = s.play();
                *open_name = name.clone();
                *stream = Some(s);
                let mut d = state.devices.lock().unwrap();
                d.active = name;
                d.present = sel.is_empty() || d.devices.iter().any(|n| n == &sel);
                tracing::info!("audio: capturing from '{}'", open_name);
            }
            Err(e) => {
                *open_name = String::new();
                let mut d = state.devices.lock().unwrap();
                d.active = String::new();
                d.present = sel.is_empty();
                tracing::warn!("audio: no input open ({e}); streaming silence until a device appears");
            }
        }
    };

    refresh_devices(&state);
    reopen(&host, &mut stream, &mut open_name);

    loop {
        match cmd_rx.recv_timeout(Duration::from_millis(500)) {
            Ok(AudioCmd::Open) => reopen(&host, &mut stream, &mut open_name),
            Ok(AudioCmd::Shutdown) | Err(RecvTimeoutError::Disconnected) => break,
            Err(RecvTimeoutError::Timeout) => {
                // Periodic hotplug scan.
                refresh_devices(&state);
                let sel = state.session.lock().unwrap().input_device.clone();
                let (names, default) = {
                    let d = state.devices.lock().unwrap();
                    (d.devices.clone(), d.default.clone())
                };
                let want = if sel.is_empty() { default.clone() } else { sel.clone() };
                let selected_present = sel.is_empty() || names.iter().any(|n| n == &sel);
                let need_reopen = if stream.is_none() {
                    // Nothing open — try if the wanted device is now present.
                    selected_present && !want.is_empty()
                } else {
                    // Open, but the target device changed underneath us.
                    selected_present && !want.is_empty() && want != open_name
                };
                if need_reopen {
                    reopen(&host, &mut stream, &mut open_name);
                }
            }
        }
    }
    tracing::info!("audio thread exiting");
}

/// Refresh the device list snapshot in shared state.
fn refresh_devices(state: &AppState) {
    let (names, default) = enumerate();
    let mut d = state.devices.lock().unwrap();
    d.devices = names;
    d.default = default;
}

fn open_device(host: &cpal::Host, sel: &str, state: &Arc<AppState>) -> anyhow::Result<(Stream, String)> {
    let device = if sel.is_empty() || sel == "default" {
        host.default_input_device()
            .ok_or_else(|| anyhow::anyhow!("no default input device"))?
    } else {
        host.input_devices()?
            .find(|d| d.name().map(|n| n == sel).unwrap_or(false))
            .ok_or_else(|| anyhow::anyhow!("input device '{sel}' not found"))?
    };
    let name = device.name().unwrap_or_else(|_| "unknown".into());
    let (config, fmt) = choose_config(&device)?;
    if config.sample_rate.0 != crate::state::SAMPLE_RATE {
        tracing::warn!(
            "audio: '{name}' opened at {} Hz, not 48 kHz — pitch may drift (set the device to 48 kHz)",
            config.sample_rate.0
        );
    }
    let stream = build_stream(&device, &config, fmt, state)?;
    Ok((stream, name))
}

/// Prefer a 48 kHz i16/f32 config; fall back to the device default.
fn choose_config(device: &Device) -> anyhow::Result<(StreamConfig, SampleFormat)> {
    if let Ok(ranges) = device.supported_input_configs() {
        let mut best: Option<(StreamConfig, SampleFormat)> = None;
        for r in ranges {
            let sf = r.sample_format();
            let ok_fmt = matches!(sf, SampleFormat::F32 | SampleFormat::I16 | SampleFormat::U16);
            if ok_fmt
                && r.min_sample_rate().0 <= crate::state::SAMPLE_RATE
                && r.max_sample_rate().0 >= crate::state::SAMPLE_RATE
            {
                let cfg = r.with_sample_rate(cpal::SampleRate(crate::state::SAMPLE_RATE)).config();
                let prefer = sf == SampleFormat::I16;
                if prefer {
                    return Ok((cfg, sf));
                }
                best.get_or_insert((cfg, sf));
            }
        }
        if let Some(b) = best {
            return Ok(b);
        }
    }
    let def = device.default_input_config()?;
    Ok((def.config(), def.sample_format()))
}

fn build_stream(
    device: &Device,
    config: &StreamConfig,
    fmt: SampleFormat,
    state: &Arc<AppState>,
) -> anyhow::Result<Stream> {
    let channels = config.channels as usize;
    let sh = state.shared.clone();
    let err = |e| tracing::error!("audio stream error: {e}");

    macro_rules! typed {
        ($t:ty, $to_i16:expr) => {{
            let conv: fn($t) -> i16 = $to_i16;
            device.build_input_stream(
                config,
                move |data: &[$t], _: &cpal::InputCallbackInfo| {
                    let mut q = sh.pcm.lock().unwrap();
                    let mut peak = 0f32;
                    let mut sumsq = 0f64;
                    let mut cnt = 0usize;
                    for frame in data.chunks(channels) {
                        let l = conv(frame[0]);
                        let r = if channels > 1 { conv(frame[1]) } else { l };
                        while q.len() + 2 > PCM_CAP {
                            q.pop_front();
                            q.pop_front();
                        }
                        q.push_back(l);
                        q.push_back(r);
                        let lf = l as f32 / 32768.0;
                        let rf = r as f32 / 32768.0;
                        peak = peak.max(lf.abs()).max(rf.abs());
                        sumsq += (lf * lf + rf * rf) as f64;
                        cnt += 2;
                    }
                    drop(q);
                    let rms = if cnt > 0 { (sumsq / cnt as f64).sqrt() as f32 } else { 0.0 };
                    store_f32(&sh.level, rms.min(1.0));
                    let decayed = (load_f32(&sh.peak) * 0.92).max(peak).min(1.0);
                    store_f32(&sh.peak, decayed);
                    sh.mark_audio();
                },
                err,
                None,
            )?
        }};
    }

    let stream = match fmt {
        SampleFormat::F32 => typed!(f32, |s: f32| (s.clamp(-1.0, 1.0) * 32767.0) as i16),
        SampleFormat::I16 => typed!(i16, |s: i16| s),
        SampleFormat::U16 => typed!(u16, |s: u16| (s as i32 - 32768) as i16),
        other => anyhow::bail!("unsupported sample format {other:?}"),
    };
    Ok(stream)
}
