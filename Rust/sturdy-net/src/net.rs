//! TLS settings, timers and DNS, TCP, UDP, the HTTP client and server, and WebSocket.

use crate::{bytes_ptr, check, flag, millis, run, run_object, Error, ErrorKind, Lower, Object, Op, Reply, Result};
use crate::ffi;
use std::time::Duration;

// ------------------------------------------------------------------------------------------------------------------------------
// TLS
// ------------------------------------------------------------------------------------------------------------------------------

/// How to verify a server. Chain and host name are always checked unless `pinned_sha256` names the exact certificate(s) accepted.
#[derive(Debug, Clone, Default)]
pub struct TlsClient {
    /// Name sent as SNI and checked against the certificate; `None`: the host being connected to.
    pub server_name: Option<String>,
    /// Do not trust the operating system's roots (use `ca_pem` only).
    pub no_system_roots: bool,
    /// Extra trusted CA certificates, PEM.
    pub ca_pem: Option<String>,
    /// Hex SHA-256 of acceptable leaf certificates; when present they replace chain validation.
    pub pinned_sha256: Vec<String>,
    /// Peer-to-peer opt-in: accept any certificate and compare the peer's fingerprint yourself. Never the default.
    pub identify_peer_by_certificate_only: bool,
    pub client_certificate_pem: Option<String>,
    pub client_private_key_pem: Option<String>,
    /// ALPN protocols to offer, most preferred first.
    pub alpn: Vec<String>,
    /// Time allowed for the TLS handshake; `None`: 10 s.
    pub handshake_timeout: Option<Duration>,
}

impl TlsClient {
    /// Trust exactly the certificate with this hex SHA-256 digest.
    pub fn pinned(sha256: impl Into<String>) -> Self {
        Self { pinned_sha256: vec![sha256.into()], ..Self::default() }
    }

    pub(crate) fn lower(&self, cx: &mut Lower) -> ffi::SturdyTlsClientOptions {
        let (pinned, pinned_count) = cx.list(&self.pinned_sha256);
        let (alpn, alpn_count) = cx.list(&self.alpn);
        ffi::SturdyTlsClientOptions {
            server_name: cx.optional(&self.server_name),
            no_system_roots: flag(self.no_system_roots),
            ca_pem: cx.optional(&self.ca_pem),
            pinned_sha256: pinned,
            pinned_sha256_count: pinned_count,
            identify_peer_by_certificate_only: flag(self.identify_peer_by_certificate_only),
            client_certificate_pem: cx.optional(&self.client_certificate_pem),
            client_private_key_pem: cx.optional(&self.client_private_key_pem),
            alpn,
            alpn_count,
            handshake_timeout_ms: millis(self.handshake_timeout) as u32,
        }
    }
}

/// How to present and check identities on the accepting side.
#[derive(Debug, Clone, Default)]
pub struct TlsServer {
    pub certificate_pem: String,
    pub private_key_pem: String,
    /// CAs whose client certificates are accepted (mutual TLS).
    pub client_ca_pem: Option<String>,
    pub require_client_certificate: bool,
    /// Peer-to-peer opt-in: accept any self-signed client certificate and authorise by its fingerprint yourself.
    pub accept_any_client_certificate: bool,
    pub pinned_client_sha256: Vec<String>,
    pub alpn: Vec<String>,
    /// Time allowed for each client's TLS handshake; `None`: 10 s.
    pub handshake_timeout: Option<Duration>,
}

impl TlsServer {
    pub fn new(certificate_pem: impl Into<String>, private_key_pem: impl Into<String>) -> Self {
        Self { certificate_pem: certificate_pem.into(), private_key_pem: private_key_pem.into(), ..Self::default() }
    }

