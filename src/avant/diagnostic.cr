module Avant
  struct Diagnostic
    getter location : Location
    getter message : String

    def initialize(@location, @message)
    end

    def to_s(io : IO) : Nil
      io << @location << ": " << @message
    end
  end

  class CompileError < Exception
    getter diagnostics : Array(Diagnostic)

    def initialize(@diagnostics : Array(Diagnostic))
      super(@diagnostics.map(&.to_s).join('\n'))
    end

    def self.at(location : Location, message : String)
      new([Diagnostic.new(location, message)])
    end
  end
end
