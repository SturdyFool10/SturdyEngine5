//! HTTP serving: servers, routers (parameters, middleware, static files), streamed and Server-Sent-Event bodies, WebSocket upgrades on the
//! HTTP port, the keyed rate limiter and the cookie jar.

use crate::net::WebSocket;
use crate::{bytes_ptr, check, flag, millis, run, Lower, Object, Result};
use crate::ffi;
use core::ffi::{c_char, c_void, CStr};
use std::sync::{Arc, Mutex};
use std::time::Duration;

fn text(p: *const c_char) -> String {
    if p.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
    }
}

fn pairs(items: *const ffi::SturdyKeyValue, count: u32) -> Vec<(String, String)> {
    if items.is_null() {
        return Vec::new();
    }
    unsafe { core::slice::from_raw_parts(items, count as usize) }.iter().map(|h| (text(h.name), text(h.value))).collect()
}

// ------------------------------------------------------------------------------------------------------------------------------
// Requests and responses
// ------------------------------------------------------------------------------------------------------------------------------

/// A request delivered to a handler.
#[derive(Debug, Clone, Default)]
pub struct HttpRequest {
    pub method: String,
    pub target: String,
    pub remote_address: String,
    /// 11 = HTTP/1.1, 20 = HTTP/2, 30 = HTTP/3.
    pub version: u32,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
    /// With `spool_requests_above`: the temporary file holding a large body (`body` is then empty). Deleted after the response is sent.
    pub body_file: Option<String>,
    // Filled in when the request came through a router:
    /// The percent-decoded path without the query.
    pub path: String,
    /// The raw query string without '?'.
    pub query: String,
    /// `:name` / `*name` captures of the matched pattern.
    pub params: Vec<(String, String)>,
}

impl HttpRequest {
    /// A header value, looked up case-insensitively.
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(n, _)| n.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }

    /// A route parameter (`:id`, `*rest`).
    pub fn param(&self, name: &str) -> Option<&str> {
        self.params.iter().find(|(n, _)| n == name).map(|(_, v)| v.as_str())
    }

    /// The first `name=value` pair of the query, percent- and '+'-decoded.
    pub fn query_param(&self, name: &str) -> Option<String> {
        self.query.split('&').filter_map(|pair| pair.split_once('=')).find_map(|(k, v)| {
            (percent_decode(k, true).ok()? == name).then(|| percent_decode(v, true).ok()).flatten()
        })
    }

    pub fn body_text(&self) -> String {
        String::from_utf8_lossy(&self.body).into_owned()
    }

    fn from_c(request: &ffi::SturdyHttpRequest) -> HttpRequest {
        let body = if request.body.is_null() { Vec::new() } else { unsafe { core::slice::from_raw_parts(request.body, request.body_size as usize) }.to_vec() };
        HttpRequest {
            method: text(request.method),
            target: text(request.target),
            remote_address: text(request.remote_address),
            version: request.version,
            headers: pairs(request.headers, request.header_count),
            body,
            body_file: (!request.body_file.is_null()).then(|| text(request.body_file)),
            path: text(request.path),
            query: text(request.query),
            params: pairs(request.params, request.param_count),
        }
    }
}

/// A file (or a range of one) to serve as the response body, streamed from disk.
#[derive(Debug, Clone)]
pub struct BodyFile {
    pub path: String,
    pub offset: u64,
    /// `None`: to the end of the file.
    pub length: Option<u64>,
}

/// A complete response. Build with [`Response::new`] and the `with_*` methods.
#[derive(Debug, Clone, Default)]
pub struct Response {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
    /// Send `Connection: close` and drop the connection afterwards.
    pub close_connection: bool,
    pub body_file: Option<BodyFile>,
    /// Chunks buffered before a write waits for the client (0: 16).
    pub stream_queue: u32,
}

impl Response {
    pub fn new(status: u16) -> Self {
        Self { status, ..Self::default() }
    }

    pub fn with_header(mut self, name: impl Into<String>, value: impl Into<String>) -> Self {
        self.headers.push((name.into(), value.into()));
        self
    }

    pub fn with_body(mut self, body: impl Into<Vec<u8>>) -> Self {
        self.body = body.into();
        self
    }

    pub fn with_file(mut self, file: BodyFile) -> Self {
        self.body_file = Some(file);
        self
    }

