//! Generative "sigil" art — the deterministic ASCII emblem the web client draws for each track, ported
//! byte-for-byte from `helio-client/dist/app.js` (and the server's `art.js`). Same 32-bit FNV seed + the
//! same mulberry32 stream reproduce the identical grid here, so `helio sigil` previews exactly what the
//! Gallery shows and `--out` writes the matching PNG. Zero extra deps: the PNG uses stored (uncompressed)
//! zlib blocks, so no compression crate is pulled into the size-optimized binary.

use anyhow::{Context, Result};

const COLS: usize = 42;
const ROWS: usize = 16;
pub const STYLES: [&str; 6] = [
    "sigil-a",
    "sigil-b",
    "sigil-c",
    "sonar",
    "matrix",
    "spectrogram",
];

// ── deterministic hash + rng (identical bit-math to the JS: Math.imul → wrapping_mul, >>>0 → u32) ──
fn g_hash(s: &str) -> u32 {
    let mut h: u32 = 2166136261;
    // JS charCodeAt iterates UTF-16 code units, so match that exactly for non-ASCII seeds.
    for u in s.encode_utf16() {
        h ^= u as u32;
        h = h.wrapping_mul(16777619);
    }
    h
}

struct Rng {
    s: u32,
}
impl Rng {
    fn new(seed: u32) -> Self {
        Rng { s: seed }
    }
    fn next(&mut self) -> f64 {
        self.s = self.s.wrapping_add(0x6D2B_79F5);
        let mut t = (self.s ^ (self.s >> 15)).wrapping_mul(1 | self.s);
        t = t.wrapping_add((t ^ (t >> 7)).wrapping_mul(61 | t)) ^ t;
        ((t ^ (t >> 14)) as f64) / 4294967296.0
    }
}

const RAMP: [char; 6] = [' ', '·', '░', '▒', '▓', '█'];
// The JS uses the LITERAL 6.283 (a truncated τ), not std::f64::consts::TAU (6.283185…). To reproduce
// its art byte-for-byte the same truncated value must be used here — so this is deliberate, not sloppy.
#[allow(clippy::approx_constant)]
const TAU_JS: f64 = 6.283;
// JS Math.round is floor(x+0.5) (rounds .5 toward +∞) — Rust f64::round differs on negatives, so match JS.
fn js_round(x: f64) -> f64 {
    (x + 0.5).floor()
}
fn ramp(v: f64) -> char {
    let n = RAMP.len() as f64;
    let idx = js_round(v * (n - 1.0)).clamp(0.0, n - 1.0) as usize;
    RAMP[idx]
}
fn mir(c: char) -> char {
    match c {
        '╱' => '╲',
        '╲' => '╱',
        '┌' => '┐',
        '┐' => '┌',
        '└' => '┘',
        '┘' => '└',
        '├' => '┤',
        '┤' => '├',
        _ => c,
    }
}

fn sigil_a(rng: &mut Rng) -> Vec<Vec<char>> {
    let half = COLS.div_ceil(2);
    let ax = 0.6 + rng.next() * 1.7;
    let ay = 0.7 + rng.next() * 1.8;
    let px = rng.next() * TAU_JS;
    let py = rng.next() * TAU_JS;
    let cx = (half - 1) as f64;
    let cy = (ROWS as f64 - 1.0) / 2.0;
    let mut g = Vec::with_capacity(ROWS);
    for y in 0..ROWS {
        let mut line = vec![' '; COLS];
        for x in 0..half {
            let nx = (x as f64 - cx) / half as f64;
            let ny = ((y as f64 - cy) / ROWS as f64) * 2.6;
            let mut v = (nx * 3.1 * ax + px).sin() * (ny * 3.1 * ay + py).cos();
            v = (v + 1.0) / 2.0;
            v *= (1.0 - (nx * nx + ny * ny).sqrt() * 0.85).max(0.0);
            v += (rng.next() - 0.5) * 0.12;
            let ch = ramp(v * 1.2);
            line[x] = ch;
            line[COLS - 1 - x] = ch;
        }
        g.push(line);
    }
    g
}

