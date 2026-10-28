//! QUIC, peer-to-peer links, the relay, and NAT traversal.

use crate::ffi;
use crate::net::{TcpStream, TlsClient, TlsServer, UdpSocket};
use crate::{bytes_ptr, check, flag, millis, run, run_object, Lower, Object, Result};
use core::ffi::c_void;
use std::time::Duration;

// ------------------------------------------------------------------------------------------------------------------------------
// QUIC
// ------------------------------------------------------------------------------------------------------------------------------

/// Limits both QUIC endpoints advertise. Zero / `None` means the default noted.
#[derive(Debug, Clone, Default)]
pub struct QuicLimits {
    /// `None`: 30 s.
    pub idle_timeout: Option<Duration>,
    /// Keep-alive probes this often (`None`: off). Keep it under the idle timeout and your NAT's UDP timeout.
    pub keep_alive: Option<Duration>,
    /// `None`: 10 s.
    pub handshake_timeout: Option<Duration>,
    /// Streams the peer may have open (0: 100).
    pub max_bidirectional_streams: u32,
    pub max_unidirectional_streams: u32,
    /// Receive windows (0: 256 KiB per stream, 2 MiB per connection). Memory is bounded by these.
    pub stream_receive_window: u64,
    pub connection_receive_window: u64,
    /// Do not offer unreliable datagrams.
    pub no_datagrams: bool,
    /// Unacknowledged bytes one stream may buffer before `send` waits (0: 1 MiB).
    pub stream_send_buffer: u64,
    /// Datagrams buffered for `receive_datagram` / waiting to be sent (0: 256 each).
    pub datagram_receive_queue: u64,
    pub datagram_send_queue: u64,
}

impl QuicLimits {
    fn lower(&self) -> ffi::SturdyQuicLimits {
        ffi::SturdyQuicLimits {
            idle_timeout_ms: millis(self.idle_timeout),
            keep_alive_ms: millis(self.keep_alive),
            handshake_timeout_ms: millis(self.handshake_timeout),
            max_bidirectional_streams: self.max_bidirectional_streams,
            max_unidirectional_streams: self.max_unidirectional_streams,
            stream_receive_window: self.stream_receive_window,
            connection_receive_window: self.connection_receive_window,
            no_datagrams: flag(self.no_datagrams),
            stream_send_buffer: self.stream_send_buffer,
            datagram_receive_queue: self.datagram_receive_queue,
            datagram_send_queue: self.datagram_send_queue,
        }
    }
}

/// Why a QUIC connection ended.
#[derive(Debug, Clone)]
pub struct QuicClose {
    pub code: u64,
    pub reason: String,
    pub from_peer: bool,
    /// False for a transport-level end (protocol error, TLS alert, idle timeout).
    pub application: bool,
    pub timed_out: bool,
}

/// One QUIC stream. `receive` returning an empty vector means the peer finished its side.
pub struct QuicStream {
    object: Object,
}

impl QuicStream {
    /// Queues data; completes once it fits under the send buffer (a slow peer paces the sender).
    pub async fn send(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_quic_stream_send(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    /// Ends the sending side cleanly after everything queued.
    pub async fn finish(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_quic_stream_finish(self.object.raw(), out) }).await.map(|_| ())
    }

