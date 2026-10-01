# Retired Stage 16: Crystal is not the port oracle. Daily proof is tests/run.av.
# Last blessing 2026-09-29: 176 examples, 0 failures. Re-run: AVANT_BLESS_HOST=1 crystal spec
{% unless env("AVANT_BLESS_HOST") == "1" %}
  {% skip_file %}
{% end %}

require "digest/sha256"
require "./spec_helper"

describe "Stage 15 compiler fixed-point" do
  root = File.expand_path("..", __DIR__)
  myc = File.expand_path("../../safepoints/myc-llvm", __DIR__)
  runner = File.expand_path("../tests/run.av", __DIR__)
  hello = File.expand_path("../examples/hello.av", __DIR__)
  golden = File.expand_path("../tests/goldens/hello.myc", __DIR__)
  main_av = File.expand_path("../compiler/main.av", __DIR__)
  bin_a = ""
  bin_b = ""
  bin_c = ""
  bin_d = ""

  before_all do
    File.exists?(myc).should be_true
    bin_a = compile_av_bin(port_driver_files)
    bin_b = File.tempname("avant-av-b")
    bin_c = File.tempname("avant-av-c")
    bin_d = File.tempname("avant-av-d")
    with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
      code, compile_out = run_bin(bin_a, ["compile"] + port_driver_files + [bin_b])
      code.should eq(0), compile_out
      File.exists?(bin_b).should eq(true), compile_out
      File.chmod(bin_b, 0o755)

      code, compile_out = run_bin(bin_b, ["compile"] + port_driver_files + [bin_c])
      code.should eq(0), compile_out
      File.exists?(bin_c).should eq(true), compile_out
      File.chmod(bin_c, 0o755)

      code, compile_out = run_bin(bin_c, ["compile"] + port_driver_files + [bin_d])
      code.should eq(0), compile_out
      File.exists?(bin_d).should eq(true), compile_out
      File.chmod(bin_d, 0o755)
    end
  end

  after_all do
    File.delete(bin_a) if File.exists?(bin_a)
    File.delete(bin_b) if File.exists?(bin_b)
    File.delete(bin_c) if File.exists?(bin_c)
    File.delete(bin_d) if File.exists?(bin_d)
  end

  it "matches C dump of hello.av to B and the golden" do
    with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
      code, dump_b = run_bin(bin_b, ["dump", hello])
      code.should eq(0), dump_b
      code, dump_c = run_bin(bin_c, ["dump", hello])
      code.should eq(0), dump_c
      dump_c.should eq(dump_b)
      dump_c.should eq(File.read(golden))
    end
  end

  it "matches sha256 of B and C myc for compiler/main.av" do
    with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
      code, myc_b = run_bin(bin_b, ["dump", main_av])
      code.should eq(0), "B dump compiler myc failed (#{myc_b.bytesize} bytes)"
      code, myc_c = run_bin(bin_c, ["dump", main_av])
      code.should eq(0), "C dump compiler myc failed (#{myc_c.bytesize} bytes)"
      Digest::SHA256.hexdigest(myc_b).should eq(Digest::SHA256.hexdigest(myc_c))
    end
  end

  it "builds compiler D from C and D runs hello.av" do
    with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
      code, run_out = run_bin(bin_d, ["run", hello])
      code.should eq(0), run_out
      run_out.should eq "Hello Avant\n"
    end
  end

  it "prints pass when compiler C runs the native suite" do
    with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc, "AVANT_COMPILER" => bin_c}) do
      code, printed = run_bin(bin_c, ["run", runner])
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
end
