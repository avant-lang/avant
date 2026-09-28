module Avant
  ROOT = File.expand_path("../..", __DIR__)

  def self.compile_file(path : String) : String
    compile(Source.read(path))
  end

  def self.compile_files(paths : Array(String)) : String
    raise "compile_files needs at least one .av file" if paths.empty?
    return compile_file(paths[0]) if paths.size == 1
    text = String.build do |io|
      paths.each_with_index do |path, i|
        io << "\n" if i > 0
        io << "// file: " << path << '\n'
        body = File.read(path)
        io << body
        io << '\n' unless body.ends_with?('\n')
      end
    end
    compile(Source.new(paths[-1], text))
  end

  def self.compile(source : Source) : String
    tokens = Lexer.new(source).tokenize
    program = Parser.new(source, tokens).parse
    Checker.new(source, program).check
    Codegen::Myc.new(program).emit
  end

  def self.compile(text : String, path : String = "<input>") : String
    compile(Source.new(path, text))
  end

  def self.myc_llvm_path : String
    if path = ENV["AVANT_MYC_LLVM"]?
      return path
    end

    sibling = File.expand_path("../myc/myc-llvm", ROOT)
    return sibling if File.exists?(sibling) && File::Info.executable?(sibling)

    "myc-llvm"
  end

    def self.run_ir(ir : String, output : IO = STDOUT, error : IO = STDERR, extra_objects : Array(String) = [] of String, linker_flags : Array(String) = [] of String) : Process::Status
      flags = Runtime.linker_flags + linker_flags
      args = ["r"]
      Runtime.ensure_objects.each { |obj| args << obj }
      extra_objects.each { |obj| args << obj }
      env = {"MYC_LINKER_FLAGS" => flags.join(" ")}
      Process.run(
        myc_llvm_path,
        args,
        env: env,
        input: IO::Memory.new(ir),
        output: output,
        error: error
      )
    end

    def self.compile_ir(ir : String, out_path : String, output : IO = STDOUT, error : IO = STDERR, extra_objects : Array(String) = [] of String, linker_flags : Array(String) = [] of String) : Process::Status
      flags = Runtime.linker_flags + linker_flags
      args = ["c"]
      Runtime.ensure_objects.each { |obj| args << obj }
      extra_objects.each { |obj| args << obj }
      args << out_path
      env = {"MYC_LINKER_FLAGS" => flags.join(" ")}
      Process.run(
        myc_llvm_path,
        args,
        env: env,
        input: IO::Memory.new(ir),
        output: output,
        error: error
      )
    end
end