    /// 1..`max_bytes` bytes as they arrive; empty at the peer's FIN. Reading is what grants the peer more flow-control credit.
    pub async fn receive(&self, max_bytes: usize) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_quic_stream_receive(self.object.raw(), max_bytes as u64, out) }).await?.bytes())
    }

    /// Reads until the peer finishes, up to `limit` bytes (`LimitExceeded` past it).
    pub async fn read_to_end(&self, limit: usize) -> Result<Vec<u8>> {
        let mut all = Vec::new();
        loop {
            let chunk = self.receive(64 * 1024).await?;
            if chunk.is_empty() {
                return Ok(all);
            }
            if all.len() + chunk.len() > limit {
                return Err(crate::Error::new(crate::ErrorKind::LimitExceeded, "stream is larger than the limit"));
            }
            all.extend_from_slice(&chunk);
        }
    }

    /// Abruptly stops sending: the peer's reads fail with `error_code`.
    pub fn reset(&self, error_code: u64) {
        unsafe { ffi::sturdy_quic_stream_reset(self.object.raw(), error_code) };
    }

    /// Asks the peer to stop sending.
    pub fn stop_sending(&self, error_code: u64) {
        unsafe { ffi::sturdy_quic_stream_stop_sending(self.object.raw(), error_code) };
    }

    pub fn id(&self) -> i64 {
        let mut id = -1i64;
        unsafe { ffi::sturdy_quic_stream_id(self.object.raw(), &mut id) };
        id
    }

    pub fn is_bidirectional(&self) -> bool {
        let mut value = 0u8;
        unsafe { ffi::sturdy_quic_stream_is_bidirectional(self.object.raw(), &mut value) };
        value != 0
    }
}

/// A QUIC connection: many independent streams and unreliable datagrams over one encrypted, address-validated UDP flow.
pub struct QuicConnection {
    object: Object,
}

impl QuicConnection {
    /// Connects and completes the TLS 1.3 handshake. `tls.alpn` is required.
    pub async fn connect(host: &str, port: u16, limits: &QuicLimits, tls: &TlsClient) -> Result<QuicConnection> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        let lowered = ffi::SturdyQuicClientOptions { limits: limits.lower(), tls: tls.lower(&mut cx) };
        let object = run_object(|out| unsafe { ffi::sturdy_quic_connect(host, port, &lowered, out) }).await?;
        Ok(QuicConnection { object })
    }

    /// Opens a stream (waits if the peer's stream limit is reached).
    pub async fn open_stream(&self, bidirectional: bool) -> Result<QuicStream> {
        let object = run_object(|out| unsafe { ffi::sturdy_quic_open_stream(self.object.raw(), flag(bidirectional), out) }).await?;
        Ok(QuicStream { object })
    }

    /// The next stream the peer opened.
    pub async fn accept_stream(&self) -> Result<QuicStream> {
        let object = run_object(|out| unsafe { ffi::sturdy_quic_accept_stream(self.object.raw(), out) }).await?;
        Ok(QuicStream { object })
    }

    /// Sends one unreliable datagram (it may be lost or reordered). `OutOfRange`-style failures surface as `ErrorKind::Abi`.
    pub fn send_datagram(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        check(unsafe { ffi::sturdy_quic_send_datagram(self.object.raw(), ptr, len) })
    }

    /// The next received datagram.
    pub async fn receive_datagram(&self) -> Result<Vec<u8>> {
        Ok(run(|out| unsafe { ffi::sturdy_quic_receive_datagram(self.object.raw(), out) }).await?.bytes())
    }

    /// Largest datagram that currently fits in one packet.
    pub fn max_datagram_size(&self) -> usize {
        let mut size = 0u64;
        unsafe { ffi::sturdy_quic_max_datagram_size(self.object.raw(), &mut size) };
        size as usize
    }

    /// Closes with an application error code (0 = clean).
    pub fn close(&self, error_code: u64, reason: &str) {
        let mut cx = Lower::default();
        let reason = cx.string(reason);
        unsafe { ffi::sturdy_quic_close(self.object.raw(), error_code, reason) };
    }

    /// Completes when the connection ends, for any reason.
    pub async fn closed(&self) -> Result<QuicClose> {
        let reply = run(|out| unsafe { ffi::sturdy_quic_closed(self.object.raw(), out) }).await?;
        let yes = |name: &str| reply.field(name).as_deref() == Some("1");
        Ok(QuicClose { code: reply.number() as u64, reason: reply.text(), from_peer: yes("from_peer"), application: yes("application"), timed_out: yes("timed_out") })
    }

    pub fn is_open(&self) -> bool {
        let mut open = 0u8;
        unsafe { ffi::sturdy_quic_is_open(self.object.raw(), &mut open) };
        open != 0
    }

    /// `peer`, `local`, and once the handshake finished `protocol`, `alpn`, `peer_sha256`, `cipher`.
    pub async fn info(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_quic_info(self.object.raw(), out) }).await?.fields())
    }

    /// `rtt_us`, `min_rtt_us`, `bytes_in_flight`, `cwnd`, `packets_lost`, `bytes_sent`, `bytes_received`.
    pub async fn stats(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_quic_stats(self.object.raw(), out) }).await?.fields())
    }
}