fn sigil_b(rng: &mut Rng) -> Vec<Vec<char>> {
    let cx = (COLS as f64 - 1.0) / 2.0;
    let cy = (ROWS as f64 - 1.0) / 2.0;
    let lobes = 3.0 + (rng.next() * 4.0).floor();
    let rot = rng.next() * TAU_JS;
    let warp = 0.16 + rng.next() * 0.34;
    let maxd = (cx * cx + (cy * 2.4) * (cy * 2.4)).sqrt();
    let mut g = Vec::with_capacity(ROWS);
    for y in 0..ROWS {
        let mut line = Vec::with_capacity(COLS);
        for x in 0..COLS {
            let dx = x as f64 - cx;
            let dy = (y as f64 - cy) * 2.4;
            let mut d = (dx * dx + dy * dy).sqrt() / maxd;
            d *= 1.0 + (dy.atan2(dx) * lobes + rot).sin() * warp;
            line.push(ramp(1.0 - d + (rng.next() - 0.5) * 0.1));
        }
        g.push(line);
    }
    g
}

fn sigil_c(rng: &mut Rng) -> Vec<Vec<char>> {
    let half = COLS.div_ceil(2);
    let l = ['╱', '╲', '│', '─', '┼', '·', ' ', ' ', ' '];
    let mut g = Vec::with_capacity(ROWS);
    for _y in 0..ROWS {
        let mut line = vec![' '; COLS];
        for x in 0..half {
            // JS short-circuits the else branch, so the second rng() only runs when the first ≥ 0.46.
            let ch = if rng.next() < 0.46 {
                ' '
            } else {
                l[(rng.next() * l.len() as f64).floor() as usize]
            };
            line[x] = ch;
            line[COLS - 1 - x] = mir(ch);
        }
        g.push(line);
    }
    g
}

fn sonar(rng: &mut Rng) -> Vec<Vec<char>> {
    let cx = COLS as f64 * (0.32 + rng.next() * 0.36);
    let cy = ROWS as f64 * (0.34 + rng.next() * 0.32);
    let ring = 2.0 + rng.next() * 1.6;
    let sweep = rng.next() * TAU_JS;
    let mut g = Vec::with_capacity(ROWS);
    for y in 0..ROWS {
        let mut line = Vec::with_capacity(COLS);
        for x in 0..COLS {
            let dx = x as f64 - cx;
            let dy = (y as f64 - cy) * 2.2;
            let d = (dx * dx + dy * dy).sqrt();
            let mut ch = if d % ring < 0.82 { '·' } else { ' ' };
            let behind = (((sweep - dy.atan2(dx)) % TAU_JS) + TAU_JS) % TAU_JS;
            if behind < 1.15 {
                let b = 1.0 - behind / 1.15;
                ch = if b > 0.6 {
                    '▒'
                } else if b > 0.3 {
                    '░'
                } else if ch == ' ' {
                    '·'
                } else {
                    ch
                };
            }
            if d < 1.3 {
                ch = '◉';
            }
            line.push(ch);
        }
        g.push(line);
    }
    g
}

fn matrix(rng: &mut Rng) -> Vec<Vec<char>> {
    let mut heads: Vec<(i32, i32)> = Vec::with_capacity(COLS);
    let base = (ROWS as f64 * 0.3).floor() as i32;
    for _x in 0..COLS {
        let hy = (rng.next() * ROWS as f64 * 1.6).floor() as i32 - base;
        let len = 3 + (rng.next() * ROWS as f64).floor() as i32;
        heads.push((hy, len));
    }
    let mut g = Vec::with_capacity(ROWS);
    for y in 0..ROWS {
        let mut line = Vec::with_capacity(COLS);
        for &(hy, len) in &heads {
            let dist = hy - y as i32;
            let ch = if dist == 0 {
                '█'
            } else if dist > 0 && dist < len {
                let b = 1.0 - dist as f64 / len as f64;
                if b > 0.62 {
                    '▓'
                } else if b > 0.36 {
                    '▒'
                } else if b > 0.14 {
                    '░'
                } else {
                    '·'
                }
            } else {
                ' '
            };
            line.push(ch);
        }
        g.push(line);
    }
    g
}

