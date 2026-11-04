// Std-only WebSocket echo server for the harness (thread per
// connection, TCP_NODELAY). Text frames are echoed; ping gets pong.
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};

fn sha1(data: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let bitlen = (data.len() as u64) * 8;
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bitlen.to_be_bytes());
    for chunk in msg.chunks_exact(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([chunk[i * 4], chunk[i * 4 + 1], chunk[i * 4 + 2], chunk[i * 4 + 3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for i in 0..80 {
            let (f, k) = match i {
                0..=19 => ((b & c) | ((!b) & d), 0x5A827999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let tmp = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(w[i]);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = tmp;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }
    let mut out = [0u8; 20];
    for i in 0..5 {
        out[i * 4..i * 4 + 4].copy_from_slice(&h[i].to_be_bytes());
    }
    out
}

fn b64(data: &[u8]) -> String {
    const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for ch in data.chunks(3) {
        let b = [ch[0], *ch.get(1).unwrap_or(&0), *ch.get(2).unwrap_or(&0)];
        let v = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
        out.push(T[(v >> 18) as usize & 63] as char);
        out.push(T[(v >> 12) as usize & 63] as char);
        out.push(if ch.len() > 1 { T[(v >> 6) as usize & 63] as char } else { '=' });
        out.push(if ch.len() > 2 { T[v as usize & 63] as char } else { '=' });
    }
    out
}

fn read_exact_n(s: &mut TcpStream, n: usize) -> std::io::Result<Vec<u8>> {
    let mut buf = vec![0u8; n];
    s.read_exact(&mut buf)?;
    Ok(buf)
}

fn handle(mut s: TcpStream) {
    let _ = s.set_nodelay(true);
    // Handshake: read until blank line.
    let mut head: Vec<u8> = Vec::new();
    let mut byte = [0u8; 1];
    while !head.ends_with(b"\r\n\r\n") {
        match s.read(&mut byte) {
            Ok(0) => return,
            Ok(_) => head.push(byte[0]),
            Err(_) => return,
        }
        if head.len() > 16384 {
            return;
        }
    }
    let text = String::from_utf8_lossy(&head);
    let mut key = "";
    for line in text.lines() {
        if let Some(v) = line.strip_prefix("Sec-WebSocket-Key: ") {
            key = v.trim();
        }
    }
    let accept = b64(&sha1(format!("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key).as_bytes()));
    let resp = format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        accept
    );
    if s.write_all(resp.as_bytes()).is_err() {
        return;
    }
    loop {
        let h = match read_exact_n(&mut s, 2) {
            Ok(v) => v,
            Err(_) => return,
        };
        let op = h[0] & 0x0f;
        let masked = h[1] & 0x80 != 0;
        let mut plen = (h[1] & 0x7f) as usize;
        if plen == 126 {
            let e = match read_exact_n(&mut s, 2) {
                Ok(v) => v,
                Err(_) => return,
            };
            plen = ((e[0] as usize) << 8) | e[1] as usize;
        } else if plen == 127 {
            return;
        }
        let mask = if masked {
            match read_exact_n(&mut s, 4) {
                Ok(v) => v,
                Err(_) => return,
            }
        } else {
            vec![0; 4]
        };
        let mut payload = match read_exact_n(&mut s, plen) {
            Ok(v) => v,
            Err(_) => return,
        };
        if masked {
            for i in 0..plen {
                payload[i] ^= mask[i & 3];
            }
        }
        let first = match op {
            1 | 2 => 0x81u8,
            9 => 0x8a,
            8 => return,
            _ => continue,
        };
        let mut out = vec![first];
        if plen < 126 {
            out.push(plen as u8);
        } else {
            out.push(126);
            out.push((plen >> 8) as u8);
            out.push(plen as u8);
        }
        out.extend_from_slice(&payload);
        if s.write_all(&out).is_err() {
            return;
        }
    }
}

fn main() {
    let port: u16 = std::env::args().nth(1).and_then(|a| a.parse().ok()).unwrap_or(18311);
    let listener = TcpListener::bind(("127.0.0.1", port)).expect("bind");
    for stream in listener.incoming() {
        match stream {
            Ok(s) => {
                std::thread::spawn(move || handle(s));
            }
            Err(_) => break,
        }
    }
}
