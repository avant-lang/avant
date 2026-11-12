# GC microbenchmarks: a Crystal port of Valk's suite, SCENARIO-ISOLATED, with an
# eighth scenario added by Avant.
#
# modified from ../../main.cr: the program takes scenario numbers and runs
# those, in the order given (no argument: the seven scenarios in upstream's
# order, printing what ../../main.cr prints; `3 4` isolates scenario 4; `0`
# runs nothing); scenario 8, "reverse live links", is ADDED BY AVANT and is
# not part of Valk's suite (scenario 5 with the rotation, 2 stores a round,
# replaced by an in-place reversal, 200,000 stores a round). Nothing else
# is changed. The headline rows are measured with ../../main.cr.
#
# Origin as ../../main.cr: ported from Valk's examples/bench/gc/main.valk
# (https://github.com/valk-lang/valk, tag 0.8.5, commit 33224300; MIT,
# (c) 2024 Lorenz Vandevelde; the license is in ../../upstream/LICENSE).

class Node
  property next : Node?

  def len : Int64
    n : Node? = self
    c = 0_i64
    while n
      n = n.next
      c += 1
    end
    c
  end
end

class Tree
  property left : Tree?
  property right : Tree?

  def initialize(@left : Tree? = nil, @right : Tree? = nil)
  end

  def self.build(depth : Int32) : Tree
    return Tree.new if depth <= 0
    Tree.new(Tree.build(depth - 1), Tree.build(depth - 1))
  end

  def check : Int64
    res = 1_i64
    if lt = @left
      res += lt.check
    end
    if rt = @right
      res += rt.check
    end
    res
  end
end

# Valk's globals g_sink and g_keep.
module G
  class_property sink : Node?
  class_property keep : Node?
end

def chain(n : Int64) : Node
  first = Node.new
  prev = first
  i = 1_i64
  while i < n
    node = Node.new
    prev.next = node
    prev = node
    i += 1
  end
  first
end

def us_since(start : Time::Instant) : Int64
  (Time.instant - start).total_microseconds.to_i64
end

def print_time(us : Int64)
  puts "time_us:  #{us}"
  puts "time_ms:  #{us // 1000}"
end

def mem_kb : UInt64
  stats = GC.stats
  (stats.heap_size - stats.free_bytes) // 1024
end

def header(name : String)
  puts ""
  puts "=== #{name} ==="
end

def ok_fail(ok : Bool) : String
  ok ? "OK" : "FAIL"
end

def collect_n(n : Int32)
  i = 0
  while i < n
    GC.collect
    i += 1
  end
end

# 1) Objects that die at once: every allocation is real (stored into the
#    sink), only the last stays live.
def bench_short_lived
  header("short-lived alloc")
  amount = 10_000_000_i64
  root = Node.new

  start = Time.instant
  i = amount
  while i > 0
    i -= 1
    n = Node.new
    G.sink = n
    root.next = n if i == 0
  end
  us = us_since(start)
  collect_n(2)

  puts "objects:  #{amount}"
  print_time(us)
  puts "rate:     #{amount * 1000 // (us + 1)} allocs/ms"
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{root.len}"
end

# 2) Large stable live set; allocate one trigger object before each
#    forced collection.
def bench_stable_collect
  header("stable-heap forced collects")
  live = 500_000_i64
  collects = 500_i64
  list = chain(live)

  GC.collect

  start = Time.instant
  c = 0_i64
  while c < collects
    G.sink = Node.new
    GC.collect
    c += 1
  end
  us = us_since(start)

  puts "live:     #{live}"
  puts "collects: #{collects}"
  print_time(us)
  puts "rate:     #{collects * 1_000_000 // (us + 1)} collects/s"
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{ok_fail(list.len == live)}"
end

# 3) Build a long-lived chain (each link is a store into the node before).
def bench_build_chain
  header("build long-lived chain")
  amount = 5_000_000_i64

  start = Time.instant
  list = chain(amount)
  us = us_since(start)

  # Pin so the next scenario can drop it.
  G.keep = list

  puts "objects:  #{amount}"
  print_time(us)
  puts "rate:     #{amount * 1000 // (us + 1)} allocs/ms"
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{ok_fail(list.len == amount)}"
end

