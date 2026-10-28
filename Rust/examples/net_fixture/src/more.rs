//! The second half of the self-test: HTTP serving, concurrency primitives, tasks, streams, the file system and the generic stream layer.

use super::{check, expect, join2};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Duration;
use sturdy_net::*;

fn text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}

pub async fn serve() {
    // Router with parameters, middleware, not_found, rate limit and static files.
    let scratch = std::env::temp_dir().join("sturdy_net_fixture_serve");
    let _ = std::fs::remove_dir_all(&scratch);
    let _ = std::fs::create_dir_all(scratch.join("site"));
    let _ = std::fs::write(scratch.join("site").join("index.html"), "<h1>home</h1>");
    let _ = std::fs::write(scratch.join("range.bin"), "0123456789");

    let Some(router) = expect(Router::new(), "router") else { return };
    expect(router.middleware_fn(|request| request.header("X-Deny").map(|_| Response::new(403).with_body("denied"))), "middleware");
    expect(
        router.get("/users/:id", |request, responder| {
            let reply = format!("user {} path={} query={}", request.param("id").unwrap_or("?"), request.path, request.query);
            let _ = responder.text(200, &reply);
        }),
        "route",
    );
    let range_path = scratch.join("range.bin").to_string_lossy().into_owned();
    expect(
        router.get("/range", move |_, responder| {
            let response = Response::new(200).with_file(BodyFile { path: range_path.clone(), offset: 2, length: Some(5) });
            let _ = responder.send(&response);
        }),
        "range route",
    );
    expect(
        router.get("/stream", |_, responder| {
            let Ok(writer) = responder.stream(&Response::new(200)) else { return };
            block_on(async {
                for piece in ["one,", "two,", "three"] {
                    let _ = writer.write(piece.as_bytes()).await;
                }
            });
            writer.finish();
        }),
        "stream route",
    );
    expect(
        router.get("/sse", |_, responder| {
            let Ok(writer) = responder.sse(&Response::new(200)) else { return };
            block_on(async {
                let _ = writer.write_sse(&SseEvent { data: "a\nb".into(), event: Some("tick".into()), id: None, retry: Some(Duration::from_millis(1500)) }).await;
            });
            writer.finish();
        }),
        "sse route",
    );
    expect(router.not_found(|request, responder| { let _ = responder.text(404, &format!("nothing at {}", request.path)); }), "not_found");
    let files = StaticFiles { root: scratch.join("site").to_string_lossy().into_owned(), ..StaticFiles::default() };
    expect(router.static_files("/static", &files), "static files");
    check(router.static_files("/x", &StaticFiles { root: "/definitely/not/here".into(), ..StaticFiles::default() }).is_err(), "a bad static root is refused");

    let options = HttpServerOptions { bind_address: "127.0.0.1".into(), ..HttpServerOptions::default() };
    let Some(server) = expect(HttpServer::start_router(&options, &router), "server on a router") else { return };
    drop(router); // the server keeps what it needs
    let base = format!("http://127.0.0.1:{}", server.port().unwrap_or(0));
    let Some(client) = expect(HttpClient::new(&HttpClientOptions::default()), "client") else { return };
    if let Some(r) = expect(client.get(&format!("{base}/users/7?q=1")).await, "routed get") {
        check(r.status == 200 && r.text() == "user 7 path=/users/7 query=q=1", "route parameters and query");
    }
    if let Some(r) = expect(client.request("GET", &format!("{base}/users/7"), &[("X-Deny".into(), "1".into())], b"").await, "denied get") {
        check(r.status == 403 && r.text() == "denied", "middleware answers first");
    }
    if let Some(r) = expect(client.get(&format!("{base}/static/index.html")).await, "static") {
        check(r.text() == "<h1>home</h1>", "static file");
    }
    if let Some(r) = expect(client.get(&format!("{base}/nowhere")).await, "404") {
        check(r.status == 404 && r.text() == "nothing at /nowhere", "custom not_found");
    }
    if let Some(r) = expect(client.get(&format!("{base}/range")).await, "range") {
        check(r.text() == "23456", "file range body");
    }
    if let Some(r) = expect(client.get(&format!("{base}/stream")).await, "stream") {
        check(r.text() == "one,two,three", "streamed body");
    }
    if let Some(r) = expect(client.get(&format!("{base}/sse")).await, "sse") {
        check(r.header("content-type") == Some("text/event-stream") && r.text() == "event: tick\nretry: 1500\ndata: a\ndata: b\n\n", "server-sent events");
    }
    drop(server);

    // WebSocket on the HTTP port, plus a cookie jar shared with a client.
    let ws_options = HttpServerOptions { bind_address: "127.0.0.1".into(), ..HttpServerOptions::default() };
    let Some(ws_server) = expect(
        HttpServer::create(&ws_options, |request, responder| {
            if request.target == "/set" {
                let _ = responder.send(&Response::new(200).with_header("Set-Cookie", "session=xyz; Path=/").with_body("set"));
            } else {
                let cookie = request.header("Cookie").unwrap_or("").to_string();
                let _ = responder.text(200, &format!("cookie={cookie}"));
            }
        }),
        "create",
    ) else {
        return;
    };
    expect(
        ws_server.on_websocket("/ws", |socket, _request| {
            block_on(async {
                if let Ok(Message::Text(message)) = socket.receive().await {
                    let _ = socket.send_text(&format!("echo:{message}")).await;
                }
                let _ = socket.close(1000, "bye").await;
            });
        }),
        "on_websocket",
    );
    expect(ws_server.begin(), "begin");
    let port = ws_server.port().unwrap_or(0);
    if let Some(socket) = expect(WebSocket::connect(&format!("ws://127.0.0.1:{port}/ws"), &WebSocketClientOptions::default()).await, "ws connect") {
        expect(socket.send_text("hi").await, "ws send");
        check(socket.receive().await == Ok(Message::Text("echo:hi".into())), "the HTTP-server handler echoes");
        check(socket.receive().await.is_err(), "and then closes");
    }
    let Some(jar) = expect(CookieJar::new(), "jar") else { return };
    let jar_client = HttpClient::new(&HttpClientOptions { cookie_jar: Some(jar.clone()), ..HttpClientOptions::default() });
    if let Some(jar_client) = expect(jar_client, "cookie client") {
        let _ = jar_client.get(&format!("http://127.0.0.1:{port}/set")).await;
        if let Some(r) = expect(jar_client.get(&format!("http://127.0.0.1:{port}/show")).await, "show") {
            check(r.text() == "cookie=session=xyz", "the client returns cookies");
        }
        check(jar.all().await.map(|c| c.len()).unwrap_or(0) == 1 && jar_client.cookie_jar().is_some(), "the jar the client was given is the one it fills");
    }
    drop(ws_server);

    if let Some(limiter) = expect(RateLimiter::new(1.0, 2.0, 100), "limiter") {
        check(limiter.try_acquire("k", 1.0) == Ok(true) && limiter.try_acquire("k", 1.0) == Ok(true) && limiter.try_acquire("k", 1.0) == Ok(false), "the burst, then refusal");
        check(limiter.retry_after("k", 1.0).map(|d| d > Duration::ZERO).unwrap_or(false), "retry_after");
    }
    check(percent_decode("a%20b+c", true).as_deref() == Ok("a b c") && percent_decode("%zz", false).is_err(), "percent_decode");
    check(format_sse(&SseEvent { data: "x".into(), event: Some("a\nb".into()), ..SseEvent::default() }).as_deref() == Ok("event: ab\ndata: x\n\n"), "format_sse");
    let _ = std::fs::remove_dir_all(&scratch);
}

