require "./spec_helper"

GOLDEN_ALL = [
  {"examples/hello.av", "examples/hello"},
  {"examples/fact.av", "examples/fact"},
  {"examples/vec2.av", "examples/vec2"},
  {"examples/sieve.av", "examples/sieve"},
  {"examples/methods.av", "examples/methods"},
  {"examples/trees.av", "examples/trees"},
  {"examples/binarytrees.av", "examples/binarytrees"},
  {"examples/switch.av", "examples/switch"},
  {"examples/defaults.av", "examples/defaults"},
  {"examples/sqrt.av", "examples/sqrt"},
  {"examples/zlib.av", "examples/zlib"},
  {"examples/add.av", "examples/add"},
  {"examples/errors.av", "examples/errors"},
  {"examples/collect.av", "examples/collect"},
  {"examples/json.av", "examples/json"},
  {"examples/http.av", "examples/http"},
  {"examples/spawn.av", "examples/spawn"},
  {"examples/matmul.av", "examples/matmul"},
  {"tests/fixtures/part3/chr.av", "part3/chr"},
  {"tests/fixtures/part3/slice.av", "part3/slice"},
  {"tests/fixtures/part3/index.av", "part3/index"},
  {"tests/fixtures/part3/buf.av", "part3/buf"},
  {"tests/fixtures/part3/conv.av", "part3/conv"},
  {"tests/fixtures/part3/exists.av", "part3/exists"},
  {"tests/fixtures/part3/argv.av", "part3/argv"},
  {"tests/fixtures/part3/env.av", "part3/env"},
  {"tests/fixtures/part3/interp.av", "part3/interp"},
  {"tests/fixtures/part3/pop.av", "part3/pop"},
  {"tests/fixtures/part3/run.av", "part3/run"},
  {"tests/fixtures/stage11/defaults.av", "stage11/defaults"},
  {"tests/fixtures/stage11/overload.av", "stage11/overload"},
  {"tests/fixtures/stage11/operator.av", "stage11/operator"},
  {"tests/fixtures/stage11/generic.av", "stage11/generic"},
  {"tests/fixtures/stage13/quote_fn.av", "stage13/quote_fn"},
  {"tests/fixtures/stage13/splice_name.av", "stage13/splice_name"},
  {"tests/fixtures/stage13/hygiene.av", "stage13/hygiene"},
  {"tests/fixtures/stage13/fields.av", "stage13/fields"},
  {"tests/fixtures/stage13/method.av", "stage13/method"},
]

describe "Stage 14 goldens" do
  root = File.expand_path("..", __DIR__)

  it "keeps host dumps matching the blessed files" do
    GOLDEN_ALL.each do |src_rel, stem|
      path = File.join(root, src_rel)
      text = File.read(path)
      base = File.join(root, "tests/goldens", stem)
      File.read("#{base}.tok").should eq(dump_host_tokens(text, path)), "#{stem}.tok"
      File.read("#{base}.ast").should eq(dump_host_ast(text, path)), "#{stem}.ast"
      File.read("#{base}.check").should eq(dump_host_typed(text, path)), "#{stem}.check"
      File.read("#{base}.myc").should eq(Avant.compile_file(path)), "#{stem}.myc"
    end

    path = File.join(root, "tests/fixtures/stage12/main.av")
    text = File.read(path)
    base = File.join(root, "tests/goldens/stage12/main")
    File.read("#{base}.tok").should eq(dump_host_tokens(text, path)), "stage12/main.tok"
    File.read("#{base}.ast").should eq(dump_host_ast(text, path)), "stage12/main.ast"
    File.read("#{base}.myc").should eq(Avant.compile_file(path)), "stage12/main.myc"

    File.read(File.join(root, "tests/goldens/hello.myc")).should eq(
      File.read(File.join(root, "tests/goldens/examples/hello.myc"))
    )
  end
end