    fn lower(&self, cx: &mut Lower, stream: bool, sse: bool) -> ffi::SturdyHttpResponse {
        let (headers, header_count) = cx.pairs(&self.headers);
        let (body, body_size) = bytes_ptr(&self.body);
        let file = self.body_file.as_ref();
        ffi::SturdyHttpResponse {
            status: self.status as u32,
            headers,
            header_count,
            body,
            body_size,
            close_connection: flag(self.close_connection),
            body_file: file.map_or(core::ptr::null(), |f| cx.string(&f.path)),
            body_offset: file.map_or(0, |f| f.offset),
            body_length: file.and_then(|f| f.length).unwrap_or(0),
            limit_body_length: flag(file.is_some_and(|f| f.length.is_some())),
            stream: flag(stream),
            sse: flag(sse),
            stream_queue: self.stream_queue,
        }
    }
}

/// Answers one request. Call one of the `respond*`/`stream`/`sse` methods now or later, from any thread. A responder dropped unanswered
/// sends a 500, so a handler that forgets (or panics) never leaves the client hanging.
pub struct Responder {
    raw: Option<ffi::SturdyHttpResponder>,
}

impl Responder {
    fn take(&mut self) -> ffi::SturdyHttpResponder {
        self.raw.take().expect("responder already used")
    }

    /// Hands the responder back after a rejected response (so the handler can try again).
    fn restore(&mut self, raw: ffi::SturdyHttpResponder, result: Result<()>) -> Result<()> {
        if result.is_err() {
            self.raw = Some(raw);
        }
        result
    }

    pub fn respond(mut self, status: u16, headers: &[(String, String)], body: &[u8]) -> Result<()> {
        let raw = self.take();
        let mut cx = Lower::default();
        let (headers_ptr, header_count) = cx.pairs(headers);
        let (body_ptr, body_len) = bytes_ptr(body);
        let result = check(unsafe { ffi::sturdy_http_respond(raw, status as u32, headers_ptr, header_count, body_ptr, body_len) });
        self.restore(raw, result)
    }

    /// Sends a full [`Response`] (a body from bytes or from a file).
    pub fn send(mut self, response: &Response) -> Result<()> {
        let raw = self.take();
        let mut cx = Lower::default();
        let lowered = response.lower(&mut cx, false, false);
        let result = check(unsafe { ffi::sturdy_http_respond_ex(raw, &lowered, core::ptr::null_mut()) });
        self.restore(raw, result)
    }

    /// Starts a chunked response and returns the writer for its body. Dropping the writer ends the body.
    pub fn stream(mut self, response: &Response) -> Result<BodyWriter> {
        self.start_stream(response, false)
    }

    /// Starts a Server-Sent Events response (`text/event-stream`, `Cache-Control: no-cache`).
    pub fn sse(mut self, response: &Response) -> Result<BodyWriter> {
        self.start_stream(response, true)
    }

    fn start_stream(&mut self, response: &Response, sse: bool) -> Result<BodyWriter> {
        let raw = self.take();
        let mut cx = Lower::default();
        let lowered = response.lower(&mut cx, !sse, sse);
        let mut body = ffi::SturdyAsyncObject::default();
        let result = check(unsafe { ffi::sturdy_http_respond_ex(raw, &lowered, &mut body) });
        let result = self.restore(raw, result);
        result.map(|()| BodyWriter { object: Object::from_raw(body) })
    }

    /// Sends a canned response: plain text, HTML, JSON (`body` must be valid JSON) or a redirect (`body` is the Location).
    pub fn text(self, status: u16, body: &str) -> Result<()> {
        self.canned(ffi::STURDY_HTTP_RESPONSE_TEXT, status, body)
    }

    pub fn html(self, status: u16, body: &str) -> Result<()> {
        self.canned(ffi::STURDY_HTTP_RESPONSE_HTML, status, body)
    }

    pub fn json(self, status: u16, body: &str) -> Result<()> {
        self.canned(ffi::STURDY_HTTP_RESPONSE_JSON, status, body)
    }

    pub fn redirect(self, status: u16, location: &str) -> Result<()> {
        self.canned(ffi::STURDY_HTTP_RESPONSE_REDIRECT, status, location)
    }

    fn canned(mut self, kind: i32, status: u16, body: &str) -> Result<()> {
        let raw = self.take();
        let mut cx = Lower::default();
        let body_ptr = cx.string(body);
        let result = check(unsafe { ffi::sturdy_http_respond_canned(raw, kind, status as u32, body_ptr) });
        self.restore(raw, result)
    }
}