pub async fn concurrency() {
    // Cancellation.
    if let Some(source) = expect(CancelSource::new(), "cancel source") {
        let Some(token) = expect(source.token(), "token") else { return };
        let waiting = token.cancelled();
        let sleeper = sleep_op(Duration::from_secs(60));
        if let Some(sleeper) = expect(sleeper, "sleeper") {
            expect(sleeper.link_cancel(&token), "link");
            check(source.cancel() && !source.cancel(), "cancel reports the first call");
            check(waiting.await.is_ok(), "waiting on a token ends");
            check(sleeper.await.is_err(), "a linked op is cancelled");
        }
        let timed = sleep_op(Duration::from_secs(60)).and_then(|op| op.with_timeout(Duration::from_millis(40)));
        if let Some(timed) = expect(timed, "with_timeout") {
            check(matches!(timed.await, Err(e) if e.kind == ErrorKind::TimedOut), "with_timeout fails with TimedOut");
        }
    }

    // Channels.
    if let Some((tx, rx)) = expect(channel(2), "channel") {
        check(tx.try_send(b"a") == Ok(TrySend::Sent) && tx.try_send(b"b") == Ok(TrySend::Sent) && tx.try_send(b"c") == Ok(TrySend::Full), "buffer fills");
        check(rx.recv().await.unwrap_or(None) == Some(b"a".to_vec()), "recv");
        check(rx.try_recv() == Ok(TryRecv::Message(b"b".to_vec())) && rx.try_recv() == Ok(TryRecv::Empty), "try_recv");
        let producer = tx.clone();
        drop(tx);
        expect(producer.send(b"last").await, "send");
        drop(producer);
        check(rx.recv().await.unwrap_or(None) == Some(b"last".to_vec()), "buffered messages survive closing");
        check(rx.recv().await.unwrap_or(Some(vec![])).is_none(), "then the end");
    }
    if let Some((tx, rx)) = expect(oneshot(), "oneshot") {
        check(tx.send(b"once") == Ok(true) && tx.send(b"twice") == Ok(false), "only the first send counts");
        check(rx.recv().await.unwrap_or_default() == b"once", "oneshot value");
    }
    if let Some((tx, rx)) = expect(watch(b"v0"), "watch") {
        check(rx.borrow().unwrap_or_default() == b"v0" && !rx.has_changed(), "watch starts unchanged");
        expect(tx.send(b"v1"), "watch send");
        check(rx.changed().await.is_ok() && rx.borrow_and_update().unwrap_or_default() == b"v1", "watch changed");
    }
    if let Some((tx, rx)) = expect(broadcast(2), "broadcast") {
        let other = tx.subscribe();
        check(tx.send(b"hi") == Ok(2), "send reports the receivers");
        check(rx.recv().await == Ok(Broadcast::Message(b"hi".to_vec())), "broadcast delivers");
        drop(other);
    }

    // Semaphore, locks, notify, barrier.
    if let Some(semaphore) = expect(Semaphore::new(1), "semaphore") {
        let permit = semaphore.try_acquire(1).ok().flatten();
        check(permit.is_some() && semaphore.try_acquire(1).ok().flatten().is_none(), "permits run out");
        drop(permit);
        check(semaphore.available() == 1, "dropping a permit returns it");
        let permit = expect(semaphore.acquire(1).await, "acquire");
        drop(permit);
    }
    if let Some(mutex) = expect(AsyncMutex::new(b"start"), "mutex") {
        if let Some(guard) = expect(mutex.lock().await, "lock") {
            check(mutex.try_lock().ok().flatten().is_none(), "a held mutex refuses try_lock");
            expect(guard.set(b"changed"), "guard set");
        }
        check(mutex.lock().await.map(|g| g.get().unwrap_or_default()).unwrap_or_default() == b"changed", "the next holder sees the change");
    }
    if let Some(notify) = expect(Notify::new(), "notify") {
        notify.notify_one();
        check(notify.notified().await.is_ok(), "a stored notification is consumed");
    }
    if let Some(barrier) = expect(Barrier::new(2), "barrier") {
        let (a, b) = join2(barrier.wait(), barrier.wait()).await;
        check(matches!((a, b), (Ok(x), Ok(y)) if x != y), "exactly one party leads");
    }

    // Tasks.
    for lane in [Lane::Light, Lane::Heavy, Lane::Blocking] {
        if let Some(op) = expect(spawn(lane, |_| Ok(TaskOutput { number: 7, bytes: b"result".to_vec(), text: "done".into() })), "spawn") {
            if let Some(reply) = expect(op.await, "task") {
                check(reply.number() == 7 && reply.bytes() == b"result" && reply.text() == "done", "task output");
            }
        }
    }
    if let Some(op) = expect(spawn(Lane::Light, |_| Err("boom".to_string())), "failing spawn") {
        check(matches!(op.await, Err(e) if e.message == "boom"), "a task error fails its op");
    }
    if let Some(set) = expect(JoinSet::new(), "join set") {
        for i in 0..4i64 {
            expect(set.spawn(Lane::Light, move |_| Ok(TaskOutput { number: i, ..TaskOutput::default() })), "join spawn");
        }
        expect(set.spawn(Lane::Blocking, |_| Err("one bad".to_string())), "bad spawn");
        let (mut sum, mut failed) = (0, 0);
        for _ in 0..5 {
            match set.join_next().await {
                Ok(Joined::Done(o)) => sum += o.number,
                Ok(Joined::Failed(m)) if m == "one bad" => failed += 1,
                _ => {}
            }
        }
        check(sum == 6 && failed == 1, "a join set yields every task");
        check(matches!(set.join_next().await, Ok(Joined::Empty)), "and then reports empty");
    }
    if let Some(local) = expect(LocalSet::new(), "local set") {
        if let Some(op) = expect(local.spawn(|_| Ok(TaskOutput { number: 9, ..TaskOutput::default() })), "local spawn") {
            check(local.run_pending() == 1, "run_pending runs it");
            check(op.await.map(|r| r.number()).unwrap_or(0) == 9, "local task result");
        }
    }
    if let Some(thread) = expect(DedicatedThread::new("fixture"), "dedicated") {
        if let Some(op) = expect(thread.run(|_| Ok(TaskOutput { number: 5, ..TaskOutput::default() })), "dedicated run") {
            check(op.await.map(|r| r.number()).unwrap_or(0) == 5, "dedicated thread result");
        }
    }
    if let Some(local) = expect(TaskLocal::new(), "task local") {
        check(local.get().is_none(), "unbound outside a scope");
        let seen = std::cell::RefCell::new(None);
        expect(local.scope(b"abc", || *seen.borrow_mut() = local.get()), "scope");
        check(seen.borrow().as_deref() == Some(&b"abc"[..]), "bound inside");
    }
    let main_runs = Arc::new(AtomicUsize::new(0));
    let counter = main_runs.clone();
    expect(main_thread::run(move || { counter.fetch_add(1, Ordering::Relaxed); }), "main thread run");
    expect(main_thread::pump(), "pump");
    check(main_runs.load(Ordering::Relaxed) == 1, "main-thread work runs when pumped");

    // Runtime, timers.
    check(!expect(runtime::ranked_cores(false), "cores").unwrap_or_default().is_empty(), "topology");
    check(expect(runtime::io_lane_stats().await, "io stats").is_some(), "io lane stats");
    if let Some(interval) = expect(Interval::new(Duration::from_millis(15)), "interval") {
        check(interval.tick().await.is_ok() && interval.tick().await.is_ok(), "interval ticks");
    }
    expect(runtime::sleep_until_unix_ms(0).await, "a deadline in the past");
}

