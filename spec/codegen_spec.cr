require "./spec_helper"

describe Avant::Codegen::Myc do
  it "emits PRINTF for puts of a string" do
    ir = compile(%(fn main {\n  puts("Hello Avant")\n}\n))
    ir.should contain(%(FUNC :main))
    ir.should contain(%(PUSH "Hello Avant"))
    ir.should contain(%(PUSH "%s\\n"))
    ir.should contain("PRINTF 1")
    ir.should contain("TYPE :i32")
    ir.should contain("PUSH 0")
  end

  it "emits a recursive fact in myc IR" do
    ir = compile(File.read("#{__DIR__}/../examples/fact.av"))
    ir.should contain("FUNC :fact")
    ir.should contain("PARAM 0")
    ir.should contain("LOCAL :n :i32")
    ir.should contain("BINARY :less_eq")
    ir.should contain("CALL :fact")
    ir.should contain("BINARY :mul")
    ir.should contain("FUNC :main")
    ir.should contain(%(PUSH "%d\\n"))
  end

  it "synthesizes myc main from fn run" do
    ir = compile(%(fn run {\n  puts("hi")\n}\n))
    ir.should contain("FUNC :run")
    ir.should contain("FUNC :main")
    ir.should contain("CALL :run")
    ir.should contain("PUSH 0")
  end
end