/// Options for [`QuicListener::listen`].
#[derive(Debug, Clone, Default)]
pub struct QuicServerOptions {
    pub limits: QuicLimits,
    /// Certificate, key and `alpn` are required.
    pub tls: TlsServer,
    pub bind_address: String,
    pub port: u16,
    /// 0: 1024.
    pub max_connections: u64,
    /// 0: 32.
    pub max_connections_per_address: u64,
    /// Skip the Retry round trip that proves client addresses are real (trusted networks only).
    pub no_address_validation: bool,
}

/// A QUIC server endpoint.
pub struct QuicListener {
    object: Object,
}

impl QuicListener {
    pub async fn listen(options: &QuicServerOptions) -> Result<QuicListener> {
        let mut cx = Lower::default();
        let lowered = ffi::SturdyQuicServerOptions {
            limits: options.limits.lower(),
            tls: options.tls.lower(&mut cx),
            bind_address: cx.string(&options.bind_address),
            port: options.port,
            max_connections: options.max_connections,
            max_connections_per_address: options.max_connections_per_address,
            no_address_validation: flag(options.no_address_validation),
        };
        let object = run_object(|out| unsafe { ffi::sturdy_quic_listen(&lowered, out) }).await?;
        Ok(QuicListener { object })
    }

    /// The next connection whose handshake completed.
    pub async fn accept(&self) -> Result<QuicConnection> {
        let object = run_object(|out| unsafe { ffi::sturdy_quic_accept(self.object.raw(), out) }).await?;
        Ok(QuicConnection { object })
    }