pub async fn streams() {
    let collect = |s: Result<Stream>| async move { s.ok()?.collect().await.ok().map(|items| items.iter().map(|i| text(i)).collect::<Vec<_>>()) };
    let numbers = || Stream::of_items(&[b"1", b"2", b"3", b"4", b"5"]);
    check(collect(numbers()).await == Some(vec!["1", "2", "3", "4", "5"].into_iter().map(String::from).collect()), "of_items + collect");
    check(collect(numbers().and_then(|s| s.map(|i| i.iter().map(|b| b + 1).collect()))).await.map(|v| v[0].clone()) == Some("2".into()), "map");
    check(collect(numbers().and_then(|s| s.filter(|i| (i[0] - b'0') % 2 == 1))).await.map(|v| v.len()) == Some(3), "filter");
    check(collect(numbers().and_then(|s| s.take(2))).await.map(|v| v.len()) == Some(2), "take");
    check(collect(numbers().and_then(|s| s.skip(3))).await.map(|v| v.len()) == Some(2), "skip");
    check(collect(numbers().and_then(|s| s.chunks(2))).await.map(|v| v.len()) == Some(3), "chunks");
    check(
        collect(Stream::of_items(&[b"a"]).and_then(|a| Stream::of_items(&[b"b"]).and_then(|b| a.chain_with(b)))).await == Some(vec!["a".to_string(), "b".to_string()]),
        "chain_with",
    );
    if let Some(stream) = numbers().ok() {
        check(stream.count().await.ok() == Some(5), "count");
    }
    if let Some(stream) = numbers().ok() {
        let total = stream.fold(b"0", |acc, item| (text(acc).parse::<u32>().unwrap_or(0) + text(item).parse::<u32>().unwrap_or(0)).to_string().into_bytes()).await;
        check(total.ok() == Some(b"15".to_vec()), "fold");
    }
    if let Some(stream) = numbers().ok() {
        check(stream.last().await.ok().flatten() == Some(b"5".to_vec()), "last");
    }
    if let Some(stream) = numbers().ok() {
        check(stream.next().await.ok().flatten() == Some(b"1".to_vec()), "next");
        check(stream.take(1).is_ok(), "next does not consume");
    }
    let mut produced = 0;
    let generated = Stream::generate(move || {
        produced += 1;
        (produced <= 3).then(|| produced.to_string().into_bytes())
    });
    check(collect(generated).await.map(|v| v.len()) == Some(3), "generate");

    // A channel feeds a stream that feeds a channel.
    if let (Some((tx, rx)), Some((out_tx, out_rx))) = (expect(channel(4), "c1"), expect(channel(4), "c2")) {
        if let Some(stream) = expect(rx.into_stream().and_then(|s| s.map(|i| [i, b"?"].concat())), "pipeline") {
            let forward = stream.forward(&out_tx);
            let feed = async {
                let _ = tx.send(b"q").await;
                let got = out_rx.recv().await;
                drop(tx);
                got
            };
            let (forwarded, got) = join2(forward, feed).await;
            check(forwarded.is_ok() && matches!(got, Ok(Some(ref m)) if m == b"q?"), "channel -> stream -> channel");
        }
    }
    check(unpack_frames(&pack_frames(&[b"", b"abc"]).unwrap_or_default()).ok() == Some(vec![vec![], b"abc".to_vec()]), "pack/unpack round-trip");
}

