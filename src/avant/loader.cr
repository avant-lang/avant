module Avant
  def self.module_name_of(path : String) : String
    base = File.basename(path)
    if base.ends_with?(".av") && base.size > 3
      base[0..-4]
    else
      base
    end
  end

  def self.tag_module(program : AST::Program, name : String, path : String) : Nil
    program.module_name = name
    program.module_path = path
    program.structs.each do |s|
      s.module_name = name
      s.methods.each { |fn| fn.module_name = name }
    end
    program.classes.each do |c|
      c.module_name = name
      c.methods.each { |fn| fn.module_name = name }
    end
    program.functions.each { |fn| fn.module_name = name }
    program.libs.each { |lib_def| lib_def.module_name = name }
    program.quotes.each do |q|
      q.module_name = name
      q.functions.each { |fn| fn.module_name = name }
    end
    program.comptimes.each do |c|
      c.module_name = name
      c.quote.module_name = name
      c.quote.functions.each { |fn| fn.module_name = name }
    end
    program.structs.each do |s|
      s.quotes.each do |q|
        q.module_name = name
        q.functions.each { |fn| fn.module_name = name }
      end
      s.comptimes.each do |c|
        c.module_name = name
        c.quote.module_name = name
        c.quote.functions.each { |fn| fn.module_name = name }
      end
    end
    program.classes.each do |cl|
      cl.quotes.each do |q|
        q.module_name = name
        q.functions.each { |fn| fn.module_name = name }
      end
      cl.comptimes.each do |c|
        c.module_name = name
        c.quote.module_name = name
        c.quote.functions.each { |fn| fn.module_name = name }
      end
    end
  end

  class Loader
    def initialize
      @loaded = {} of String => AST::Program
      @stack = [] of String
      @order = [] of String
      @graph = {} of String => Array(String)
    end

    def self.load(path : String) : AST::Program
      new.load_root(path)
    end

    def load_root(path : String) : AST::Program
      root = File.expand_path(path)
      unless File.exists?(root)
        raise CompileError.at(Location.new(path, 1, 1, 0), "cannot read #{path}")
      end
      load_file(root)
      merge(root)
    end

    private def load_file(path : String) : AST::Program
      if @stack.includes?(path)
        cycle = (@stack + [path]).map { |p| Avant.module_name_of(p) }.join(" -> ")
        raise CompileError.at(Location.new(path, 1, 1, 0), "import cycle: #{cycle}")
      end
      if existing = @loaded[path]?
        return existing
      end
      @stack << path
      source = Source.read(path)
      tokens = Lexer.new(source).tokenize
      program = Parser.new(source, tokens).parse
      name = Avant.module_name_of(path)
      Avant.tag_module(program, name, path)
      @loaded[path] = program
      imports = [] of String
      seen = Set(String).new
      program.imports.each do |imp|
        if seen.includes?(imp.name)
          raise CompileError.at(imp.location, "already imported #{imp.name}")
        end
        seen << imp.name
        child = File.expand_path("#{imp.name}.av", File.dirname(path))
        unless File.exists?(child)
          raise CompileError.at(imp.location, "cannot find #{imp.name}.av next to #{path}")
        end
        load_file(child)
        imports << Avant.module_name_of(child)
      end
      @graph[name] = imports
      @order << path
      @stack.pop
      program
    end

    private def merge(root_path : String) : AST::Program
      root = @loaded[root_path]
      structs = [] of AST::StructDef
      classes = [] of AST::ClassDef
      functions = [] of AST::Function
      libs = [] of AST::LibDef
      quotes = [] of AST::QuoteDecl
      comptimes = [] of AST::ComptimeWalk
      @order.each do |path|
        p = @loaded[path]
        structs.concat(p.structs)
        classes.concat(p.classes)
        functions.concat(p.functions)
        libs.concat(p.libs)
        quotes.concat(p.quotes)
        comptimes.concat(p.comptimes)
      end
      merged = AST::Program.new(root.location, structs, functions, classes, libs, root.imports, quotes, comptimes)
      merged.module_name = root.module_name
      merged.module_path = root.module_path
      merged.root_module = root.module_name
      merged.import_graph = @graph
      merged
    end
  end
end
