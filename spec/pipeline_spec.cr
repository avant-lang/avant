require "./spec_helper"

describe "Stage 1 programs" do
  it "runs hello.av" do
    output = run_av("#{__DIR__}/../examples/hello.av")
    output.should eq "Hello Avant\n"
  end

  it "runs fact.av" do
    output = run_av("#{__DIR__}/../examples/fact.av")
    output.should eq "120\n"
  end

  it "evaluates arguments left to right" do
    ir = compile(<<-AV)
      fn add(a: Int, b: Int): Int {
        a + b
      }
      fn main {
        puts(add(2, 3))
      }
      AV
    run_ir(ir).should eq "5\n"
  end

  it "runs vec2.av" do
    output = run_av("#{__DIR__}/../examples/vec2.av")
    output.should match(/^25(\.0+)?\n$/)
  end

  it "runs sieve.av" do
    run_av("#{__DIR__}/../examples/sieve.av").should eq "25\n"
  end
end