pub async fn files_and_streams() {
    let scratch = std::env::temp_dir().join("sturdy_net_fixture_io");
    let _ = std::fs::remove_dir_all(&scratch);
    let root = scratch.to_string_lossy().into_owned();
    expect(fs::create_directories(&format!("{root}/a/b")).await, "create_directories");
    let file = format!("{root}/a/b/one.txt");
    expect(fs::write(&file, b"hello ").await, "write");
    expect(fs::append(&file, b"world").await, "append");
    check(fs::read(&file, 0).await.unwrap_or_default() == b"hello world", "read");
    check(fs::read_range(&file, 6, 5).await.unwrap_or_default() == b"world", "read_range");
    check(fs::size(&file).await == Ok(11) && fs::exists(&file).await == Ok(true), "size and exists");
    check(fs::list(&format!("{root}/a/b"), 0, false).await.map(|l| l.len()).unwrap_or(0) == 1, "list");
    if let Ok(iter) = fs::DirIter::new(&format!("{root}/a/b"), false) {
        check(iter.next().await.ok().flatten().is_some() && iter.next().await.ok().flatten().is_none(), "dir iterator");
    }
    check(fs::remove_file(&file).await == Ok(true) && fs::remove_directory(&format!("{root}/a/b")).await == Ok(true), "remove");

    // Generic streams.
    if let (Some(reader), Some(writer)) = (expect(MemoryReader::new(b"copy me"), "reader"), expect(MemoryWriter::new(), "writer")) {
        check(copy(&reader, &writer, &CopyOptions::default()).await == Ok(7) && writer.bytes().unwrap_or_default() == b"copy me", "copy");
    }
    if let Some((a, b)) = expect(memory_pipe(4), "pipe") {
        expect(write_all(&a, b"one\r\ntwo\n").await, "write into the pipe");
        if let Some(reader) = expect(BufferedReader::new(&b, 0), "buffered reader") {
            check(reader.read_line(0).await.ok().flatten().as_deref() == Some("one"), "read_line");
            check(reader.read_exact(3).await.unwrap_or_default() == b"two", "read_exact");
        }
    }
    if let Some((a, b)) = expect(memory_pipe(8), "pipe 2") {
        if let (Some(sender), Some(receiver)) = (expect(Framed::new(&a, FrameCodec::LengthDelimited, 0), "framed a"), expect(Framed::new(&b, FrameCodec::LengthDelimited, 0), "framed b")) {
            expect(sender.send(b"frame").await, "framed send");
            check(receiver.next().await.ok().flatten() == Some(b"frame".to_vec()), "framed next");
        }
    }
    if let (Some(sink), Some(buffered)) = (expect(MemoryWriter::new(), "sink"), None::<()>.or(Some(()))) {
        let _ = buffered;
        if let Some(writer) = expect(BufferedWriter::new(&sink, 64), "buffered writer") {
            expect(writer.write(b"abc").await, "buffered write");
            check(sink.bytes().unwrap_or_default().is_empty(), "small writes are held back");
            expect(writer.flush().await, "flush");
            check(sink.bytes().unwrap_or_default() == b"abc", "flush sends them");
        }
    }

    // A child's pipes through the generic layer, and its status.
    let cat = Command::new("/bin/cat").spawn();
    if let Some(child) = expect(cat, "spawn cat") {
        let sink = MemoryWriter::new().ok();
        if let Some(sink) = &sink {
            let (stdout, options) = (child.stdout(), CopyOptions::default());
            let copying = copy(&stdout, sink, &options);
            let feeding = async {
                let _ = write_all(&child.stdin(), b"through cat").await;
                child.close_stdin();
            };
            let (copied, _) = join2(copying, feeding).await;
            check(copied == Ok(11) && sink.bytes().unwrap_or_default() == b"through cat", "io from a child's pipes");
        }
        check(child.id() > 0, "child id");
        check(child.wait_status().await.map(|s| s.success).unwrap_or(false), "exit status");
    }
    let _ = std::fs::remove_dir_all(&scratch);
}

