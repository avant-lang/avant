module Avant
  module Bind
    DIR = File.expand_path("../../tools", __DIR__)
    SOURCE = File.join(DIR, "bind.c")
    BIN = File.expand_path("../../bin/avant-bind", __DIR__)

    def self.generate(header : String, lib_name : String) : String
      unless File.exists?(header)
        raise CompileError.at(Location.new(header, 1, 1, 0), "header is missing")
      end
      ensure_bin
      output = IO::Memory.new
      error = IO::Memory.new
      status = Process.run(BIN, [lib_name, header], output: output, error: error)
      unless status.success?
        msg = error.to_s.strip
        msg = "avant-bind failed (#{status.exit_code})" if msg.empty?
        raise CompileError.at(Location.new(header, 1, 1, 0), msg)
      end
      output.to_s
    end

    def self.ensure_bin : String
      unless File.exists?(SOURCE)
        raise CompileError.at(Location.new(SOURCE, 1, 1, 0), "bind source is missing")
      end
      compile_bin if stale?
      BIN
    end

    private def self.stale? : Bool
      return true unless File.exists?(BIN)
      File.info(SOURCE).modification_time > File.info(BIN).modification_time
    end

    private def self.compile_bin : Nil
      Dir.mkdir_p(File.dirname(BIN))
      llvm_config = ENV["LLVM_CONFIG"]? || "llvm-config"
      cflags = capture(llvm_config, ["--cflags"]).split
      libdir = capture(llvm_config, ["--libdir"]).strip
      cc = ENV["CC"]? || "cc"
      args = ["-O2", "-std=c11"] + cflags + [SOURCE, "-o", BIN, "-L#{libdir}", "-lclang", "-Wl,-rpath,#{libdir}"]
      captured = IO::Memory.new
      status = Process.run(cc, args, output: captured, error: captured)
      unless status.success?
        raise CompileError.at(Location.new(SOURCE, 1, 1, 0), "failed to build avant-bind:\n#{captured}")
      end
    end

    private def self.capture(command : String, args : Array(String)) : String
      output = IO::Memory.new
      error = IO::Memory.new
      status = Process.run(command, args, output: output, error: error)
      unless status.success?
        raise CompileError.at(Location.new(command, 1, 1, 0), "#{command} #{args.join(" ")} failed:\n#{error}")
      end
      output.to_s
    end
  end
end
