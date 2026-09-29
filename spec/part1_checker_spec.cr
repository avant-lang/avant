# Retired Stage 16: Crystal is not the port oracle. Daily proof is tests/run.av.
# Last blessing 2026-09-29: 176 examples, 0 failures. Re-run: AVANT_BLESS_HOST=1 crystal spec
{% unless env("AVANT_BLESS_HOST") == "1" %}
  {% skip_file %}
{% end %}

require "./spec_helper"

describe "Stage 8 Part 1 checker" do
  it "matches the Crystal host typed dump on Stage 1–3 examples, switch.av, defaults.av, lib/fun, errors.av, and collect.av" do
    bin = compile_av_bin(port_compiler_files)
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
        File.expand_path("../examples/collect.av", __DIR__) => File.read(File.expand_path("../examples/collect.av", __DIR__)),
      }

      fixtures.each do |path, text|
        code, out = run_bin(bin, ["--check", path])
        code.should eq 0
        out.should eq dump_host_typed(text, path)
      end
    ensure
      File.delete(bin) if File.exists?(bin)
    end
  end

  it "matches the Crystal host on checker errors and locals" do
    bin = compile_av_bin(port_compiler_files)
    begin
      dir = File.tempname("avant-check")
      Dir.mkdir(dir)
      begin
        cases = {
          "hello.av"   => %(fn main {\n  puts("Hello Avant")\n}\n),
          "run.av"     => %(fn run {\n  puts("hi")\n}\n),
          "add.av"     => %(fn add(a: Int, b: Int): Int {\n  a + b\n}\nfn main {\n  puts(add(2, 3))\n}\n),
          "locals.av"  => %(fn main {\n  x = 1\n  x = 2\n  puts(x)\n}\n),
          "nomain.av"  => %(fn fact(n: Int): Int {\n  n\n}\n),
          "unknown.av" => %(fn main {\n  puts(x)\n}\n),
          "ifbool.av"  => %(fn main {\n  if 1 { puts("x") }\n}\n),
          "noret.av"   => %(fn f: Int {\n}\nfn main {\n  puts(f())\n}\n),
          "leak.av"    => %(fn main {\n  if true {\n    y = 3\n  }\n  puts(y)\n}\n),
          "badtype.av" => %(fn main {\n  x: Float = 1\n  puts(1)\n}\n),
        }
        cases.each do |name, text|
          path = File.join(dir, name)
          File.write(path, text)
          code, out = run_bin(bin, ["--check", path])
          code.should eq 0
          out.should eq dump_host_typed(text, path)
        end
      ensure
        Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
        Dir.delete(dir)
      end
    ensure
      File.delete(bin) if File.exists?(bin)
    end
  end
end
