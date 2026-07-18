//! helio — drive the heliod daemon over its loopback control API.
//! A small, colourful TUI-style console.

use anyhow::{Context, Result};
use base64::Engine;
use clap::{Parser, Subcommand, ValueEnum};
use helio_core::api::{Ack, DevicesResponse, ImageUpdate, MetaUpdate, RegisterRequest, Status};
use std::io::IsTerminal;
use std::path::{Path, PathBuf};

mod sigil;

// ───────────────────────── palette (zero-dep ANSI) ─────────────────────────
mod ui {
    use std::io::IsTerminal;
    use std::sync::OnceLock;

    static ON: OnceLock<bool> = OnceLock::new();
    pub fn on() -> bool {
        *ON.get_or_init(|| {
            std::env::var_os("NO_COLOR").is_none() && std::io::stdout().is_terminal()
        })
    }
    pub fn paint(code: &str, s: &str) -> String {
        if on() {
            format!("\x1b[{code}m{s}\x1b[0m")
        } else {
            s.to_string()
        }
    }
    pub const ACCENT: &str = "38;5;148"; // yellow-green (#b0e233-ish)
    pub const GREEN: &str = "38;5;84";
    pub const AMBER: &str = "38;5;214";
    pub const RED: &str = "38;5;203";
    pub const WHITE: &str = "97";
    pub const GREY: &str = "38;5;245";
    pub const DIMC: &str = "2";
    pub const BOLD: &str = "1";

    pub fn accent(s: &str) -> String {
        paint(ACCENT, s)
    }
    pub fn green(s: &str) -> String {
        paint(GREEN, s)
    }
    pub fn amber(s: &str) -> String {
        paint(AMBER, s)
    }
    pub fn red(s: &str) -> String {
        paint(RED, s)
    }
    pub fn white(s: &str) -> String {
        paint(WHITE, s)
    }
    pub fn grey(s: &str) -> String {
        paint(GREY, s)
    }
    pub fn dim(s: &str) -> String {
        paint(DIMC, s)
    }
    pub fn bold(s: &str) -> String {
        paint(BOLD, s)
    }

    /// 24-bit truecolor foreground — for tinting sigil art to a track's exact accent hex.
    pub fn truecolor(r: u8, g: u8, b: u8, s: &str) -> String {
        if on() {
            format!("\x1b[38;2;{r};{g};{b}m{s}\x1b[0m")
        } else {
            s.to_string()
        }
    }
}

#[derive(Parser)]
#[command(
    name = "helio",
    version,
    about = "Control the heliod broadcast appliance"
)]
struct Cli {
    #[arg(long, env = "HELIO_URL", default_value = "http://127.0.0.1:4777")]
    url: String,
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Show the daemon dashboard (add --watch for a live view).
    Status {
        #[arg(long, short)]
        watch: bool,
        /// Compact single-card view, sized for a small screen.
        #[arg(long)]
        alt: bool,
    },
    /// List audio input devices.
    Devices,
    /// Select the input device ("default" to follow the system default).
    Device { name: String },
    /// Register this device with a backend (one-time, single-use invite).
    Register {
        #[arg(long)]
        invite: String,
        #[arg(long)]
        artist: String,
        #[arg(long)]
        server: Option<String>,
    },
    /// Go on air.
    Start,
    /// Go off air.
    Stop,
    /// Toggle presence (no arg), or set it explicitly: `presence away` / `presence on-deck`.
    Presence {
        #[arg(value_enum)]
        state: Option<PresenceArg>,
    },
    /// Update channel/track metadata (only the flags you pass change).
    Meta(MetaArgs),
    /// Update the image and its transforms.
    Image(ImageArgs),
    /// Publish + manage collections (albums of recordings) on the server, for the Gallery.
    Collection {
        #[command(subcommand)]
        cmd: CollectionCmd,
    },
    /// Render a track's generative sigil — preview in the terminal, or --out a PNG.
    Sigil {
        /// Seed string (usually the track name). The art is deterministic from this + the style.
        seed: String,
        /// Style: sigil-a | sigil-b | sigil-c | sonar | matrix | spectrogram.
        #[arg(long, default_value = "sigil-a")]
        style: String,
        /// Accent colour as #rrggbb — tints the preview and the PNG.
        #[arg(long, default_value = "#57d9a0")]
        color: String,
        /// Render every style instead of just one.
        #[arg(long)]
        all: bool,
        /// Write PNG(s) here (adds .png; with --all writes one <path>-<style>.png each).
        #[arg(long)]
        out: Option<String>,
        /// PNG pixels per grid cell (default 22 → 924×352).
        #[arg(long, default_value_t = 22)]
        cell: usize,
    },
}