    pub fn port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_quic_listener_port(self.object.raw(), &mut port) })?;
        Ok(port)
    }

    /// `connections_accepted`, `retries_sent`, `rejected_limit`, `handshakes_failed`, ...
    pub async fn metrics(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_quic_listener_metrics(self.object.raw(), out) }).await?.fields())
    }

    pub fn stop(&self) {
        unsafe { ffi::sturdy_quic_listener_stop(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Peers
// ------------------------------------------------------------------------------------------------------------------------------

/// Settings for a [`Peer`] node.
#[derive(Debug, Clone, Default)]
pub struct PeerOptions {
    /// Required: peers only talk to nodes with the same network id.
    pub network_id: String,
    pub display_name: String,
    /// Long-term identity (`generate_identity`, persisted); `None`: an ephemeral one.
    pub identity: Option<crate::Identity>,
    /// Do not accept inbound connections.
    pub no_listen: bool,
    pub bind_address: String,
    pub port: u16,
    /// 0: 64.
    pub max_peers: u64,
    /// 0: 1 MiB.
    pub max_message_bytes: u64,
    /// If non-empty, only these peer ids may connect or be connected to.
    pub allowed_peers: Vec<String>,
    pub no_peer_exchange: bool,
    /// Announce on the local network.
    pub lan_discovery: bool,
    pub no_auto_connect: bool,
    pub handshake_timeout: Option<Duration>,
    /// Inbound links that may be mid-handshake at once (0: 32).
    pub max_pending_inbound: u64,
    /// Keep-alive pings and the silence after which a link is dropped (`None`: 15 s and 45 s).
    pub ping_interval: Option<Duration>,
    pub idle_timeout: Option<Duration>,
    /// Events buffered for `next_event` (0: 1024).
    pub event_queue: u64,
    /// LAN discovery: UDP port (0: 47831), multicast group (`None`: 239.255.83.31), announce period (`None`: 5 s).
    pub discovery_port: u16,
    pub discovery_group: Option<String>,
    pub discovery_interval: Option<Duration>,
}

/// A connected peer.
#[derive(Debug, Clone)]
pub struct PeerInfo {
    pub id: String,
    pub name: String,
    pub address: String,
    pub inbound: bool,
    pub listen_port: u16,
}

fn peer_info(reply: &crate::Reply) -> PeerInfo {
    PeerInfo {
        id: reply.field("id").unwrap_or_default(),
        name: reply.field("name").unwrap_or_default(),
        address: reply.field("address").unwrap_or_default(),
        inbound: reply.field("inbound").as_deref() == Some("1"),
        listen_port: reply.field("listen_port").and_then(|p| p.parse().ok()).unwrap_or(0),
    }
}

/// Something that happened on a [`Peer`] node.
#[derive(Debug, Clone)]
pub enum PeerEvent {
    Connected(PeerInfo),
    Disconnected { peer: String, reason: String },
    Message { peer: String, channel: u16, data: Vec<u8> },
    /// A candidate address was learned (LAN announcement or peer exchange).
    Discovered { peer: String, address: String },
}

/// A peer-to-peer node: encrypted, mutually authenticated links with self-certifying ids (the fingerprint of the peer's certificate).
pub struct Peer {
    object: Object,
}

impl Peer {
    pub async fn start(options: &PeerOptions) -> Result<Peer> {
        let mut cx = Lower::default();
        let (allowed_peers, allowed_peer_count) = cx.list(&options.allowed_peers);
        let lowered = ffi::SturdyPeerOptions {
            network_id: cx.string(&options.network_id),
            display_name: cx.string(&options.display_name),
            identity_certificate_pem: options.identity.as_ref().map_or(core::ptr::null(), |i| cx.string(&i.certificate_pem)),
            identity_private_key_pem: options.identity.as_ref().map_or(core::ptr::null(), |i| cx.string(&i.private_key_pem)),
            no_listen: flag(options.no_listen),
            bind_address: cx.string(&options.bind_address),
            port: options.port,
            max_peers: options.max_peers,
            max_message_bytes: options.max_message_bytes,
            allowed_peers,
            allowed_peer_count,
            no_peer_exchange: flag(options.no_peer_exchange),
            lan_discovery: flag(options.lan_discovery),
            no_auto_connect: flag(options.no_auto_connect),
            handshake_timeout_ms: millis(options.handshake_timeout),
            max_pending_inbound: options.max_pending_inbound,
            ping_interval_ms: millis(options.ping_interval),
            idle_timeout_ms: millis(options.idle_timeout),
            event_queue: options.event_queue,
            discovery_port: options.discovery_port,
            discovery_group: cx.optional(&options.discovery_group),
            discovery_interval_ms: millis(options.discovery_interval),
        };
        let object = run_object(|out| unsafe { ffi::sturdy_peer_start(&lowered, out) }).await?;
        Ok(Peer { object })
    }

    /// Our peer id.
    pub fn id(&self) -> Result<String> {
        crate::read_string(|b, c, l| unsafe { ffi::sturdy_peer_id(self.object.raw(), b, c, l) })
    }

    pub fn listen_port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_peer_listen_port(self.object.raw(), &mut port) })?;
        Ok(port)
    }

    /// The node's identity; persist it to keep the same id across runs.
    pub async fn identity(&self) -> Result<crate::Identity> {
        let reply = run(|out| unsafe { ffi::sturdy_peer_identity(self.object.raw(), out) }).await?;
        Ok(crate::Identity {
            certificate_pem: reply.field("certificate_pem").unwrap_or_default(),
            private_key_pem: reply.field("private_key_pem").unwrap_or_default(),
            sha256: reply.field("sha256").unwrap_or_default(),
        })
    }

    /// Dials a peer. `expected_id` pins it: the link is refused if someone else answers.
    pub async fn connect(&self, host: &str, port: u16, expected_id: Option<&str>) -> Result<PeerInfo> {
        let mut cx = Lower::default();
        let host = cx.string(host);
        let expected = expected_id.map_or(core::ptr::null(), |id| cx.string(id));
        let reply = run(|out| unsafe { ffi::sturdy_peer_connect(self.object.raw(), host, port, expected, out) }).await?;
        Ok(peer_info(&reply))
    }

    /// Meets a peer through a relay when neither can be dialed. Both sides use the same `token` and opposite `dial`.
    pub async fn connect_via_relay(&self, relay_host: &str, relay_port: u16, token: &str, dial: bool, expected_id: Option<&str>) -> Result<PeerInfo> {
        self.connect_via_relay_tls(relay_host, relay_port, token, dial, expected_id, None).await
    }

    /// Same, with TLS on the hop to the relay itself (`relay_tls`); the peers' own end-to-end TLS runs inside it either way.
    pub async fn connect_via_relay_tls(&self, relay_host: &str, relay_port: u16, token: &str, dial: bool, expected_id: Option<&str>, relay_tls: Option<&TlsClient>) -> Result<PeerInfo> {
        let mut cx = Lower::default();
        let host = cx.string(relay_host);
        let token = cx.string(token);
        let expected = expected_id.map_or(core::ptr::null(), |id| cx.string(id));
        let lowered = relay_tls.map(|t| t.lower(&mut cx));
        let tls_ptr = lowered.as_ref().map_or(core::ptr::null(), |t| t as *const _);
        let reply = run(|out| unsafe { ffi::sturdy_peer_connect_via_relay(self.object.raw(), host, relay_port, token, flag(dial), expected, tls_ptr, out) }).await?;
        Ok(peer_info(&reply))
    }

    /// Sends a message on channel 0..0xFEFF to one peer.
    pub async fn send(&self, peer_id: &str, channel: u16, data: &[u8]) -> Result<()> {
        let mut cx = Lower::default();
        let id = cx.string(peer_id);
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_peer_send(self.object.raw(), id, channel, ptr, len, out) }).await.map(|_| ())
    }

    /// Sends to every connected peer; returns how many links took it.
    pub async fn broadcast(&self, channel: u16, data: &[u8]) -> Result<usize> {
        self.broadcast_except(channel, data, None).await
    }

    /// Same, skipping `except` (forwarding what that peer just sent).
    pub async fn broadcast_except(&self, channel: u16, data: &[u8], except: Option<&str>) -> Result<usize> {
        let mut cx = Lower::default();
        let except = except.map_or(core::ptr::null(), |id| cx.string(id));
        let (ptr, len) = bytes_ptr(data);
        Ok(run(|out| unsafe { ffi::sturdy_peer_broadcast(self.object.raw(), channel, ptr, len, except, out) }).await?.number() as usize)
    }

    /// The next event. Not consuming events eventually stalls the peers' readers.
    pub async fn next_event(&self) -> Result<PeerEvent> {
        let reply = run(|out| unsafe { ffi::sturdy_peer_next_event(self.object.raw(), out) }).await?;
        let peer = reply.field("peer").unwrap_or_default();
        Ok(match reply.number() {
            0 => PeerEvent::Connected(PeerInfo {
                id: peer,
                name: reply.field("name").unwrap_or_default(),
                address: reply.field("address").unwrap_or_default(),
                inbound: reply.field("inbound").as_deref() == Some("1"),
                listen_port: 0,
            }),
            1 => PeerEvent::Disconnected { peer, reason: reply.field("reason").unwrap_or_default() },
            2 => PeerEvent::Message { peer, channel: reply.field("channel").and_then(|c| c.parse().ok()).unwrap_or(0), data: reply.bytes() },
            _ => PeerEvent::Discovered { peer, address: reply.field("address").unwrap_or_default() },
        })
    }

    /// Currently connected peers.
    pub async fn peers(&self) -> Result<Vec<PeerInfo>> {
        let reply = run(|out| unsafe { ffi::sturdy_peer_list(self.object.raw(), out) }).await?;
        let fields = reply.fields();
        let mut peers = Vec::new();
        let mut index = 0;
        loop {
            let find = |prefix: &str| fields.iter().find(|(k, _)| *k == format!("{prefix}:{index}")).map(|(_, v)| v.clone());
            let Some(id) = find("id") else { break };
            peers.push(PeerInfo {
                id,
                name: find("name").unwrap_or_default(),
                address: find("address").unwrap_or_default(),
                inbound: find("inbound").as_deref() == Some("1"),
                listen_port: find("listen_port").and_then(|p| p.parse().ok()).unwrap_or(0),
            });
            index += 1;
        }
        Ok(peers)
    }

    pub fn disconnect(&self, peer_id: &str) {
        self.disconnect_with_reason(peer_id, None);
    }

    /// Drops a peer; `reason` is what the other side sees.
    pub fn disconnect_with_reason(&self, peer_id: &str, reason: Option<&str>) {
        let mut cx = Lower::default();
        let id = cx.string(peer_id);
        let reason = reason.map_or(core::ptr::null(), |r| cx.string(r));
        unsafe { ffi::sturdy_peer_disconnect(self.object.raw(), id, reason) };
    }

    /// Refuses the peer from now on and drops it if connected.
    pub fn block(&self, peer_id: &str) {
        let mut cx = Lower::default();
        let id = cx.string(peer_id);
        unsafe { ffi::sturdy_peer_block(self.object.raw(), id) };
    }

    pub fn stop(&self) {
        unsafe { ffi::sturdy_peer_stop(self.object.raw()) };
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Relay and NAT traversal
// ------------------------------------------------------------------------------------------------------------------------------

/// Settings for a [`RelayServer`]. Zero means the default.
#[derive(Debug, Clone, Default)]
pub struct RelayOptions {
    pub bind_address: String,
    pub port: u16,
    /// Wrap the hop to the relay in TLS.
    pub tls: Option<TlsServer>,
    pub max_sessions: u64,
    pub max_waiting: u64,
    pub max_waiting_per_address: u64,
    pub pair_timeout: Option<Duration>,
    pub idle_timeout: Option<Duration>,
    /// Cut a session after this many relayed bytes (0: unlimited).
    pub max_session_bytes: u64,
    /// Longest rendezvous token accepted (0: 128).
    pub max_token_bytes: u64,
}

type Authorize = Box<dyn Fn(&str, &str) -> bool + Send + Sync + 'static>;

unsafe extern "C" fn authorize_trampoline(user_data: *mut c_void, token: *const core::ffi::c_char, client: *const core::ffi::c_char) -> u8 {
    let check = unsafe { &*(user_data as *const Authorize) };
    let text = |p: *const core::ffi::c_char| if p.is_null() { String::new() } else { unsafe { core::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned() };
    // A panicking check refuses the client rather than unwinding into C++.
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| check(&text(token), &text(client)))).unwrap_or(false) as u8
}

