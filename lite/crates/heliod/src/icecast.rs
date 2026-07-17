//! Icecast source client (Transport trait impl) — pure Rust over a blocking TcpStream.
//!
//! Uses the modern Icecast `PUT /mount` source method with HTTP Basic auth. Auth
//! failures are surfaced as `TransportError::Auth` (fatal); everything else is
//! `Transient` (the broadcast loop retries with backoff while ON AIR).

use base64::Engine;
use helio_core::pipeline::{StreamMeta, Transport, TransportError};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

pub struct IcecastTransport {
    host: String,
    port: u16,
    /// Mount path WITHOUT leading slash, e.g. `2cac….opus`.
    mount: String,
    password: String,
    stream: Option<TcpStream>,
}

impl IcecastTransport {
    pub fn new(host: String, port: u16, mount: String, password: String) -> Self {
        Self {
            host,
            port,
            mount: mount.trim_start_matches('/').to_string(),
            password,
            stream: None,
        }
    }
}

impl Transport for IcecastTransport {
    fn connect(&mut self, content_type: &str, meta: &StreamMeta) -> Result<(), TransportError> {
        let addr = (self.host.as_str(), self.port)
            .to_socket_addrs()
            .map_err(|e| TransportError::Transient(format!("resolve {}: {e}", self.host)))?
            .next()
            .ok_or_else(|| TransportError::Transient(format!("no address for {}", self.host)))?;

        let mut sock = TcpStream::connect_timeout(&addr, Duration::from_secs(10))
            .map_err(|e| TransportError::Transient(format!("connect: {e}")))?;
        sock.set_nodelay(true).ok();
        sock.set_write_timeout(Some(Duration::from_secs(10))).ok();
        sock.set_read_timeout(Some(Duration::from_secs(8))).ok();

        let auth = base64::engine::general_purpose::STANDARD
            .encode(format!("source:{}", self.password));
        let req = format!(
            "PUT /{mount} HTTP/1.1\r\n\
             Host: {host}:{port}\r\n\
             Authorization: Basic {auth}\r\n\
             User-Agent: heliod/0.1\r\n\
             Content-Type: {ct}\r\n\
             Ice-Public: {public}\r\n\
             Ice-Name: {name}\r\n\
             Ice-Description: {desc}\r\n\
             Ice-Genre: {genre}\r\n\
             Expect: 100-continue\r\n\r\n",
            mount = self.mount,
            host = self.host,
            port = self.port,
            ct = content_type,
            public = if meta.public { 1 } else { 0 },
            name = meta.name,
            desc = meta.description,
            genre = meta.genre,
        );
        sock.write_all(req.as_bytes())
            .map_err(|e| TransportError::Transient(format!("write headers: {e}")))?;

        // Read the status line. Icecast replies 100-continue or 200 on success, 401 on bad auth.
        let mut buf = [0u8; 512];
        let n = sock.read(&mut buf).unwrap_or(0);
        let resp = String::from_utf8_lossy(&buf[..n]);
        let code = parse_status(&resp);
        match code {
            Some(100) | Some(200) => {
                self.stream = Some(sock);
                Ok(())
            }
            Some(401) | Some(403) if resp.to_lowercase().contains("unauthorized") => {
                Err(TransportError::Auth(format!("icecast rejected credentials: {}", code.unwrap())))
            }
            Some(401) => Err(TransportError::Auth("icecast 401 unauthorized".into())),
            Some(c) => Err(TransportError::Transient(format!("icecast status {c}"))),
            // Some Icecast configs send no immediate reply and just accept the body.
            None if n == 0 => {
                self.stream = Some(sock);
                Ok(())
            }
            None => Err(TransportError::Transient(format!("unparseable response: {resp:?}"))),
        }
    }

    fn send(&mut self, data: &[u8]) -> Result<(), TransportError> {
        let s = self
            .stream
            .as_mut()
            .ok_or_else(|| TransportError::Transient("not connected".into()))?;
        s.write_all(data)
            .map_err(|e| TransportError::Transient(format!("send: {e}")))
    }

    fn close(&mut self) {
        if let Some(s) = self.stream.take() {
            let _ = s.shutdown(std::net::Shutdown::Both);
        }
    }
}

/// Pull the numeric status from an `HTTP/1.x NNN ...` line.
fn parse_status(resp: &str) -> Option<u16> {
    let line = resp.lines().next()?;
    line.split_whitespace().nth(1)?.parse().ok()
}