pub fn parallel_extras() {
    use sturdy_net::par;
    check(par::num_threads() >= 1, "num_threads");
    let joined = AtomicUsize::new(0);
    expect(par::join(|| { joined.fetch_add(1, Ordering::Relaxed); }, || { joined.fetch_add(10, Ordering::Relaxed); }), "join");
    check(joined.load(Ordering::Relaxed) == 11, "join runs both");
    let spawned = Arc::new(AtomicUsize::new(0));
    expect(par::scope(|scope| {
        for _ in 0..20 {
            let spawned = spawned.clone();
            let _ = scope.spawn(move || { spawned.fetch_add(1, Ordering::Relaxed); });
        }
    }), "scope");
    check(spawned.load(Ordering::Relaxed) == 20, "scope waits for what it spawned");
    check(par::count(1000, |i| i % 3 == 0) == Ok(334), "count");
    check(par::any(1000, |i| i == 777) == Ok(true) && par::all(1000, |i| i < 1000) == Ok(true), "any and all");
    check(par::find_first(1000, |i| i % 7 == 3 && i > 100) == Ok(Some(101)) && par::find_first(10, |_| false) == Ok(None), "find_first");
    check(par::map_reduce_i64(1000, |i| i as i64, 0, |a, b| a + b) == Ok(499_500), "map_reduce_i64");
    check(par::map_reduce_f64(100, |i| i as f64, 0.0, f64::max) == Ok(99.0), "map_reduce_f64");
    let mut values: Vec<i32> = (0..2000).map(|i| (i * 7919) % 1000).collect();
    expect(par::sort_by(&mut values, true, |a, b| a < b), "sort_by");
    check(values.windows(2).all(|w| w[0] <= w[1]), "sorted with a Rust comparator");
    let big = vec![3_000_000_000u32; 1000];
    check(par::reduce(&big, par::Reduction::Sum).ok().flatten().map(|r| r.u64) == Some(3_000_000_000_000), "u32 sums do not overflow");
    check(par::reduce::<f64>(&[], par::Reduction::Min).ok().flatten().is_none(), "min of nothing");
    let windows = AtomicUsize::new(0);
    expect(par::for_windows(10, 4, |_| { windows.fetch_add(1, Ordering::Relaxed); }), "for_windows");
    check(windows.load(Ordering::Relaxed) == 7, "for_windows count");
}