    pub(crate) fn lower(&self, cx: &mut Lower) -> ffi::SturdyTlsServerOptions {
        let (pinned, pinned_count) = cx.list(&self.pinned_client_sha256);
        let (alpn, alpn_count) = cx.list(&self.alpn);
        ffi::SturdyTlsServerOptions {
            certificate_pem: cx.string(&self.certificate_pem),
            private_key_pem: cx.string(&self.private_key_pem),
            client_ca_pem: cx.optional(&self.client_ca_pem),
            require_client_certificate: flag(self.require_client_certificate),
            accept_any_client_certificate: flag(self.accept_any_client_certificate),
            pinned_client_sha256: pinned,
            pinned_client_sha256_count: pinned_count,
            alpn,
            alpn_count,
            handshake_timeout_ms: millis(self.handshake_timeout) as u32,
        }
    }
}

/// A self-signed ECDSA P-256 certificate and key (for peers and development servers).
#[derive(Debug, Clone)]
pub struct Identity {
    pub certificate_pem: String,
    pub private_key_pem: String,
    /// Hex SHA-256 of the certificate: share it out of band and pin it on the other side.
    pub sha256: String,
}

/// Generates a self-signed identity valid for `valid_days`.
pub async fn generate_identity(common_name: &str, valid_days: u32) -> Result<Identity> {
    let mut cx = Lower::default();
    let name = cx.string(common_name);
    let reply = run(|out| unsafe { ffi::sturdy_tls_generate_identity(name, valid_days, out) }).await?;
    Ok(Identity {
        certificate_pem: reply.field("certificate_pem").unwrap_or_default(),
        private_key_pem: reply.field("private_key_pem").unwrap_or_default(),
        sha256: reply.field("sha256").unwrap_or_default(),
    })
}

/// Hex SHA-256 of the first certificate in `certificate_pem`.
pub fn certificate_sha256(certificate_pem: &str) -> Result<String> {
    let mut cx = Lower::default();
    let pem = cx.string(certificate_pem);
    crate::read_string(|buffer, capacity, length| unsafe { ffi::sturdy_tls_certificate_sha256(pem, buffer, capacity, length) })
}

// ------------------------------------------------------------------------------------------------------------------------------
// Timers, signals, DNS
// ------------------------------------------------------------------------------------------------------------------------------

/// The same timer as an [`Op`] you can cancel, link to a [`crate::CancelToken`] or give a deadline ([`Op::with_timeout`]).
pub fn sleep_op(duration: Duration) -> Result<Op> {
    Op::start(|out| unsafe { ffi::sturdy_timer_sleep(millis(Some(duration)), out) })
}

/// Completes after `duration`, without occupying a thread.
pub async fn sleep(duration: Duration) -> Result<()> {
    run(|out| unsafe { ffi::sturdy_timer_sleep(millis(Some(duration)), out) }).await.map(|_| ())
}

/// Completes when the process receives SIGINT or SIGTERM; returns the signal number.
pub async fn shutdown_signal() -> Result<i32> {
    Ok(run(|out| unsafe { ffi::sturdy_signal_shutdown(out) }).await?.number() as i32)
}

/// Resolves a host name to numeric addresses.
pub async fn resolve(host: &str) -> Result<Vec<String>> {
    let mut cx = Lower::default();
    let host = cx.string(host);
    let reply = run(|out| unsafe { ffi::sturdy_dns_resolve(host, out) }).await?;
    Ok(reply.fields().into_iter().map(|(_, address)| address).collect())
}

/// Asks a STUN server which public `(host, port)` this machine appears as.
pub async fn stun_query(server: Option<&str>, port: u16, timeout: Duration) -> Result<(String, u16)> {
    let mut cx = Lower::default();
    let server = server.map_or(core::ptr::null(), |s| cx.string(s));
    let reply = run(|out| unsafe { ffi::sturdy_stun_query(server, port, millis(Some(timeout)), out) }).await?;
    Ok((reply.text(), reply.number() as u16))
}

// ------------------------------------------------------------------------------------------------------------------------------
// TCP
// ------------------------------------------------------------------------------------------------------------------------------

/// A TCP stream, optionally TLS. Dropping it closes the connection.
pub struct TcpStream {
    pub(crate) object: Object,
}

impl TcpStream {
    pub(crate) fn from_object(object: Object) -> Self {
        Self { object }
    }