#[derive(Subcommand)]
enum CollectionCmd {
    /// List all collections on the server.
    Ls {
        #[command(flatten)]
        cfg: ServerCfg,
    },
    /// Create an empty collection (or reuse an existing one with the same name).
    Create {
        #[arg(long)]
        name: String,
        /// Optional collection description / blurb.
        #[arg(long)]
        description: Option<String>,
        /// Generative track-art style: sigil-a | sigil-b | sigil-c | sonar | matrix | spectrogram.
        #[arg(long)]
        art: Option<String>,
        #[command(flatten)]
        cfg: ServerCfg,
    },
    /// Upload a folder of tracks as a collection (creates or reuses it by name).
    Upload {
        /// Folder containing the audio files.
        folder: String,
        #[arg(long)]
        name: String,
        /// Optional collection description / blurb.
        #[arg(long)]
        description: Option<String>,
        /// Generative track-art style: sigil-a | sigil-b | sigil-c | sonar | matrix | spectrogram.
        #[arg(long)]
        art: Option<String>,
        /// Only upload files of this extension (e.g. `mp3`). Default: any of mp3/flac/wav.
        #[arg(long)]
        ext: Option<String>,
        /// Also set the collection cover from this image file.
        #[arg(long)]
        cover: Option<String>,
        #[command(flatten)]
        cfg: ServerCfg,
    },
    /// Add one or more track files to an existing collection.
    Add {
        id: String,
        #[arg(required = true)]
        files: Vec<String>,
        #[command(flatten)]
        cfg: ServerCfg,
    },
    /// Delete a whole collection, or just one track with --track.
    Rm {
        id: String,
        #[arg(long)]
        track: Option<String>,
        #[command(flatten)]
        cfg: ServerCfg,
    },
    /// Rename a collection (--name), set its --description / --art, or retitle a track (--track --title).
    Edit {
        id: String,
        #[arg(long)]
        name: Option<String>,
        #[arg(long)]
        description: Option<String>,
        /// Generative track-art style: sigil-a | sigil-b | sigil-c | sonar | matrix | spectrogram.
        #[arg(long)]
        art: Option<String>,
        #[arg(long)]
        track: Option<String>,
        #[arg(long)]
        title: Option<String>,
        #[command(flatten)]
        cfg: ServerCfg,
    },
    /// Set the collection cover image.
    Cover {
        id: String,
        image: String,
        #[command(flatten)]
        cfg: ServerCfg,
    },
}

/// Where + how to reach the SERVER for collection commands. Unlike the other subcommands (which drive
/// the local daemon), these talk straight to the backend. Each field falls back to the persisted
/// session, so a registered device needs no flags at all.
#[derive(clap::Args)]
struct ServerCfg {
    /// Server base URL (default: your registered server, or $HELIO_SERVER).
    #[arg(long, env = "HELIO_SERVER")]
    server: Option<String>,
    /// Bearer token (default: your session's snapshot token, or $HELIO_TOKEN).
    #[arg(long, env = "HELIO_TOKEN")]
    token: Option<String>,
    /// Artist name (default: your registered artist, or $HELIO_ARTIST).
    #[arg(long, env = "HELIO_ARTIST")]
    artist: Option<String>,
}

#[derive(Copy, Clone, ValueEnum)]
enum PresenceArg {
    OnDeck,
    Away,
}

#[derive(clap::Args)]
struct MetaArgs {
    #[arg(long)]
    channel: Option<String>,
    #[arg(long)]
    title: Option<String>,
    #[arg(long)]
    note: Option<String>,
    #[arg(long)]
    date: Option<String>,
    #[arg(long)]
    episode: Option<u32>,
    #[arg(long)]
    font: Option<String>,
    #[arg(long)]
    accent: Option<String>,
    #[arg(long)]
    btc: Option<String>,
    #[arg(long)]
    ln: Option<String>,
    #[arg(long)]
    lq: Option<String>,
}

#[derive(clap::Args)]
struct ImageArgs {
    #[arg(long)]
    path: Option<String>,
    #[arg(long)]
    clear: bool,
    #[arg(long, allow_hyphen_values = true)]
    pan_x: Option<f32>,
    #[arg(long, allow_hyphen_values = true)]
    pan_y: Option<f32>,
    #[arg(long)]
    opacity: Option<f32>,
    #[arg(long)]
    blur: Option<f32>,
    #[arg(long)]
    scale: Option<f32>,
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    let http = reqwest::blocking::Client::new();
    let url = |p: &str| format!("{}{p}", cli.url.trim_end_matches('/'));