impl Drop for Responder {
    fn drop(&mut self) {
        if let Some(raw) = self.raw.take() {
            let body = b"Internal Server Error";
            unsafe { ffi::sturdy_http_respond(raw, 500, core::ptr::null(), 0, body.as_ptr(), body.len() as u64) };
        }
    }
}

/// One Server-Sent Event.
#[derive(Debug, Clone, Default)]
pub struct SseEvent {
    /// May contain newlines; each line becomes a `data:` field.
    pub data: String,
    pub event: Option<String>,
    pub id: Option<String>,
    /// Reconnect delay hint.
    pub retry: Option<Duration>,
}

impl SseEvent {
    pub fn new(data: impl Into<String>) -> Self {
        Self { data: data.into(), ..Self::default() }
    }

    fn lower(&self, cx: &mut Lower) -> ffi::SturdySseEvent {
        ffi::SturdySseEvent {
            data: cx.string(&self.data),
            event: cx.optional(&self.event),
            id: cx.optional(&self.id),
            retry_ms: self.retry.map_or(0, |d| d.as_millis().min(u32::MAX as u128) as u32),
            has_retry: flag(self.retry.is_some()),
        }
    }
}

/// The wire format of one event, ending with the blank line that dispatches it. Line breaks in `event` and `id` are dropped.
pub fn format_sse(event: &SseEvent) -> Result<String> {
    let mut cx = Lower::default();
    let lowered = event.lower(&mut cx);
    crate::read_string(|b, c, l| unsafe { ffi::sturdy_http_format_sse(&lowered, b, c, l) })
}

/// Percent-decodes `text` (`plus_is_space` for query strings); an error on a malformed escape or an encoded NUL.
pub fn percent_decode(text: &str, plus_is_space: bool) -> Result<String> {
    let mut cx = Lower::default();
    let input = cx.string(text);
    crate::read_string(|b, c, l| unsafe { ffi::sturdy_http_percent_decode(input, flag(plus_is_space), b, c, l) })
}

/// Writes the body of a streamed response. Dropping it ends the body.
pub struct BodyWriter {
    object: Object,
}

impl BodyWriter {
    /// Queues a chunk. Finishes once it is accepted into the bounded queue (so a slow client paces the producer); fails with `Closed`
    /// once the client is gone or the body was finished.
    pub async fn write(&self, data: &[u8]) -> Result<()> {
        let (ptr, len) = bytes_ptr(data);
        run(|out| unsafe { ffi::sturdy_http_body_write(self.object.raw(), ptr, len, out) }).await.map(|_| ())
    }

    /// Queues one Server-Sent Event.
    pub async fn write_sse(&self, event: &SseEvent) -> Result<()> {
        let mut cx = Lower::default();
        let lowered = event.lower(&mut cx);
        run(|out| unsafe { ffi::sturdy_http_body_write_sse(self.object.raw(), &lowered, out) }).await.map(|_| ())
    }

    /// Ends the body once queued chunks drain.
    pub fn finish(&self) {
        unsafe { ffi::sturdy_http_body_finish(self.object.raw()) };
    }

    /// Completes when the client disconnected or the server stopped, so a producer can stop.
    pub async fn closed(&self) -> Result<()> {
        run(|out| unsafe { ffi::sturdy_http_body_closed(self.object.raw(), out) }).await.map(|_| ())
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Handlers
// ------------------------------------------------------------------------------------------------------------------------------

/// A request handler. It runs on a handler thread and must answer through its [`Responder`] (now or later, from any thread).
pub type Handler = Box<dyn Fn(HttpRequest, Responder) + Send + Sync + 'static>;
type WebSocketHandler = Box<dyn Fn(WebSocket, HttpRequest) + Send + Sync + 'static>;

unsafe extern "C" fn http_trampoline(user_data: *mut c_void, request: *const ffi::SturdyHttpRequest, responder: ffi::SturdyHttpResponder) {
    let handler = unsafe { &*(user_data as *const Handler) };
    let responder = Responder { raw: Some(responder) };
    let request = HttpRequest::from_c(unsafe { &*request });
    // A panic must not unwind into C++; the dropped responder answers 500 for the request that panicked.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| handler(request, responder)));
}

unsafe extern "C" fn websocket_trampoline(user_data: *mut c_void, socket: ffi::SturdyAsyncObject, request: *const ffi::SturdyHttpRequest) {
    let handler = unsafe { &*(user_data as *const WebSocketHandler) };
    let socket = WebSocket::from_object(Object::from_raw(socket));
    let request = HttpRequest::from_c(unsafe { &*request });
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| handler(socket, request)));
}