/// A rendezvous relay: pairs two connections that name the same token and forwards opaque bytes. It cannot read the peers' traffic when
/// they run TLS through it.
pub struct RelayServer {
    object: Option<Object>,
    authorize: *mut Authorize,
}

// The authorizer is `Send + Sync`, and the object is an engine handle usable from any thread.
unsafe impl Send for RelayServer {}
unsafe impl Sync for RelayServer {}

impl RelayServer {
    pub fn start(options: &RelayOptions) -> Result<RelayServer> {
        Self::start_inner(options, None)
    }

    /// Starts a relay that asks `authorize(token, client_address)` before pairing a client (on a network thread: answer quickly).
    pub fn start_with_authorizer(options: &RelayOptions, authorize: impl Fn(&str, &str) -> bool + Send + Sync + 'static) -> Result<RelayServer> {
        Self::start_inner(options, Some(Box::new(authorize)))
    }

    fn start_inner(options: &RelayOptions, authorize: Option<Authorize>) -> Result<RelayServer> {
        let mut cx = Lower::default();
        let lowered = ffi::SturdyRelayOptions {
            bind_address: cx.string(&options.bind_address),
            port: options.port,
            use_tls: flag(options.tls.is_some()),
            tls: options.tls.as_ref().map(|t| t.lower(&mut cx)).unwrap_or_default(),
            max_sessions: options.max_sessions,
            max_waiting: options.max_waiting,
            max_waiting_per_address: options.max_waiting_per_address,
            pair_timeout_ms: millis(options.pair_timeout),
            idle_timeout_ms: millis(options.idle_timeout),
            max_session_bytes: options.max_session_bytes,
            max_token_bytes: options.max_token_bytes,
        };
        let boxed: *mut Authorize = authorize.map_or(core::ptr::null_mut(), |a| Box::into_raw(Box::new(a)));
        let callback: Option<ffi::SturdyRelayAuthorizeFn> = if boxed.is_null() { None } else { Some(authorize_trampoline) };
        let mut raw = ffi::SturdyAsyncObject::default();
        if let Err(error) = check(unsafe { ffi::sturdy_relay_server_start(&lowered, callback, boxed as *mut c_void, &mut raw) }) {
            if !boxed.is_null() {
                drop(unsafe { Box::from_raw(boxed) });
            }
            return Err(error);
        }
        Ok(RelayServer { object: Some(Object::from_raw(raw)), authorize: boxed })
    }

