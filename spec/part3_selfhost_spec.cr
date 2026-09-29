# Retired Stage 16: Crystal is not the port oracle. Daily proof is tests/run.av.
# Last blessing 2026-09-29: 176 examples, 0 failures. Re-run: AVANT_BLESS_HOST=1 crystal spec
{% unless env("AVANT_BLESS_HOST") == "1" %}
  {% skip_file %}
{% end %}

require "./spec_helper"

describe "Stage 8 Part 3 identity surface" do
  it "matches the Crystal host myc IR on bootstrap builtins the compiler sources use" do
    bin = compile_av_bin(port_compiler_files)
    begin
      dir = File.tempname("avant-part3")
      Dir.mkdir(dir)
      begin
        cases = {
          "chr.av"     => %(fn main {\n  puts(chr(65))\n}\n),
          "slice.av"   => %(fn main {\n  puts(str_slice("abcd", 1, 3))\n}\n),
          "index.av"   => %(fn main {\n  s = "A"\n  puts(s[0])\n}\n),
          "buf.av"     => %(fn main {\n  b = buf_new()\n  b.push_str("hi")\n  b.push_byte(33)\n  puts(b.to_s)\n}\n),
          "conv.av"    => %(fn main {\n  n = 3\n  puts(n.to_u64.to_i)\n}\n),
          "exists.av"  => %(fn main {\n  if file_exists("/") {\n    puts(1)\n  } else {\n    puts(0)\n  }\n}\n),
          "argv.av"    => %(fn main {\n  a = argv()\n  puts(a.size)\n}\n),
          "env.av"     => %(fn main {\n  if s = env_get("PATH") {\n    puts(s.size > 0)\n  } else {\n    puts(false)\n  }\n}\n),
          "interp.av"  => %(fn main {\n  name = "Avant"\n  puts("hello ${name}")\n}\n),
          "pop.av"     => %(fn main {\n  xs: Array(Int) = [1, 2]\n  puts(xs.pop)\n}\n),
        }
        cases.each do |name, text|
          path = File.join(dir, name)
          File.write(path, text)
          code, dumped = run_bin(bin, ["--myc", path])
          code.should eq(0), "#{name}: #{dumped}"
          dumped.should eq(Avant.compile(text, path)), name
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

describe "Stage 8 Part 3 self-host" do
  it "builds compiler B from compiler A and B runs hello.av" do
    root = File.expand_path("..", __DIR__)
    myc = File.expand_path("../../myc/myc-llvm", __DIR__)
    File.exists?(myc).should be_true

    hello = File.expand_path("../examples/hello.av", __DIR__)
    bin_a = compile_av_bin(port_driver_files)
    bin_b = File.tempname("avant-av-b")
    begin
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
        code, compile_out = run_bin(bin_a, ["compile"] + port_driver_files + [bin_b])
        code.should eq(0), compile_out
        File.exists?(bin_b).should eq(true), compile_out
        File.chmod(bin_b, 0o755)

        code, run_out = run_bin(bin_b, ["run", hello])
        code.should eq(0), run_out
        run_out.should eq "Hello Avant\n"
      end
    ensure
      File.delete(bin_a) if File.exists?(bin_a)
      File.delete(bin_b) if File.exists?(bin_b)
    end
  end
end
