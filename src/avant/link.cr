module Avant
  class LinkJob
    getter objects : Array(String)
    getter flags : Array(String)

    def initialize(@objects = [] of String, @flags = [] of String)
    end

    def empty? : Bool
      @objects.empty? && @flags.empty?
    end

    def self.parse(args : Array(String)) : LinkJob
      objects = [] of String
      flags = [] of String
      i = 0
      while i < args.size
        arg = args[i]
        case
        when arg == "--lib"
          name = args[i + 1]? || raise CompileError.at(Location.new("<argv>", 1, 1, 0), "--lib needs a name")
          flags << "-l#{name}"
          i += 2
        when arg.starts_with?("--lib=")
          flags << "-l#{arg[6..]}"
          i += 1
        when arg == "--cc"
          path = args[i + 1]? || raise CompileError.at(Location.new("<argv>", 1, 1, 0), "--cc needs a .c file")
          objects << compile_c(path)
          i += 2
        when arg.starts_with?("--cc=")
          objects << compile_c(arg[5..])
          i += 1
        when arg.starts_with?("-l") && arg.size > 2
          flags << arg
          i += 1
        when arg.ends_with?(".c")
          objects << compile_c(arg)
          i += 1
        when arg.ends_with?(".o")
          objects << arg
          i += 1
        else
          raise CompileError.at(Location.new(arg, 1, 1, 0), "unknown link argument #{arg}")
        end
      end
      new(objects, flags)
    end

    def self.compile_c(path : String, output : String? = nil) : String
      unless File.exists?(path)
        raise CompileError.at(Location.new(path, 1, 1, 0), "C source is missing")
      end
      obj = output || "#{File.dirname(path)}/#{File.basename(path, ".c")}.o"
      cc = ENV["CC"]? || "cc"
      captured = IO::Memory.new
      status = Process.run(
        cc,
        ["-c", "-O2", "-std=c11", "-o", obj, path],
        output: captured,
        error: captured
      )
      unless status.success?
        raise CompileError.at(Location.new(path, 1, 1, 0), "failed to compile C:\n#{captured}")
      end
      obj
    end
  end
end