    match cli.cmd {
        Cmd::Status { watch, alt } => {
            let render = if alt {
                render_status_alt
            } else {
                render_status
            };
            if watch {
                loop {
                    print!("\x1b[2J\x1b[3J\x1b[H"); // clear
                    match http
                        .get(url("/status"))
                        .send()
                        .and_then(|r| r.json::<Status>())
                    {
                        Ok(s) => print!("{}", render(&s)),
                        Err(e) => println!("{}  {e}", ui::red("daemon unreachable")),
                    }
                    println!("\n  {}", ui::dim("live — ctrl-c to exit"));
                    std::thread::sleep(std::time::Duration::from_secs(1));
                }
            } else {
                let s: Status = http
                    .get(url("/status"))
                    .send()?
                    .json()
                    .context("daemon unreachable")?;
                print!("{}", render(&s));
            }
        }
        Cmd::Devices => {
            let d: DevicesResponse = http.get(url("/devices")).send()?.json()?;
            print_devices(&d);
        }
        Cmd::Device { name } => ack(
            "input",
            http.post(url("/device"))
                .json(&serde_json::json!({ "device": name }))
                .send()?,
        )?,
        Cmd::Register {
            invite,
            artist,
            server,
        } => {
            let body = RegisterRequest {
                invite_code: invite,
                artist,
                server,
            };
            ack("register", http.post(url("/register")).json(&body).send()?)?;
        }
        Cmd::Start => ack("on air", http.post(url("/broadcast/start")).send()?)?,
        Cmd::Stop => ack("off air", http.post(url("/broadcast/stop")).send()?)?,
        Cmd::Presence { state } => {
            // No arg → toggle: read the current presence and flip it.
            let away = match state {
                Some(PresenceArg::Away) => true,
                Some(PresenceArg::OnDeck) => false,
                None => {
                    let s: Status = http
                        .get(url("/status"))
                        .send()?
                        .json()
                        .context("daemon unreachable")?;
                    !s.away
                }
            };
            let a: Ack = http
                .post(url("/presence"))
                .json(&serde_json::json!({ "away": away }))
                .send()?
                .json()
                .context("bad daemon response")?;
            if a.ok {
                let label = if away {
                    ui::amber("AWAY")
                } else {
                    ui::green("ON DECK")
                };
                println!("  {}  presence → {}", ui::green("✓"), label);
            } else {
                eprintln!("  {}  {}", ui::red("✗"), a.error.unwrap_or_default());
                std::process::exit(1);
            }
        }
        Cmd::Meta(m) => {
            let body = MetaUpdate {
                channel: m.channel,
                title: m.title,
                note: m.note,
                date: m.date,
                episode: m.episode,
                font: m.font,
                accent: m.accent,
                btc: m.btc,
                ln: m.ln,
                lq: m.lq,
            };
            ack("metadata", http.post(url("/meta")).json(&body).send()?)?;
        }
        Cmd::Image(i) => {
            let body = ImageUpdate {
                path: i.path,
                clear: if i.clear { Some(true) } else { None },
                pan_x: i.pan_x,
                pan_y: i.pan_y,
                opacity: i.opacity,
                blur: i.blur,
                scale: i.scale,
            };
            ack("image", http.post(url("/image")).json(&body).send()?)?;
        }
        Cmd::Collection { cmd } => run_collection(cmd)?,
        Cmd::Sigil {
            seed,
            style,
            color,
            all,
            out,
            cell,
        } => sigil::run(&seed, &style, &color, all, out.as_deref(), cell)?,
    }
    Ok(())
}

// ───────────────────────── collections (server-direct) ─────────────────────────

/// Resolved server target: flags → environment → persisted session, in that order.
struct Cfg {
    server: String,
    token: String,
    artist: String,
}

fn resolve_cfg(o: &ServerCfg, need_auth: bool) -> Result<Cfg> {
    let sess = helio_core::Session::load(&helio_core::session_path(&helio_core::home()))
        .unwrap_or_default();
    let pick = |flag: &Option<String>, fallback: String| {
        flag.clone()
            .filter(|s| !s.trim().is_empty())
            .unwrap_or(fallback)
    };
    let server = pick(&o.server, sess.registration.server)
        .trim_end_matches('/')
        .to_string();
    let token = pick(&o.token, sess.broadcast.snapshot_token);
    let artist = pick(&o.artist, sess.artist);
    if server.is_empty() {
        anyhow::bail!("no server — pass --server or register this device first");
    }
    if need_auth && token.is_empty() {
        anyhow::bail!("no token — pass --token (or $HELIO_TOKEN), or register this device first");
    }
    Ok(Cfg {
        server,
        token,
        artist,
    })
}