fn spectro(rng: &mut Rng) -> Vec<Vec<char>> {
    let f1 = 1.0 + rng.next() * 3.0;
    let f2 = 2.0 + rng.next() * 5.0;
    let ph = rng.next() * TAU_JS;
    let mut bands = vec![0.0f64; COLS];
    for (x, band) in bands.iter_mut().enumerate() {
        let t = x as f64 / COLS as f64;
        let a = 0.5 + 0.5 * (t * TAU_JS * f1 + ph).sin() * (t * TAU_JS * f2 * 0.5).cos();
        *band = (a.abs() * (0.55 + rng.next() * 0.45)).clamp(0.06, 1.0);
    }
    let mut g = Vec::with_capacity(ROWS);
    for y in 0..ROWS {
        let mut line = Vec::with_capacity(COLS);
        for &band in &bands {
            let top = 1.0 - band;
            let yy = y as f64 / (ROWS as f64 - 1.0);
            let ch = if yy >= top {
                let dep = (yy - top) / (1.0 - top + 1e-6);
                if dep > 0.66 {
                    '█'
                } else if dep > 0.33 {
                    '▓'
                } else {
                    '▒'
                }
            } else {
                ' '
            };
            line.push(ch);
        }
        g.push(line);
    }
    g
}

fn normalize_style(style: &str) -> &'static str {
    STYLES
        .iter()
        .copied()
        .find(|&s| s == style)
        .unwrap_or("sigil-a")
}

/// The reproducible 8-hex id that produced a sigil (the shareable "hash that leads to the image").
pub fn art_hash(seed: &str, style: &str) -> String {
    let s = normalize_style(style);
    format!("{:08x}", g_hash(&format!("{seed}|{s}")))
}

fn generate(seed: &str, style: &str) -> Vec<Vec<char>> {
    let s = normalize_style(style);
    let mut rng = Rng::new(g_hash(&format!("{seed}|{s}")));
    match s {
        "sigil-b" => sigil_b(&mut rng),
        "sigil-c" => sigil_c(&mut rng),
        "sonar" => sonar(&mut rng),
        "matrix" => matrix(&mut rng),
        "spectrogram" => spectro(&mut rng),
        _ => sigil_a(&mut rng),
    }
}

// ── PNG (RGB, 8-bit) with stored zlib blocks — no compression crate ────────────────────────────────
fn bright(ch: char) -> f64 {
    match ch {
        ' ' => 0.0,
        '·' => 0.2,
        '░' => 0.4,
        '▒' => 0.6,
        '▓' => 0.82,
        '█' => 1.0,
        '◉' => 0.95,
        '╱' | '╲' => 0.6,
        '│' | '─' => 0.55,
        '┼' => 0.7,
        _ => 0.5,
    }
}

fn hex_rgb(hex: &str) -> (u8, u8, u8) {
    let h = hex.trim_start_matches('#');
    if h.len() == 6 {
        if let Ok(n) = u32::from_str_radix(h, 16) {
            return (
                ((n >> 16) & 255) as u8,
                ((n >> 8) & 255) as u8,
                (n & 255) as u8,
            );
        }
    }
    (88, 217, 160)
}

fn crc32(buf: &[u8]) -> u32 {
    let mut c: u32 = 0xFFFF_FFFF;
    for &b in buf {
        c ^= b as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 {
                0xEDB8_8320 ^ (c >> 1)
            } else {
                c >> 1
            };
        }
    }
    c ^ 0xFFFF_FFFF
}

fn adler32(data: &[u8]) -> u32 {
    let (mut a, mut b): (u32, u32) = (1, 0);
    for &byte in data {
        a = (a + byte as u32) % 65521;
        b = (b + a) % 65521;
    }
    (b << 16) | a
}

/// zlib stream wrapping `raw` in stored (BTYPE=00) DEFLATE blocks — valid, just uncompressed.
fn zlib_store(raw: &[u8]) -> Vec<u8> {
    let mut out = vec![0x78, 0x01]; // CMF/FLG: 0x7801 is a valid zlib header (check ÷31 == 0)
    let mut i = 0;
    if raw.is_empty() {
        out.extend_from_slice(&[1, 0, 0, 0xff, 0xff]); // one final empty stored block
    }
    while i < raw.len() {
        let block = (raw.len() - i).min(0xFFFF);
        let bfinal = if i + block >= raw.len() { 1u8 } else { 0u8 };
        out.push(bfinal); // BFINAL + BTYPE=00
        let len = block as u16;
        out.extend_from_slice(&len.to_le_bytes());
        out.extend_from_slice(&(!len).to_le_bytes());
        out.extend_from_slice(&raw[i..i + block]);
        i += block;
    }
    out.extend_from_slice(&adler32(raw).to_be_bytes());
    out
}