    /// Connects (resolving, trying each address). `tls`: a verified TLS handshake. `timeout` `None`: 30 s.
    pub async fn connect(host: &str, port: u16, tls: Option<&TlsClient>, timeout: Option<Duration>) -> Result<TcpStream> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        let lowered = tls.map(|t| t.lower(&mut cx));
        let tls_ptr = lowered.as_ref().map_or(core::ptr::null(), |t| t as *const _);
        let object = run_object(|out| unsafe { ffi::sturdy_tcp_connect(host, port, tls_ptr, millis(timeout), out) }).await?;
        Ok(TcpStream { object })
    }

    /// Sends all of `data`; returns the byte count.
    pub async fn send(&self, data: &[u8]) -> Result<usize> {
        let (ptr, len) = bytes_ptr(data);
        Ok(run(|out| unsafe { ffi::sturdy_tcp_send(self.object.raw(), ptr, len, out) }).await?.number() as usize)
    }

    /// Receives 1..`max_bytes` bytes as soon as any arrive; an empty vector means the peer finished.
    pub async fn receive(&self, max_bytes: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_tcp_receive(self.object.raw(), max_bytes as u64, out) }).await?.bytes())
    }

    /// Receives exactly `count` bytes (`UnexpectedEof` if the peer closes first).
    pub async fn receive_exact(&self, count: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_tcp_receive_exact(self.object.raw(), count as u64, out) }).await?.bytes())
    }

    /// Receives up to and including `delimiter`; `LimitExceeded` past `max_bytes`.
    pub async fn receive_until(&self, delimiter: &str, max_bytes: usize) -> Result<Vec<u8>> {
        let mut cx = Lower::default();
        let delimiter = cx.string(delimiter);
        Ok(run(|out| unsafe { ffi::sturdy_tcp_receive_until(self.object.raw(), delimiter, max_bytes as u64, out) }).await?.bytes())
    }

    /// Half-close: tells the peer no more data is coming while reads continue.
    pub async fn shutdown_write(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_tcp_shutdown_write(self.object.raw(), out) }).await.map(|_| ())
    }

    /// Upgrades the open, quiet connection to TLS in place (client role).
    pub async fn start_tls_client(&self, host: &str, tls: &TlsClient) -> Result<()> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        let lowered = tls.lower(&mut cx);
        run(|out| unsafe { ffi::sturdy_tcp_start_tls_client(self.object.raw(), host, &lowered, out) }).await.map(|_| ())
    }

    /// Upgrades the open, quiet connection to TLS in place (server role).
    pub async fn start_tls_server(&self, tls: &TlsServer) -> Result<()> {
        let mut cx = Lower::default();
        let lowered = tls.lower(&mut cx);
        run(|out| unsafe { ffi::sturdy_tcp_start_tls_server(self.object.raw(), &lowered, out) }).await.map(|_| ())
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_tcp_close(self.object.raw()) };
    }

    pub fn is_open(&self) -> bool {
        let mut open = 0u8;
        unsafe { ffi::sturdy_tcp_is_open(self.object.raw(), &mut open) };
        open != 0
    }

    /// Disables (or re-enables) Nagle's algorithm.
    pub async fn set_nodelay(&self, enabled: bool) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_tcp_set_nodelay(self.object.raw(), flag(enabled), out) }).await.map(|_| ())
    }

    pub async fn set_keepalive(&self, enabled: bool) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_tcp_set_keepalive(self.object.raw(), flag(enabled), out) }).await.map(|_| ())
    }

    /// `peer`, `local` ("host:port") and, over TLS, `protocol`, `cipher`, `alpn`, `peer_sha256`.
    pub async fn info(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_tcp_info(self.object.raw(), out) }).await?.fields())
    }
}

/// A listening TCP socket (plain or TLS).
pub struct TcpListener {
    object: Object,
}

impl TcpListener {
    /// Starts listening. `host` empty: all interfaces. `port` 0: any free port. `tls`: every connection completes a TLS handshake first.
    pub async fn listen(host: &str, port: u16, tls: Option<&TlsServer>) -> Result<TcpListener> {
        Self::listen_with_backlog(host, port, tls, 0).await
    }

