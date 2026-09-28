require "./spec_helper"

describe "Stage 5" do
  it "tokenizes lib and fun" do
    kinds = token_kinds("lib math { fun sqrt(x: Float64): Float64 }")
    kinds.should contain(Avant::Token::Kind::Lib)
    kinds.should contain(Avant::Token::Kind::Fun)
  end

  it "parses a lib block" do
    program = parse(File.read("#{__DIR__}/../examples/sqrt.av"))
    program.libs.size.should eq 1
    mod = program.libs[0]
    mod.name.should eq "math"
    mod.funs.size.should eq 1
    decl = mod.funs[0]
    decl.name.should eq "sqrt"
    decl.params.size.should eq 1
    decl.params[0].name.should eq "x"
    decl.params[0].type.name.should eq "Float64"
    decl.return_type.not_nil!.name.should eq "Float64"
  end

  it "parses a fun with no parameter list" do
    program = parse(File.read("#{__DIR__}/../examples/zlib.av"))
    fn = program.libs[0].funs[0]
    fn.name.should eq "zlibVersion"
    fn.params.should be_empty
    fn.return_type.not_nil!.name.should eq "String"
  end

  it "rejects a top-level fun" do
    ex = compile_error(<<-AV)
      fun sqrt(x: Float64): Float64
      fn main {
        puts(1)
      }
      AV
    ex.message.should match(/fun belongs in a lib block/)
  end

  it "rejects a fun with a body" do
    expect_raises(Avant::CompileError, /no body/) do
      parse(<<-AV)
        lib math {
          fun sqrt(x: Float64): Float64 {
            x
          }
        }
        fn main {
          puts(1)
        }
        AV
    end
  end

  it "emits an external myc FUNC for a C fun" do
    ir = compile(File.read("#{__DIR__}/../examples/sqrt.av"))
    ir.should contain("FUNC :sqrt")
    ir.should contain("TYPE :f64")
    ir.should contain("CALL :sqrt")
  end

  it "runs sqrt.av against libm" do
    run_av("#{__DIR__}/../examples/sqrt.av").should eq "2\n"
  end

  it "runs zlib.av against zlib" do
    out = run_av("#{__DIR__}/../examples/zlib.av", linker_flags: ["-lz"])
    out.should match(/^\d+\.\d+/)
  end

  it "calls atoi from libc" do
    ir = compile(<<-AV)
      lib libc {
        fun atoi(s: String): Int
      }
      fn main {
        puts(libc.atoi("42"))
      }
      AV
    run_ir(ir).should eq "42\n"
  end

  it "runs add.av against a compiled C file" do
    obj = Avant::LinkJob.compile_c("#{__DIR__}/../examples/c/add.c")
    run_av("#{__DIR__}/../examples/add.av", extra_objects: [obj]).should eq "5\n"
  end

  it "links a compiled C object and calls it" do
    obj = Avant::LinkJob.compile_c("#{__DIR__}/../examples/c/add.c")
    ir = compile(<<-AV)
      lib add {
        fun avant_add(a: Int, b: Int): Int
        fun avant_sentinel: Ptr(Void)
        fun avant_is_null(p: Ptr(Void)): Int
      }
      fn main {
        puts(add.avant_add(2, 3))
        p = add.avant_sentinel()
        puts(add.avant_is_null(p))
      }
      AV
    run_ir(ir, extra_objects: [obj]).should eq "5\n0\n"
  end

  it "generates a lib block from a C header" do
    text = Avant::Bind.generate("#{__DIR__}/../examples/c/add.h", "add")
    text.should contain("lib add {")
    text.should contain("fun avant_add(a: Int, b: Int): Int")
    text.should contain("fun avant_sentinel: Ptr(Void)")
    text.should contain("fun avant_is_null(p: Ptr(Void)): Int")
  end

  it "runs code generated from a header" do
    obj = Avant::LinkJob.compile_c("#{__DIR__}/../examples/c/add.c")
    bound = Avant::Bind.generate("#{__DIR__}/../examples/c/add.h", "add")
    ir = compile(bound + <<-AV)

      fn main {
        puts(add.avant_add(20, 22))
      }
      AV
    run_ir(ir, extra_objects: [obj]).should eq "42\n"
  end

  it "generates zlibVersion from a stub header" do
    text = Avant::Bind.generate("#{__DIR__}/../examples/c/zlib_version.h", "z")
    text.should contain("fun zlibVersion: String")
  end
end