/// Read a server response, returning its JSON on success or a clean error otherwise.
fn ok_json(resp: reqwest::blocking::Response) -> Result<serde_json::Value> {
    let status = resp.status();
    let v: serde_json::Value = resp.json().unwrap_or(serde_json::Value::Null);
    if status.is_success() && v.get("error").is_none() {
        Ok(v)
    } else {
        let msg = v
            .get("error")
            .and_then(|e| e.as_str())
            .map(|s| s.to_string())
            .unwrap_or_else(|| format!("HTTP {}", status.as_u16()));
        anyhow::bail!(msg)
    }
}

/// Create-or-reuse a collection by name (with an optional description + art style) → its id.
fn create_collection(
    http: &reqwest::blocking::Client,
    c: &Cfg,
    name: &str,
    description: &str,
    art: &str,
) -> Result<String> {
    let mut body = serde_json::json!({ "artist": c.artist, "name": name });
    if !description.is_empty() {
        body["description"] = serde_json::json!(description);
    }
    if !art.is_empty() {
        body["artStyle"] = serde_json::json!(art);
    }
    let v = ok_json(
        http.post(format!("{}/collections", c.server))
            .header("Authorization", format!("Bearer {}", c.token))
            .json(&body)
            .send()?,
    )?;
    Ok(v["id"]
        .as_str()
        .context("server returned no collection id")?
        .to_string())
}

/// Stream one audio file to a collection. Track name = the file's stem; ext drives the stored format.
/// Retries transport failures (dropped connections on flaky links) up to 3 times; a real HTTP error
/// response (413 too large, 429 quota, …) surfaces immediately without retry.
fn upload_track(
    http: &reqwest::blocking::Client,
    c: &Cfg,
    id: &str,
    path: &Path,
) -> Result<serde_json::Value> {
    let name = path
        .file_stem()
        .and_then(|s| s.to_str())
        .context("bad filename")?
        .to_string();
    let ext = path
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_lowercase();
    let name_b64 = base64::engine::general_purpose::STANDARD.encode(name.as_bytes());
    let url = format!("{}/collections/{}/tracks", c.server, id);
    let mut attempt = 0u32;
    loop {
        attempt += 1;
        // Open the file fresh each attempt (the streaming Body consumes it).
        let file = std::fs::File::open(path).with_context(|| format!("open {}", path.display()))?;
        let sent = http
            .post(&url)
            .header("Authorization", format!("Bearer {}", c.token))
            .header("X-Track-Name", &name_b64)
            .header("X-Track-Ext", &ext)
            .header("Content-Type", "application/octet-stream")
            .body(reqwest::blocking::Body::from(file))
            .send();
        match sent {
            Ok(resp) => return ok_json(resp), // HTTP round-trip completed — 2xx ok, 4xx/5xx → error, no retry
            Err(_e) if attempt < 3 => {
                std::thread::sleep(std::time::Duration::from_secs(2)); // transport drop → back off + retry
            }
            Err(e) => return Err(anyhow::Error::new(e).context("upload failed after 3 attempts")),
        }
    }
}

fn set_cover(http: &reqwest::blocking::Client, c: &Cfg, id: &str, image: &Path) -> Result<()> {
    let ext = image
        .extension()
        .and_then(|s| s.to_str())
        .unwrap_or("")
        .to_lowercase();
    let ct = match ext.as_str() {
        "png" => "image/png",
        "webp" => "image/webp",
        _ => "image/jpeg",
    };
    let bytes = std::fs::read(image).with_context(|| format!("read {}", image.display()))?;
    ok_json(
        http.put(format!("{}/collections/{}/cover", c.server, id))
            .header("Authorization", format!("Bearer {}", c.token))
            .header("Content-Type", ct)
            .body(bytes)
            .send()?,
    )?;
    Ok(())
}

