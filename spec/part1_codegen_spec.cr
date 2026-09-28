require "./spec_helper"

describe "Stage 8 Part 1 myc emit" do
  it "matches the Crystal host myc IR on Stage 1–3 examples, switch.av, defaults.av, lib/fun, errors.av, collect.av, json.av, http.av, spawn.av, matmul.av, and fn run" do
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
        File.expand_path("../examples/json.av", __DIR__) => File.read(File.expand_path("../examples/json.av", __DIR__)),
        File.expand_path("../examples/http.av", __DIR__) => File.read(File.expand_path("../examples/http.av", __DIR__)),
        File.expand_path("../examples/spawn.av", __DIR__) => File.read(File.expand_path("../examples/spawn.av", __DIR__)),
        File.expand_path("../examples/matmul.av", __DIR__) => File.read(File.expand_path("../examples/matmul.av", __DIR__)),
      }

      fixtures.each do |path, text|
        code, out = run_bin(bin, ["--myc", path])
        code.should eq 0
        out.should eq Avant.compile(text, path)
      end

      dir = File.tempname("avant-myc")
      Dir.mkdir(dir)
      begin
        path = File.join(dir, "run.av")
        text = %(fn run {\n  puts("hi")\n}\n)
        File.write(path, text)
        code, out = run_bin(bin, ["--myc", path])
        code.should eq 0
        out.should eq Avant.compile(text, path)
      ensure
        Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
        Dir.delete(dir)
      end
    ensure
      File.delete(bin) if File.exists?(bin)
    end
  end
end