# 4) Drop a large graph and reclaim.
def bench_free_chain
  header("free long-lived chain")
  list : Node? = G.keep
  if list.nil?
    puts "skipped: no chain from build step"
    return
  end
  amount = list.len
  check = amount

  start = Time.instant
  # Drop every root, this local included, before reclaiming.
  G.keep = nil
  list = nil
  collect_n(4)
  us = us_since(start)

  puts "objects:  #{amount}"
  print_time(us)
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{check}"
end

# 5) Rotate a live list: the head goes to the tail, 50 times.
def bench_mutate_links
  header("mutate live links")
  live = 200_000_i64
  rounds = 50
  list = chain(live)
  GC.collect

  start = Time.instant
  r = 0
  while r < rounds
    # Rotate: first -> end, second becomes the new head.
    head = list
    second = head.next
    break if second.nil?
    head.next = nil
    tail = second
    while n = tail.next
      tail = n
    end
    tail.next = head
    list = second
    r += 1
  end
  us = us_since(start)
  GC.collect

  puts "live:     #{live}"
  puts "rounds:   #{rounds}"
  print_time(us)
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{ok_fail(list.len == live)}"
end

# 6) Allocate short-lived garbage while a large live set stays rooted.
def bench_churn_with_live
  header("short-lived churn with large live set")
  live = 500_000_i64
  churn = 5_000_000_i64
  list = chain(live)
  GC.collect

  start = Time.instant
  i = churn
  while i > 0
    i -= 1
    n = Node.new
    G.sink = n
  end
  GC.collect
  us = us_since(start)

  puts "live:     #{live}"
  puts "churn:    #{churn}"
  print_time(us)
  puts "rate:     #{churn * 1000 // (us + 1)} allocs/ms"
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{ok_fail(list.len == live)}"
end

# 7) Build and discard trees.
def bench_tree_churn
  header("tree churn")
  depth = 14
  trees = 400_i64
  # nodes per full binary tree: 2^(depth+1) - 1
  nodes_per = (1_i64 << (depth + 1)) - 1

  start = Time.instant
  check = 0_i64
  i = 0_i64
  while i < trees
    check += Tree.build(depth).check
    i += 1
  end
  GC.collect
  us = us_since(start)

  puts "depth:    #{depth}"
  puts "trees:    #{trees}"
  puts "nodes:    ~#{trees * nodes_per}"
  print_time(us)
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{check}"
end

# 8) ADDED BY AVANT, not part of Valk's suite: reverse a live list in place.
def bench_reverse_links
  header("reverse live links")
  live = 200_000_i64
  rounds = 50
  list = chain(live)
  GC.collect

  start = Time.instant
  r = 0
  while r < rounds
    prev : Node? = nil
    cur : Node? = list
    while cur
      nxt = cur.next
      cur.next = prev
      prev = cur
      cur = nxt
    end
    break if prev.nil?
    list = prev
    r += 1
  end
  us = us_since(start)
  GC.collect

  puts "live:     #{live}"
  puts "rounds:   #{rounds}"
  puts "stores:   #{live * rounds}"
  print_time(us)
  puts "mem_kb:   #{mem_kb}"
  puts "verify:   #{ok_fail(list.len == live)}"
end

def run_scenario(which : Int32)
  case which
  when 1 then bench_short_lived
  when 2 then bench_stable_collect
  when 3 then bench_build_chain
  when 4 then bench_free_chain
  when 5 then bench_mutate_links
  when 6 then bench_churn_with_live
  when 7 then bench_tree_churn
  when 8 then bench_reverse_links
  end
end

puts "Crystal GC bench suite"
puts "mem_kb before: #{mem_kb}"

if ARGV.empty?
  (1..7).each { |which| run_scenario(which) }
end
ARGV.each { |arg| run_scenario(arg.to_i) }

puts ""
puts "=== done ==="
puts "mem_kb after: #{mem_kb}"