fn run_collection(cmd: CollectionCmd) -> Result<()> {
    // No global timeout — track uploads can be large and slow; control calls are quick regardless.
    let http = reqwest::blocking::Client::new();
    match cmd {
        CollectionCmd::Ls { cfg } => {
            let c = resolve_cfg(&cfg, false)?;
            let v = ok_json(http.get(format!("{}/collections", c.server)).send()?)?;
            print_collections(&v);
        }
        CollectionCmd::Create {
            name,
            description,
            art,
            cfg,
        } => {
            let c = resolve_cfg(&cfg, true)?;
            let id = create_collection(
                &http,
                &c,
                &name,
                description.as_deref().unwrap_or(""),
                art.as_deref().unwrap_or(""),
            )?;
            println!(
                "  {}  {}  {}",
                ui::green("✓"),
                ui::white(&name),
                ui::dim(&id)
            );
        }
        CollectionCmd::Upload {
            folder,
            name,
            description,
            art,
            ext,
            cover,
            cfg,
        } => {
            let c = resolve_cfg(&cfg, true)?;
            let want = ext.map(|e| e.trim_start_matches('.').to_lowercase());
            let mut files: Vec<PathBuf> = std::fs::read_dir(&folder)
                .with_context(|| format!("read folder {folder}"))?
                .filter_map(|e| e.ok().map(|e| e.path()))
                .filter(|p| p.is_file())
                .filter(|p| {
                    let e = p
                        .extension()
                        .and_then(|s| s.to_str())
                        .unwrap_or("")
                        .to_lowercase();
                    let is_audio = matches!(e.as_str(), "mp3" | "flac" | "wav");
                    let ok_filter = want.as_ref().is_none_or(|w| &e == w);
                    is_audio && ok_filter
                })
                .collect();
            files.sort();
            if files.is_empty() {
                anyhow::bail!("no matching audio files in {folder} (looked for mp3/flac/wav)");
            }
            let id = create_collection(
                &http,
                &c,
                &name,
                description.as_deref().unwrap_or(""),
                art.as_deref().unwrap_or(""),
            )?;
            print!("{}", banner());
            println!("\n{}", rule("UPLOAD"));
            println!("   {}  {}", ui::grey("collection"), ui::white(&name));
            println!("   {}  {}\n", ui::grey("id        "), ui::dim(&id));

            let total = files.len();
            let mut done = 0;
            for (i, f) in files.iter().enumerate() {
                let fname = f.file_name().and_then(|s| s.to_str()).unwrap_or("");
                let sz = f.metadata().map(|m| m.len()).unwrap_or(0);
                print!(
                    "   {} {:>2}/{}  {:<24} {:>9}  … ",
                    ui::dim("↑"),
                    i + 1,
                    total,
                    ui::white(fname),
                    ui::grey(&human_size(sz))
                );
                flush();
                match upload_track(&http, &c, &id, f) {
                    Ok(v) => {
                        done += 1;
                        let d = v["track"]["durationSec"]
                            .as_u64()
                            .map(fmt_hms)
                            .unwrap_or_default();
                        let fmt = v["track"]["format"].as_str().unwrap_or("");
                        println!("{}  {}", ui::green("✓"), ui::dim(&format!("{fmt} {d}")));
                    }
                    Err(e) => println!("{}  {}", ui::red("✗"), ui::red(&e.to_string())),
                }
            }
            if let Some(cov) = cover {
                print!("   {}  {:<27} … ", ui::dim("▤ cover"), ui::white(&cov));
                flush();
                match set_cover(&http, &c, &id, Path::new(&cov)) {
                    Ok(()) => println!("{}", ui::green("✓")),
                    Err(e) => println!("{}  {}", ui::red("✗"), ui::red(&e.to_string())),
                }
            }
            println!(
                "\n  {}  {}/{} tracks live on {}\n",
                ui::green("done"),
                done,
                total,
                ui::white(&c.server)
            );
        }
        CollectionCmd::Add { id, files, cfg } => {
            let c = resolve_cfg(&cfg, true)?;
            for f in &files {
                print!("   {}  {}  … ", ui::dim("↑"), ui::white(f));
                flush();
                match upload_track(&http, &c, &id, Path::new(f)) {
                    Ok(_) => println!("{}", ui::green("✓")),
                    Err(e) => println!("{}  {}", ui::red("✗"), ui::red(&e.to_string())),
                }
            }
        }
        CollectionCmd::Rm { id, track, cfg } => {
            let c = resolve_cfg(&cfg, true)?;
            let (u, what) = match &track {
                Some(t) => (
                    format!("{}/collections/{}/tracks/{}", c.server, id, t),
                    "track removed",
                ),
                None => (
                    format!("{}/collections/{}", c.server, id),
                    "collection removed",
                ),
            };
            ok_json(
                http.delete(u)
                    .header("Authorization", format!("Bearer {}", c.token))
                    .send()?,
            )?;
            println!("  {}  {}", ui::green("✓"), ui::grey(what));
        }
        CollectionCmd::Edit {
            id,
            name,
            description,
            art,
            track,
            title,
            cfg,
        } => {
            let c = resolve_cfg(&cfg, true)?;
            let mut body = serde_json::Map::new();
            if let Some(n) = name {
                body.insert("name".into(), serde_json::json!(n));
            }
            if let Some(d) = description {
                body.insert("description".into(), serde_json::json!(d));
            }
            if let Some(a) = art {
                body.insert("artStyle".into(), serde_json::json!(a));
            }
            match (&track, &title) {
                (Some(t), Some(ti)) => {
                    body.insert(
                        "tracks".into(),
                        serde_json::json!([{ "id": t, "name": ti }]),
                    );
                }
                (Some(_), None) | (None, Some(_)) => {
                    anyhow::bail!("--track and --title must be used together");
                }
                (None, None) => {}
            }
            if body.is_empty() {
                anyhow::bail!(
                    "nothing to edit — pass --name, --description, --art, or --track with --title"
                );
            }
            ok_json(
                http.patch(format!("{}/collections/{}", c.server, id))
                    .header("Authorization", format!("Bearer {}", c.token))
                    .json(&serde_json::Value::Object(body))
                    .send()?,
            )?;
            println!("  {}  {}", ui::green("✓"), ui::grey("updated"));
        }
        CollectionCmd::Cover { id, image, cfg } => {
            let c = resolve_cfg(&cfg, true)?;
            set_cover(&http, &c, &id, Path::new(&image))?;
            println!("  {}  {}", ui::green("✓"), ui::grey("cover set"));
        }
    }
    Ok(())
}

