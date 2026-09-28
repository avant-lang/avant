require "./spec_helper"

describe "Stage 8 Part 2 driver" do
  it "dumps IR and runs Stage 1–6 examples matching the Crystal host" do
    root = File.expand_path("..", __DIR__)
    myc = File.expand_path("../../myc/myc-llvm", __DIR__)
    File.exists?(myc).should be_true

    bin = compile_av_bin(port_driver_files)
    begin
      hello = File.expand_path("../examples/hello.av", __DIR__)
      fact = File.expand_path("../examples/fact.av", __DIR__)
      vec2 = File.expand_path("../examples/vec2.av", __DIR__)
      sieve = File.expand_path("../examples/sieve.av", __DIR__)
      switch = File.expand_path("../examples/switch.av", __DIR__)
      methods = File.expand_path("../examples/methods.av", __DIR__)
      trees = File.expand_path("../examples/trees.av", __DIR__)
      binarytrees = File.expand_path("../examples/binarytrees.av", __DIR__)
      defaults = File.expand_path("../examples/defaults.av", __DIR__)
      sqrt = File.expand_path("../examples/sqrt.av", __DIR__)
      zlib = File.expand_path("../examples/zlib.av", __DIR__)
      add = File.expand_path("../examples/add.av", __DIR__)
      add_c = File.expand_path("../examples/c/add.c", __DIR__)
      errors = File.expand_path("../examples/errors.av", __DIR__)
      collect = File.expand_path("../examples/collect.av", __DIR__)
      json = File.expand_path("../examples/json.av", __DIR__)
      http = File.expand_path("../examples/http.av", __DIR__)
      spawn = File.expand_path("../examples/spawn.av", __DIR__)
      matmul = File.expand_path("../examples/matmul.av", __DIR__)

      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
        [hello, fact, vec2, sieve, switch, methods, trees, binarytrees, defaults, sqrt, zlib, add, errors, collect, json, http, spawn, matmul].each do |path|
          code, out = run_bin(bin, ["dump", path])
          code.should eq 0
          out.should eq Avant.compile_file(path)
        end

        [hello, fact, vec2, sieve, switch, methods, trees, binarytrees, defaults, sqrt, errors, collect, json, http, spawn].each do |path|
          code, out = run_bin(bin, ["run", path])
          code.should eq 0
          out.should eq run_av(path)
        end

        code, out = run_bin(bin, ["run", zlib, "--lib", "z"])
        code.should eq 0
        out.should eq run_av(zlib, linker_flags: ["-lz"])

        code, out = run_bin(bin, ["run", add, "--cc", add_c])
        code.should eq 0
        obj = Avant::LinkJob.compile_c(add_c)
        out.should eq run_av(add, extra_objects: [obj])

        code, out = run_bin(bin, ["run", matmul])
        code.should eq 0
        host = run_av(matmul).strip.split('\n')
        lines = out.strip.split('\n')
        lines.size.should eq 4
        lines[0].should eq lines[1]
        lines[0].should eq host[0]
        lines[1].should eq host[1]

        code, out = run_bin(bin, [hello])
        code.should eq 0
        out.should eq "Hello Avant\n"
      end
    ensure
      File.delete(bin) if File.exists?(bin)
    end
  end
end
