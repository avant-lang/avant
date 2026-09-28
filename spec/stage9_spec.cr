require "./spec_helper"

describe "Stage 9 process_run_out" do
  it "captures subprocess stdout to a file" do
    path = File.tempname("avant-out")
    begin
      src = <<-AV
        fn main {
          args: Array(String) = []
          args.push("hello")
          puts(process_run_out("/bin/echo", args, "#{path}"))
        }
        AV
      run_src(src).should eq "0\n"
      File.read(path).should eq "hello\n"
    ensure
      File.delete(path) if File.exists?(path)
    end
  end
end

describe "Stage 9 native runner" do
  root = File.expand_path("..", __DIR__)
  myc = File.expand_path("../../myc/myc-llvm", __DIR__)
  runner = File.expand_path("../tests/run.av", __DIR__)
  bin_a = ""

  before_all do
    File.exists?(myc).should be_true
    bin_a = compile_av_bin(port_driver_files)
  end

  after_all do
    File.delete(bin_a) if File.exists?(bin_a)
  end

  it "matches the Crystal host myc IR on process_run_out" do
    dir = File.tempname("avant-stage9-ir")
    Dir.mkdir(dir)
    begin
      path = File.join(dir, "out.av")
      text = %(fn main {\n  args: Array(String) = []\n  puts(process_run_out("/bin/true", args, "/tmp/avant-stage9-ir-out.txt"))\n}\n)
      File.write(path, text)
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
        code, dumped = run_bin(bin_a, ["dump", path])
        code.should eq(0), dumped
        dumped.should eq(Avant.compile(text, path))
      end
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "prints pass for a directory of case files" do
    dir = File.tempname("avant-stage9-pass")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "ok.av"), %(fn test_ok(c: Checks) {\n  c.suite("tests/cases/ok.av")\n  c.expect(true, "ok")\n}\n))
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc, "AVANT_COMPILER" => bin_a, "AVANT_TEST_DIR" => dir}) do
        code, printed = run_bin(bin_a, ["run", runner])
        code.should eq(0), printed
        printed.should match(/PASS  tests\/cases\/ok\.av/)
        printed.should match(/Test Suites: 1 passed, 1 total/)
        printed.should match(/Tests:       1 passed, 1 total/)
        printed.should match(/Ran all test suites/)
      end
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "prints fail when an expect is false" do
    dir = File.tempname("avant-stage9")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "boom.av"), %(fn test_boom(c: Checks) {\n  c.suite("tests/cases/boom.av")\n  c.expect(false, "intentional")\n}\n))
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc, "AVANT_COMPILER" => bin_a, "AVANT_TEST_DIR" => dir}) do
        code, printed = run_bin(bin_a, ["run", runner])
        code.should eq(0), printed
        printed.should match(/FAIL  tests\/cases\/boom\.av/)
        printed.should match(/intentional/)
        printed.should match(/Test Suites: 1 failed, 1 total/)
        printed.should match(/Tests:       1 failed, 1 total/)
      end
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "does not instrument dump unless --coverage is set" do
    dir = File.tempname("avant-stage9-nocov")
    Dir.mkdir(dir)
    begin
      path = File.join(dir, "hello.av")
      File.write(path, %(fn main {\n  puts("hi")\n}\n))
      with_env({"AVANT_ROOT" => root, "AVANT_MYC_LLVM" => myc}) do
        code, dumped = run_bin(bin_a, ["dump", path])
        code.should eq(0), dumped
        dumped.includes?("avant_cov_hit").should be_false
        dumped.should eq(Avant.compile(File.read(path), path))
        code2, cov = run_bin(bin_a, ["dump", "--coverage", path])
        code2.should eq(0), cov
        cov.includes?("avant_cov_hit").should be_true
        cov.includes?("avant_cov_init").should be_true
      end
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end
end