fn png_chunk(typ: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(12 + data.len());
    out.extend_from_slice(&(data.len() as u32).to_be_bytes());
    out.extend_from_slice(typ);
    out.extend_from_slice(data);
    let mut crc_in = Vec::with_capacity(4 + data.len());
    crc_in.extend_from_slice(typ);
    crc_in.extend_from_slice(data);
    out.extend_from_slice(&crc32(&crc_in).to_be_bytes());
    out
}

fn encode_png(w: usize, h: usize, rgb: &[u8]) -> Vec<u8> {
    let stride = 1 + w * 3;
    let mut raw = vec![0u8; h * stride];
    for y in 0..h {
        // raw[y*stride] stays 0 = filter type "None"
        raw[y * stride + 1..y * stride + 1 + w * 3]
            .copy_from_slice(&rgb[y * w * 3..(y + 1) * w * 3]);
    }
    let idat = zlib_store(&raw);
    let mut ihdr = Vec::with_capacity(13);
    ihdr.extend_from_slice(&(w as u32).to_be_bytes());
    ihdr.extend_from_slice(&(h as u32).to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]); // 8-bit, RGB, deflate, no filter, no interlace
    let mut png = Vec::new();
    png.extend_from_slice(&[137, 80, 78, 71, 13, 10, 26, 10]);
    png.extend_from_slice(&png_chunk(b"IHDR", &ihdr));
    png.extend_from_slice(&png_chunk(b"IDAT", &idat));
    png.extend_from_slice(&png_chunk(b"IEND", &[]));
    png
}

/// Render a sigil to a PNG byte buffer. cell = pixels per grid cell.
pub fn render_png(seed: &str, style: &str, color: &str, cell: usize) -> Vec<u8> {
    let grid = generate(seed, style);
    let (r, g, b) = hex_rgb(color);
    let (w, h) = (COLS * cell, ROWS * cell);
    let mut rgb = vec![0u8; w * h * 3];
    for (ry, row) in grid.iter().enumerate() {
        for (rx, &ch) in row.iter().enumerate() {
            let v = bright(ch);
            if v <= 0.0 {
                continue;
            }
            let (cr, cg, cb) = (
                (r as f64 * v).round() as u8,
                (g as f64 * v).round() as u8,
                (b as f64 * v).round() as u8,
            );
            for py in 0..cell {
                let mut o = ((ry * cell + py) * w + rx * cell) * 3;
                for _px in 0..cell {
                    rgb[o] = cr;
                    rgb[o + 1] = cg;
                    rgb[o + 2] = cb;
                    o += 3;
                }
            }
        }
    }
    encode_png(w, h, &rgb)
}

// ── command ────────────────────────────────────────────────────────────────────────────────────────
use crate::ui;

/// `helio sigil` — preview a track's sigil in the terminal and/or write its PNG.
pub fn run(
    seed: &str,
    style: &str,
    color: &str,
    all: bool,
    out: Option<&str>,
    cell: usize,
) -> Result<()> {
    let (r, g, b) = hex_rgb(color);
    let styles: Vec<&str> = if all {
        STYLES.to_vec()
    } else {
        vec![normalize_style(style)]
    };

    print!("{}", crate::banner());
    for s in &styles {
        let hash = art_hash(seed, s);
        println!(
            "\n{}",
            crate::rule(&format!("SIGIL · {}", s.to_uppercase()))
        );
        println!(
            "   {} {}    {} {}",
            ui::grey("seed"),
            ui::white(seed),
            ui::grey("id"),
            ui::accent(&hash),
        );
        println!();
        for row in generate(seed, s) {
            let line: String = row.into_iter().collect();
            println!("   {}", ui::truecolor(r, g, b, &line));
        }
    }

    if let Some(path) = out {
        let base = path.strip_suffix(".png").unwrap_or(path);
        println!();
        for s in &styles {
            let file = if all {
                format!("{base}-{s}.png")
            } else {
                format!("{base}.png")
            };
            let png = render_png(seed, s, color, cell);
            std::fs::write(&file, &png).with_context(|| format!("write {file}"))?;
            println!(
                "   {}  {}  {}",
                ui::green("✓ png"),
                ui::white(&file),
                ui::dim(&format!(
                    "{}×{} · {}",
                    COLS * cell,
                    ROWS * cell,
                    human(png.len())
                )),
            );
        }
    }
    Ok(())
}

fn human(bytes: usize) -> String {
    if bytes >= 1_000_000 {
        format!("{:.1} MB", bytes as f64 / 1e6)
    } else {
        format!("{} KB", bytes / 1000)
    }
}
