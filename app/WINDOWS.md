# Building & running heliograph on Windows

heliograph is an openFrameworks 0.12.1 app. It uses **no addons** and does all networking/encoding by
shelling out to the `curl` and `ffmpeg` command-line tools (no in-process HTTP/codec library). That keeps
it portable, but the two external tools must be present and the shell command lines must be
Windows-compatible. This doc covers the build + the current cross-platform status.

## Prerequisites
1. **openFrameworks 0.12.1** for Windows (Visual Studio release) — https://openframeworks.cc/download/
2. **Visual Studio 2022** with the "Desktop development with C++" workload.
3. **curl** — ships with Windows 10/11 as `curl.exe` (already on `PATH`). Verify: `curl --version`.
4. **ffmpeg** — install and put it on `PATH` (e.g. `winget install Gyan.FFmpeg`, or unzip a static build
   and add its `bin` to `PATH`). Verify: `ffmpeg -version`. *(heliograph only auto-probes Homebrew/system
   paths on macOS; on Windows it relies on `ffmpeg` being on `PATH`.)*

## Build
1. Copy this app folder into `openFrameworks/apps/myApps/heliograph` (or keep it anywhere and set the OF
   path in the project).
2. Regenerate the Visual Studio project with the OF **projectGenerator** (`projectGenerator-vs.exe`) —
   point it at this folder, "Update". This produces `heliograph.sln` / `heliograph.vcxproj`.
3. Open `heliograph.sln`, select **Release / x64**, Build. The exe lands in `bin/`.
4. Copy the `bin/data/` folder (fonts, seed `session.json`) next to the exe if the generator didn't.

## Runtime
- `B` broadcast · `R` record · `S` settings. In settings: **REGISTER** (invite code → server),
  **BROADCAST** (Icecast host/mount/password + snapshot URL/token), **PUBLISH** (collection name →
  upload your recordings to the gallery, parity with the `helio` CLI).
- Broadcasting streams Opus/PCM to Icecast via `ffmpeg` and pushes an HD PNG snapshot + metadata to the
  server over REST (`curl`). Publishing extracts audio from each recorded `.mp4` (`ffmpeg`) and uploads it
  as a collection track over REST (`curl`).

## Cross-platform status (what's fixed vs. what to verify)
The shell-out layer was written for POSIX `sh`. These are now handled per-platform:
- **Argument quoting** — `gsShQuote()` uses single quotes on macOS/Linux and **double quotes on Windows**
  (`cmd.exe` ignores single quotes). ✅
- **Discarding stderr** — `gsNullDev()` returns `/dev/null` on POSIX and **`NUL`** on Windows. ✅

Still POSIX-flavoured — **please test these on Windows and report anything off:**
- **Background processes**: a few fire-and-forget `curl` calls end with a trailing `&` (`pushSnapshot`,
  `verifyRegistration`, the ICY-timestamp push). On `cmd.exe` a trailing `&` is a command separator, not
  a backgrounding operator, so those `curl` calls run **synchronously** — the snapshot push (every ~15 s)
  may cause a brief UI hitch instead of running in the background. If that's noticeable, wrap them in
  `start /b …` on Windows (or move them onto a worker thread). Broadcast audio (a long-lived `ffmpeg`
  pipe) is unaffected.
- **Paths**: recordings default to the OS "Movies/Videos" folder under `HelioRecordings/`; verify the
  Windows path resolves (set a custom folder in settings if needed).

If a Windows build hits a shell error, the first thing to check is a `curl`/`ffmpeg` command line that
still assumes `sh` semantics — those are the only cross-platform risk in the codebase.
