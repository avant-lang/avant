module Avant
  class CLI
    def initialize(@argv : Array(String))
    end

    def run : Int32
      if @argv.empty? || @argv[0] == "-h" || @argv[0] == "--help" || @argv[0] == "help"
        usage(STDOUT)
        return 0
      end

      command = @argv[0]
      args = @argv[1..]

      begin
        case command
        when "dump", "d"
          paths, _rest = take_av_files(args)
          abort_usage("dump needs a .av file") if paths.empty?
          STDOUT.print Avant.compile_files(paths)
        when "run", "r"
          paths, rest = take_av_files(args)
          abort_usage("run needs a .av file") if paths.empty?
          link = LinkJob.parse(rest)
          run_ir(Avant.compile_files(paths), paths[-1], link)
        when "compile", "c"
          paths, rest = take_av_files(args)
          abort_usage("compile needs a .av file") if paths.empty?
          out_path, link_args = take_out_path(rest, paths[-1])
          link = LinkJob.parse(link_args)
          compile_ir(Avant.compile_files(paths), paths[-1], out_path, link)
        when "bind", "b"
          run_bind(args)
        else
          if command.ends_with?(".av")
            paths, rest = take_av_files(@argv)
            link = LinkJob.parse(rest)
            run_ir(Avant.compile_files(paths), paths[-1], link)
          else
            abort_usage("unknown command #{command}")
          end
        end
        0
      rescue ex : CompileError
        STDERR.puts ex.message
        1
      end
    end

    private def run_bind(args : Array(String)) : Nil
      lib_name = nil.as(String?)
      header = nil.as(String?)
      i = 0
      while i < args.size
        arg = args[i]
        case
        when arg == "--lib"
          lib_name = args[i + 1]? || abort_usage("--lib needs a name")
          i += 2
        when arg.starts_with?("--lib=")
          lib_name = arg[6..]
          i += 1
        when arg.starts_with?("-")
          abort_usage("unknown bind argument #{arg}")
        else
          abort_usage("bind takes one header") if header
          header = arg
          i += 1
        end
      end
      header = header || abort_usage("bind needs a C header")
      lib_name ||= File.basename(header, ".h")
      STDOUT.print Bind.generate(header, lib_name)
    end

    private def take_av_files(args : Array(String)) : {Array(String), Array(String)}
      paths = [] of String
      i = 0
      while i < args.size && args[i].ends_with?(".av")
        paths << args[i]
        i += 1
      end
      {paths, args[i..]}
    end

    private def take_out_path(args : Array(String), source : String) : {String, Array(String)}
      if args.empty?
        return {File.basename(source, ".av"), args}
      end
      first = args[0]
      if first.starts_with?("-") || first.ends_with?(".c") || first.ends_with?(".o")
        return {File.basename(source, ".av"), args}
      end
      {first, args[1..]}
    end

    private def run_ir(ir : String, path : String, link : LinkJob) : Nil
      status = Avant.run_ir(ir, extra_objects: link.objects, linker_flags: link.flags)
      unless status.success?
        raise CompileError.at(Location.new(path, 1, 1, 0), "myc-llvm failed (#{status.exit_code})")
      end
    end

    private def compile_ir(ir : String, path : String, out_path : String, link : LinkJob) : Nil
      status = Avant.compile_ir(ir, out_path, extra_objects: link.objects, linker_flags: link.flags)
      unless status.success?
        raise CompileError.at(Location.new(path, 1, 1, 0), "myc-llvm failed (#{status.exit_code})")
      end
    end

    private def abort_usage(message : String) : NoReturn
      STDERR.puts message
      usage(STDERR)
      exit(2)
    end

    private def usage(io : IO) : Nil
      io.puts <<-USAGE
      Avant compiler (Stage 7)

        avant run FILE.av [FILE.av ...] [--lib NAME] [--cc FILE.c]
        avant dump FILE.av [FILE.av ...]
        avant compile FILE.av [FILE.av ...] [OUT] [--lib NAME] [--cc FILE.c]
        avant bind [--lib NAME] HEADER.h

      Stage 7: LangArena ports. Heap is GenImmix (D34).
      Extra .av files are concatenated (shared helper + task).
      Entry: fn main, or fn run (a myc main is synthesized).
      USAGE
    end
  end
end
