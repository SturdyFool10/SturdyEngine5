//! `sturdy::net` is the engine's Tokio-and-Rayon layer; this checks it links against the statically bundled engine and works without an
//! engine session (no window, no GPU). The wider self-test lives in `FfiNetRustTest` (Rust/examples/net_fixture).

use sturdy::net::{block_on, par, sleep, TcpListener, TcpStream};
use std::time::Duration;

#[test]
fn tcp_echo_over_the_bundled_engine() {
    block_on(async {
        let listener = TcpListener::listen("127.0.0.1", 0, None).await.expect("listen");
        let port = listener.port().expect("port");
        let accepting = listener.accept();
        let connecting = TcpStream::connect("127.0.0.1", port, None, None);
        let mut accepting = Box::pin(accepting);
        let mut connecting = Box::pin(connecting);
        // Drive both until they complete.
        let (mut server, mut client) = (None, None);
        std::future::poll_fn(|cx| {
            use std::future::Future;
            if server.is_none() {
                if let std::task::Poll::Ready(v) = accepting.as_mut().poll(cx) {
                    server = Some(v.expect("accept"));
                }
            }
            if client.is_none() {
                if let std::task::Poll::Ready(v) = connecting.as_mut().poll(cx) {
                    client = Some(v.expect("connect"));
                }
            }
            if server.is_some() && client.is_some() { std::task::Poll::Ready(()) } else { std::task::Poll::Pending }
        })
        .await;
        let (server, client) = (server.unwrap(), client.unwrap());
        client.send(b"hello").await.expect("send");
        assert_eq!(server.receive_exact(5).await.expect("receive"), b"hello");
        sleep(Duration::from_millis(5)).await.expect("sleep");
    });
}

#[test]
fn parallel_helpers_run_on_the_engine_pool() {
    let data: Vec<i64> = (1..=10_000).collect();
    assert_eq!(par::sum(&data).unwrap(), 10_000 * 10_001 / 2);
    assert_eq!(par::min_max(&data).unwrap(), (1, 10_000));
}
