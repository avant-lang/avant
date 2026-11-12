// Rust hyper 1.x hello-world server on tokio's multi-thread runtime, for the
// HTTP harness (bench/http/run.sh).
//
// Adapted from valk-lang/valk, examples/bench/http/main.rs (commit 3322430,
// tag 0.8.5). MIT, (c) 2024 Lorenz Vandevelde. Kept from the original: the
// dependencies and release profile (see Cargo.toml), `set_nodelay(true)`, and
// the connection settings `keep_alive(true).pipeline_flush(true).writev(true)`.
// Changed for this harness: the port comes from argv[1] (default 18301)
// instead of the fixed 9001; the body is `hello` with
// `Content-Type: text/plain; charset=utf-8`, like the other rows (the
// original answers `Hello, World!` with no content type); `GET /quit` answers
// `bye` and ends the process; the runtime is built with
// `Builder::new_multi_thread()` so that argv[2] can fix the worker thread
// count (absent or 0: tokio's default, one per hardware thread), where the
// original uses `#[tokio::main(flavor = "multi_thread")]`; a third argument
// `nodate` switches hyper's automatic `date` header off (a labelled variant
// of the harness, never the headline row: the answer is then 84 bytes, like
// the servers that send no `Date`).
// Usage: rust_hyper [port] [threads] [nodate]

use std::convert::Infallible;
use std::sync::atomic::{AtomicBool, Ordering};

use http_body_util::Full;
use hyper::{
    body::{Bytes, Incoming},
    header::{HeaderValue, CONTENT_TYPE},
    server::conn::http1,
    service::service_fn,
    Request, Response,
};
use hyper_util::rt::TokioIo;
use tokio::net::TcpListener;

static BODY: &[u8] = b"hello";
static BYE: &[u8] = b"bye";
static NO_DATE: AtomicBool = AtomicBool::new(false);

async fn handle(req: Request<Incoming>) -> Result<Response<Full<Bytes>>, Infallible> {
    let quit = req.uri().path() == "/quit";
    let mut res = Response::new(Full::new(Bytes::from_static(if quit { BYE } else { BODY })));
    res.headers_mut().insert(
        CONTENT_TYPE,
        HeaderValue::from_static("text/plain; charset=utf-8"),
    );
    if quit {
        // Answer first, then leave: the harness waits for the port to be free.
        std::thread::spawn(|| {
            std::thread::sleep(std::time::Duration::from_millis(50));
            std::process::exit(0);
        });
    }
    Ok(res)
}

async fn serve(port: u16) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let listener = TcpListener::bind(("127.0.0.1", port)).await?;

    loop {
        let (stream, _) = listener.accept().await?;
        stream.set_nodelay(true)?;

        tokio::spawn(async move {
            let io = TokioIo::new(stream);
            let mut http = http1::Builder::new();
            http.keep_alive(true).pipeline_flush(true).writev(true);
            if NO_DATE.load(Ordering::Relaxed) {
                http.auto_date_header(false);
            }
            let _ = http.serve_connection(io, service_fn(handle)).await;
        });
    }
}

fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let mut args = std::env::args().skip(1);
    let port: u16 = args.next().and_then(|a| a.parse().ok()).unwrap_or(18301);
    let threads: usize = args.next().and_then(|a| a.parse().ok()).unwrap_or(0);
    NO_DATE.store(args.next().as_deref() == Some("nodate"), Ordering::Relaxed);

    let mut builder = tokio::runtime::Builder::new_multi_thread();
    builder.enable_io();
    if threads > 0 {
        builder.worker_threads(threads);
    }
    builder.build()?.block_on(serve(port))
}
