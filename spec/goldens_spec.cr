# Retired Stage 16: Crystal is not the port oracle. Daily proof is tests/run.av.
# Last blessing 2026-09-29: 176 examples, 0 failures. Re-run: AVANT_BLESS_HOST=1 crystal spec
{% unless env("AVANT_BLESS_HOST") == "1" %}
  {% skip_file %}
{% end %}

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
  {"tests/fixtures/overloading/defaults.av", "overloading/defaults"},
  {"tests/fixtures/overloading/overload.av", "overloading/overload"},
  {"tests/fixtures/overloading/operator.av", "overloading/operator"},
  {"tests/fixtures/overloading/generic.av", "overloading/generic"},
  {"tests/fixtures/macros/quote_fn.av", "macros/quote_fn"},
  {"tests/fixtures/macros/splice_name.av", "macros/splice_name"},
  {"tests/fixtures/macros/hygiene.av", "macros/hygiene"},
  {"tests/fixtures/macros/fields.av", "macros/fields"},
  {"tests/fixtures/macros/method.av", "macros/method"},
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

    path = File.join(root, "tests/fixtures/modules/main.av")
    text = File.read(path)
    base = File.join(root, "tests/goldens/modules/main")
    File.read("#{base}.tok").should eq(dump_host_tokens(text, path)), "modules/main.tok"
    File.read("#{base}.ast").should eq(dump_host_ast(text, path)), "modules/main.ast"
    File.read("#{base}.myc").should eq(Avant.compile_file(path)), "modules/main.myc"

    File.read(File.join(root, "tests/goldens/hello.myc")).should eq(
      File.read(File.join(root, "tests/goldens/examples/hello.myc"))
    )
  end
end
