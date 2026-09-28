require "./spec_helper"

describe Avant::Parser do
  it "parses fn main without parameter parens" do
    program = parse("fn main {\n  puts(\"hi\")\n}\n")
    program.functions.size.should eq 1
    fn = program.functions[0]
    fn.name.should eq "main"
    fn.params.should be_empty
    fn.return_type.should be_nil
    fn.body.size.should eq 1
    stmt = fn.body[0].as(Avant::AST::ExprStmt)
    call = stmt.expr.as(Avant::AST::Call)
    call.callee.should eq "puts"
    call.args[0].as(Avant::AST::StringLiteral).value.should eq "hi"
  end

  it "parses typed parameters and a return type" do
    program = parse("fn fact(n: Int): Int {\n  return n\n}\n")
    fn = program.functions[0]
    fn.params.size.should eq 1
    fn.params[0].name.should eq "n"
    fn.params[0].type.name.should eq "Int"
    fn.return_type.not_nil!.name.should eq "Int"
    fn.body[0].should be_a(Avant::AST::ReturnStmt)
  end

  it "parses if without parentheses" do
    program = parse(<<-AV)
      fn fact(n: Int): Int {
        if n <= 1 { return 1 }
        n
      }
      AV
    stmt = program.functions[0].body[0].as(Avant::AST::IfStmt)
    stmt.cond.should be_a(Avant::AST::Binary)
    stmt.then_body[0].should be_a(Avant::AST::ReturnStmt)
    stmt.else_body.should be_empty
  end

  it "parses else if" do
    program = parse(<<-AV)
      fn f(n: Int): Int {
        if n < 0 { return 0 } else if n == 0 { return 1 } else { return 2 }
      }
      AV
    outer = program.functions[0].body[0].as(Avant::AST::IfStmt)
    outer.else_body.size.should eq 1
    outer.else_body[0].should be_a(Avant::AST::IfStmt)
  end

  it "allows a space before a type colon" do
    program = parse("fn f(n : Int) : Int {\n  n\n}\n")
    fn = program.functions[0]
    fn.params[0].type.name.should eq "Int"
    fn.return_type.not_nil!.name.should eq "Int"
  end

  it "continues an expression after a newline following an operator" do
    program = parse(<<-AV)
      fn f(n: Int): Int {
        n *
        n
      }
      AV
    stmt = program.functions[0].body[0].as(Avant::AST::ExprStmt)
    stmt.expr.should be_a(Avant::AST::Binary)
  end
end