pub async fn primitives() {
    // Blocking mutex.
    if let Some(mutex) = expect(SyncMutex::new(b"v0", "fixture"), "sync mutex") {
        if let Some(guard) = expect(mutex.lock(), "lock") {
            check(mutex.try_lock().ok().flatten().is_none(), "a held lock refuses try_lock");
            check(guard.get().unwrap_or_default() == b"v0", "guard get");
            expect(guard.set(b"v1"), "guard set");
        }
        check(mutex.try_lock().ok().flatten().and_then(|g| g.get().ok()) == Some(b"v1".to_vec()), "the next holder sees the change");
    }

    // SPSC ring across two threads.
    if let Some((producer, consumer)) = expect(spsc_ring(64, 16), "ring") {
        check(producer.try_push(&[0u8; 33]) == Ok(RingStatus::TooLarge), "too large for the slot");
        let thread = std::thread::spawn(move || {
            let mut sent = 0u32;
            while sent < 5000 {
                if producer.try_push(&sent.to_le_bytes()) == Ok(RingStatus::Ok) {
                    sent += 1;
                }
            }
        });
        let (mut next, mut in_order) = (0u32, true);
        while next < 5000 {
            if let Ok(Some(item)) = consumer.try_pop() {
                in_order &= item == next.to_le_bytes();
                next += 1;
            }
        }
        let _ = thread.join();
        check(in_order && consumer.is_empty(), "5000 messages cross the ring in order");
    }

    // Arc.
    if let Some(arc) = expect(ArcBytes::new(b"shared"), "arc") {
        let clone = arc.clone();
        check(arc.use_count() == 2 && arc.ptr_eq(&clone) && clone.bytes().unwrap_or_default() == b"shared", "clones share one value");
    }
    let dropped = Arc::new(AtomicUsize::new(0));
    struct Counted(Arc<AtomicUsize>);
    impl Drop for Counted {
        fn drop(&mut self) {
            self.0.fetch_add(1, Ordering::Relaxed);
        }
    }
    if let Some(shared) = expect(SharedArc::new(Counted(dropped.clone())), "shared arc") {
        let clone = shared.clone();
        check(shared.use_count() == 2 && shared.ptr_eq(&clone), "a Rust value behind the engine's Arc");
        drop(shared);
        check(dropped.load(Ordering::Relaxed) == 0, "kept alive by the clone");
        drop(clone);
        check(dropped.load(Ordering::Relaxed) == 1, "dropped with the last reference");
    }

    // Promise.
    if let Some((promise, op)) = expect(sturdy_net::promise(), "promise") {
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(30));
            let _ = promise.set_value(7, b"later", "text");
        });
        if let Some(reply) = expect(op.await, "the future") {
            check(reply.number() == 7 && reply.bytes() == b"later" && reply.text() == "text", "the future carries the value");
        }
    }
    if let Some((promise, op)) = expect(sturdy_net::promise(), "promise 2") {
        drop(promise);
        check(matches!(op.await, Err(e) if e.kind == ErrorKind::Closed), "a dropped promise fails its op with Closed");
    }
    check(expect(sturdy_net::ready(3, b"", "t"), "ready").is_some_and(|op| op.wait().map(|r| r.number()) == Ok(3)), "ready");

    // Execution senders.
    use sturdy_net::exec;
    if let Some(chain) = expect(exec::just(Lane::Light, |_| Ok(TaskOutput { number: 2, ..TaskOutput::default() })), "just") {
        let chain = chain.then(Lane::Heavy, |_, input| Ok(TaskOutput { number: input.number * 10, ..TaskOutput::default() }));
        let chain = chain.and_then(|c| c.then(Lane::Blocking, |_, input| Ok(TaskOutput { number: input.number + 1, text: "done".into(), ..TaskOutput::default() })));
        if let Some(chain) = expect(chain, "then") {
            if let Some(run) = expect(chain.submit(), "submit") {
                if let Some(reply) = expect(run.await, "run") {
                    check(reply.number() == 21 && reply.text() == "done", "stages feed each other across lanes");
                }
            }
            check(chain.sync_wait().map(|r| r.number()) == Ok(21), "sync_wait");
        }
    }
    if let Some(failing) = expect(exec::just(Lane::Light, |_| Err("stage one failed".to_string())), "failing") {
        let ran = Arc::new(AtomicUsize::new(0));
        let counter = ran.clone();
        if let Some(chain) = expect(failing.then(Lane::Light, move |_, _| { counter.fetch_add(1, Ordering::Relaxed); Ok(TaskOutput::default()) }), "then") {
            check(matches!(chain.sync_wait(), Err(e) if e.message == "stage one failed") && ran.load(Ordering::Relaxed) == 0, "a failure skips the later stages");
        }
    }
}
