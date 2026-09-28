require "./spec_helper"

describe "Stage 6" do
  it "tokenizes interpolation, nil, and unions" do
    kinds = token_kinds(%(x: Int? = nil\n"hi ${n}"))
    kinds.should contain(Avant::Token::Kind::Question)
    kinds.should contain(Avant::Token::Kind::NilKw)
    kinds.should contain(Avant::Token::Kind::InterpOpen)
    kinds.should contain(Avant::Token::Kind::InterpClose)
  end

  it "parses T? and postfix ?" do
    program = parse(<<-AV)
      fn parse_id(raw: Int): Int? {
        raw
      }
      fn main {
        n = parse_id(1)?
        puts(n)
      }
      AV
    program.functions[0].return_type.not_nil!.nilable.should be_true
    try = program.functions[1].body[0].as(Avant::AST::AssignStmt).value
    try.should be_a(Avant::AST::Try)
  end

  it "parses a trailing block" do
    program = parse(<<-AV)
      fn main {
        xs: Array(Int) = [1]
        xs.each { |n| puts(n) }
      }
      AV
    call = program.functions[0].body[1].as(Avant::AST::ExprStmt).expr.as(Avant::AST::Call)
    call.callee.should eq "each"
    call.block.not_nil!.params.should eq ["n"]
  end

  it "runs errors.av" do
    run_av("#{__DIR__}/../examples/errors.av").should eq "41\n0\nn=41\n"
  end

  it "runs collect.av" do
    run_av("#{__DIR__}/../examples/collect.av").should eq "6\n3\n10\n2\nhi avant\n"
  end

  it "runs json.av" do
    run_av("#{__DIR__}/../examples/json.av").should eq "7\nada\n"
  end

  it "runs http.av via uSockets" do
    run_av("#{__DIR__}/../examples/http.av").should eq "Hello Avant\n"
  end

  it "runs spawn.av" do
    run_av("#{__DIR__}/../examples/spawn.av").should eq "42\n"
  end

  it "runs matmul.av with matching sequential and parallel checksums" do
    lines = run_av("#{__DIR__}/../examples/matmul.av").strip.split('\n')
    lines.size.should eq 4
    lines[0].should eq lines[1]
    lines[0].to_i.should be > 0
  end
end