/// Owns boxed handlers and frees them when the last owner (a router and every server using it) is gone.
#[derive(Default)]
struct HandlerStore {
    handlers: Mutex<Vec<*mut Handler>>,
    websockets: Mutex<Vec<*mut WebSocketHandler>>,
}

unsafe impl Send for HandlerStore {}
unsafe impl Sync for HandlerStore {}

impl HandlerStore {
    fn add(&self, handler: Handler) -> *mut c_void {
        let pointer = Box::into_raw(Box::new(handler));
        self.handlers.lock().unwrap().push(pointer);
        pointer as *mut c_void
    }

    fn add_websocket(&self, handler: WebSocketHandler) -> *mut c_void {
        let pointer = Box::into_raw(Box::new(handler));
        self.websockets.lock().unwrap().push(pointer);
        pointer as *mut c_void
    }
}

impl Drop for HandlerStore {
    fn drop(&mut self) {
        for pointer in self.handlers.get_mut().unwrap().drain(..) {
            drop(unsafe { Box::from_raw(pointer) });
        }
        for pointer in self.websockets.get_mut().unwrap().drain(..) {
            drop(unsafe { Box::from_raw(pointer) });
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Server
// ------------------------------------------------------------------------------------------------------------------------------

/// Settings for an [`HttpServer`]. `Default` gives the engine's defaults.
#[derive(Debug, Clone)]
pub struct HttpServerOptions {
    pub bind_address: String,
    /// 0: any free port.
    pub port: u16,
    /// PEM files; both or neither (neither: plain HTTP).
    pub tls_certificate_chain_file: Option<String>,
    pub tls_private_key_file: Option<String>,
    /// Reactor threads serving the sockets; 0: automatic.
    pub io_threads: u32,
    pub handler_threads: u32,
    pub max_connections: u32,
    pub max_request_body_bytes: u64,
    pub max_request_header_bytes: u32,
    pub max_response_body_bytes: u64,
    /// Time allowed to drain active sessions on stop; `None`: 5 s.
    pub shutdown_timeout: Option<Duration>,
    /// 0: unlimited.
    pub max_connections_per_ip: u32,
    /// Request bodies larger than this go to a temporary file ([`HttpRequest::body_file`]); 0: always in memory.
    pub spool_requests_above: u64,
    /// Largest spooled upload (bigger ones get 413); 0: 1 GiB.
    pub max_spooled_body_bytes: u64,
    pub http2: bool,
    /// Streams one HTTP/2 connection may have in flight; 0: 100.
    pub http2_max_concurrent_streams: u32,
    /// Also serve HTTP/3 over QUIC (needs TLS) and advertise it with `Alt-Svc`.
    pub http3: bool,
    /// UDP port for HTTP/3; 0: the same number as the TCP port.
    pub http3_port: u16,
    pub http3_max_concurrent_streams: u32,
    pub http3_max_connections: u64,
    pub http3_max_connections_per_address: u64,
    // WebSockets upgraded through `on_websocket`:
    pub websocket_max_message_bytes: u64,
    pub websocket_max_queued_send_bytes: u64,
    pub websocket_idle_timeout: Option<Duration>,
    pub websocket_compression: bool,
    /// If non-empty, an upgrade request carrying an Origin header must match one exactly.
    pub websocket_allowed_origins: Vec<String>,
    pub websocket_subprotocols: Vec<String>,
    /// Upgraded sockets alive at once; 0: 1024.
    pub max_websockets: u64,
}

impl Default for HttpServerOptions {
    fn default() -> Self {
        Self {
            bind_address: "0.0.0.0".into(),
            port: 0,
            tls_certificate_chain_file: None,
            tls_private_key_file: None,
            io_threads: 0,
            handler_threads: 0,
            max_connections: 256,
            max_request_body_bytes: 1 << 20,
            max_request_header_bytes: 32 << 10,
            max_response_body_bytes: 8 << 20,
            shutdown_timeout: None,
            max_connections_per_ip: 0,
            spool_requests_above: 0,
            max_spooled_body_bytes: 0,
            http2: true,
            http2_max_concurrent_streams: 0,
            http3: false,
            http3_port: 0,
            http3_max_concurrent_streams: 0,
            http3_max_connections: 0,
            http3_max_connections_per_address: 0,
            websocket_max_message_bytes: 0,
            websocket_max_queued_send_bytes: 0,
            websocket_idle_timeout: None,
            websocket_compression: false,
            websocket_allowed_origins: Vec::new(),
            websocket_subprotocols: Vec::new(),
            max_websockets: 0,
        }
    }
}

impl HttpServerOptions {
    fn lower(&self, cx: &mut Lower) -> ffi::SturdyHttpServerOptions {
        let (origins, origin_count) = cx.list(&self.websocket_allowed_origins);
        let (subprotocols, subprotocol_count) = cx.list(&self.websocket_subprotocols);
        ffi::SturdyHttpServerOptions {
            bind_address: cx.string(&self.bind_address),
            port: self.port,
            tls_certificate_chain_file: cx.optional(&self.tls_certificate_chain_file),
            tls_private_key_file: cx.optional(&self.tls_private_key_file),
            handler_threads: self.handler_threads,
            max_connections: self.max_connections,
            max_request_body_bytes: self.max_request_body_bytes,
            max_request_header_bytes: self.max_request_header_bytes,
            max_response_body_bytes: self.max_response_body_bytes,
            max_connections_per_ip: self.max_connections_per_ip,
            no_http2: flag(!self.http2),
            enable_http3: flag(self.http3),
            http3_port: self.http3_port,
            io_threads: self.io_threads,
            shutdown_timeout_ms: millis(self.shutdown_timeout) as u32,
            spool_requests_above: self.spool_requests_above,
            max_spooled_body_bytes: self.max_spooled_body_bytes,
            http2_max_concurrent_streams: self.http2_max_concurrent_streams,
            http3_max_concurrent_streams: self.http3_max_concurrent_streams,
            http3_max_connections: self.http3_max_connections,
            http3_max_connections_per_address: self.http3_max_connections_per_address,
            websocket_max_message_bytes: self.websocket_max_message_bytes,
            websocket_max_queued_send_bytes: self.websocket_max_queued_send_bytes,
            websocket_idle_timeout_ms: millis(self.websocket_idle_timeout),
            websocket_compression: flag(self.websocket_compression),
            websocket_allowed_origins: origins,
            websocket_allowed_origin_count: origin_count,
            websocket_subprotocols: subprotocols,
            websocket_subprotocol_count: subprotocol_count,
            max_websockets: self.max_websockets,
        }
    }
}

/// An HTTP(S) server. Stops (and drains) when dropped.
pub struct HttpServer {
    object: Option<Object>,
    handlers: Arc<HandlerStore>,
}

impl HttpServer {
    /// Starts serving. `handler` runs on a handler thread for each request and must answer through its [`Responder`].
    pub fn start(options: &HttpServerOptions, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<HttpServer> {
        let server = Self::create(options, handler)?;
        server.begin()?;
        Ok(server)
    }

    /// Creates the server without starting it, so WebSocket paths can be added with [`HttpServer::on_websocket`] first.
    pub fn create(options: &HttpServerOptions, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<HttpServer> {
        let handlers = Arc::new(HandlerStore::default());
        let user_data = handlers.add(Box::new(handler));
        let mut cx = Lower::default();
        let lowered = options.lower(&mut cx);
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_http_server_create(&lowered, Some(http_trampoline), user_data, &mut raw) })?;
        Ok(HttpServer { object: Some(Object::from_raw(raw)), handlers })
    }

    /// Starts a server that serves `router`.
    pub fn start_router(options: &HttpServerOptions, router: &Router) -> Result<HttpServer> {
        let mut cx = Lower::default();
        let lowered = options.lower(&mut cx);
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_http_server_start_router(&lowered, router.object.raw(), &mut raw) })?;
        // The router's handlers must outlive this server: share their store.
        Ok(HttpServer { object: Some(Object::from_raw(raw)), handlers: router.handlers.clone() })
    }

    /// Upgrades requests whose path (without query) equals `path` to WebSockets, on the same port. Call before [`HttpServer::begin`].
    pub fn on_websocket(&self, path: &str, handler: impl Fn(WebSocket, HttpRequest) + Send + Sync + 'static) -> Result<()> {
        let user_data = self.handlers.add_websocket(Box::new(handler));
        let mut cx = Lower::default();
        let path = cx.string(path);
        check(unsafe { ffi::sturdy_http_server_on_websocket(self.raw(), path, Some(websocket_trampoline), user_data) })
    }

    /// Opens the listener and starts serving.
    pub fn begin(&self) -> Result<()> {
        check(unsafe { ffi::sturdy_http_server_begin(self.raw()) })
    }

    fn raw(&self) -> ffi::SturdyAsyncObject {
        self.object.as_ref().map(Object::raw).unwrap_or_default()
    }

    pub fn port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_http_server_port(self.raw(), &mut port) })?;
        Ok(port)
    }

    /// The UDP port HTTP/3 is served on (0 when it is not).
    pub fn http3_port(&self) -> Result<u16> {
        let mut port = 0u16;
        check(unsafe { ffi::sturdy_http_server_http3_port(self.raw(), &mut port) })?;
        Ok(port)
    }

    /// `running`, `tls`, `http2` ("1"/"0"), `port`, `http3_port`.
    pub async fn info(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_http_server_info(self.raw(), out) }).await?.fields())
    }

    /// Counters (decimal text): `requests`, `responses_2xx`..`responses_5xx`, `connections_accepted`, `websocket_upgrades`, ...
    pub async fn metrics(&self) -> Result<Vec<(String, String)>> {
        Ok(run(|out| unsafe { ffi::sturdy_http_server_metrics(self.raw(), out) }).await?.fields())
    }

    pub fn stop(&self) {
        unsafe { ffi::sturdy_http_server_stop(self.raw()) };
    }
}

