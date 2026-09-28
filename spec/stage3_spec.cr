require "./spec_helper"

describe "Stage 3" do
  it "tokenizes class, self, and compound assignment" do
    kinds = token_kinds("class Counter { self.value += 1 }")
    kinds.should contain(Avant::Token::Kind::Class)
    kinds.should contain(Avant::Token::Kind::SelfKw)
    kinds.should contain(Avant::Token::Kind::PlusEq)
  end

  it "parses a class, an inherent method, and an external method" do
    program = parse(File.read("#{__DIR__}/../examples/methods.av"))
    program.classes.size.should eq 1
    program.classes[0].name.should eq "Counter"
    program.classes[0].methods.map(&.name).should eq ["initialize", "inc"]
    program.structs[0].methods.map(&.name).should eq ["sum"]
    ext = program.functions.find { |fn| fn.receiver }
    ext.not_nil!.name.should eq "scaled"
    ext.not_nil!.receiver.not_nil!.name.should eq "p"
  end

  it "parses Type.new and a parenthesized method call" do
    program = parse(<<-AV)
      class Counter {
        value: Int
        fn initialize(value: Int) { self.value = value }
        fn add(n: Int): Int { value + n }
      }
      fn main {
        c = Counter.new(10)
        puts(c.add(2))
      }
      AV
    body = program.functions[0].body
    bind = body[0].as(Avant::AST::AssignStmt)
    ctor = bind.value.as(Avant::AST::Call)
    ctor.callee.should eq "new"
    ctor.receiver.as(Avant::AST::Name).ident.should eq "Counter"
    call = body[1].as(Avant::AST::ExprStmt).expr.as(Avant::AST::Call)
    inner = call.args[0].as(Avant::AST::Call)
    inner.callee.should eq "add"
    inner.receiver.should be_a(Avant::AST::Name)
  end

  it "rejects a class constructed like a struct" do
    ex = compile_error(<<-AV)
      class Counter {
        value: Int
        fn initialize(value: Int) { self.value = value }
      }
      fn main {
        c = Counter { value: 1 }
        puts(1)
      }
      AV
    ex.message.should match(/constructed with Counter\.new/)
  end

  it "rejects .new on a struct" do
    ex = compile_error(<<-AV)
      struct Point {
        x: Int
      }
      fn main {
        p = Point.new()
        puts(1)
      }
      AV
    ex.message.should match(/structs are constructed/)
  end

  it "runs methods.av" do
    run_av("#{__DIR__}/../examples/methods.av").should eq "7\n6\n11\n11\n"
  end

  it "shares class identity and copies structs" do
    ir = compile(<<-AV)
      struct Point {
        x: Int
      }
      class Box {
        value: Int
        fn initialize(value: Int) {
          self.value = value
        }
      }
      fn main {
        p = Point { x: 1 }
        q = p
        p.x = 9
        puts(q.x)
        a = Box.new(1)
        b = a
        a.value = 9
        puts(b.value)
      }
      AV
    run_ir(ir).should eq "1\n9\n"
  end
end