    fn raw(&self) -> ffi::SturdyAsyncObject {
        self.object.as_ref().map(Object::raw).unwrap_or_default()
    }

    pub fn port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_relay_server_port(self.raw(), &mut port) })?;
        Ok(port)
    }

    /// `sessions_started`, `sessions_active`, `waiting`, `bytes_relayed`, `rejected`, `timed_out`.
    pub async fn metrics(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_relay_server_metrics(self.raw(), out) }).await?.fields())
    }

    pub fn stop(&self) {
        unsafe { ffi::sturdy_relay_server_stop(self.raw()) };
    }
}

impl Drop for RelayServer {
    fn drop(&mut self) {
        // Releasing the object stops the relay; only then is the authorizer freed.
        self.object = None;
        if !self.authorize.is_null() {
            drop(unsafe { Box::from_raw(self.authorize) });
        }
    }
}

/// A fresh unguessable rendezvous token.
pub fn random_relay_token() -> Result<String> {
    crate::read_string(|b, c, l| unsafe { ffi::sturdy_relay_random_token(b, c, l) })
}

/// Connects to a relay and waits for the partner. The returned stream is connected to the partner: start TLS on it
/// ([`TcpStream::start_tls_client`] / `start_tls_server`) before sending anything private.
pub async fn relay_connect(host: &str, port: u16, token: &str, dial: bool, relay_tls: Option<&TlsClient>, timeout: Option<Duration>) -> Result<TcpStream> {
    let mut cx = Lower::default();
    let host = cx.string(host);
    let token = cx.string(token);
    let lowered = relay_tls.map(|t| t.lower(&mut cx));
    let tls_ptr = lowered.as_ref().map_or(core::ptr::null(), |t| t as *const _);
    let object = run_object(|out| unsafe { ffi::sturdy_relay_connect(host, port, token, flag(dial), tls_ptr, millis(timeout), out) }).await?;
    Ok(TcpStream::from_object(object))
}