    /// Same, with an explicit pending-connection queue length (0: 1024).
    pub async fn listen_with_backlog(host: &str, port: u16, tls: Option<&TlsServer>, backlog: i32) -> Result<TcpListener> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        let lowered = tls.map(|t| t.lower(&mut cx));
        let tls_ptr = lowered.as_ref().map_or(core::ptr::null(), |t| t as *const _);
        let object = run_object(|out| unsafe { ffi::sturdy_tcp_listen(host, port, tls_ptr, backlog, out) }).await?;
        Ok(TcpListener { object })
    }

    /// Waits for the next connection.
    pub async fn accept(&self) -> Result<TcpStream> {
        let object = run_object(|out| unsafe { ffi::sturdy_tcp_accept(self.object.raw(), out) }).await?;
        Ok(TcpStream { object })
    }

    pub fn port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_tcp_listener_port(self.object.raw(), &mut port) })?;
        Ok(port)
    }

    /// "host:port" the listener is bound to.
    pub fn address(&self) -> Result<String> {
        crate::read_string(|b, c, l| unsafe { ffi::sturdy_tcp_listener_address(self.object.raw(), b, c, l) })
    }

    pub fn is_open(&self) -> bool {
        let mut open = 0u8;
        unsafe { ffi::sturdy_tcp_listener_is_open(self.object.raw(), &mut open) };
        open != 0
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_tcp_listener_close(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// UDP
// ------------------------------------------------------------------------------------------------------------------------------

/// One received datagram.
#[derive(Debug, Clone)]
pub struct Datagram {
    pub data: Vec<u8>,
    pub from_host: String,
    pub from_port: u16,
}

/// Socket options for [`UdpSocket::bind_with`].
#[derive(Debug, Clone, Copy, Default)]
pub struct UdpOptions {
    pub reuse_address: bool,
    pub broadcast: bool,
    /// Kernel buffer sizes in bytes; 0: the OS default.
    pub receive_buffer: u64,
    pub send_buffer: u64,
}

/// A bound UDP socket.
pub struct UdpSocket {
    pub(crate) object: Object,
}

impl UdpSocket {
    /// Binds `host:port` (`host` empty: all interfaces, `port` 0: any).
    pub async fn bind(host: &str, port: u16, reuse_address: bool, broadcast: bool) -> Result<UdpSocket> {
        Self::bind_with(host, port, &UdpOptions { reuse_address, broadcast, ..UdpOptions::default() }).await
    }

    /// Same, with every socket option.
    pub async fn bind_with(host: &str, port: u16, options: &UdpOptions) -> Result<UdpSocket> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        let lowered = ffi::SturdyUdpOptions {
            reuse_address: flag(options.reuse_address),
            broadcast: flag(options.broadcast),
            receive_buffer: options.receive_buffer,
            send_buffer: options.send_buffer,
        };
        let object = run_object(|out| unsafe { ffi::sturdy_udp_bind(host, port, &lowered, out) }).await?;
        Ok(UdpSocket { object })
    }

    /// Fixes the peer: [`UdpSocket::send`] then needs no address, and only that peer's datagrams are received.
    pub async fn connect(&self, host: &str, port: u16) -> Result<()> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        run(|out| unsafe { ffi::sturdy_udp_connect(self.object.raw(), host, port, out) }).await.map(|_| ())
    }

    /// Sends one datagram on a connected socket.
    pub async fn send(&self, data: &[u8]) -> Result<usize> {
        let (ptr, len) = bytes_ptr(data);
        Ok(run(|out| unsafe { ffi::sturdy_udp_send(self.object.raw(), ptr, len, out) }).await?.number() as usize)
    }

    pub async fn send_to(&self, host: &str, port: u16, data: &[u8]) -> Result<usize> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        let (ptr, len) = bytes_ptr(data);
        Ok(run(|out| unsafe { ffi::sturdy_udp_send_to(self.object.raw(), host, port, ptr, len, out) }).await?.number() as usize)
    }

    /// Receives the next datagram (at most `max_bytes`).
    pub async fn receive(&self, max_bytes: usize) -> Result<Datagram> {
        let reply = run(|out| unsafe { ffi::sturdy_udp_receive(self.object.raw(), max_bytes as u64, out) }).await?;
        Ok(Datagram { data: reply.bytes(), from_host: reply.text(), from_port: reply.number() as u16 })
    }

    pub fn port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_udp_port(self.object.raw(), &mut port) })?;
        Ok(port)
    }

    /// "host:port" the socket is bound to.
    pub fn address(&self) -> Result<String> {
        crate::read_string(|b, c, l| unsafe { ffi::sturdy_udp_address(self.object.raw(), b, c, l) })
    }

    pub async fn set_broadcast(&self, enabled: bool) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_udp_set_broadcast(self.object.raw(), flag(enabled), out) }).await.map(|_| ())
    }

    /// `interface`: the local interface address, or `None` for the default one.
    pub async fn join_multicast(&self, group: &str, interface: Option<&str>) -> Result<()> {
        let mut cx = Lower::default();
        let group = cx.string(group);
        let interface = interface.map_or(core::ptr::null(), |i| cx.string(i));
        run(|out| unsafe { ffi::sturdy_udp_join_multicast(self.object.raw(), group, interface, out) }).await.map(|_| ())
    }

    pub async fn leave_multicast(&self, group: &str, interface: Option<&str>) -> Result<()> {
        let mut cx = Lower::default();
        let group = cx.string(group);
        let interface = interface.map_or(core::ptr::null(), |i| cx.string(i));
        run(|out| unsafe { ffi::sturdy_udp_leave_multicast(self.object.raw(), group, interface, out) }).await.map(|_| ())
    }

    pub async fn set_multicast_loopback(&self, enabled: bool) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_udp_set_multicast_loopback(self.object.raw(), flag(enabled), out) }).await.map(|_| ())
    }

    pub async fn set_multicast_ttl(&self, hops: u32) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_udp_set_multicast_ttl(self.object.raw(), hops, out) }).await.map(|_| ())
    }

    pub fn is_open(&self) -> bool {
        let mut open = 0u8;
        unsafe { ffi::sturdy_udp_is_open(self.object.raw(), &mut open) };
        open != 0
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_udp_close(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// HTTP
// ------------------------------------------------------------------------------------------------------------------------------

/// A response from [`HttpClient::request`].
#[derive(Debug, Clone)]
pub struct HttpResponse {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
    /// The URL after redirects.
    pub url: String,
}

impl HttpResponse {
    /// A header value, looked up case-insensitively.
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(n, _)| n.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }

    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.body).into_owned()
    }
}

