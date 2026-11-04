// Std-only thread-per-connection hello-world server for the
// five-language harness (bench/http/run.sh). Keep-alive, body "hello".
// GET /quit shuts the process down after answering.
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};

static QUIT: AtomicBool = AtomicBool::new(false);

const RESP: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 5\r\n\r\nhello";
const RESP_BYE: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 3\r\nConnection: close\r\n\r\nbye";

fn find_head(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n").map(|p| p + 4)
}

fn handle(mut s: TcpStream) {
    let _ = s.set_nodelay(true);
    let mut buf: Vec<u8> = Vec::with_capacity(16384);
    let mut chunk = [0u8; 8192];
    loop {
        while let Some(head) = find_head(&buf) {
            let is_quit = buf.starts_with(b"GET /quit");
            let resp = if is_quit { RESP_BYE } else { RESP };
            if s.write_all(resp).is_err() {
                return;
            }
            buf.drain(..head);
            if is_quit {
                QUIT.store(true, Ordering::SeqCst);
                std::process::exit(0);
            }
        }
        match s.read(&mut chunk) {
            Ok(0) => return,
            Ok(n) => buf.extend_from_slice(&chunk[..n]),
            Err(_) => return,
        }
    }
}

fn main() {
    let port: u16 = std::env::args()
        .nth(1)
        .and_then(|a| a.parse().ok())
        .unwrap_or(18301);
    let listener = TcpListener::bind(("127.0.0.1", port)).expect("bind");
    for stream in listener.incoming() {
        if QUIT.load(Ordering::SeqCst) {
            break;
        }
        match stream {
            Ok(s) => {
                std::thread::spawn(move || handle(s));
            }
            Err(_) => break,
        }
    }
}
