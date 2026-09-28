module Avant
  class Source
    getter path : String
    getter text : String

    def initialize(@path, @text)
    end

    def self.read(path : String) : Source
      new(path, File.read(path))
    end
  end
end