/// Router port-mapping settings. Zero / `None` means the default.
#[derive(Debug, Clone, Default)]
pub struct PortMapOptions {
    /// Router address; `None`: the default gateway (found on Linux only).
    pub gateway: Option<String>,
    /// `None`: one hour.
    pub lifetime: Option<Duration>,
    /// 0: the same number as the internal port.
    pub external_port: u16,
    /// Per method; `None`: 3 s.
    pub timeout: Option<Duration>,
    /// Bit set: 1 PCP, 2 NAT-PMP, 4 UPnP. 0: all three.
    pub methods: u32,
    /// 0: 5351.
    pub gateway_port: u16,
    /// UPnP discovery target (`None`: the SSDP multicast group) and port (0: 1900).
    pub ssdp_host: Option<String>,
    pub ssdp_port: u16,
}

/// A forwarded port: remove it with [`PortMapping::remove`] (and renew before half the lifetime is over).
pub struct PortMapping {
    object: Object,
    pub external_host: String,
    pub external_port: u16,
    /// Granted lifetime; zero means permanent (UPnP only).
    pub lifetime: Duration,
    pub method: String,
}

impl PortMapping {
    /// Asks the router to forward a public port to this machine (PCP, NAT-PMP or UPnP IGD).
    pub async fn map(tcp: bool, internal_port: u16, options: &PortMapOptions) -> Result<PortMapping> {
        let mut cx = Lower::default();
        let lowered = lower_port_map(options, &mut cx);
        let reply = run(|out| unsafe { ffi::sturdy_nat_map_port(flag(tcp), internal_port, &lowered, out) }).await?;
        let external_host = reply.field("external_host").unwrap_or_default();
        let external_port = reply.field("external_port").and_then(|p| p.parse().ok()).unwrap_or(0);
        let method = reply.field("method").unwrap_or_default();
        let lifetime = Duration::from_secs(reply.number().max(0) as u64);
        let object = reply.into_object()?;
        Ok(PortMapping { object, external_host, external_port, lifetime, method })
    }

