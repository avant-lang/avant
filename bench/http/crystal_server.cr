# HTTP::Server hello-world server for the five-language harness
# (bench/http/run.sh). Body "hello". GET /quit exits after answering.
require "http/server"

port = (ARGV[0]? || "18301").to_i
server = HTTP::Server.new do |ctx|
  if ctx.request.path == "/quit"
    ctx.response.content_type = "text/plain; charset=utf-8"
    ctx.response.print "bye"
    spawn do
      sleep 50.milliseconds
      exit 0
    end
  else
    ctx.response.content_type = "text/plain; charset=utf-8"
    ctx.response.print "hello"
  end
end
server.bind_tcp "127.0.0.1", port
server.listen
