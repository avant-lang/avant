module Avant
  ROOT = File.expand_path("../..", __DIR__)

  def self.compile_file(path : String) : String
    source = Source.read(path)
    compile_program(Loader.load(path), source)
  end

  def self.compile_files(paths : Array(String)) : String
    raise "compile_files needs at least one .av file" if paths.empty?
    if paths.size == 1
      return compile_file(paths[0])
    end
    raise CompileError.at(Location.new(paths[0], 1, 1, 0), "compile takes one root .av (use import)")
  end

  def self.compile(source : Source) : String
    tokens = Lexer.new(source).tokenize
    program = Parser.new(source, tokens).parse
    unless program.imports.empty?
      if File.exists?(source.path)
        return compile_file(source.path)
      end
      raise CompileError.at(program.imports[0].location, "import needs a .av file root")
    end
    name = module_name_of(source.path)
    tag_module(program, name, source.path)
    program.root_module = name
    program.import_graph = {name => [] of String} of String => Array(String)
    compile_program(program, source)
  end

  def self.compile_program(program : AST::Program, source : Source) : String
    Expander.new(source, program).expand
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

    shard = File.expand_path("../safepoints/myc-llvm", ROOT)
    return shard if File.exists?(shard) && File::Info.executable?(shard)

    sibling = File.expand_path("../myc/myc-llvm", ROOT)
    return sibling if File.exists?(sibling) && File::Info.executable?(sibling)

    "myc-llvm"
  end

    def self.myc_gc_safepoint_args : Array(String)
      args = [] of String
      args << "--final" if ENV["AVANT_MYC_FINAL"]? == "1"
      args.concat([
        "--gc-root=avant_gc_root",
        "--gc-reload=avant_gc_reload",
        "--gc-enter=avant_gc_enter",
        "--gc-leave=avant_gc_leave",
        "--gc-leaf=avant_type_map,avant_barrier,avant_cov_hit,avant_cov_init,avant_pin,avant_is_heap,avant_array_get_ptr,avant_array_set_ptr,avant_array_pop_ptr",
      ])
      args
    end

    def self.run_ir(ir : String, output : IO = STDOUT, error : IO = STDERR, extra_objects : Array(String) = [] of String, linker_flags : Array(String) = [] of String) : Process::Status
      flags = Runtime.linker_flags + linker_flags
      args = ["r"]
      myc_gc_safepoint_args.each { |a| args << a }
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
      myc_gc_safepoint_args.each { |a| args << a }
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
