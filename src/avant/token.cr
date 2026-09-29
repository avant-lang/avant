module Avant
  struct Token
    enum Kind
      Eof
      Newline
      Ident
      Integer
      Float
      String
      Fn
      If
      Else
      Return
      Struct
      Class
      Lib
      Fun
      SelfKw
      While
      True
      False
      NilKw
      Spawn
      BreakKw
      ContinueKw
      Switch
      Case
      PlusEq
      MinusEq
      StarEq
      SlashEq
      LBrace
      RBrace
      LParen
      RParen
      LBracket
      RBracket
      Comma
      Colon
      Dot
      Plus
      Minus
      Star
      Slash
      Percent
      Eq
      EqEq
      NotEq
      Less
      LessEq
      Greater
      GreaterEq
      Bang
      Question
      Pipe
      PipePipe
      Amp
      AmpAmp
      Caret
      Tilde
      LessLess
      GreaterGreater
      InterpOpen
      InterpMid
      InterpClose
      Import
      Pub
      Quote
      Comptime
      Pound
    end

    getter kind : Kind
    getter location : Location
    getter value : String

    def initialize(@kind, @location, @value = "")
    end

    def keyword? : Bool
      kind.fn? || kind.if? || kind.else? || kind.return? || kind.struct? || kind.class? || kind.lib? || kind.fun? || kind.self_kw? || kind.while? || kind.true? || kind.false? || kind.nil_kw? || kind.spawn? || kind.break_kw? || kind.continue_kw? || kind.switch? || kind.case? || kind.import? || kind.pub? || kind.quote? || kind.comptime?
    end

    def assign_op? : Bool
      kind.eq? || kind.plus_eq? || kind.minus_eq? || kind.star_eq? || kind.slash_eq?
    end
  end
end
