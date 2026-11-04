// net/http hello-world server for the five-language harness
// (bench/http/run.sh). Body "hello". GET /quit exits after answering.
package main

import (
	"fmt"
	"net/http"
	"os"
)

func main() {
	port := "18301"
	if len(os.Args) > 1 {
		port = os.Args[1]
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/quit", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		fmt.Fprint(w, "bye")
		go os.Exit(0)
	})
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		fmt.Fprint(w, "hello")
	})
	if err := http.ListenAndServe("127.0.0.1:"+port, mux); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