fn print_collections(v: &serde_json::Value) {
    print!("{}", banner());
    println!("\n{}", rule("COLLECTIONS"));
    let empty = Vec::new();
    let cols = v["collections"].as_array().unwrap_or(&empty);
    if cols.is_empty() {
        println!("   {}\n", ui::dim("no collections yet"));
        return;
    }
    for c in cols {
        let name = c["name"].as_str().unwrap_or("");
        let artist = c["artist"].as_str().unwrap_or("");
        let n = c["trackCount"].as_u64().unwrap_or(0);
        let id = c["id"].as_str().unwrap_or("");
        println!(
            "   {}  {}  {}",
            ui::bold(&ui::white(name)),
            ui::accent(&format!("{n} track{}", if n == 1 { "" } else { "s" })),
            ui::grey(artist)
        );
        println!("   {} {}\n", ui::dim("id"), ui::dim(id));
    }
}

fn human_size(bytes: u64) -> String {
    let b = bytes as f64;
    if b >= 1e9 {
        format!("{:.1} GB", b / 1e9)
    } else if b >= 1e6 {
        format!("{:.1} MB", b / 1e6)
    } else if b >= 1e3 {
        format!("{:.0} KB", b / 1e3)
    } else {
        format!("{bytes} B")
    }
}

fn flush() {
    use std::io::Write;
    std::io::stdout().flush().ok();
}

// ───────────────────────── rendering ─────────────────────────

fn banner() -> String {
    let art = [
        r"   (((  ⦿  )))",
        r"   ╻ ╻┏━╸╻  ╻┏━┓",
        r"   ┣━┫┣╸ ┃  ┃┃ ┃",
        r"   ╹ ╹┗━╸┗━╸╹┗━┛",
    ];
    let mut s = String::from("\n");
    for (i, line) in art.iter().enumerate() {
        if i == 0 {
            s += &format!("{}\n", ui::amber(line));
        } else {
            s += &format!("{}\n", ui::accent(line));
        }
    }
    s += &format!("   {}\n", ui::dim("broadcast console"));
    s
}

fn rule(title: &str) -> String {
    let head = format!("━━ {title} ");
    let pad = 44usize.saturating_sub(head.chars().count());
    format!("  {}{}", ui::accent(&head), ui::dim(&"━".repeat(pad)))
}

fn kv(k: &str, v: &str) -> String {
    format!("   {}  {}", ui::grey(&format!("{k:<9}")), v)
}

fn meter(v: f32) -> String {
    // Perceptual (dB) meter: linear amplitude → dBFS mapped over [-60, 0] dB. A linear bar makes
    // loud audio look tiny (0.06 linear = -24 dB = actually loud); dB matches what you hear.
    let w = 18usize;
    let db = if v > 0.0001 { 20.0 * v.log10() } else { -90.0 };
    let norm = ((db + 60.0) / 60.0).clamp(0.0, 1.0);
    let f = (norm * w as f32).round() as usize;
    // Zone colour: green (safe) · amber (hot) · red (near clip).
    let code = if db >= -3.0 {
        ui::RED
    } else if db >= -12.0 {
        ui::AMBER
    } else {
        ui::GREEN
    };
    let bar = format!(
        "{}{}",
        ui::paint(code, &"█".repeat(f)),
        ui::dim(&"░".repeat(w - f))
    );
    let label = if v > 0.0001 {
        format!("{db:>4.0} dB")
    } else {
        " -inf".to_string()
    };
    format!("[{bar}] {}", ui::white(&label))
}

