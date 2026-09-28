require "./spec_helper"

describe "Stage 4" do
  it "declares avant_alloc and does not emit MALLOC" do
    ir = compile(File.read("#{__DIR__}/../examples/methods.av"))
    ir.should contain("FUNC :avant_alloc")
    ir.should contain("CALL :avant_alloc")
    ir.should_not contain("MALLOC")
  end

  it "allocates array buffers through the runtime" do
    ir = compile(File.read("#{__DIR__}/../examples/sieve.av"))
    ir.should contain("CALL :avant_alloc")
    ir.should_not contain("MALLOC")
  end

  it "runs methods.av through the runtime" do
    run_av("#{__DIR__}/../examples/methods.av").should eq "7\n6\n11\n11\n"
  end

  it "runs trees.av" do
    run_av("#{__DIR__}/../examples/trees.av").should eq "120\n"
  end

  it "keeps class identity across many allocations" do
    ir = compile(<<-AV)
      class Box {
        value: Int
        fn initialize(value: Int) {
          self.value = value
        }
      }
      fn main {
        i = 0
        last = Box.new(0)
        while i < 1000 {
          last = Box.new(i)
          i = i + 1
        }
        puts(last.value)
      }
      AV
    run_ir(ir).should eq "999\n"
  end
end
