require "./spec_helper"

describe "Stage 2" do
  it "parses a struct and a struct literal" do
    program = parse(File.read("#{__DIR__}/../examples/vec2.av"))
    program.structs.size.should eq 1
    program.structs[0].name.should eq "Vec2"
    program.structs[0].fields.map(&.name).should eq ["x", "y"]
  end

  it "parses while, assignment, and indexing" do
    program = parse(File.read("#{__DIR__}/../examples/sieve.av"))
    body = program.functions[0].body
    body[0].should be_a(Avant::AST::AssignStmt)
    body[2].should be_a(Avant::AST::WhileStmt)
  end

  it "tokenizes floats and brackets" do
    kinds = token_kinds("xs[0] = 3.14")
    kinds.should contain(Avant::Token::Kind::LBracket)
    kinds.should contain(Avant::Token::Kind::Float)
    kinds.should contain(Avant::Token::Kind::Eq)
  end

  it "assigns outward instead of shadowing" do
    compile(<<-AV)
      fn main {
        x = 1
        if true {
          x = 2
        }
        puts(x)
      }
      AV
  end

  it "requires a type on an empty array" do
    ex = compile_error(<<-AV)
      fn main {
        xs = []
        puts(1)
      }
      AV
    ex.message.should match(/empty array needs a type/)
  end
end