fn render_status(s: &Status) -> String {
    let mut o = banner();

    // STATUS
    o += &format!("\n{}\n", rule("STATUS"));
    let state = if s.on_air {
        format!("{}  {}", ui::green("◉"), ui::bold(&ui::green("ON AIR")))
    } else if s.state == helio_core::api::State::Unregistered {
        format!("{}  {}", ui::red("○"), ui::red("UNREGISTERED"))
    } else {
        format!("{}  {}", ui::grey("○"), ui::grey("READY"))
    };
    let uptime = fmt_hms(s.uptime_s);
    o += &format!(
        "   {}{}\n",
        state,
        if s.on_air {
            ui::dim(&format!(
                "   ·  uptime {uptime}  ·  {} reconnects",
                s.reconnects
            ))
        } else {
            String::new()
        }
    );

    // CHANNEL
    o += &format!("\n{}\n", rule("CHANNEL"));
    o += &format!(
        "   {}  {}   {}\n",
        ui::bold(&ui::white(&s.channel)),
        ui::accent(&format!("№{:03}", s.episode)),
        ui::grey(&s.artist)
    );

    // PRESENCE
    o += &format!("\n{}\n", rule("PRESENCE"));
    let pres = if s.away {
        ui::amber("●  AWAY")
    } else {
        ui::green("●  ON DECK")
    };
    o += &format!("   {}\n", pres);

    // INPUT
    o += &format!("\n{}\n", rule("INPUT"));
    let dev = if s.input_device.is_empty() {
        "(system default)".to_string()
    } else {
        s.input_device.clone()
    };
    let cap = if s.active_input.is_empty() {
        ui::red("○ no input")
    } else {
        format!("{} {}", ui::green("●"), ui::grey(&s.active_input))
    };
    o += &format!(
        "{}\n",
        kv("device", &format!("{}   {}", ui::white(&dev), cap))
    );
    o += &format!("{}\n", kv("level", &meter(s.level)));
    o += &format!("{}\n", kv("peak", &meter(s.peak)));

    // LINK
    o += &format!("\n{}\n", rule("LINK"));
    o += &format!("{}\n", kv("server", &ui::white(&s.server)));
    if !s.channel_id.is_empty() {
        o += &format!("{}\n", kv("channel", &ui::dim(&s.channel_id)));
    }
    o += "\n";
    o
}

// ───────────────────────── alt (small-screen) status card ─────────────────────────
//
// One card, five lines, sized like a hardware panel readout: a static brand frame
// top and bottom, three content lines in between. Numbers are kept to the ones that
// answer "how long / how many" (episode, uptime, reconnects) — level/peak become a
// single glyph bar, presence/input/link become dot glyphs. Padding is always computed
// from the *plain* text length before colour codes are added, so ANSI escapes never
// throw off alignment.
const ALT_CW: usize = 50;

fn card_border(
    left: char,
    right: char,
    label_colored: &str,
    label_len: usize,
    cw: usize,
) -> String {
    let used = 1 + 2 + label_len + 1 + 1; // corner + "─ " + label + " " + corner
    let dashes = (cw + 4).saturating_sub(used);
    format!(
        "{left}─ {label_colored} {}{right}",
        ui::dim(&"─".repeat(dashes))
    )
}

fn card_line(lc: &str, l_len: usize, rc: &str, r_len: usize, cw: usize) -> String {
    let pad = cw.saturating_sub(l_len + r_len).max(1);
    format!("│ {lc}{}{rc} │", " ".repeat(pad))
}

/// Compact level bar: filled = signal, dim dots = headroom, a single raised tick = peak.
/// No dB numbers — the shape of the bar and where the tick sits says it all.
fn mini_bar(level: f32, peak: f32, w: usize) -> String {
    let norm = |v: f32| -> f32 {
        let db = if v > 0.0001 { 20.0 * v.log10() } else { -90.0 };
        ((db + 60.0) / 60.0).clamp(0.0, 1.0)
    };
    let lvl_f = (norm(level) * w as f32).round() as usize;
    let peak_i = (norm(peak) * w as f32).round() as usize;
    let peak_i = peak_i.min(w.saturating_sub(1));
    let code = if norm(level) >= 0.95 {
        ui::RED
    } else if norm(level) >= 0.80 {
        ui::AMBER
    } else {
        ui::GREEN
    };
    let mut s = String::new();
    for i in 0..w {
        if i == peak_i {
            s += &ui::paint(ui::WHITE, "╻");
        } else if i < lvl_f {
            s += &ui::paint(code, "█");
        } else {
            s += &ui::paint(ui::DIMC, "░");
        }
    }
    s
}

