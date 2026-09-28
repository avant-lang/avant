module Avant
  class Lexer
    KEYWORDS = {
      "fn"     => Token::Kind::Fn,
      "if"     => Token::Kind::If,
      "else"   => Token::Kind::Else,
      "return" => Token::Kind::Return,
      "struct" => Token::Kind::Struct,
      "class"  => Token::Kind::Class,
      "lib"    => Token::Kind::Lib,
      "fun"    => Token::Kind::Fun,
      "self"   => Token::Kind::SelfKw,
      "while"  => Token::Kind::While,
      "true"   => Token::Kind::True,
      "false"  => Token::Kind::False,
      "nil"    => Token::Kind::NilKw,
      "spawn"    => Token::Kind::Spawn,
      "break"    => Token::Kind::BreakKw,
      "continue" => Token::Kind::ContinueKw,
      "switch"   => Token::Kind::Switch,
      "case"     => Token::Kind::Case,
    }

    def initialize(@source : Source)
      @bytes = @source.text
      @file_path = @source.path
      @pos = 0
      @line = 1
      @column = 1
      @queue = [] of Token
      @intern = {} of String => String
    end

    def tokenize : Array(Token)
      tokens = [] of Token

      loop do
        skip_spaces
        break if at_end? && @queue.empty?

        unless @queue.empty?
          tokens << @queue.shift
          next
        end

        if peek == '/' && peek_next == '/'
          skip_line_comment
          next
        end

        if peek == '\n'
          loc = here
          bump
          tokens << Token.new(:newline, loc) unless tokens.last?.try(&.kind.newline?)
          next
        end

        tokens << next_token
      end

      tokens << Token.new(:eof, here)
      tokens
    end

    private def next_token : Token
      return @queue.shift unless @queue.empty?
      read_token
    end

    private def read_token : Token
      loc = here
      c = peek

      case c
      when '{' then bump; Token.new(:l_brace, loc)
      when '}' then bump; Token.new(:r_brace, loc)
      when '(' then bump; Token.new(:l_paren, loc)
      when ')' then bump; Token.new(:r_paren, loc)
      when '[' then bump; Token.new(:l_bracket, loc)
      when ']' then bump; Token.new(:r_bracket, loc)
      when ',' then bump; Token.new(:comma, loc)
      when ':' then bump; Token.new(:colon, loc)
      when '|'
        bump
        if peek == '|'
          bump
          Token.new(:pipe_pipe, loc)
        else
          Token.new(:pipe, loc)
        end
      when '&'
        bump
        if peek == '&'
          bump
          Token.new(:amp_amp, loc)
        else
          Token.new(:amp, loc)
        end
      when '^' then bump; Token.new(:caret, loc)
      when '~' then bump; Token.new(:tilde, loc)
      when '?' then bump; Token.new(:question, loc)
      when '.' then bump; Token.new(:dot, loc)
      when '+'
        bump
        if peek == '='
          bump
          Token.new(:plus_eq, loc)
        else
          Token.new(:plus, loc)
        end
      when '-'
        bump
        if peek == '='
          bump
          Token.new(:minus_eq, loc)
        else
          Token.new(:minus, loc)
        end
      when '*'
        bump
        if peek == '='
          bump
          Token.new(:star_eq, loc)
        else
          Token.new(:star, loc)
        end
      when '%' then bump; Token.new(:percent, loc)
      when '/'
        bump
        if peek == '='
          bump
          Token.new(:slash_eq, loc)
        else
          Token.new(:slash, loc)
        end
      when '='
        bump
        if peek == '='
          bump
          Token.new(:eq_eq, loc)
        else
          Token.new(:eq, loc)
        end
      when '!'
        bump
        if peek == '='
          bump
          Token.new(:not_eq, loc)
        else
          Token.new(:bang, loc)
        end
      when '<'
        bump
        if peek == '='
          bump
          Token.new(:less_eq, loc)
        elsif peek == '<'
          bump
          Token.new(:less_less, loc)
        else
          Token.new(:less, loc)
        end
      when '>'
        bump
        if peek == '='
          bump
          Token.new(:greater_eq, loc)
        elsif peek == '>'
          bump
          Token.new(:greater_greater, loc)
        else
          Token.new(:greater, loc)
        end
      when '"'
        read_string_token(loc)
      when '0'..'9'
        read_number(loc)
      when 'a'..'z', 'A'..'Z', '_'
        ident = read_ident
        if kind = KEYWORDS[ident]?
          Token.new(kind, loc)
        else
          Token.new(:ident, loc, intern(ident))
        end
      else
        bump
        raise CompileError.at(loc, "unexpected character #{c.inspect}")
      end
    end

    private def read_number(loc : Location) : Token
      start = @pos
      hex = false
      if peek == '0' && (peek_next == 'x' || peek_next == 'X')
        bump
        bump
        hex = true
        unless hex_digit?(peek)
          raise CompileError.at(here, "hex literal needs a digit after 0x")
        end
        while hex_digit?(peek)
          bump
        end
      else
        while peek.ascii_number?
          bump
        end
        is_float = false
        if peek == '.' && peek_next.ascii_number?
          is_float = true
          bump
          while peek.ascii_number?
            bump
          end
        end
        if exponent_ahead?
          is_float = true
          bump
          if peek == '+' || peek == '-'
            bump
          end
          while peek.ascii_number?
            bump
          end
        end
        if is_float
          return Token.new(:float, loc, intern(@bytes[start, @pos - start]))
        end
      end
      text = @bytes[start, @pos - start]
      suffix = read_int_suffix
      Token.new(:integer, loc, intern(suffix.empty? ? text : text + suffix))
    end

    private def read_int_suffix : String
      if peek == 'u' && peek_next == '6' && peek_at(2) == '4' && !ident_continue?(peek_at(3))
        bump
        bump
        bump
        return "u64"
      end
      if peek == 'i' && peek_next == '6' && peek_at(2) == '4' && !ident_continue?(peek_at(3))
        bump
        bump
        bump
        return "i64"
      end
      ""
    end

    private def hex_digit?(c : Char) : Bool
      c.ascii_number? || ('a'..'f').includes?(c) || ('A'..'F').includes?(c)
    end

    private def intern(s : String) : String
      if existing = @intern[s]?
        return existing
      end
      @intern[s] = s
    end

    private def exponent_ahead? : Bool
      return false unless peek == 'e' || peek == 'E'
      c = peek_next
      return true if c.ascii_number?
      if c == '+' || c == '-'
        return peek_at(2).ascii_number?
      end
      false
    end

    private def read_ident : String
      start = @pos
      while ident_continue?(peek)
        bump
      end
      @bytes[start, @pos - start]
    end

    private def read_string_token(loc : Location) : Token
      bump
      frag, kind = read_string_body
      case kind
      when :end
        Token.new(:string, loc, intern(frag))
      when :more
        rest = lex_interpolation(frag, loc)
        first = Token.new(:interp_open, loc, intern(frag))
        rest.each { |tok| @queue << tok }
        first
      else
        raise CompileError.at(loc, "unterminated string")
      end
    end

    private def lex_interpolation(first_frag : String, loc : Location) : Array(Token)
      tokens = [] of Token
      loop do
        expr = lex_interp_expr
        after = read_string_fragment
        case after[1]
        when :end
          tokens.concat(expr)
          tokens << Token.new(:interp_close, here, after[0])
          return tokens
        when :more
          tokens.concat(expr)
          tokens << Token.new(:interp_mid, here, after[0])
        end
      end
    end

    private def lex_interp_expr : Array(Token)
      tokens = [] of Token
      depth = 0
      loop do
        skip_spaces
        if at_end?
          raise CompileError.at(here, "unterminated string interpolation")
        end
        if peek == '\n'
          loc = here
          bump
          tokens << Token.new(:newline, loc) unless tokens.last?.try(&.kind.newline?)
          next
        end
        if peek == '/' && peek_next == '/'
          skip_line_comment
          next
        end
        tok = read_token
        if tok.kind.l_brace?
          depth += 1
          tokens << tok
        elsif tok.kind.r_brace?
          if depth == 0
            return tokens
          end
          depth -= 1
          tokens << tok
        elsif tok.kind.interp_open?
          raise CompileError.at(tok.location, "nested string interpolation is not in Stage 6")
        else
          tokens << tok
        end
      end
    end

    # Returns {fragment, :end} if the string closed, {fragment, :more} if another ${ follows.
    private def read_string_fragment : {String, Symbol}
      read_string_body
    end

    private def read_string_body : {String, Symbol}
      frag = String.build do |io|
        loop do
          if at_end?
            raise CompileError.at(here, "unterminated string")
          end
          c = peek
          case c
          when '"'
            bump
            break
          when '\n'
            raise CompileError.at(here, "unterminated string")
          when '\\'
            bump
            io << escape
          when '$'
            if peek_next == '{'
              bump
              bump
              return {intern(io.to_s), :more}
            end
            bump
            io << c
          else
            bump
            io << c
          end
        end
      end
      {intern(frag), :end}
    end

    private def escape : Char
      if at_end?
        raise CompileError.at(here, "unterminated string escape")
      end
      c = peek
      bump
      case c
      when 'n'  then '\n'
      when 't'  then '\t'
      when 'r'  then '\r'
      when '\\' then '\\'
      when '"'  then '"'
      when '$'  then '$'
      else
        raise CompileError.at(here, "unknown string escape \\#{c}")
      end
    end

    private def ident_start?(c : Char) : Bool
      c.ascii_letter? || c == '_'
    end

    private def ident_continue?(c : Char) : Bool
      ident_start?(c) || c.ascii_number?
    end

    private def skip_spaces : Nil
      while peek == ' ' || peek == '\t' || peek == '\r'
        bump
      end
    end

    private def skip_line_comment : Nil
      bump if peek == '/'
      bump if peek == '/'
      if peek == ' ' && peek_at(1) == 'f' && peek_at(2) == 'i' && peek_at(3) == 'l' &&
         peek_at(4) == 'e' && peek_at(5) == ':' && peek_at(6) == ' '
        7.times { bump }
        start = @pos
        until at_end? || peek == '\n'
          bump
        end
        @file_path = @bytes[start...@pos]
        @line = 0
        @column = 1
        return
      end
      until at_end? || peek == '\n'
        bump
      end
    end

    private def here : Location
      Location.new(@file_path, @line, @column, @pos)
    end

    private def at_end? : Bool
      @pos >= @bytes.size
    end

    private def peek : Char
      return '\0' if at_end?
      @bytes[@pos]
    end

    private def peek_next : Char
      peek_at(1)
    end

    private def peek_at(n : Int32) : Char
      return '\0' if @pos + n >= @bytes.size
      @bytes[@pos + n]
    end

    private def bump : Char
      c = peek
      return c if at_end?
      @pos += 1
      if c == '\n'
        @line += 1
        @column = 1
      else
        @column += 1
      end
      c
    end
  end
end