impl Drop for HttpServer {
    fn drop(&mut self) {
        // Releasing the object stops the server and waits for in-flight handlers; only then may the handlers be freed (when `handlers`
        // drops after this).
        self.object = None;
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Router
// ------------------------------------------------------------------------------------------------------------------------------

/// Static file serving options.
#[derive(Debug, Clone, Default)]
pub struct StaticFiles {
    /// Directory served; files outside it are unreachable, including through symlinks.
    pub root: String,
    /// Served for a request naming a directory; `None`: "index.html".
    pub index_file: Option<String>,
    /// Sent as Cache-Control; `None`: "public, max-age=300".
    pub cache_control: Option<String>,
    /// Files and directories starting with '.' are hidden (404) unless set.
    pub allow_dotfiles: bool,
    /// Allow symlinks that point outside `root`.
    pub follow_symlinks: bool,
    /// Do not serve `name.br` / `name.gz` siblings to clients that accept them.
    pub no_precompressed: bool,
    /// Served (status 200) when nothing matches a GET — single-page-app routing.
    pub spa_fallback: Option<String>,
    /// Extra or overriding MIME mappings (extension with dot -> type).
    pub mime_types: Vec<(String, String)>,
}

/// A URL router. Patterns are '/'-separated literals, `:name` (one segment) and a final `*name` (the rest); literals beat parameters,
/// which beat wildcards. Build it completely, then serve it with [`HttpServer::start_router`]: a running server holds an immutable snapshot.
pub struct Router {
    object: Object,
    handlers: Arc<HandlerStore>,
}

impl Router {
    pub fn new() -> Result<Router> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_router_create(&mut raw) })?;
        Ok(Router { object: Object::from_raw(raw), handlers: Arc::new(HandlerStore::default()) })
    }

    /// Adds a route (`method` upper-case). `handler` answers through its [`Responder`] with `path`, `query` and `params` filled in.
    pub fn route(&self, method: &str, pattern: &str, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<&Self> {
        let user_data = self.handlers.add(Box::new(handler));
        let mut cx = Lower::default();
        let (method, pattern) = (cx.string(method), cx.string(pattern));
        check(unsafe { ffi::sturdy_router_route(self.object.raw(), method, pattern, Some(http_trampoline), user_data) })?;
        Ok(self)
    }

    pub fn get(&self, pattern: &str, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<&Self> {
        self.route("GET", pattern, handler)
    }

    pub fn post(&self, pattern: &str, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<&Self> {
        self.route("POST", pattern, handler)
    }

    pub fn put(&self, pattern: &str, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<&Self> {
        self.route("PUT", pattern, handler)
    }

    pub fn patch(&self, pattern: &str, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<&Self> {
        self.route("PATCH", pattern, handler)
    }

    pub fn delete(&self, pattern: &str, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<&Self> {
        self.route("DELETE", pattern, handler)
    }

    /// Adds middleware that runs before every matching route: return `Some(response)` to answer the request itself (auth, rate limits),
    /// `None` to let it continue to the route.
    pub fn middleware_fn(&self, check_request: impl Fn(&HttpRequest) -> Option<Response> + Send + Sync + 'static) -> Result<&Self> {
        let user_data = self.handlers.add(Box::new(move |request: HttpRequest, responder: Responder| match check_request(&request) {
            Some(response) => {
                let _ = responder.send(&response);
            }
            None => responder.pass(),
        }));
        check(unsafe { ffi::sturdy_router_use(self.object.raw(), Some(http_trampoline), user_data) })?;
        Ok(self)
    }

    /// Serves files under `mount` (e.g. "/assets") from `options.root`, GET and HEAD only.
    pub fn static_files(&self, mount: &str, options: &StaticFiles) -> Result<&Self> {
        let mut cx = Lower::default();
        let mount = cx.string(mount);
        let (mime_types, mime_type_count) = cx.pairs(&options.mime_types);
        let lowered = ffi::SturdyStaticFilesOptions {
            root: cx.string(&options.root),
            index_file: cx.optional(&options.index_file),
            cache_control: cx.optional(&options.cache_control),
            allow_dotfiles: flag(options.allow_dotfiles),
            follow_symlinks: flag(options.follow_symlinks),
            no_precompressed: flag(options.no_precompressed),
            spa_fallback: cx.optional(&options.spa_fallback),
            mime_types,
            mime_type_count,
        };
        check(unsafe { ffi::sturdy_router_static_files(self.object.raw(), mount, &lowered) })?;
        Ok(self)
    }

    /// Replaces the default 404. The handler must answer before it returns.
    pub fn not_found(&self, handler: impl Fn(HttpRequest, Responder) + Send + Sync + 'static) -> Result<&Self> {
        let user_data = self.handlers.add(Box::new(handler));
        check(unsafe { ffi::sturdy_router_not_found(self.object.raw(), Some(http_trampoline), user_data) })?;
        Ok(self)
    }

    /// Middleware answering 429 with Retry-After once one client address exceeds `rate_per_second` (bursts up to `burst`).
    pub fn rate_limit(&self, rate_per_second: f64, burst: f64, max_keys: u64) -> Result<&Self> {
        check(unsafe { ffi::sturdy_router_rate_limit(self.object.raw(), rate_per_second, burst, max_keys) })?;
        Ok(self)
    }

    /// Runs one request through the router synchronously (tests, in-process calls): `(status, headers, body)`.
    pub async fn dispatch(&self, method: &str, target: &str, remote_address: &str, headers: &[(String, String)], body: &[u8]) -> Result<(u16, Vec<(String, String)>, Vec<u8>)> {
        let mut cx = Lower::default();
        let (method, target, remote) = (cx.string(method), cx.string(target), cx.string(remote_address));
        let (headers_ptr, header_count) = cx.pairs(headers);
        let (body_ptr, body_len) = bytes_ptr(body);
        let reply = run(|out| unsafe { ffi::sturdy_router_dispatch(self.object.raw(), method, target, remote, headers_ptr, header_count, body_ptr, body_len, out) }).await?;
        Ok((reply.number() as u16, reply.fields(), reply.bytes()))
    }
}

impl Responder {
    /// Lets a middleware's request continue to the route: the responder is released unanswered, without the 500 `Drop` would send. (The
    /// engine discards the handle when the middleware callback returns.)
    fn pass(mut self) {
        self.raw = None;
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Rate limiter, cookie jar
// ------------------------------------------------------------------------------------------------------------------------------

/// A keyed token bucket: each key may use `rate_per_second` operations per second on average, with bursts up to `burst`.
pub struct RateLimiter {
    object: Object,
}

impl RateLimiter {
    /// `max_keys` 0: 10000 (the oldest keys are evicted past it).
    pub fn new(rate_per_second: f64, burst: f64, max_keys: u64) -> Result<RateLimiter> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_rate_limiter_create(rate_per_second, burst, max_keys, &mut raw) })?;
        Ok(RateLimiter { object: Object::from_raw(raw) })
    }

    /// Takes `cost` tokens for `key` if available.
    pub fn try_acquire(&self, key: &str, cost: f64) -> Result<bool> {
        let mut cx = Lower::default();
        let key = cx.string(key);
        let mut acquired = 0u8;
        check(unsafe { ffi::sturdy_rate_limiter_try_acquire(self.object.raw(), key, cost, &mut acquired) })?;
        Ok(acquired != 0)
    }

    /// How long until `key` could take `cost` tokens (zero if it could now).
    pub fn retry_after(&self, key: &str, cost: f64) -> Result<Duration> {
        let mut cx = Lower::default();
        let key = cx.string(key);
        let mut millis = 0i64;
        check(unsafe { ffi::sturdy_rate_limiter_retry_after_ms(self.object.raw(), key, cost, &mut millis) })?;
        Ok(Duration::from_millis(millis.max(0) as u64))
    }

    pub fn tracked_keys(&self) -> Result<u64> {
        let mut keys = 0u64;
        check(unsafe { ffi::sturdy_rate_limiter_tracked_keys(self.object.raw(), &mut keys) })?;
        Ok(keys)
    }
}

/// One cookie (RFC 6265).
#[derive(Debug, Clone, Default)]
pub struct Cookie {
    pub name: String,
    pub value: String,
    /// Lowercase, no leading dot.
    pub domain: String,
    /// `None`: "/".
    pub path: Option<String>,
    /// The Domain attribute was given: subdomains receive it too. Off: only the exact host.
    pub domain_wide: bool,
    /// Only sent over https.
    pub secure: bool,
    pub http_only: bool,
    /// Seconds since the Unix epoch; 0: a session cookie.
    pub expires_unix: i64,
}

/// A thread-safe cookie store: a client fills it from `Set-Cookie` and sends matching cookies.
#[derive(Clone)]
pub struct CookieJar {
    object: Arc<Object>,
}

impl core::fmt::Debug for CookieJar {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str("CookieJar")
    }
}

impl CookieJar {
    pub fn new() -> Result<CookieJar> {
        let mut raw = ffi::SturdyAsyncObject::default();
        check(unsafe { ffi::sturdy_cookie_jar_create(&mut raw) })?;
        Ok(CookieJar { object: Arc::new(Object::from_raw(raw)) })
    }

    pub(crate) fn from_object(object: Object) -> CookieJar {
        CookieJar { object: Arc::new(object) }
    }

    pub(crate) fn raw(&self) -> ffi::SturdyAsyncObject {
        self.object.raw()
    }

    pub fn add(&self, cookie: &Cookie) -> Result<()> {
        let mut cx = Lower::default();
        let lowered = ffi::SturdyCookie {
            name: cx.string(&cookie.name),
            value: cx.string(&cookie.value),
            domain: cx.string(&cookie.domain),
            path: cx.optional(&cookie.path),
            domain_wide: flag(cookie.domain_wide),
            secure: flag(cookie.secure),
            http_only: flag(cookie.http_only),
            expires_unix: cookie.expires_unix,
        };
        check(unsafe { ffi::sturdy_cookie_jar_add(self.object.raw(), &lowered) })
    }

    /// Stores the cookies a response set.
    pub fn store_from_response(&self, host: &str, path: &str, https: bool, response_headers: &[(String, String)]) -> Result<()> {
        let mut cx = Lower::default();
        let (host, path) = (cx.string(host), cx.string(path));
        let (headers, header_count) = cx.pairs(response_headers);
        check(unsafe { ffi::sturdy_cookie_jar_store_from_response(self.object.raw(), host, path, flag(https), headers, header_count) })
    }

    /// "a=1; b=2" for the cookies that apply to a request (empty if none).
    pub fn header_for(&self, host: &str, path: &str, https: bool) -> Result<String> {
        let mut cx = Lower::default();
        let (host, path) = (cx.string(host), cx.string(path));
        crate::read_string(|b, c, l| unsafe { ffi::sturdy_cookie_jar_header_for(self.object.raw(), host, path, flag(https), b, c, l) })
    }

    /// Every stored cookie.
    pub async fn all(&self) -> Result<Vec<Cookie>> {
        let reply = run(|out| unsafe { ffi::sturdy_cookie_jar_all(self.object.raw(), out) }).await?;
        let fields = reply.fields();
        let mut cookies = Vec::new();
        for index in 0.. {
            let find = |prefix: &str| fields.iter().find(|(k, _)| *k == format!("{prefix}:{index}")).map(|(_, v)| v.clone());
            let Some(name) = find("name") else { break };
            cookies.push(Cookie {
                name,
                value: find("value").unwrap_or_default(),
                domain: find("domain").unwrap_or_default(),
                path: find("path"),
                domain_wide: find("domain_wide").as_deref() == Some("1"),
                secure: find("secure").as_deref() == Some("1"),
                http_only: find("http_only").as_deref() == Some("1"),
                expires_unix: find("expires").and_then(|e| e.parse().ok()).unwrap_or(0),
            });
        }
        Ok(cookies)
    }

    pub fn clear(&self) -> Result<()> {
        check(unsafe { ffi::sturdy_cookie_jar_clear(self.object.raw()) })
    }
}