    /// Removes the mapping from the router.
    pub async fn remove(&self, options: &PortMapOptions) -> Result<()> {
        let mut cx = Lower::default();
        let lowered = lower_port_map(options, &mut cx);
        run(|out| unsafe { ffi::sturdy_nat_unmap_port(self.object.raw(), &lowered, out) }).await.map(|_| ())
    }
}

fn lower_port_map(options: &PortMapOptions, cx: &mut Lower) -> ffi::SturdyPortMapOptions {
    ffi::SturdyPortMapOptions {
        gateway: cx.optional(&options.gateway),
        lifetime_seconds: options.lifetime.map_or(0, |d| d.as_secs() as i64),
        external_port: options.external_port,
        timeout_ms: millis(options.timeout),
        methods: options.methods,
        gateway_port: options.gateway_port,
        ssdp_host: cx.optional(&options.ssdp_host),
        ssdp_port: options.ssdp_port,
    }
}

/// The default gateway's address, if the platform lets the engine read the routing table (Linux).
pub fn default_gateway() -> Result<Option<String>> {
    let gateway = crate::read_string(|b, c, l| unsafe { ffi::sturdy_nat_default_gateway(b, c, l) })?;
    Ok(if gateway.is_empty() { None } else { Some(gateway) })
}

/// UDP hole punching: both peers call this at once with the other's candidate addresses (`host:port`) and a shared secret. Returns the
/// candidate that answered; keep using the same socket afterwards. `TimedOut` is typical of symmetric NATs (use a relay).
pub async fn punch_hole(socket: &UdpSocket, candidates: &[String], shared_secret: &str, timeout: Duration) -> Result<(String, u16)> {
    let mut cx = Lower::default();
    let (list, count) = cx.list(candidates);
    let secret = cx.string(shared_secret);
    let reply = run(|out| unsafe { ffi::sturdy_nat_punch_hole(socket.object.raw(), list, count, secret, millis(Some(timeout)), out) }).await?;
    Ok((reply.text(), reply.number() as u16))
}
