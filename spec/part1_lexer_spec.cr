# Retired Stage 16: Crystal is not the port oracle. Daily proof is tests/run.av.
# Last blessing 2026-09-29: 176 examples, 0 failures. Re-run: AVANT_BLESS_HOST=1 crystal spec
{% unless env("AVANT_BLESS_HOST") == "1" %}
  {% skip_file %}
{% end %}

require "./spec_helper"

describe "Stage 8 Part 1 lexer and parser" do
  it "matches the Crystal host tokens and AST on Stage 1–3 examples, switch.av, defaults.av, lib/fun, errors.av, and lexer fixtures" do
    bin = compile_av_bin(port_lexer_files)
    begin
      fixtures = {
        File.expand_path("../examples/hello.av", __DIR__) => File.read(File.expand_path("../examples/hello.av", __DIR__)),
        File.expand_path("../examples/fact.av", __DIR__)  => File.read(File.expand_path("../examples/fact.av", __DIR__)),
        File.expand_path("../examples/vec2.av", __DIR__)  => File.read(File.expand_path("../examples/vec2.av", __DIR__)),
        File.expand_path("../examples/sieve.av", __DIR__) => File.read(File.expand_path("../examples/sieve.av", __DIR__)),
        File.expand_path("../examples/switch.av", __DIR__) => File.read(File.expand_path("../examples/switch.av", __DIR__)),
        File.expand_path("../examples/methods.av", __DIR__) => File.read(File.expand_path("../examples/methods.av", __DIR__)),
        File.expand_path("../examples/trees.av", __DIR__) => File.read(File.expand_path("../examples/trees.av", __DIR__)),
        File.expand_path("../examples/binarytrees.av", __DIR__) => File.read(File.expand_path("../examples/binarytrees.av", __DIR__)),
        File.expand_path("../examples/defaults.av", __DIR__) => File.read(File.expand_path("../examples/defaults.av", __DIR__)),
        File.expand_path("../examples/sqrt.av", __DIR__) => File.read(File.expand_path("../examples/sqrt.av", __DIR__)),
        File.expand_path("../examples/zlib.av", __DIR__) => File.read(File.expand_path("../examples/zlib.av", __DIR__)),
        File.expand_path("../examples/add.av", __DIR__) => File.read(File.expand_path("../examples/add.av", __DIR__)),
        File.expand_path("../examples/errors.av", __DIR__) => File.read(File.expand_path("../examples/errors.av", __DIR__)),
      }

      fixtures.each do |path, text|
        code, out = run_bin(bin, [path])
        code.should eq 0
        out.should eq dump_host_tokens(text, path)
      end

      fixtures.each do |path, text|
        code, out = run_bin(bin, ["--ast", path])
        code.should eq 0
        out.should eq dump_host_ast(text, path)
      end

      dir = File.tempname("avant-lex")
      Dir.mkdir(dir)
      begin
        cases = {
          "empty.av"   => "",
          "hello.av"   => %(fn main {\n  puts("hi")\n}\n),
          "float.av"   => "1.5e-2 4.8e+00",
          "string.av"  => %("hello"),
          "escape.av"  => %("a\\nb"),
          "ops.av"     => "&& || & | ^ << >> ~",
          "comment.av" => "fn main { // comment\n}\n",
          "interp.av"  => %("hello ${name}"),
          "hex.av"     => "0xFFu64 1i64",
          "switch.av"  => "switch kind { case 0 { 1 } else { 2 } }",
        }
        cases.each do |name, text|
          path = File.join(dir, name)
          File.write(path, text)
          code, out = run_bin(bin, [path])
          code.should eq 0
          out.should eq dump_host_tokens(text, path)
        end

        bad = File.join(dir, "bad.av")
        File.write(bad, %("hello))
        code, out = run_bin(bin, [bad])
        code.should eq 0
        out.should match(/unterminated/)
      ensure
        Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
        Dir.delete(dir)
      end
    ensure
      File.delete(bin) if File.exists?(bin)
    end
  end
end
