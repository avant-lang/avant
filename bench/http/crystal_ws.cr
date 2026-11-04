# WebSocket echo server for the harness. Text messages are echoed.
require "http/server"
require "http/web_socket"

port = (ARGV[0]? || "18311").to_i
ws = HTTP::WebSocketHandler.new do |socket, _ctx|
  socket.on_message do |message|
    socket.send(message)
  end
end
server = HTTP::Server.new([ws])
server.bind_tcp "127.0.0.1", port
server.listen
