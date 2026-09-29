require "./spec_helper"

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

describe "Stage 17 recovery" do
  it "keeps src/ as recovery: host-built A still runs hello.av with the nursery on" do
    root = File.expand_path("..", __DIR__)
    myc = File.expand_path("../../myc/myc-llvm", __DIR__)
    hello = File.expand_path("../examples/hello.av", __DIR__)
    File.exists?(File.join(root, "src/cli.cr")).should be_true

    bin_a = compile_av_bin(port_driver_files)
    begin
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc, "AVANT_NURSERY" => "1"}) do
        code, run_out = run_bin(bin_a, ["run", hello])
        code.should eq(0), run_out
        run_out.should eq "Hello Avant\n"
      end
    ensure
      File.delete(bin_a) if File.exists?(bin_a)
    end
  end

  it "reclaims short-lived trees under a heap cap with the copying nursery" do
    with_env({"AVANT_HEAP_MAX_MB" => "3", "AVANT_NURSERY" => "1"}) do
      run_src(RECLAIM).should eq "#{120 * 2096128}\n"
    end
  end

  it "reports young collections and copied bytes" do
    bin = compile_bin(RECLAIM)
    begin
      output = IO::Memory.new
      error = IO::Memory.new
      with_env({"AVANT_GC_STATS" => "1", "AVANT_NURSERY" => "1"}) do
        status = Process.run(bin, [] of String, output: output, error: error)
        status.exit_code.should eq(0), error.to_s + output.to_s
      end
      output.to_s.should eq "#{120 * 2096128}\n"
      stats = error.to_s
      stats.should contain("young=")
      stats.should_not contain("young=0 full")
      stats.should_not contain("copied=0 live")
    ensure
      File.delete(bin) if File.exists?(bin)
    end
  end

  it "reclaims short-lived trees when from-space is discarded after young copy" do
    with_env({"AVANT_HEAP_MAX_MB" => "3", "AVANT_NURSERY" => "1", "AVANT_NURSERY_DISCARD" => "1"}) do
      run_src(RECLAIM).should eq "#{120 * 2096128}\n"
    end
  end
end
