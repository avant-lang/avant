require "./spec_helper"

describe Avant::Lexer do
  it "tokenizes an empty file as eof" do
    token_kinds("").should eq [Avant::Token::Kind::Eof]
  end

  it "tokenizes hello" do
    kinds = token_kinds(%(fn main {\n  puts("hi")\n}\n))
    kinds.should eq [
      Avant::Token::Kind::Fn,
      Avant::Token::Kind::Ident,
      Avant::Token::Kind::LBrace,
      Avant::Token::Kind::Newline,
      Avant::Token::Kind::Ident,
      Avant::Token::Kind::LParen,
      Avant::Token::Kind::String,
      Avant::Token::Kind::RParen,
      Avant::Token::Kind::Newline,
      Avant::Token::Kind::RBrace,
      Avant::Token::Kind::Newline,
      Avant::Token::Kind::Eof,
    ]
  end

  it "tokenizes scientific floats" do
    tok = tokenize("1.5e-2").find(&.kind.float?)
    tok.not_nil!.value.should eq "1.5e-2"
    tok = tokenize("4.8e+00").find(&.kind.float?)
    tok.not_nil!.value.should eq "4.8e+00"
  end

  it "keeps string contents without quotes" do
    tok = tokenize(%("hello")).find(&.kind.string?)
    tok.not_nil!.value.should eq "hello"
  end

  it "decodes string escapes" do
    tok = tokenize(%("a\\nb")).find(&.kind.string?)
    tok.not_nil!.value.should eq "a\nb"
  end

  it "tokenizes bitwise and logical operators" do
    kinds = token_kinds("&& || & | ^ << >> ~")
    kinds.should eq [
      Avant::Token::Kind::AmpAmp,
      Avant::Token::Kind::PipePipe,
      Avant::Token::Kind::Amp,
      Avant::Token::Kind::Pipe,
      Avant::Token::Kind::Caret,
      Avant::Token::Kind::LessLess,
      Avant::Token::Kind::GreaterGreater,
      Avant::Token::Kind::Tilde,
      Avant::Token::Kind::Eof,
    ]
  end

  it "treats // as a comment to end of line" do
    kinds = token_kinds("fn main { // comment\n}\n")
    kinds.should eq [
      Avant::Token::Kind::Fn,
      Avant::Token::Kind::Ident,
      Avant::Token::Kind::LBrace,
      Avant::Token::Kind::Newline,
      Avant::Token::Kind::RBrace,
      Avant::Token::Kind::Newline,
      Avant::Token::Kind::Eof,
    ]
  end

  it "tokenizes interpolation" do
    kinds = token_kinds(%("hello ${name}"))
    kinds.should contain(Avant::Token::Kind::InterpOpen)
    kinds.should contain(Avant::Token::Kind::InterpClose)
  end

  it "rejects unterminated strings" do
    expect_raises(Avant::CompileError, /unterminated/) do
      tokenize(%("hello))
    end
  end

  it "tokenizes hex and 64-bit suffixes" do
    tok = tokenize("0xFFu64").find(&.kind.integer?)
    tok.not_nil!.value.should eq "0xFFu64"
    tok = tokenize("1i64").find(&.kind.integer?)
    tok.not_nil!.value.should eq "1i64"
  end
end
