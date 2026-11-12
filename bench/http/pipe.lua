-- wrk script for HTTP pipelining: every write carries `depth` copies of the
-- request (`wrk ... -s pipe.lua -- 16`), so the server sees `depth` requests
-- per round trip on each connection.
--
-- From valk-lang/valk, examples/bench/http/pipe.lua (commit 3322430, tag
-- 0.8.5). MIT, (c) 2024 Lorenz Vandevelde. The code below is unchanged; only
-- this header comment was added.
init = function(args)
  local r = {}
  local depth = tonumber(args[1]) or 1
  for i = 1, depth do
    r[i] = wrk.format()
  end
  req = table.concat(r)
end

request = function()
  return req
end