fn render_status_alt(s: &Status) -> String {
    let cw = ALT_CW;
    let mut o = String::from("\n");

    o += &card_border(
        '╭',
        '╮',
        &ui::accent("HELIOGRAPH"),
        "HELIOGRAPH".chars().count(),
        cw,
    );
    o += "\n";

    // state + uptime
    let (sym, code, label) = if s.on_air {
        ("◉", ui::GREEN, "ON AIR")
    } else if s.state == helio_core::api::State::Unregistered {
        ("○", ui::RED, "UNREGISTERED")
    } else {
        ("○", ui::GREY, "READY")
    };
    let left_len = sym.chars().count() + 1 + label.chars().count();
    let left = format!(
        "{} {}",
        ui::paint(code, sym),
        ui::bold(&ui::paint(code, label))
    );
    let uptime = if s.on_air {
        fmt_hms(s.uptime_s)
    } else {
        String::new()
    };
    o += &card_line(
        &left,
        left_len,
        &ui::dim(&uptime),
        uptime.chars().count(),
        cw,
    );
    o += "\n";

    // channel + artist
    let chan_plain = format!("{} №{:03}", s.channel, s.episode);
    let chan = format!(
        "{} {}",
        ui::white(&s.channel),
        ui::accent(&format!("№{:03}", s.episode))
    );
    o += &card_line(
        &chan,
        chan_plain.chars().count(),
        &ui::grey(&s.artist),
        s.artist.chars().count(),
        cw,
    );
    o += "\n";

    // presence + input level
    let (psym, pcode, plabel) = if s.away {
        ("●", ui::AMBER, "AWAY")
    } else {
        ("●", ui::GREEN, "ON DECK")
    };
    let pres_plain_len = psym.chars().count() + 1 + plabel.chars().count();
    let pres = ui::paint(pcode, &format!("{sym} {plabel}", sym = psym));
    let (isym, icode) = if s.active_input.is_empty() {
        ("○", ui::RED)
    } else {
        ("●", ui::GREEN)
    };
    let bar_w = 14usize;
    let in_len = isym.chars().count() + 1 + bar_w;
    let bar = format!(
        "{} {}",
        ui::paint(icode, isym),
        mini_bar(s.level, s.peak, bar_w)
    );
    o += &card_line(&pres, pres_plain_len, &bar, in_len, cw);
    o += "\n";

    // link, in the closing frame
    let mut link_plain = format!("⇄ {}", s.server);
    let mut link_colored = format!("⇄ {}", ui::white(&s.server));
    if s.reconnects > 0 {
        link_plain += &format!(" · {} reconnects", s.reconnects);
        link_colored += &ui::dim(&format!(" · {} reconnects", s.reconnects));
    }
    o += &card_border('╰', '╯', &link_colored, link_plain.chars().count(), cw);
    o += "\n";
    o
}

fn print_devices(d: &DevicesResponse) {
    print!("{}", banner());
    println!("\n{}", rule("INPUTS"));
    let sel = if d.selected.is_empty() {
        "(system default)".to_string()
    } else {
        d.selected.clone()
    };
    println!("   {} {}\n", ui::grey("selected:"), ui::white(&sel));
    if d.devices.is_empty() {
        println!("   {}", ui::dim("no input devices found"));
    }
    for dev in &d.devices {
        let mut tags = Vec::new();
        if dev.is_current {
            tags.push(ui::green("● capturing"));
        }
        if dev.is_default {
            tags.push(ui::grey("default"));
        }
        let tagstr = if tags.is_empty() {
            String::new()
        } else {
            format!("   {}", tags.join(ui::dim(" · ").as_str()))
        };
        let mark = if dev.is_current {
            ui::green("▸")
        } else {
            ui::dim("·")
        };
        println!("   {} {}{}", mark, ui::white(&dev.name), tagstr);
    }
    println!();
}

fn ack(what: &str, resp: reqwest::blocking::Response) -> Result<()> {
    let a: Ack = resp.json().context("bad daemon response")?;
    if a.ok {
        println!("  {}  {}", ui::green("✓"), ui::grey(what));
    } else {
        eprintln!(
            "  {}  {} {}",
            ui::red("✗"),
            ui::grey(what),
            a.error.unwrap_or_default()
        );
        std::process::exit(1);
    }
    Ok(())
}

fn fmt_hms(secs: u64) -> String {
    let (h, m, s) = (secs / 3600, (secs % 3600) / 60, secs % 60);
    if h > 0 {
        format!("{h}:{m:02}:{s:02}")
    } else {
        format!("{m:02}:{s:02}")
    }
}

// keep IsTerminal import used even if colours are forced off in tests
#[allow(dead_code)]
fn _tty() -> bool {
    std::io::stdout().is_terminal()
}
