require "./spec_helper"

describe "Stage 11" do
  it "parses trailing default arguments" do
    program = parse(<<-AV)
      fn greet(name: String, times: Int = 1): Int {
        times
      }
      AV
    fn = program.functions[0]
    fn.params.size.should eq 2
    fn.params[0].default.should be_nil
    fn.params[1].default.should be_a(Avant::AST::IntegerLiteral)
  end

  it "parses operator method names" do
    program = parse(<<-AV)
      struct Vec2 {
        x: Int
        fn +(other: Vec2): Vec2 {
          other
        }
      }
      AV
    program.structs[0].methods[0].name.should eq "+"
  end

  it "fills default arguments at the call site" do
    run_src(<<-AV).should eq "1\n4\n"
      fn greet(name: String, times: Int = 1): Int {
        times
      }
      fn main {
        puts(greet("a"))
        puts(greet("a", 4))
      }
      AV
  end

  it "rejects a non-trailing default" do
    ex = compile_error(<<-AV)
      fn f(a: Int = 1, b: Int): Int {
        a + b
      }
      fn main {
        puts(1)
      }
      AV
    ex.message.should match(/trailing/)
  end

  it "overloads on argument types" do
    run_src(<<-AV).should eq "7\n2\n"
      fn show(n: Int): Int {
        n
      }
      fn show(s: String): Int {
        s.size
      }
      fn main {
        puts(show(7))
        puts(show("ab"))
      }
      AV
  end

  it "rejects a duplicate param list" do
    ex = compile_error(<<-AV)
      fn show(n: Int): Int { n }
      fn show(n: Int): String { "x" }
      fn main {
        puts(1)
      }
      AV
    ex.message.should match(/already defined/)
  end

  it "rejects an ambiguous call" do
    ex = compile_error(<<-AV)
      fn f(x: Int): Int { x }
      fn f(x: Int, y: Int = 0): Int { x + y }
      fn main {
        puts(f(1))
      }
      AV
    ex.message.should match(/ambiguous/)
  end

  it "looks up operator methods on the left type" do
    run_src(<<-AV).should eq "4\n6\n"
      struct Vec2 {
        x: Int
        y: Int
        fn +(other: Vec2): Vec2 {
          Vec2 { x: x + other.x, y: y + other.y }
        }
      }
      fn main {
        a = Vec2 { x: 1, y: 2 }
        b = Vec2 { x: 3, y: 4 }
        s = a + b
        puts(s.x)
        puts(s.y)
      }
      AV
  end

  it "infers user generic functions" do
    run_src(<<-AV).should eq "9\n2\n8\n"
      fn id(x: T): T {
        x
      }
      fn first(xs: Array(T)): T {
        xs[0]
      }
      fn main {
        puts(id(9))
        puts(id("xy").size)
        xs: Array(Int) = [8, 1]
        puts(first(xs))
      }
      AV
  end

  it "infers a generic method" do
    run_src(<<-AV).should eq "11\n"
      struct Box {
        n: Int
        fn wrap(v: T): T {
          v
        }
      }
      fn main {
        b = Box { n: 0 }
        puts(b.wrap(11))
      }
      AV
  end

  it "rejects a generic call it cannot infer" do
    ex = compile_error(<<-AV)
      fn empty: Array(T) {
        xs: Array(T) = []
        xs
      }
      fn main {
        puts(empty().size)
      }
      AV
    ex.message.should match(/cannot infer|unknown function/)
  end
end
