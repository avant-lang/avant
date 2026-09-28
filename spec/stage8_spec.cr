require "./spec_helper"

describe "Stage 8 Part 0" do
  it "tokenizes switch and case" do
    kinds = token_kinds("switch kind { case 0 { 1 } else { 2 } }")
    kinds.should contain(Avant::Token::Kind::Switch)
    kinds.should contain(Avant::Token::Kind::Case)
    kinds.should contain(Avant::Token::Kind::Else)
  end

  it "parses switch as a statement and as an expression" do
    program = parse(<<-AV)
      fn name_of(kind: Int): String {
        switch kind {
          case 0 {
            "eof"
          }
          case 1, 2 {
            "ident-or-int"
          }
          else {
            "other"
          }
        }
      }
      fn main {
        n = switch 1 {
          case 0 {
            0
          }
          else {
            1
          }
        }
        puts(n)
      }
      AV
    sw = program.functions[0].body[0].as(Avant::AST::ExprStmt).expr.as(Avant::AST::SwitchExpr)
    sw.cases.size.should eq 2
    sw.cases[1].labels.should eq [1_i64, 2_i64]
    sw.else_body.should_not be_nil
    bind = program.functions[1].body[0].as(Avant::AST::AssignStmt)
    bind.value.should be_a(Avant::AST::SwitchExpr)
  end

  it "parses class field defaults" do
    program = parse(File.read("#{__DIR__}/../examples/defaults.av"))
    fields = program.classes[0].fields
    fields[0].default.should be_a(Avant::AST::IntegerLiteral)
    fields[1].default.should be_a(Avant::AST::StringLiteral)
  end

  it "rejects struct field defaults" do
    expect_raises(Avant::CompileError, /struct fields have no defaults/) do
      parse(<<-AV)
        struct Point {
          x: Int = 0
        }
        fn main {
          puts(1)
        }
        AV
    end
  end

  it "rejects duplicate case labels" do
    ex = compile_error(<<-AV)
      fn main {
        switch 1 {
          case 1 {
            puts(1)
          }
          case 1 {
            puts(2)
          }
        }
      }
      AV
    ex.message.should match(/duplicate case/)
  end

  it "rejects a value switch without else" do
    ex = compile_error(<<-AV)
      fn f(n: Int): String {
        switch n {
          case 0 {
            "zero"
          }
        }
      }
      fn main {
        puts(f(0))
      }
      AV
    ex.message.should match(/needs else/)
  end

  it "rejects a string discriminant" do
    ex = compile_error(<<-AV)
      fn main {
        switch "x" {
          case 0 {
            puts(1)
          }
        }
      }
      AV
    ex.message.should match(/Int or Bool/)
  end

  it "emits myc SWITCH" do
    ir = compile(File.read("#{__DIR__}/../examples/switch.av"))
    ir.should contain("SWITCH")
    ir.should contain("CASE 0")
    ir.should contain("CASE 1")
    ir.should contain("CASE 2")
    ir.should contain("ENDSWITCH")
  end

  it "runs switch.av" do
    run_av("#{__DIR__}/../examples/switch.av").should eq "eof\nident-or-int\nident-or-int\nother\nnonzero\n"
  end

  it "runs a Bool switch as 0/1" do
    run_src(<<-AV).should eq "yes\n"
      fn main {
        s = switch true {
          case 0 {
            "no"
          }
          case 1 {
            "yes"
          }
          else {
            "other"
          }
        }
        puts(s)
      }
      AV
  end

  it "runs a void switch without else as a no-op miss" do
    run_src(<<-AV).should eq "ok\n"
      fn main {
        switch 9 {
          case 0 {
            puts("hit")
          }
        }
        puts("ok")
      }
      AV
  end

  it "runs defaults.av" do
    run_av("#{__DIR__}/../examples/defaults.av").should eq "0\n0\n1\n5\n0\n"
  end

  it "stores nilable class field defaults" do
    run_src(<<-AV).should eq "0\n"
      class Node {
        n: Int = 0
        next: Node? = nil
        fn initialize {
        }
      }
      fn main {
        a = Node.new()
        if n = a.next {
          puts(1)
        } else {
          puts(0)
        }
      }
      AV
  end

  it "rejects a non-literal field default" do
    ex = compile_error(<<-AV)
      class Box {
        n: Int = 1 + 2
        fn initialize {
        }
      }
      fn main {
        puts(Box.new().n)
      }
      AV
    ex.message.should match(/literal or nil/)
  end

  it "reads and writes a file" do
    path = File.tempname("avant-io")
    begin
      src = <<-AV
        fn main {
          w = file_write("#{path}", "hello")
          puts(w)
          if s = file_read("#{path}") {
            puts(s)
          } else {
            puts("missing")
          }
        }
        AV
      run_src(src).should eq "0\nhello\n"
    ensure
      File.delete(path) if File.exists?(path)
    end
  end

  it "returns nil when file_read fails" do
    run_src(<<-AV).should eq "missing\n"
      fn main {
        if s = file_read("/no/such/avant-file-#{Process.pid}") {
          puts(s)
        } else {
          puts("missing")
        }
      }
      AV
  end

  it "exposes argv on a compiled binary" do
    src = <<-AV
      fn main {
        args = argv()
        puts(args.size)
        i = 1
        while i < args.size {
          puts(args[i])
          i = i + 1
        }
      }
      AV
    bin = compile_bin(src)
    begin
      code, out = run_bin(bin, ["hello", "world"])
      code.should eq 0
      lines = out.strip.split('\n')
      lines[0].should eq "3"
      lines[1].should eq "hello"
      lines[2].should eq "world"
    ensure
      File.delete(bin) if File.exists?(bin)
    end
  end

  it "runs a subprocess and returns the exit code" do
    run_src(<<-AV).should eq "0\n1\n"
      fn main {
        puts(process_run("/bin/true", []))
        puts(process_run("/bin/false", []))
      }
      AV
  end

  it "reads an environment variable" do
    run_src(<<-AV).should eq "yes\n"
      fn main {
        if s = env_get("PATH") {
          puts("yes")
        } else {
          puts("no")
        }
      }
      AV
  end

  it "returns nil when env_get misses" do
    run_src(<<-AV).should eq "missing\n"
      fn main {
        if s = env_get("AVANT_NO_SUCH_VAR_XYZ") {
          puts(s)
        } else {
          puts("missing")
        }
      }
      AV
  end

  it "checks whether a path exists" do
    run_src(<<-AV).should eq "yes\nno\n"
      fn main {
        if file_exists("/") {
          puts("yes")
        } else {
          puts("no")
        }
        if file_exists("/no/such/avant-path") {
          puts("yes")
        } else {
          puts("no")
        }
      }
      AV
  end
end
