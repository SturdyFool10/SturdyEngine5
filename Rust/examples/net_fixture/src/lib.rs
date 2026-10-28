//! Self-test for `sturdy-net`: every protocol family through the safe layer, against the real engine.
//!
//! Exported as `sturdy_net_selftest(message, capacity) -> failures`; the host test prints `message` when anything failed.

use std::cell::RefCell;
use std::future::Future;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;

use sturdy_net::*;

mod more;

thread_local! {
    static FAILURES: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

fn check(ok: bool, what: &str) {
    if !ok {
        FAILURES.with(|f| f.borrow_mut().push(what.to_string()));
    }
}

fn expect<T>(result: Result<T>, what: &str) -> Option<T> {
    match result {
        Ok(value) => Some(value),
        Err(error) => {
            check(false, &format!("{what}: {error}"));
            None
        }
    }
}

/// Polls two futures concurrently on the calling thread (a tiny `join!`).
async fn join2<A: Future, B: Future>(a: A, b: B) -> (A::Output, B::Output) {
    let mut a = Box::pin(a);
    let mut b = Box::pin(b);
    let (mut ra, mut rb) = (None, None);
    std::future::poll_fn(move |cx| {
        if ra.is_none() {
            if let std::task::Poll::Ready(v) = a.as_mut().poll(cx) {
                ra = Some(v);
            }
        }
        if rb.is_none() {
            if let std::task::Poll::Ready(v) = b.as_mut().poll(cx) {
                rb = Some(v);
            }
        }
        if ra.is_some() && rb.is_some() {
            std::task::Poll::Ready((ra.take().unwrap(), rb.take().unwrap()))
        } else {
            std::task::Poll::Pending
        }
    })
    .await
}

async fn tcp() {
    let Some(listener) = expect(TcpListener::listen("127.0.0.1", 0, None).await, "listen") else { return };
    let port = listener.port().unwrap_or(0);
    let (server, client) = join2(listener.accept(), TcpStream::connect("127.0.0.1", port, None, None)).await;
    let (Some(server), Some(client)) = (expect(server, "accept"), expect(client, "connect")) else { return };
    expect(client.send(b"hello rust\n").await, "send");
    check(expect(server.receive_until("\n", 100).await, "receive_until").as_deref() == Some(b"hello rust\n".as_slice()), "line arrives");
    expect(server.send(b"0123456789").await, "send back");
    check(expect(client.receive_exact(10).await, "receive_exact").as_deref() == Some(b"0123456789".as_slice()), "exact bytes arrive");
    expect(client.shutdown_write().await, "shutdown_write");
    check(expect(server.receive(10).await, "receive at EOF").map_or(false, |b| b.is_empty()), "half close reads as end of stream");
    let info = expect(client.info().await, "info").unwrap_or_default();
    check(info.iter().any(|(k, v)| k == "peer" && v.starts_with("127.0.0.1:")), "info reports the peer");

    // Refused connection reports a kind and a message.
    match TcpStream::connect("127.0.0.1", 1, None, Some(Duration::from_secs(2))).await {
        Ok(_) => check(false, "connecting to a closed port must fail"),
        Err(error) => check(!error.message.is_empty() && error.kind != ErrorKind::Cancelled, "a refused connect carries an error"),
    }

    // TLS with a generated identity, pinned.
    let Some(identity) = expect(generate_identity("rust.test", 30).await, "identity") else { return };
    check(certificate_sha256(&identity.certificate_pem).ok().as_deref() == Some(identity.sha256.as_str()), "digest matches");
    let tls_server = TlsServer::new(identity.certificate_pem.clone(), identity.private_key_pem.clone());
    let Some(secure) = expect(TcpListener::listen("127.0.0.1", 0, Some(&tls_server)).await, "tls listen") else { return };
    let secure_port = secure.port().unwrap_or(0);
    let pinned = TlsClient::pinned(identity.sha256.clone());
    let (s, c) = join2(secure.accept(), TcpStream::connect("127.0.0.1", secure_port, Some(&pinned), None)).await;
    if let (Some(s), Some(c)) = (expect(s, "tls accept"), expect(c, "tls connect")) {
        expect(c.send(b"secret").await, "tls send");
        check(expect(s.receive_exact(6).await, "tls receive").as_deref() == Some(b"secret".as_slice()), "TLS data arrives");
    }
    let wrong = TlsClient::pinned("f".repeat(64));
    check(TcpStream::connect("127.0.0.1", secure_port, Some(&wrong), None).await.is_err(), "a wrong pin is refused");
}

async fn udp_and_misc() {
    let (Some(a), Some(b)) = (
        expect(UdpSocket::bind("127.0.0.1", 0, false, false).await, "bind a"),
        expect(UdpSocket::bind("127.0.0.1", 0, false, false).await, "bind b"),
    ) else {
        return;
    };
    expect(a.send_to("127.0.0.1", b.port().unwrap_or(0), b"ping").await, "udp send");
    if let Some(datagram) = expect(b.receive(100).await, "udp receive") {
        check(datagram.data == b"ping" && datagram.from_port == a.port().unwrap_or(1), "datagram and sender port");
    }
    check(!expect(resolve("localhost").await, "resolve").unwrap_or_default().is_empty(), "localhost resolves");

    // Dropping a pending future cancels it: this sleep would otherwise hold the test for a minute.
    let started = std::time::Instant::now();
    {
        let mut pending = Box::pin(sleep(Duration::from_secs(60)));
        let waker = std::task::Waker::noop();
        let mut cx = std::task::Context::from_waker(waker);
        let _ = pending.as_mut().poll(&mut cx);
    }
    check(started.elapsed() < Duration::from_secs(5), "dropping a pending future returns promptly");
    expect(sleep(Duration::from_millis(20)).await, "sleep");
}

async fn http() {
    let seen = Arc::new(AtomicUsize::new(0));
    let counter = seen.clone();
    let options = HttpServerOptions { bind_address: "127.0.0.1".into(), ..Default::default() };
    let Some(server) = expect(
        HttpServer::start(&options, move |request, responder| {
            counter.fetch_add(1, Ordering::SeqCst);
            if request.target == "/panic" {
                panic!("handler panics must not cross into C++");
            }
            if request.target == "/later" {
                std::thread::spawn(move || {
                    std::thread::sleep(Duration::from_millis(30));
                    let _ = responder.respond(200, &[], b"later");
                });
                return;
            }
            let reply = format!("{} {} {}", request.method, request.target, String::from_utf8_lossy(&request.body));
            let _ = responder.respond(201, &[("X-Method".into(), request.method.clone())], reply.as_bytes());
        }),
        "http server",
    ) else {
        return;
    };
    let port = server.port().unwrap_or(0);
    let Some(client) = expect(HttpClient::new(&HttpClientOptions::default()), "http client") else { return };
    let base = format!("http://127.0.0.1:{port}");
    if let Some(r) = expect(client.post(&format!("{base}/echo?x=1"), "text/plain", b"payload").await, "post") {
        check(r.status == 201 && r.text() == "POST /echo?x=1 payload" && r.header("x-method") == Some("POST"), "request through the closure handler");
    }
    check(expect(client.get(&format!("{base}/later")).await, "later").map(|r| r.text()).as_deref() == Some("later"), "a response sent later from another thread");
    check(expect(client.get(&format!("{base}/panic")).await, "panic").map(|r| r.status) == Some(500), "a panicking handler answers 500");
    check(seen.load(Ordering::SeqCst) >= 3, "the handler ran");
    check(server.http3_port().unwrap_or(1) == 0, "no HTTP/3 unless asked for");
    let metrics = expect(server.metrics().await, "metrics").unwrap_or_default();
    check(metrics.iter().any(|(k, _)| k == "requests"), "metrics list the request counter");
}

async fn websocket() {
    let options = WebSocketServerOptions { bind_address: "127.0.0.1".into(), subprotocols: vec!["rs.v1".into()], ..Default::default() };
    let Some(server) = expect(WebSocketServer::listen(&options).await, "ws listen") else { return };
    let url = format!("ws://127.0.0.1:{}/chat", server.port().unwrap_or(0));
    let client_options = WebSocketClientOptions { subprotocols: vec!["rs.v1".into()], ..Default::default() };
    let (peer, client) = join2(server.accept(), WebSocket::connect(&url, &client_options)).await;
    let (Some(peer), Some(client)) = (expect(peer, "ws accept"), expect(client, "ws connect")) else { return };
    expect(client.send_text("hi").await, "send_text");
    check(expect(peer.receive().await, "ws receive") == Some(Message::Text("hi".into())), "text message");
    expect(peer.send_binary(&[1, 2, 3]).await, "send_binary");
    check(expect(client.receive().await, "ws receive binary") == Some(Message::Binary(vec![1, 2, 3])), "binary message");
    let info = expect(client.info().await, "ws info").unwrap_or_default();
    check(info.iter().any(|(k, v)| k == "subprotocol" && v == "rs.v1"), "subprotocol negotiated");
    expect(client.close(1000, "bye").await, "close");
    check(!client.is_open(), "closed after the close completes");
}

async fn quic() {
    if features() & feature::QUIC == 0 {
        return;
    }
    let Some(identity) = expect(generate_identity("quic.rust", 30).await, "identity") else { return };
    let mut tls = TlsServer::new(identity.certificate_pem.clone(), identity.private_key_pem.clone());
    tls.alpn = vec!["rust-test".into()];
    let options = QuicServerOptions { tls, bind_address: "127.0.0.1".into(), ..Default::default() };
    let Some(listener) = expect(QuicListener::listen(&options).await, "quic listen") else { return };
    let port = listener.port().unwrap_or(0);
    let mut client_tls = TlsClient::pinned(identity.sha256.clone());
    client_tls.alpn = vec!["rust-test".into()];
    let (server, client) = join2(listener.accept(), QuicConnection::connect("127.0.0.1", port, &QuicLimits::default(), &client_tls)).await;
    let (Some(server), Some(client)) = (expect(server, "quic accept"), expect(client, "quic connect")) else { return };
    let Some(stream) = expect(client.open_stream(true).await, "open stream") else { return };
    expect(stream.send(b"over quic").await, "stream send");
    expect(stream.finish().await, "finish");
    if let Some(peer_stream) = expect(server.accept_stream().await, "accept stream") {
        check(expect(peer_stream.read_to_end(100).await, "read_to_end").as_deref() == Some(b"over quic".as_slice()), "stream bytes arrive");
        check(peer_stream.is_bidirectional(), "stream direction");
    }
    check(client.max_datagram_size() > 100, "datagrams available");
    expect(client.send_datagram(b"dg"), "send datagram");
    check(expect(server.receive_datagram().await, "receive datagram").as_deref() == Some(b"dg".as_slice()), "datagram arrives");
    check(client.send_datagram(&vec![0u8; client.max_datagram_size() + 70_000]).is_err(), "an oversized datagram is refused");
    let info = expect(client.info().await, "info").unwrap_or_default();
    check(info.iter().any(|(k, v)| k == "alpn" && v == "rust-test"), "ALPN reported");
    let (closed, ()) = join2(server.closed(), async { client.close(7, "rust done") }).await;
    if let Some(closed) = expect(closed, "closed") {
        check(closed.code == 7 && closed.reason == "rust done" && closed.from_peer, "close code reaches the peer");
    }
}

async fn files_and_processes() {
    let dir = std::env::temp_dir().join("sturdy_net_rust_selftest");
    let _ = std::fs::remove_dir_all(&dir);
    let _ = std::fs::create_dir_all(&dir);
    let path = dir.join("f.bin").to_string_lossy().into_owned();
    if let Some(file) = expect(File::open(&path, OpenMode::READ | OpenMode::WRITE | OpenMode::CREATE).await, "open") {
        expect(file.write(b"0123456789").await, "write");
        expect(file.write_at(2, b"XY").await, "write_at");
        check(expect(file.read_at(0, 10).await, "read_at").as_deref() == Some(b"01XY456789".as_slice()), "positional write is visible");
        check(expect(file.size().await, "size") == Some(10), "size");
        expect(file.seek(0, SeekFrom::Start).await, "seek");
        check(expect(file.read_to_end(100).await, "read_to_end").as_deref() == Some(b"01XY456789".as_slice()), "read_to_end");
        expect(file.sync(false).await, "sync");
    }
    check(File::open("/definitely/not/here/x", OpenMode::READ).await.map(|_| ()).unwrap_err().kind == ErrorKind::NotFound, "a missing file is NotFound");

    if let Some(watcher) = expect(FileWatcher::watch(&dir.to_string_lossy(), false, Duration::from_millis(20), false), "watch") {
        let next = watcher.next();
        let write = async {
            sleep(Duration::from_millis(100)).await.ok();
            let _ = std::fs::write(dir.join("made.txt"), "x");
        };
        let (event, ()) = join2(next, write).await;
        check(expect(event, "watch event").map_or(false, |e| e.path.contains("made.txt")), "a created file is reported");
    }

    if features() & feature::PROCESS != 0 {
        let command = Command::new("sh").args(["-c", "cat; echo err >&2; exit 3"]);
        if let Some(output) = expect(command.run(b"fed", Some(Duration::from_secs(5)), 0).await, "run") {
            check(output.stdout == b"fed" && output.stderr == "err\n" && output.code == 3 && !output.success, "process output and exit code");
        }
        if let Some(child) = expect(command.spawn(), "spawn") {
            expect(child.write_stdin(b"live").await, "stdin");
            child.close_stdin();
            check(expect(child.read_stdout(100).await, "stdout").as_deref() == Some(b"live".as_slice()), "child stdout");
            check(expect(child.wait().await, "wait") == Some(3), "child exit code");
        }
        check(Command::new("definitely-not-a-program-xyz").spawn().is_err(), "a missing program fails to spawn");
    }
    let _ = std::fs::remove_dir_all(&dir);
}

async fn relay_and_peers() {
    let Some(relay) = expect(RelayServer::start(&RelayOptions { bind_address: "127.0.0.1".into(), ..Default::default() }), "relay") else { return };
    let port = relay.port().unwrap_or(0);
    let Some(token) = expect(random_relay_token(), "token") else { return };
    check(token.len() == 32, "token length");
    let (a, b) = join2(relay_connect("127.0.0.1", port, &token, true, None, None), relay_connect("127.0.0.1", port, &token, false, None, None)).await;
    if let (Some(a), Some(b)) = (expect(a, "relay dial"), expect(b, "relay accept")) {
        expect(a.send(b"relayed").await, "relay send");
        check(expect(b.receive_exact(7).await, "relay receive").as_deref() == Some(b"relayed".as_slice()), "bytes cross the relay");
    }

    let options = PeerOptions { network_id: "rust-net".into(), display_name: "rs".into(), bind_address: "127.0.0.1".into(), ..Default::default() };
    let (Some(first), Some(second)) = (expect(Peer::start(&options).await, "peer 1"), expect(Peer::start(&options).await, "peer 2")) else { return };
    let (Ok(first_id), Ok(first_port)) = (first.id(), first.listen_port()) else { return check(false, "peer id/port") };
    let linked = expect(second.connect("127.0.0.1", first_port, Some(&first_id)).await, "peer connect");
    check(linked.map_or(false, |p| p.id == first_id), "pinned peer link");
    expect(second.send(&first_id, 5, b"peer hello").await, "peer send");
    let mut seen = false;
    for _ in 0..8 {
        match first.next_event().await {
            Ok(PeerEvent::Message { channel: 5, data, .. }) if data == b"peer hello" => {
                seen = true;
                break;
            }
            Ok(_) => {}
            Err(_) => break,
        }
    }
    check(seen, "peer message arrives as an event");
    check(expect(first.peers().await, "peers").map_or(false, |p| !p.is_empty()), "peers are listed");
    first.stop();
    second.stop();
    let _ = default_gateway();
}

fn parallel() {
    let counter = AtomicUsize::new(0);
    expect(par::for_each(10_000, 0, |_| {
        counter.fetch_add(1, Ordering::Relaxed);
    }), "par for_each");
    check(counter.load(Ordering::Relaxed) == 10_000, "every index visited once");
    let covered = AtomicUsize::new(0);
    expect(par::for_each_chunk(1000, 64, |b, e| {
        covered.fetch_add((e - b) as usize, Ordering::Relaxed);
    }), "par chunks");
    check(covered.load(Ordering::Relaxed) == 1000, "chunks cover the range");
    let ints: Vec<i64> = (1..=100_000).collect();
    check(par::sum(&ints).ok() == Some(100_000 * 100_001 / 2), "par sum");
    check(par::min_max(&ints).ok() == Some((1, 100_000)), "par min/max");
    check(par::min_max::<i64>(&[]).is_err(), "min/max of nothing is an error");
    let mut floats: Vec<f32> = (0..5000).map(|i| ((i * 7919) % 1009) as f32).collect();
    expect(par::sort(&mut floats), "par sort");
    check(floats.windows(2).all(|w| w[0] <= w[1]), "sorted");
    if let Some(pool) = expect(ThreadPool::new(2, "rust-pool"), "pool") {
        check(pool.threads() == 2, "pool size");
        let on_pool = AtomicUsize::new(0);
        expect(pool.for_each(500, 0, |_| {
            on_pool.fetch_add(1, Ordering::Relaxed);
        }), "pool for_each");
        check(on_pool.load(Ordering::Relaxed) == 500, "pool visits every index");
        let ran = AtomicUsize::new(0);
        expect(pool.install(|| {
            ran.fetch_add(1, Ordering::Relaxed);
        }), "install");
        check(ran.load(Ordering::Relaxed) == 1, "install ran the task");
    }
}

/// Runs everything. Returns the number of failed checks; `message` receives their descriptions (NUL-terminated, truncated to `capacity`).
///
/// # Safety
/// `message` must be null or point to `capacity` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sturdy_net_selftest(message: *mut u8, capacity: usize) -> i32 {
    block_on(async {
        tcp().await;
        udp_and_misc().await;
        http().await;
        websocket().await;
        quic().await;
        files_and_processes().await;
        relay_and_peers().await;
        more::serve().await;
        more::concurrency().await;
        more::streams().await;
        more::files_and_streams().await;
        more::primitives().await;
    });
    parallel();
    more::parallel_extras();
    let failures = FAILURES.with(|f| std::mem::take(&mut *f.borrow_mut()));
    if !message.is_null() && capacity > 0 {
        let text = failures.join("; ");
        let bytes = text.as_bytes();
        let n = bytes.len().min(capacity - 1);
        unsafe {
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), message, n);
            *message.add(n) = 0;
        }
    }
    failures.len() as i32
}
