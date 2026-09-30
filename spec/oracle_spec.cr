require "./spec_helper"

describe "Stage 16 recovery" do
  it "keeps src/ as recovery: the host still builds compiler A and A runs hello.av" do
    root = File.expand_path("..", __DIR__)
    myc = File.expand_path("../../myc/myc-llvm", __DIR__)
    hello = File.expand_path("../examples/hello.av", __DIR__)
    File.exists?(File.join(root, "src/cli.cr")).should be_true
    File.exists?(File.join(root, "compiler/main.av")).should be_true
    File.exists?(myc).should be_true

    bin_a = compile_av_bin(port_driver_files)
    begin
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
        code, run_out = run_bin(bin_a, ["run", hello])
        code.should eq(0), run_out
        run_out.should eq "Hello Avant\n"
      end
    ensure
      File.delete(bin_a) if File.exists?(bin_a)
    end
  end
end
