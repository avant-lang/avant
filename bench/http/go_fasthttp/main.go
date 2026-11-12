// Go fasthttp hello-world server for the HTTP harness (bench/http/run.sh).
//
// Adapted from valk-lang/valk, examples/bench/http/main.go (commit 3322430,
// tag 0.8.5). MIT, (c) 2024 Lorenz Vandevelde. Kept from the original:
// `fasthttp.Server{Handler: handler}` with every setting at its default.
// Changed for this harness: the port comes from os.Args[1] (default 18301)
// instead of the fixed 9002; the body is `hello` with
// `Content-Type: text/plain; charset=utf-8`, like the other rows (the
// original answers `Hello, World!` as `text/plain`); `GET /quit` answers
// `bye` and ends the process; a second argument `nodate` sets
// `NoDefaultDate` and `NoDefaultServerHeader` (a labelled variant of the
// harness, never the headline row: the answer is then 84 bytes, like the
// servers that send neither header). The thread count is GOMAXPROCS, as in
// the original. go.mod and go.sum fix the versions.
// Usage: [GOMAXPROCS=n] go_fasthttp [port] [nodate]
package main

import (
	"os"
	"time"

	"github.com/valyala/fasthttp"
)

func handler(ctx *fasthttp.RequestCtx) {
	ctx.SetContentType("text/plain; charset=utf-8")
	if string(ctx.Path()) == "/quit" {
		ctx.SetBodyString("bye")
		// Answer first, then leave: the harness waits for the port to be free.
		go func() {
			time.Sleep(50 * time.Millisecond)
			os.Exit(0)
		}()
		return
	}
	ctx.SetBodyString("hello")
}

func main() {
	port := "18301"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	s := &fasthttp.Server{Handler: handler}
	if len(os.Args) > 2 && os.Args[2] == "nodate" {
		s.NoDefaultDate = true
		s.NoDefaultServerHeader = true
	}
	if err := s.ListenAndServe("127.0.0.1:" + port); err != nil {
		panic(err)
	}
}
