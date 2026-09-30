require "./spec_helper"

describe "Stage 12 parser" do
  it "parses import and pub" do
    program = parse(<<-AV)
      import lexer
      pub class Parser {
        pub fn parse: Int {
          1
        }
        fn skip {
          0
        }
      }
      pub fn (v: Parser) scaled: Int {
        1
      }
      AV
    program.imports.size.should eq 1
    program.imports[0].name.should eq "lexer"
    program.classes[0].vis.should eq true
    program.classes[0].methods[0].vis.should eq true
    program.classes[0].methods[1].vis.should eq false
    program.functions[0].vis.should eq true
    program.functions[0].receiver.should_not be_nil
  end

  it "parses a qualified type name" do
    program = parse(<<-AV)
      fn f(x: lexer.Lexer): Int {
        1
      }
      AV
    tn = program.functions[0].params[0].type
    tn.qualifier.should eq "lexer"
    tn.name.should eq "Lexer"
  end
end

describe "Stage 12 modules" do
  it "loads a neighbouring file and imports pub names" do
    dir = File.tempname("avant-s12")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "vec.av"), <<-AV)
        pub struct Vec2 {
          x: Int
          y: Int
        }
        pub fn origin: Vec2 {
          Vec2 { x: 0, y: 0 }
        }
        AV
      File.write(File.join(dir, "main.av"), <<-AV)
        import vec
        fn main {
          v = origin()
          puts(v.x)
        }
        AV
      run_av(File.join(dir, "main.av")).should eq "0\n"
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "hides a private helper from another file" do
    dir = File.tempname("avant-s12")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "util.av"), <<-AV)
        fn hidden: Int {
          1
        }
        pub fn shown: Int {
          hidden()
        }
        AV
      File.write(File.join(dir, "main.av"), <<-AV)
        import util
        fn main {
          puts(hidden())
        }
        AV
      ex = expect_raises(Avant::CompileError) do
        Avant.compile_file(File.join(dir, "main.av"))
      end
      ex.message.should match(/unknown function hidden/)
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "rejects an import cycle" do
    dir = File.tempname("avant-s12")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "a.av"), <<-AV)
        import b
        pub fn from_a: Int { 1 }
        AV
      File.write(File.join(dir, "b.av"), <<-AV)
        import a
        pub fn from_b: Int { 1 }
        AV
      File.write(File.join(dir, "main.av"), <<-AV)
        import a
        fn main {
          puts(from_a())
        }
        AV
      ex = expect_raises(Avant::CompileError) do
        Avant.compile_file(File.join(dir, "main.av"))
      end
      ex.message.should match(/import cycle/)
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "rejects a duplicate pub name from two imports" do
    dir = File.tempname("avant-s12")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "one.av"), <<-AV)
        pub class Box {
          n: Int
          fn initialize(n: Int) {
            self.n = n
          }
        }
        AV
      File.write(File.join(dir, "two.av"), <<-AV)
        pub class Box {
          n: Int
          fn initialize(n: Int) {
            self.n = n
          }
        }
        AV
      File.write(File.join(dir, "main.av"), <<-AV)
        import one
        import two
        fn main {
          puts(1)
        }
        AV
      ex = expect_raises(Avant::CompileError) do
        Avant.compile_file(File.join(dir, "main.av"))
      end
      ex.message.should match(/already defined/)
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "merges imported pub overloads" do
    dir = File.tempname("avant-s12")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "show_int.av"), <<-AV)
        pub fn show(n: Int): Int {
          n
        }
        AV
      File.write(File.join(dir, "show_str.av"), <<-AV)
        pub fn show(s: String): Int {
          s.size
        }
        AV
      File.write(File.join(dir, "main.av"), <<-AV)
        import show_int
        import show_str
        fn main {
          puts(show(7))
          puts(show("ab"))
        }
        AV
      run_av(File.join(dir, "main.av")).should eq "7\n2\n"
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "resolves a clash by qualifying" do
    dir = File.tempname("avant-s12")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "alpha.av"), <<-AV)
        pub fn value: Int { 1 }
        AV
      File.write(File.join(dir, "beta.av"), <<-AV)
        pub class value {
          n: Int
          fn initialize(n: Int) {
            self.n = n
          }
        }
        AV
      File.write(File.join(dir, "main.av"), <<-AV)
        import alpha
        import beta
        fn main {
          puts(alpha.value())
          v = beta.value.new(2)
          puts(v.n)
        }
        AV
      run_av(File.join(dir, "main.av")).should eq "1\n2\n"
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "sees D20 methods from another file in the import closure" do
    dir = File.tempname("avant-s12")
    Dir.mkdir(dir)
    begin
      File.write(File.join(dir, "vec.av"), <<-AV)
        pub struct Vec2 {
          x: Int
          y: Int
        }
        AV
      File.write(File.join(dir, "ext.av"), <<-AV)
        import vec
        pub fn (v: Vec2) doubled: Vec2 {
          Vec2 { x: v.x * 2, y: v.y * 2 }
        }
        AV
      File.write(File.join(dir, "main.av"), <<-AV)
        import vec
        import ext
        fn main {
          v = Vec2 { x: 1, y: 3 }
          w = v.doubled()
          puts(w.x)
          puts(w.y)
        }
        AV
      run_av(File.join(dir, "main.av")).should eq "2\n6\n"
    ensure
      Dir.children(dir).each { |name| File.delete(File.join(dir, name)) }
      Dir.delete(dir)
    end
  end

  it "keeps a single file with no import legal" do
    run_src(<<-AV).should eq "1\n"
      fn main {
        puts(1)
      }
      AV
  end
end
