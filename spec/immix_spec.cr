require "./spec_helper"

TREES = File.read("#{__DIR__}/../examples/trees.av")

RECLAIM = <<-AV
class Node {
  item: Int
  left: Node
  right: Node

  fn initialize(item: Int) {
    self.item = item
  }
}

fn make(item: Int, depth: Int): Node {
  n = Node.new(item)
  if depth > 0 {
    n.left = make(item * 2, depth - 1)
    n.right = make(item * 2 + 1, depth - 1)
  }
  n
}

fn check(n: Node, depth: Int): Int {
  if depth == 0 {
    return n.item
  }
  check(n.left, depth - 1) + check(n.right, depth - 1) + n.item
}

fn main {
  i = 0
  sum = 0
  while i < 120 {
    t = make(1, 10)
    sum = sum + check(t, 10)
    i = i + 1
  }
  puts(sum)
}
AV

BARRIER = <<-AV
class Node {
  item: Int
  left: Node
  right: Node

  fn initialize(item: Int) {
    self.item = item
  }
}

fn make(item: Int, depth: Int): Node {
  n = Node.new(item)
  if depth > 0 {
    n.left = make(item * 2, depth - 1)
    n.right = make(item * 2 + 1, depth - 1)
  }
  n
}

fn main {
  a = Node.new(1)
  i = 0
  while i < 40 {
    t = make(1, 10)
    i = i + 1
  }
  a.left = Node.new(42)
  i = 0
  while i < 40 {
    t = make(1, 10)
    i = i + 1
  }
  puts(a.left.item)
}
AV

describe "Immix" do
  it "emits type maps, a shadow stack, and a write barrier" do
    ir = compile(TREES)
    ir.should contain("FUNC :avant_gc_enter")
    ir.should contain("FUNC :avant_gc_root")
    ir.should contain("FUNC :avant_type_map")
    ir.should contain("FUNC :avant_barrier")
    ir.should contain("PUSH 6 :u64")
    ir.should contain("CALL :avant_type_map")
    ir.should contain("CALL :avant_gc_root")
    ir.should contain("CALL :avant_barrier")
    ir.should_not contain("MALLOC")
  end

  it "still runs trees.av" do
    run_ir(compile(TREES)).should eq "120\n"
  end

  it "reclaims short-lived trees under a heap cap" do
    with_env({"AVANT_HEAP_MAX_MB" => "3"}) do
      run_ir(compile(RECLAIM)).should eq "#{120 * 2096128}\n"
    end
  end

  it "keeps an old object store through later collections" do
    with_env({"AVANT_HEAP_MAX_MB" => "3"}) do
      run_ir(compile(BARRIER)).should eq "42\n"
    end
  end
end
