require "./spec_helper"

describe "Stage 10 process_run_out stdout" do
  it "does not replay parent puts when the child redirects stdout" do
    path = File.tempname("avant-s10-discard")
    begin
      src = <<-AV
        fn main {
          puts("once")
          args: Array(String) = []
          dummy = process_run_out("/bin/true", args, "#{path}")
        }
        AV
      run_src(src).should eq "once\n"
    ensure
      File.delete(path) if File.exists?(path)
    end
  end
end

{% if env("AVANT_BLESS_HOST") == "1" %}
describe "Stage 10 port holes identity" do
  it "matches the Crystal host myc IR on dir_list and Stage 7 holes" do
    root = File.expand_path("..", __DIR__)
    myc = File.expand_path("../../safepoints/myc-llvm", __DIR__)
    File.exists?(myc).should be_true
    bin = compile_av_bin(port_driver_files)
    begin
      dir = File.tempname("avant-stage10-ir")
      Dir.mkdir(dir)
      begin
        cases = {
          "dir.av"  => %(fn main {\n  xs = dir_list("/")\n  puts(xs.size)\n}\n),
          "sha.av"  => %(fn main {\n  puts(sha256("abc"))\n}\n),
          "b64.av"  => %(fn main {\n  puts(b64_encode("aaa"))\n}\n),
          "json.av" => %(fn main {\n  doc = json_parse("{\\"n\\":1}")\n  r = json_root(doc)\n  json_free(doc)\n  puts(1)\n}\n),
          "arr.av"  => %(fn main {\n  xs = Array(Int).new(4)\n  xs.fill(7)\n  xs.reserve(8)\n  puts(xs[3])\n}\n),
        }
        with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
          cases.each do |name, text|
            path = File.join(dir, name)
            File.write(path, text)
            code, dumped = run_bin(bin, ["dump", path])
            code.should eq(0), "#{name}: #{dumped}"
            dumped.should eq(Avant.compile(text, path)), name
          end
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

describe "Stage 10 native suite" do
  root = File.expand_path("..", __DIR__)
  myc = File.expand_path("../../safepoints/myc-llvm", __DIR__)
  runner = File.expand_path("../tests/run.av", __DIR__)
  hello = File.expand_path("../examples/hello.av", __DIR__)
  bin_a = ""
  bin_b = ""

  before_all do
    File.exists?(myc).should be_true
    bin_a = compile_av_bin(port_driver_files)
    bin_b = File.tempname("avant-av-b")
    with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
      code, compile_out = run_bin(bin_a, ["compile"] + port_driver_files + [bin_b])
      code.should eq(0), compile_out
      File.exists?(bin_b).should eq(true), compile_out
      File.chmod(bin_b, 0o755)
    end
  end

  after_all do
    File.delete(bin_a) if File.exists?(bin_a)
    File.delete(bin_b) if File.exists?(bin_b)
  end

  it "prints pass when compiler B runs the native suite" do
    with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc, "AVANT_COMPILER" => bin_b}) do
      code, printed = run_bin(bin_b, ["run", runner])
      code.should eq(0), printed
      printed.should match(/PASS  tests\/cases\/smoke\.av/)
      printed.should match(/PASS  tests\/cases\/compiler\.av/)
      printed.should match(/PASS  tests\/cases\/coverage\.av/)
      printed.should match(/PASS  tests\/cases\/errors\.av/)
      printed.should match(/PASS  tests\/cases\/examples\.av/)
      printed.should match(/PASS  tests\/cases\/gc\.av/)
      printed.should match(/PASS  tests\/cases\/holes\.av/)
      printed.should match(/PASS  tests\/cases\/lang\.av/)
      printed.should match(/PASS  tests\/cases\/overloading\.av/)
      printed.should match(/PASS  tests\/cases\/modules\.av/)
      printed.should match(/PASS  tests\/cases\/macros\.av/)
      printed.should match(/PASS  tests\/cases\/goldens\.av/)
      printed.should match(/PASS  tests\/cases\/fixed_point\.av/)
      printed.should match(/PASS  tests\/cases\/oracle\.av/)
      printed.should match(/PASS  tests\/cases\/safepoints\.av/)
      printed.should match(/Test Suites: 15 passed, 15 total/)
      printed.should match(/Ran all test suites/)
    end
  end

  it "builds compiler C from B and C runs hello.av" do
    bin_c = File.tempname("avant-av-c")
    begin
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
        code, compile_out = run_bin(bin_b, ["compile"] + port_driver_files + [bin_c])
        code.should eq(0), compile_out
        File.exists?(bin_c).should eq(true), compile_out
        File.chmod(bin_c, 0o755)
        code, run_out = run_bin(bin_c, ["run", hello])
        code.should eq(0), run_out
        run_out.should eq "Hello Avant\n"
      end
    ensure
      File.delete(bin_c) if File.exists?(bin_c)
    end
  end
end
{% end %}
