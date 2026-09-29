require "./spec_helper"

describe Avant::Checker do
  it "accepts hello" do
    compile(%(fn main {\n  puts("Hello Avant")\n}\n))
  end

  it "accepts fact" do
    compile(File.read("#{__DIR__}/../examples/fact.av"))
  end

  it "requires main or run" do
    ex = compile_error("fn fact(n: Int): Int {\n  n\n}\n")
    ex.message.should match(/main or fn run/)
  end

  it "rejects unknown names" do
    ex = compile_error("fn main {\n  puts(x)\n}\n")
    ex.message.should match(/unknown name x/)
  end

  it "rejects a non-Bool if condition" do
    ex = compile_error("fn main {\n  if 1 { puts(\"x\") }\n}\n")
    ex.message.should match(/if condition must be Bool/)
  end

  it "rejects a missing return value" do
    ex = compile_error("fn f: Int {\n}\nfn main {\n  puts(f())\n}\n")
    ex.message.should match(/missing return value/)
  end

  it "binds and assigns locals" do
    compile(<<-AV)
      fn main {
        x = 1
        x = 2
        puts(x)
      }
      AV
  end

  it "does not leak names out of a block" do
    ex = compile_error(<<-AV)
      fn main {
        if true {
          y = 3
        }
        puts(y)
      }
      AV
    ex.message.should match(/unknown name y/)
  end

  it "rejects unknown types" do
    ex = compile_error("fn main {\n  x: Float = 1\n  puts(1)\n}\n")
    ex.message.should match(/unknown type Float/)
  end

  it "accepts fn run as the entry" do
    compile(%(fn run {\n  puts("hi")\n}\n))
  end

  it "type-checks a two-argument call" do
    compile(<<-AV)
      fn add(a: Int, b: Int): Int {
        a + b
      }
      fn main {
        puts(add(2, 3))
      }
      AV
  end
end