/// Settings for an [`HttpClient`]. `Default` gives the engine's defaults.
#[derive(Debug, Clone)]
pub struct HttpClientOptions {
    /// Deadline for one request including redirects; `None`: 30 s.
    pub timeout: Option<Duration>,
    /// Redirects followed; 0 returns the redirect response itself.
    pub max_redirects: u32,
    /// `None`: 16 MiB.
    pub max_response_body_bytes: Option<u64>,
    pub keep_alive: bool,
    /// Ask for and decode gzip/deflate.
    pub decompress: bool,
    /// Keep and send cookies (a private jar per client).
    pub cookies: bool,
    pub http2: bool,
    /// Switch to HTTP/3 when a server advertises it with `Alt-Svc`.
    pub http3: bool,
    /// Try HTTP/3 first, before any `Alt-Svc` was seen.
    pub http3_prior_knowledge: bool,
    pub user_agent: Option<String>,
    /// `http://[user:password@]host:port`.
    pub proxy: Option<String>,
    pub no_proxy: Vec<String>,
    pub tls: TlsClient,
    /// Follow an https -> http redirect (refused by default).
    pub allow_https_downgrade: bool,
    /// `None`: 64 KiB.
    pub max_response_header_bytes: Option<u32>,
    /// `None`: 4.
    pub max_idle_connections_per_host: Option<u32>,
    /// `None`: 30 s.
    pub idle_connection_timeout: Option<Duration>,
    /// `None`: 3 s before falling back from HTTP/3.
    pub http3_connect_timeout: Option<Duration>,
    /// A jar to share or persist (implies `cookies`); `None` with `cookies` set: a private jar.
    pub cookie_jar: Option<crate::CookieJar>,
}

