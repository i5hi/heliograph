//! Raw-PCM-over-stdin capture: heliod reads interleaved S16LE @ 48 kHz stereo from
//! stdin and feeds the pipeline. This keeps heliod completely device-agnostic — the OS
//! owns all hardware specifics. Feed it from anything, e.g.:
//!
//!   arecord -D <device> -f S16_LE -c 2 -r 48000 -t raw | heliod   (input_device="stdin")
//!
//! Used when the session's `input_device` is "stdin" (or HELIO_AUDIO=stdin). This is the
//! reliable path on hosts where cpal's ALSA capture doesn't cooperate with a driver.

use crate::state::{load_f32, store_f32, AppState, PCM_CAP};
use std::io::Read;
use std::sync::Arc;
use std::time::Duration;

pub fn run(state: Arc<AppState>) {
    {
        let mut d = state.devices.lock().unwrap();
        d.active = "stdin".into();
        d.present = true;
    }
    tracing::info!("audio: reading S16LE/48k/stereo PCM from stdin");
    let mut stdin = std::io::stdin().lock();
    let mut buf = [0u8; 8192]; // multiple of 4 (2ch × 2 bytes)
    let mut eof_hits = 0u32;

    loop {
        match stdin.read(&mut buf) {
            Ok(0) => {
                // Pipe closed (e.g. arecord died). Exit so systemd restarts the whole pipe.
                eof_hits += 1;
                if eof_hits > 3 {
                    tracing::error!("audio: stdin EOF — input pipe closed, exiting for restart");
                    std::process::exit(1);
                }
                std::thread::sleep(Duration::from_millis(200));
            }
            Ok(n) => {
                eof_hits = 0;
                let n = n - (n % 4);
                let mut q = state.shared.pcm.lock().unwrap();
                let (mut peak, mut sumsq, mut cnt) = (0f32, 0f64, 0usize);
                let mut i = 0;
                while i + 3 < n {
                    let l = i16::from_le_bytes([buf[i], buf[i + 1]]);
                    let r = i16::from_le_bytes([buf[i + 2], buf[i + 3]]);
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
                    i += 4;
                }
                drop(q);
                if cnt > 0 {
                    store_f32(&state.shared.level, ((sumsq / cnt as f64).sqrt() as f32).min(1.0));
                }
                let decayed = (load_f32(&state.shared.peak) * 0.92).max(peak).min(1.0);
                store_f32(&state.shared.peak, decayed);
                state.shared.mark_audio();
            }
            Err(e) => {
                tracing::warn!("audio: stdin read error: {e}");
                std::thread::sleep(Duration::from_millis(200));
            }
        }
    }
}
