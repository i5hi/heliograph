//! Opus encoder + a minimal, correct Ogg muxer (Encoder trait impl).
//!
//! We hand-roll Ogg page framing rather than pull a crate: it's ~40 lines, keeps the
//! dependency surface tiny (Pi-friendly), and gives us exact control over BOS/EOS,
//! serials and granule positions across reconnects.

use crate::state::{CHANNELS, FRAME_INTERLEAVED, FRAME_SAMPLES, SAMPLE_RATE};
use helio_core::pipeline::Encoder as EncoderTrait;
use opus::{Application, Channels, Encoder as OpusEnc};

/// libopus lookahead at 48 kHz is ~120–312 samples; 312 is the safe standard pre-skip.
const PRE_SKIP: u16 = 312;

pub struct OpusEncoder {
    enc: OpusEnc,
    /// Interleaved i16 not yet aligned to a full 20 ms frame.
    leftover: Vec<i16>,
    scratch: Vec<u8>,
    serial: u32,
    seq: u32,
    granule: u64,
}

impl OpusEncoder {
    pub fn new(bitrate: i32) -> anyhow::Result<Self> {
        let mut enc = OpusEnc::new(SAMPLE_RATE, Channels::Stereo, Application::Audio)
            .map_err(|e| anyhow::anyhow!("opus init: {e}"))?;
        enc.set_bitrate(opus::Bitrate::Bits(bitrate))
            .map_err(|e| anyhow::anyhow!("opus bitrate: {e}"))?;
        Ok(Self {
            enc,
            leftover: Vec::with_capacity(FRAME_INTERLEAVED * 2),
            scratch: vec![0u8; 4000],
            serial: 0x4845_4C49, // "HELI" — bumped each reset for a fresh logical stream
            seq: 0,
            granule: 0,
        })
    }

    fn opus_head() -> Vec<u8> {
        let mut h = Vec::with_capacity(19);
        h.extend_from_slice(b"OpusHead");
        h.push(1); // version
        h.push(CHANNELS as u8);
        h.extend_from_slice(&PRE_SKIP.to_le_bytes());
        h.extend_from_slice(&SAMPLE_RATE.to_le_bytes()); // input sample rate (informational)
        h.extend_from_slice(&0u16.to_le_bytes()); // output gain
        h.push(0); // channel mapping family 0 (mono/stereo)
        h
    }

    fn opus_tags() -> Vec<u8> {
        let vendor = b"heliod";
        let mut t = Vec::with_capacity(20);
        t.extend_from_slice(b"OpusTags");
        t.extend_from_slice(&(vendor.len() as u32).to_le_bytes());
        t.extend_from_slice(vendor);
        t.extend_from_slice(&0u32.to_le_bytes()); // 0 user comments
        t
    }
}

impl EncoderTrait for OpusEncoder {
    fn content_type(&self) -> &'static str {
        "application/ogg"
    }

    fn stream_headers(&mut self) -> anyhow::Result<Vec<u8>> {
        // OpusHead alone on its BOS page, then OpusTags on the next page.
        let mut out = write_page(&Self::opus_head(), self.serial, self.seq, 0, true, false);
        self.seq += 1;
        out.extend_from_slice(&write_page(&Self::opus_tags(), self.serial, self.seq, 0, false, false));
        self.seq += 1;
        Ok(out)
    }

    fn encode(&mut self, interleaved: &[i16]) -> anyhow::Result<Vec<u8>> {
        self.leftover.extend_from_slice(interleaved);
        let mut out = Vec::new();
        while self.leftover.len() >= FRAME_INTERLEAVED {
            let frame: Vec<i16> = self.leftover.drain(..FRAME_INTERLEAVED).collect();
            let n = self
                .enc
                .encode(&frame, &mut self.scratch)
                .map_err(|e| anyhow::anyhow!("opus encode: {e}"))?;
            self.granule += FRAME_SAMPLES as u64;
            out.extend_from_slice(&write_page(
                &self.scratch[..n],
                self.serial,
                self.seq,
                self.granule,
                false,
                false,
            ));
            self.seq += 1;
        }
        Ok(out)
    }

    fn trailer(&mut self) -> Vec<u8> {
        // Zero-length EOS page to close the logical stream cleanly.
        let p = write_page(&[], self.serial, self.seq, self.granule, false, true);
        self.seq += 1;
        p
    }

    fn reset(&mut self) {
        self.leftover.clear();
        self.serial = self.serial.wrapping_add(1);
        self.seq = 0;
        self.granule = 0;
    }
}

/// Ogg CRC-32: polynomial 0x04C11DB7, MSB-first, no reflection, no final xor.
fn ogg_crc(data: &[u8]) -> u32 {
    let mut crc: u32 = 0;
    for &b in data {
        crc ^= (b as u32) << 24;
        for _ in 0..8 {
            crc = if crc & 0x8000_0000 != 0 {
                (crc << 1) ^ 0x04C1_1DB7
            } else {
                crc << 1
            };
        }
    }
    crc
}

/// Frame a single packet into one Ogg page.
fn write_page(payload: &[u8], serial: u32, seq: u32, granule: u64, bos: bool, eos: bool) -> Vec<u8> {
    // Lacing: 255-byte segments, final < 255 (or an explicit 0 for empty packet).
    let mut segs: Vec<u8> = Vec::new();
    let mut rem = payload.len();
    loop {
        if rem >= 255 {
            segs.push(255);
            rem -= 255;
        } else {
            segs.push(rem as u8);
            break;
        }
    }

    let mut header_type = 0u8;
    if bos {
        header_type |= 0x02;
    }
    if eos {
        header_type |= 0x04;
    }

    let mut page = Vec::with_capacity(27 + segs.len() + payload.len());
    page.extend_from_slice(b"OggS");
    page.push(0); // stream structure version
    page.push(header_type);
    page.extend_from_slice(&granule.to_le_bytes());
    page.extend_from_slice(&serial.to_le_bytes());
    page.extend_from_slice(&seq.to_le_bytes());
    page.extend_from_slice(&[0u8; 4]); // CRC placeholder
    page.push(segs.len() as u8);
    page.extend_from_slice(&segs);
    page.extend_from_slice(payload);

    let crc = ogg_crc(&page);
    page[22..26].copy_from_slice(&crc.to_le_bytes());
    page
}