impl Default for HttpClientOptions {
    fn default() -> Self {
        Self {
            timeout: None,
            max_redirects: 5,
            max_response_body_bytes: None,
            keep_alive: true,
            decompress: true,
            cookies: false,
            http2: true,
            http3: true,
            http3_prior_knowledge: false,
            user_agent: None,
            proxy: None,
            no_proxy: Vec::new(),
            tls: TlsClient::default(),
            allow_https_downgrade: false,
            max_response_header_bytes: None,
            max_idle_connections_per_host: None,
            idle_connection_timeout: None,
            http3_connect_timeout: None,
            cookie_jar: None,
        }
    }
}

/// An HTTP client with a connection pool (HTTP/1.1 keep-alive, HTTP/2 multiplexing, HTTP/3 after `Alt-Svc`).
pub struct HttpClient {
    object: Object,
}

impl HttpClient {
    pub fn new(options: &HttpClientOptions) -> Result<HttpClient> {
        let mut cx = Lower::default();
        let (no_proxy, no_proxy_count) = cx.list(&options.no_proxy);
        let lowered = ffi::SturdyHttpClientOptions {
            timeout_ms: millis(options.timeout),
            max_redirects: options.max_redirects as i32,
            max_response_body_bytes: options.max_response_body_bytes.unwrap_or(0),
            no_keep_alive: flag(!options.keep_alive),
            no_decompress: flag(!options.decompress),
            cookies: flag(options.cookies),
            no_http2: flag(!options.http2),
            no_http3: flag(!options.http3),
            http3_prior_knowledge: flag(options.http3_prior_knowledge),
            user_agent: cx.optional(&options.user_agent),
            proxy: cx.optional(&options.proxy),
            no_proxy,
            no_proxy_count,
            tls: options.tls.lower(&mut cx),
            allow_https_downgrade: flag(options.allow_https_downgrade),
            max_response_header_bytes: options.max_response_header_bytes.unwrap_or(0),
            max_idle_connections_per_host: options.max_idle_connections_per_host.unwrap_or(0),
            idle_connection_timeout_ms: millis(options.idle_connection_timeout),
            http3_connect_timeout_ms: millis(options.http3_connect_timeout),
            cookie_jar: options.cookie_jar.as_ref().map(|jar| jar.raw()).unwrap_or_default(),
        };
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_http_client_create(&lowered, &mut raw) })?;
        Ok(HttpClient { object: Object::from_raw(raw) })
    }

    /// Sends a request. Non-2xx statuses are ordinary responses; only transport, TLS, protocol, limit and timeout problems are errors.
    pub async fn request(&self, method: &str, url: &str, headers: &[(String, String)], body: &[u8]) -> Result<HttpResponse> {
        let mut cx = Lower::default();
        let method = cx.string(method);
        let url = cx.string(url);
        let (headers_ptr, header_count) = cx.pairs(headers);
        let (body_ptr, body_len) = bytes_ptr(body);
        let reply = run(|out| unsafe { ffi::sturdy_http_client_request(self.object.raw(), method, url, headers_ptr, header_count, body_ptr, body_len, out) }).await?;
        Ok(HttpResponse { status: reply.number() as u16, headers: reply.fields(), body: reply.bytes(), url: reply.text() })
    }

    pub async fn get(&self, url: &str) -> Result<HttpResponse> {
        self.request("GET", url, &[], &[]).await
    }

    pub async fn post(&self, url: &str, content_type: &str, body: &[u8]) -> Result<HttpResponse> {
        self.request("POST", url, &[("Content-Type".to_string(), content_type.to_string())], body).await
    }

    /// The cookie jar this client uses (shared, not a copy); `None` when cookies are off.
    pub fn cookie_jar(&self) -> Option<crate::CookieJar> {
        let mut raw = ffi::SturdyAsyncObject::default();
        (unsafe { ffi::sturdy_http_client_cookie_jar(self.object.raw(), &mut raw) } == ffi::STURDY_OK).then(|| crate::CookieJar::from_object(Object::from_raw(raw)))
    }

    /// Closes idle pooled connections.
    pub fn close_idle(&self) {
        unsafe { ffi::sturdy_http_client_close_idle(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// WebSocket
// ------------------------------------------------------------------------------------------------------------------------------

/// Limits for a WebSocket. Zero / `None` means the default.
#[derive(Debug, Clone, Default)]
pub struct WebSocketOptions {
    /// 0: 1 MiB; bigger incoming messages close the socket with 1009.
    pub max_message_bytes: u64,
    /// 0: 4 MiB of unsent data.
    pub max_queued_send_bytes: u64,
    /// `None`: 60 s without inbound traffic drops the connection.
    pub idle_timeout: Option<Duration>,
    /// Negotiate permessage-deflate (the inflated size is bounded by `max_message_bytes`).
    pub compression: bool,
    /// Received messages buffered before reading pauses; 0: 16.
    pub inbox_messages: u64,
    /// Time allowed for the opening handshake; `None`: 10 s.
    pub handshake_timeout: Option<Duration>,
}

impl WebSocketOptions {
    fn lower(&self) -> ffi::SturdyWebSocketOptions {
        ffi::SturdyWebSocketOptions {
            max_message_bytes: self.max_message_bytes,
            max_queued_send_bytes: self.max_queued_send_bytes,
            idle_timeout_ms: millis(self.idle_timeout),
            compression: flag(self.compression),
            inbox_messages: self.inbox_messages,
            handshake_timeout_ms: millis(self.handshake_timeout),
        }
    }
}

/// A received WebSocket message.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Message {
    Text(String),
    Binary(Vec<u8>),
}

/// Options for [`WebSocket::connect`].
#[derive(Debug, Clone, Default)]
pub struct WebSocketClientOptions {
    pub socket: WebSocketOptions,
    pub tls: TlsClient,
    pub headers: Vec<(String, String)>,
    pub subprotocols: Vec<String>,
    pub timeout: Option<Duration>,
}

/// A WebSocket connection (client or server side).
pub struct WebSocket {
    object: Object,
}

impl WebSocket {
    /// Connects to a `ws://` or `wss://` URL.
    pub async fn connect(url: &str, options: &WebSocketClientOptions) -> Result<WebSocket> {
        let mut cx = Lower::default();
        let url = cx.string(url);
        let (headers, header_count) = cx.pairs(&options.headers);
        let (subprotocols, subprotocol_count) = cx.list(&options.subprotocols);
        let lowered = ffi::SturdyWebSocketClientOptions {
            socket: options.socket.lower(),
            tls: options.tls.lower(&mut cx),
            headers,
            header_count,
            subprotocols,
            subprotocol_count,
            timeout_ms: millis(options.timeout),
        };
        let object = run_object(|out| unsafe { ffi::sturdy_websocket_connect(url, &lowered, out) }).await?;
        Ok(WebSocket { object })
    }

    pub async fn send_text(&self, text: &str) -> Result<()> {
        let (ptr, len) = bytes_ptr(text.as_bytes());
        run(|out| unsafe { ffi::sturdy_websocket_send_text(self.object.raw(), ptr as *const core::ffi::c_char, len, out) }).await.map(|_| ())
    }

    pub async fn send_binary(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_websocket_send_binary(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    pub async fn ping(&self, payload: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(payload);
        run(|out| unsafe { ffi::sturdy_websocket_ping(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    /// The next data message; fails with `Closed` once the connection ended.
    pub async fn receive(&self) -> Result<Message> {
        let reply = run(|out| unsafe { ffi::sturdy_websocket_receive(self.object.raw(), out) }).await?;
        let data = reply.bytes();
        Ok(if reply.number() == 1 { Message::Text(String::from_utf8_lossy(&data).into_owned()) } else { Message::Binary(data) })
    }

    /// Graceful close with an RFC 6455 code (1000 normal) and a reason of up to 123 bytes.
    pub async fn close(&self, code: u16, reason: &str) -> Result<()> {
        let mut cx = Lower::default();
        let reason = cx.string(reason);
        run(|out| unsafe { ffi::sturdy_websocket_close(self.object.raw(), code, reason, out) }).await.map(|_| ())
    }

    pub fn is_open(&self) -> bool {
        let mut open = 0u8;
        unsafe { ffi::sturdy_websocket_is_open(self.object.raw(), &mut open) };
        open != 0
    }

    /// Drops the connection without a closing handshake.
    pub fn abort(&self) {
        unsafe { ffi::sturdy_websocket_abort(self.object.raw()) };
    }

    pub(crate) fn from_object(object: Object) -> WebSocket {
        WebSocket { object }
    }

    /// `target`, `origin`, `subprotocol`, `header:<Name>` per upgrade header, `peer`, the TLS fields over wss, and once closed `close_code`, `close_reason`.
    pub async fn info(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_websocket_info(self.object.raw(), out) }).await?.fields())
    }
}

/// Options for [`WebSocketServer::listen`].
#[derive(Debug, Clone, Default)]
pub struct WebSocketServerOptions {
    pub bind_address: String,
    pub port: u16,
    pub socket: WebSocketOptions,
    /// Serve `wss://` with this identity instead of `ws://`.
    pub tls: Option<TlsServer>,
    /// A request carrying an `Origin` header must match one of these exactly (empty: any).
    pub allowed_origins: Vec<String>,
    /// Request paths served (empty: any).
    pub paths: Vec<String>,
    pub subprotocols: Vec<String>,
    /// 0: 1024.
    pub max_connections: u64,
    /// 0: 16 KiB.
    pub max_request_header_bytes: u32,
}

/// Accepts WebSocket connections.
pub struct WebSocketServer {
    object: Object,
}

impl WebSocketServer {
    pub async fn listen(options: &WebSocketServerOptions) -> Result<WebSocketServer> {
        let mut cx = Lower::default();
        let (allowed_origins, allowed_origin_count) = cx.list(&options.allowed_origins);
        let (paths, path_count) = cx.list(&options.paths);
        let (subprotocols, subprotocol_count) = cx.list(&options.subprotocols);
        let tls = options.tls.as_ref().map(|t| t.lower(&mut cx)).unwrap_or_default();
        let lowered = ffi::SturdyWebSocketServerOptions {
            bind_address: cx.string(&options.bind_address),
            port: options.port,
            socket: options.socket.lower(),
            use_tls: flag(options.tls.is_some()),
            tls,
            allowed_origins,
            allowed_origin_count,
            paths,
            path_count,
            subprotocols,
            subprotocol_count,
            max_connections: options.max_connections,
            max_request_header_bytes: options.max_request_header_bytes,
        };
        let object = run_object(|out| unsafe { ffi::sturdy_websocket_server_listen(&lowered, out) }).await?;
        Ok(WebSocketServer { object })
    }

    /// Waits for the next upgraded connection.
    pub async fn accept(&self) -> Result<WebSocket> {
        let object = run_object(|out| unsafe { ffi::sturdy_websocket_accept(self.object.raw(), out) }).await?;
        Ok(WebSocket { object })
    }

    pub fn port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_websocket_server_port(self.object.raw(), &mut port) })?;
        Ok(port)
    }

    pub fn close(&self) {
        unsafe { ffi::sturdy_websocket_server_close(self.object.raw()) };
    }
}

#[allow(dead_code)]
fn _assert_send_sync() {
    fn is<T: Send + Sync>() {}
    is::<TcpStream>();
    is::<TcpListener>();
    is::<UdpSocket>();
    is::<HttpClient>();
    is::<WebSocket>();
    let _ = (Error::new(ErrorKind::Unknown, ""), None::<Op>, None::<Reply>);
}
