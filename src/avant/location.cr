module Avant
  struct Location
    getter path : String
    getter line : Int32
    getter column : Int32
    getter offset : Int32

    def initialize(@path, @line, @column, @offset)
    end

    def to_s(io : IO) : Nil
      io << @path << ':' << @line << ':' << @column
    end
  end
end
