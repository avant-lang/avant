require "./spec_helper"

describe "Stage 13 parser" do
  it "parses quote, splice, and comptime" do
    program = parse(<<-AV)
      quote {
        fn doubled(n: Int): Int {
          n + n
        }
      }
      struct Point {
        x: Int
        y: Int
      }
      comptime {
        for f in Point.fields {
          quote {
            fn #(f.name)(p: Point): #(f.type) {
              p.#(f.name)
            }
          }
        }
      }
      AV
    program.quotes.size.should eq 1
    program.quotes[0].functions[0].name.should eq "doubled"
    program.comptimes.size.should eq 1
    program.comptimes[0].var_name.should eq "f"
    program.comptimes[0].type_name.should eq "Point"
    walk_fn = program.comptimes[0].quote.functions[0]
    walk_fn.name_splice.should_not be_nil
  end
end

describe "Stage 13 macros" do
  it "injects a quoted function" do
    run_src(<<-AV).should eq "42\n"
      quote {
        fn doubled(n: Int): Int {
          n + n
        }
      }
      fn main {
        puts(doubled(21))
      }
      AV
  end

  it "splices a string as a function name" do
    run_src(<<-AV).should eq "21\n"
      quote {
        fn #("tripled")(n: Int): Int {
          n * 3
        }
      }
      fn main {
        puts(tripled(7))
      }
      AV
  end

  it "keeps quote locals from assigning outward" do
    run_src(<<-AV).should eq "2\n1\n"
      fn main {
        tmp = 1
        quote {
          tmp = 2
          puts(tmp)
        }
        puts(tmp)
      }
      AV
  end

  it "derives field accessors from Type.fields" do
    run_src(<<-AV).should eq "3\n4\n"
      struct Point {
        x: Int
        y: Int
      }
      comptime {
        for f in Point.fields {
          quote {
            fn #(f.name)(p: Point): #(f.type) {
              p.#(f.name)
            }
          }
        }
      }
      fn main {
        p = Point { x: 3, y: 4 }
        puts(x(p))
        puts(y(p))
      }
      AV
  end

  it "injects a quoted method" do
    run_src(<<-AV).should eq "6\n"
      struct Box {
        n: Int
        quote {
          fn doubled: Int {
            self.n + self.n
          }
        }
      }
      fn main {
        b = Box { n: 3 }
        puts(b.doubled())
      }
      AV
  end

  it "rejects splice outside quote" do
    ex = compile_error(<<-AV)
      fn #(name): Int {
        1
      }
      fn main {
        puts(1)
      }
      AV
    ex.message.should match(/splice #\(\) only belongs in quote/)
  end

  it "rejects comptime that is not a field walk" do
    ex = compile_error(<<-AV)
      comptime {
        puts(1)
      }
      fn main {
        puts(1)
      }
      AV
    ex.message.should match(/field walk/)
  end

  it "rejects nested quote" do
    ex = compile_error(<<-AV)
      quote {
        fn inner: Int {
          quote {
            1
          }
        }
      }
      fn main {
        puts(1)
      }
      AV
    ex.message.should match(/quote cannot nest/)
  end
end

{% if env("AVANT_BLESS_HOST") == "1" %}
describe "Stage 13 identity dump" do
  it "matches the Crystal host tokens, AST, typed dump, and myc IR on quote fixtures" do
    bin = compile_av_bin(port_compiler_files)
    begin
      dir = File.tempname("avant-s13")
      Dir.mkdir(dir)
      begin
        cases = {
          "quote_fn.av" => %(quote {\n  fn doubled(n: Int): Int {\n    n + n\n  }\n}\nfn main {\n  puts(doubled(21))\n}\n),
          "splice_name.av" => %(quote {\n  fn #("tripled")(n: Int): Int {\n    n * 3\n  }\n}\nfn main {\n  puts(tripled(7))\n}\n),
          "hygiene.av" => %(fn main {\n  tmp = 1\n  quote {\n    tmp = 2\n    puts(tmp)\n  }\n  puts(tmp)\n}\n),
          "fields.av" => %(struct Point {\n  x: Int\n  y: Int\n}\ncomptime {\n  for f in Point.fields {\n    quote {\n      fn #(f.name)(p: Point): #(f.type) {\n        p.#(f.name)\n      }\n    }\n  }\n}\nfn main {\n  p = Point { x: 3, y: 4 }\n  puts(x(p))\n  puts(y(p))\n}\n),
          "method.av" => %(struct Box {\n  n: Int\n  quote {\n    fn doubled: Int {\n      self.n + self.n\n    }\n  }\n}\nfn main {\n  b = Box { n: 3 }\n  puts(b.doubled())\n}\n),
        }
        cases.each do |name, text|
          path = File.join(dir, name)
          File.write(path, text)
          code, out = run_bin(bin, [path])
          code.should eq(0), "#{name} tokens: #{out}"
          out.should eq(dump_host_tokens(text, path)), name

          code, out = run_bin(bin, ["--ast", path])
          code.should eq(0), "#{name} ast: #{out}"
          out.should eq(dump_host_ast(text, path)), name

          code, out = run_bin(bin, ["--check", path])
          code.should eq(0), "#{name} check: #{out}"
          out.should eq(dump_host_typed(text, path)), name

          code, out = run_bin(bin, ["--myc", path])
          code.should eq(0), "#{name} myc: #{out}"
          out.should eq(Avant.compile(text, path)), name
        end

        errors = {
          "splice_out.av" => %(fn #(name): Int {\n  1\n}\nfn main {\n  puts(1)\n}\n),
          "bad_comptime.av" => %(comptime {\n  puts(1)\n}\nfn main {\n  puts(1)\n}\n),
          "nested.av" => %(quote {\n  fn inner: Int {\n    quote {\n      1\n    }\n  }\n}\nfn main {\n  puts(1)\n}\n),
        }
        errors.each do |name, text|
          path = File.join(dir, name)
          File.write(path, text)
          code, out = run_bin(bin, ["--check", path])
          code.should eq(0), "#{name}: #{out}"
          out.should eq(dump_host_typed(text, path)), name
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
{% end %}
