module Avant
  class Parser
    def initialize(@source : Source, @tokens : Array(Token))
      @index = 0
      @allow_struct_lit = true
    end

    def parse : AST::Program
      skip_newlines
      location = current.location
      structs = [] of AST::StructDef
      classes = [] of AST::ClassDef
      functions = [] of AST::Function
      libs = [] of AST::LibDef
      imports = [] of AST::ImportDecl

      until current.kind.eof?
        vis = false
        if current.kind.pub?
          vis = true
          bump
          skip_newlines
        end
        case current.kind
        when .import?
          if vis
            raise CompileError.at(current.location, "import cannot be pub")
          end
          imports << parse_import
        when .struct?
          structs << parse_struct(vis)
        when .class?
          classes << parse_class(vis)
        when .lib?
          libs << parse_lib(vis)
        when .fn?
          functions << parse_function(nil, vis)
        when .fun?
          raise CompileError.at(current.location, "fun belongs in a lib block; there is no extern fn")
        else
          if vis
            raise CompileError.at(current.location, "pub prefixes struct, class, lib, or fn")
          end
          raise CompileError.at(current.location, "expected struct, class, lib, fn, or import, found #{current.kind}")
        end
        skip_newlines
      end

      AST::Program.new(location, structs, functions, classes, libs, imports)
    end

    private def parse_import : AST::ImportDecl
      loc = expect(:import).location
      skip_newlines
      name = expect(:ident)
      AST::ImportDecl.new(loc, name.value)
    end

    private def parse_struct(vis : Bool) : AST::StructDef
      loc = expect(:struct).location
      name = expect(:ident).value
      fields, methods = parse_type_body(name)
      if field = fields.find(&.default)
        raise CompileError.at(field.location, "struct fields have no defaults; use a literal")
      end
      AST::StructDef.new(loc, name, fields, methods, vis)
    end

    private def parse_class(vis : Bool) : AST::ClassDef
      loc = expect(:class).location
      name = expect(:ident).value
      fields, methods = parse_type_body(name)
      AST::ClassDef.new(loc, name, fields, methods, vis)
    end

    private def parse_lib(vis : Bool) : AST::LibDef
      loc = expect(:lib).location
      name = expect(:ident).value
      skip_newlines
      expect(:l_brace)
      skip_newlines
      funs = [] of AST::FunDecl
      until current.kind.r_brace? || current.kind.eof?
        if current.kind.fn?
          raise CompileError.at(current.location, "inside lib, write fun (the C ABI), not fn")
        end
        funs << parse_fun_decl
        skip_newlines
      end
      expect(:r_brace)
      AST::LibDef.new(loc, name, funs, vis)
    end

    private def parse_fun_decl : AST::FunDecl
      loc = expect(:fun).location
      name_tok = expect(:ident)
      params = [] of AST::Param
      if current.kind.l_paren?
        bump
        skip_newlines
        unless current.kind.r_paren?
          loop do
            params << parse_param
            skip_newlines
            break unless match?(:comma)
            skip_newlines
          end
        end
        expect(:r_paren)
      end

      return_type = nil
      if match?(:colon)
        skip_newlines
        return_type = parse_type_name
      end

      skip_newlines
      if current.kind.l_brace?
        raise CompileError.at(current.location, "fun is a C declaration; it has no body")
      end

      params.each do |p|
        if p.default
          raise CompileError.at(p.location, "fun parameters cannot have defaults")
        end
      end

      AST::FunDecl.new(loc, name_tok.value, params, return_type)
    end

    private def parse_type_body(owner : String) : {Array(AST::Field), Array(AST::Function)}
      skip_newlines
      expect(:l_brace)
      skip_newlines
      fields = [] of AST::Field
      methods = [] of AST::Function
      until current.kind.r_brace? || current.kind.eof?
        vis = false
        if current.kind.pub?
          vis = true
          bump
          skip_newlines
          unless current.kind.fn?
            raise CompileError.at(current.location, "pub in a type body only prefixes fn")
          end
        end
        if current.kind.fn?
          methods << parse_function(owner, vis)
        else
          if vis
            raise CompileError.at(current.location, "fields of a pub type are visible; do not mark a field pub")
          end
          fields << parse_field
        end
        skip_newlines
      end
      expect(:r_brace)
      {fields, methods}
    end

    private def parse_field : AST::Field
      name = expect(:ident)
      expect(:colon)
      skip_newlines
      type = parse_type_name
      default = nil.as(AST::Expr?)
      if match?(:eq)
        skip_newlines
        default = parse_expr
      end
      AST::Field.new(name.location, name.value, type, default)
    end

    private def parse_fn_name : String
      tok = current
      if tok.kind.ident?
        return bump.value
      end
      if name = operator_method_name(tok.kind)
        bump
        return name
      end
      raise CompileError.at(tok.location, "expected function name")
    end

    private def operator_method_name(kind : Token::Kind) : String?
      case kind
      when .plus?
        "+"
      when .minus?
        "-"
      when .star?
        "*"
      when .slash?
        "/"
      when .percent?
        "%"
      when .eq_eq?
        "=="
      when .not_eq?
        "!="
      when .less?
        "<"
      when .less_eq?
        "<="
      when .greater?
        ">"
      when .greater_eq?
        ">="
      else
        nil
      end
    end

    private def parse_function(owner : String? = nil, vis : Bool = false) : AST::Function
      loc = expect(:fn).location
      receiver : AST::Param? = nil

      if owner.nil? && current.kind.l_paren?
        bump
        skip_newlines
        receiver = parse_param
        if receiver.default
          raise CompileError.at(receiver.location, "receiver cannot have a default")
        end
        skip_newlines
        expect(:r_paren)
        skip_newlines
      elsif !owner.nil? && current.kind.l_paren?
        raise CompileError.at(current.location, "inherent methods do not take a receiver; use fn name(...)")
      end

      name_loc = current.location
      name = parse_fn_name
      if name == "new" && (owner || receiver)
        raise CompileError.at(name_loc, "new is reserved; write initialize")
      end

      params = [] of AST::Param
      if current.kind.l_paren?
        bump
        skip_newlines
        unless current.kind.r_paren?
          loop do
            params << parse_param
            skip_newlines
            break unless match?(:comma)
            skip_newlines
          end
        end
        expect(:r_paren)
      end

      return_type = nil
      if match?(:colon)
        skip_newlines
        return_type = parse_type_name
      end

      skip_newlines
      body = parse_block
      AST::Function.new(loc, name, params, return_type, body, receiver, owner, vis)
    end

    private def parse_param : AST::Param
      name_tok = expect(:ident)
      expect(:colon)
      skip_newlines
      type = parse_type_name
      default = nil.as(AST::Expr?)
      if match?(:eq)
        skip_newlines
        default = parse_expr
      end
      AST::Param.new(name_tok.location, name_tok.value, type, default)
    end

    private def parse_type_name : AST::TypeName
      loc = current.location
      first = parse_nilable_type
      unless current.kind.pipe?
        return first
      end
      members = [first] of AST::TypeName
      while match?(:pipe)
        skip_newlines
        members << parse_nilable_type
      end
      AST::TypeName.union(loc, members)
    end

    private def parse_nilable_type : AST::TypeName
      tn = parse_primary_type
      while current.kind.question?
        bump
        tn = AST::TypeName.new(tn.location, tn.name, tn.args, true, tn.members, tn.qualifier)
      end
      tn
    end

    private def parse_primary_type : AST::TypeName
      tok = expect(:ident)
      qualifier = nil.as(String?)
      if current.kind.dot?
        qualifier = tok.value
        bump
        skip_newlines
        tok = expect(:ident)
      end
      args = [] of AST::TypeName
      if current.kind.l_paren?
        bump
        skip_newlines
        unless current.kind.r_paren?
          loop do
            args << parse_type_name
            skip_newlines
            break unless match?(:comma)
            skip_newlines
          end
        end
        expect(:r_paren)
      end
      AST::TypeName.new(tok.location, tok.value, args, false, [] of AST::TypeName, qualifier)
    end

    private def parse_block : Array(AST::Stmt)
      expect(:l_brace)
      skip_newlines
      stmts = [] of AST::Stmt
      until current.kind.r_brace? || current.kind.eof?
        stmts << parse_stmt
        skip_newlines
      end
      expect(:r_brace)
      stmts
    end

    private def parse_stmt : AST::Stmt
      case current.kind
      when .if?
        parse_if
      when .switch?
        expr = parse_switch
        AST::ExprStmt.new(expr.location, expr)
      when .while?
        parse_while
      when .break_kw?
        parse_break
      when .continue_kw?
        parse_continue
      when .return?
        parse_return
      when .ident?
        parse_ident_stmt
      else
        expr = parse_expr
        finish_stmt(expr)
      end
    end

    private def parse_ident_stmt : AST::Stmt
      if annotated_bind_ahead?
        loc = current.location
        name = bump
        expect(:colon)
        skip_newlines
        declared = parse_type_name
        skip_newlines
        expect(:eq)
        skip_newlines
        value = maybe_trailing_block(parse_expr)
        AST::AssignStmt.new(loc, AST::Name.new(name.location, name.value), value, declared)
      else
        expr = parse_expr
        finish_stmt(expr)
      end
    end

    private def finish_stmt(expr : AST::Expr) : AST::Stmt
      expr = maybe_trailing_block(expr)
      skip_newlines_if_continuing
      if current.assign_op?
        op = bump
        skip_newlines
        value = maybe_trailing_block(parse_expr)
        AST::AssignStmt.new(op.location, expr, value, nil, op.kind)
      else
        AST::ExprStmt.new(expr.location, expr)
      end
    end

    private def maybe_trailing_block(expr : AST::Expr) : AST::Expr
      i = @index
      while i < @tokens.size && @tokens[i].kind.newline?
        i += 1
      end
      return expr unless i < @tokens.size && @tokens[i].kind.l_brace?
      return expr unless expr.is_a?(AST::Call) || expr.is_a?(AST::FieldAccess)
      skip_newlines
      attach_block(expr, parse_trailing_block)
    end

    private def annotated_bind_ahead? : Bool
      return false unless current.kind.ident?
      i = @index + 1
      while i < @tokens.size && @tokens[i].kind.newline?
        i += 1
      end
      return false unless i < @tokens.size && @tokens[i].kind.colon?
      true
    end

    private def parse_if : AST::IfStmt
      loc = expect(:if).location
      skip_newlines
      bind = nil.as(String?)
      cond = if if_assign_ahead?
               name = bump
               skip_newlines
               expect(:eq)
               skip_newlines
               bind = name.value
               parse_cond_expr
             else
               parse_cond_expr
             end
      skip_newlines
      then_body = parse_block
      else_body = [] of AST::Stmt
      skip_newlines
      if current.kind.else?
        bump
        skip_newlines
        if current.kind.if?
          else_body << parse_if
        else
          else_body = parse_block
        end
      end
      AST::IfStmt.new(loc, cond, then_body, else_body, bind)
    end

    private def if_assign_ahead? : Bool
      return false unless current.kind.ident?
      i = @index + 1
      while i < @tokens.size && @tokens[i].kind.newline?
        i += 1
      end
      return false unless i < @tokens.size && @tokens[i].kind.eq?
      true
    end

    private def parse_while : AST::WhileStmt
      loc = expect(:while).location
      skip_newlines
      cond = parse_cond_expr
      skip_newlines
      body = parse_block
      AST::WhileStmt.new(loc, cond, body)
    end

    private def parse_switch : AST::SwitchExpr
      loc = expect(:switch).location
      skip_newlines
      cond = parse_cond_expr
      skip_newlines
      expect(:l_brace)
      skip_newlines
      cases = [] of AST::SwitchCase
      else_body = nil.as(Array(AST::Stmt)?)
      until current.kind.r_brace? || current.kind.eof?
        if current.kind.case?
          if else_body
            raise CompileError.at(current.location, "else must be the last switch arm")
          end
          cases << parse_switch_case
        elsif current.kind.else?
          if else_body
            raise CompileError.at(current.location, "duplicate else in switch")
          end
          bump
          skip_newlines
          else_body = parse_block
        else
          raise CompileError.at(current.location, "expected case or else, found #{current.kind}")
        end
        skip_newlines
      end
      expect(:r_brace)
      if cases.empty?
        raise CompileError.at(loc, "switch needs a case")
      end
      AST::SwitchExpr.new(loc, cond, cases, else_body)
    end

    private def parse_switch_case : AST::SwitchCase
      loc = expect(:case).location
      skip_newlines
      labels = [] of Int64
      loop do
        labels << parse_case_label
        skip_newlines
        break unless match?(:comma)
        skip_newlines
      end
      skip_newlines
      body = parse_block
      AST::SwitchCase.new(loc, labels, body)
    end

    private def parse_case_label : Int64
      neg = false
      if current.kind.minus?
        bump
        skip_newlines
        neg = true
      end
      unless current.kind.integer?
        raise CompileError.at(current.location, "case label must be an integer literal")
      end
      tok = bump
      bits, suffix = parse_integer(tok)
      unless suffix.empty?
        raise CompileError.at(tok.location, "case label must be an Int literal")
      end
      if neg
        if bits > 2147483648_u64
          raise CompileError.at(tok.location, "case label does not fit in Int")
        end
        -bits.to_i64
      else
        if bits > Int32::MAX.to_u64
          raise CompileError.at(tok.location, "case label does not fit in Int")
        end
        bits.to_i64
      end
    end

    # `while n { xs: Array(Int) = [] }` must not parse `n { xs: ... }` as a
    # struct literal. Required braces on if/while win; parenthesize a literal.
    private def parse_cond_expr : AST::Expr
      old = @allow_struct_lit
      @allow_struct_lit = false
      expr = parse_expr
      @allow_struct_lit = old
      expr
    end

    private def parse_break : AST::BreakStmt
      loc = expect(:break_kw).location
      AST::BreakStmt.new(loc)
    end

    private def parse_continue : AST::ContinueStmt
      loc = expect(:continue_kw).location
      AST::ContinueStmt.new(loc)
    end

    private def parse_return : AST::ReturnStmt
      loc = expect(:return).location
      expr = nil
      unless current.kind.newline? || current.kind.r_brace? || current.kind.eof?
        expr = parse_expr
      end
      AST::ReturnStmt.new(loc, expr)
    end

    private def parse_expr(min_prec = 0) : AST::Expr
      left = parse_postfix(parse_prefix)
      loop do
        skip_newlines_if_continuing
        prec = infix_prec(current.kind)
        break if prec < min_prec
        op = bump
        skip_newlines
        right = parse_expr(prec + 1)
        left = AST::Binary.new(op.location, op.kind, left, right)
      end
      left
    end

    private def parse_postfix(left : AST::Expr) : AST::Expr
      loop do
        skip_newlines_if_postfix
        case current.kind
        when .dot?
          loc = bump.location
          skip_newlines
          field = expect(:ident)
          if current.kind.l_paren?
            args = parse_arg_list
            left = AST::Call.new(loc, field.value, args, left)
          elsif field.value == "new"
            left = AST::Call.new(loc, "new", [] of AST::Expr, left)
          elsif @allow_struct_lit && struct_literal_ahead?
            qual = nil.as(String?)
            if left.is_a?(AST::Name)
              qual = left.ident
            end
            left = parse_struct_literal(field, qual)
          else
            left = AST::FieldAccess.new(loc, left, field.value)
          end
        when .l_bracket?
          loc = bump.location
          skip_newlines
          index = parse_expr
          skip_newlines
          expect(:r_bracket)
          left = AST::Index.new(loc, left, index)
        when .question?
          loc = bump.location
          left = AST::Try.new(loc, left)
        else
          break
        end
      end
      left
    end

    private def parse_prefix : AST::Expr
      skip_newlines
      tok = current
      case tok.kind
      when .integer?
        bump
        bits, suffix = parse_integer(tok)
        AST::IntegerLiteral.new(tok.location, bits, suffix)
      when .float?
        bump
        AST::FloatLiteral.new(tok.location, tok.value.to_f64)
      when .string?
        bump
        AST::StringLiteral.new(tok.location, tok.value)
      when .interp_open?
        parse_interpolated_string
      when .true?
        bump
        AST::BoolLiteral.new(tok.location, true)
      when .false?
        bump
        AST::BoolLiteral.new(tok.location, false)
      when .nil_kw?
        bump
        AST::NilLiteral.new(tok.location)
      when .spawn?
        parse_spawn(tok)
      when .switch?
        parse_switch
      when .l_bracket?
        parse_array_literal
      when .ident?
        parse_ident_expr
      when .self_kw?
        bump
        AST::Name.new(tok.location, "self")
      when .minus?
        bump
        skip_newlines
        expr = parse_expr(unary_prec)
        AST::Unary.new(tok.location, Token::Kind::Minus, expr)
      when .bang?
        bump
        skip_newlines
        expr = parse_expr(unary_prec)
        AST::Unary.new(tok.location, Token::Kind::Bang, expr)
      when .tilde?
        bump
        skip_newlines
        expr = parse_expr(unary_prec)
        AST::Unary.new(tok.location, Token::Kind::Tilde, expr)
      when .l_paren?
        bump
        skip_newlines
        old = @allow_struct_lit
        @allow_struct_lit = true
        expr = parse_expr
        @allow_struct_lit = old
        skip_newlines
        expect(:r_paren)
        expr
      else
        raise CompileError.at(tok.location, "expected expression, found #{tok.kind}")
      end
    end

    private def parse_ident_expr : AST::Expr
      tok = bump
      if tok.value == "Array" && current.kind.l_paren?
        parse_array_new(tok)
      elsif tok.value == "Hash" && current.kind.l_paren?
        parse_hash_new(tok)
      elsif current.kind.l_paren?
        parse_call(tok)
      elsif @allow_struct_lit && struct_literal_ahead?
        parse_struct_literal(tok)
      else
        AST::Name.new(tok.location, tok.value)
      end
    end

    private def parse_array_new(tok : Token) : AST::ArrayNew
      expect(:l_paren)
      skip_newlines
      elem = parse_type_name
      skip_newlines
      expect(:r_paren)
      skip_newlines
      expect(:dot)
      skip_newlines
      meth = expect(:ident)
      unless meth.value == "new"
        raise CompileError.at(meth.location, "Array(T) only supports .new")
      end
      expect(:l_paren)
      skip_newlines
      size = parse_expr
      skip_newlines
      expect(:r_paren)
      AST::ArrayNew.new(tok.location, elem, size)
    end

    private def parse_hash_new(tok : Token) : AST::HashNew
      expect(:l_paren)
      skip_newlines
      key = parse_type_name
      skip_newlines
      expect(:comma)
      skip_newlines
      val = parse_type_name
      skip_newlines
      expect(:r_paren)
      skip_newlines
      expect(:dot)
      skip_newlines
      meth = expect(:ident)
      unless meth.value == "new"
        raise CompileError.at(meth.location, "Hash(K, V) only supports .new")
      end
      if current.kind.l_paren?
        bump
        skip_newlines
        expect(:r_paren)
      end
      AST::HashNew.new(tok.location, key, val)
    end

    private def parse_array_literal : AST::ArrayLiteral
      loc = expect(:l_bracket).location
      skip_newlines
      elements = [] of AST::Expr
      unless current.kind.r_bracket?
        loop do
          elements << parse_expr
          skip_newlines
          break unless match?(:comma)
          skip_newlines
          break if current.kind.r_bracket?
        end
      end
      expect(:r_bracket)
      AST::ArrayLiteral.new(loc, elements)
    end

    private def parse_struct_literal(name : Token, qualifier : String? = nil) : AST::StructLiteral
      expect(:l_brace)
      skip_newlines
      fields = [] of {String, AST::Expr}
      until current.kind.r_brace? || current.kind.eof?
        field = expect(:ident)
        expect(:colon)
        skip_newlines
        value = parse_expr
        fields << {field.value, value}
        skip_newlines
        break unless match?(:comma)
        skip_newlines
      end
      expect(:r_brace)
      AST::StructLiteral.new(name.location, name.value, fields, qualifier)
    end

    private def struct_literal_ahead? : Bool
      return false unless current.kind.l_brace?
      i = @index + 1
      while i < @tokens.size && @tokens[i].kind.newline?
        i += 1
      end
      return false unless i < @tokens.size && @tokens[i].kind.ident?
      i += 1
      while i < @tokens.size && @tokens[i].kind.newline?
        i += 1
      end
      i < @tokens.size && @tokens[i].kind.colon?
    end

    private def parse_spawn(tok : Token) : AST::Call
      bump unless current.kind.eof? # tok is current spawn
      skip_newlines
      block = parse_trailing_block
      AST::Call.new(tok.location, "spawn", [] of AST::Expr, nil, block)
    end

    private def parse_call(name : Token) : AST::Call
      args = parse_arg_list
      AST::Call.new(name.location, name.value, args)
    end

    private def parse_interpolated_string : AST::InterpString
      open = expect(:interp_open)
      parts = [] of AST::InterpPart
      unless open.value.empty?
        parts << AST::InterpPart.new(open.value, nil)
      end
      loop do
        if current.kind.interp_close?
          close = bump
          unless close.value.empty?
            parts << AST::InterpPart.new(close.value, nil)
          end
          break
        end
        if current.kind.interp_mid?
          raise CompileError.at(current.location, "expected interpolation expression")
        end
        skip_newlines
        expr = parse_expr
        parts << AST::InterpPart.new(nil, expr)
        skip_newlines
        if current.kind.interp_mid?
          mid = bump
          unless mid.value.empty?
            parts << AST::InterpPart.new(mid.value, nil)
          end
        elsif current.kind.interp_close?
          next
        else
          raise CompileError.at(current.location, "expected end of interpolation")
        end
      end
      AST::InterpString.new(open.location, parts)
    end

    private def parse_trailing_block : AST::Block
      loc = expect(:l_brace).location
      skip_newlines
      params = [] of String
      if current.kind.pipe?
        bump
        skip_newlines
        unless current.kind.pipe?
          loop do
            params << expect(:ident).value
            skip_newlines
            break unless match?(:comma)
            skip_newlines
          end
        end
        expect(:pipe)
        skip_newlines
      end
      stmts = [] of AST::Stmt
      until current.kind.r_brace? || current.kind.eof?
        stmts << parse_stmt
        skip_newlines
      end
      expect(:r_brace)
      AST::Block.new(loc, params, stmts)
    end

    private def attach_block(left : AST::Expr, block : AST::Block) : AST::Call
      case left
      when AST::Call
        if left.block
          raise CompileError.at(block.location, "call already has a block")
        end
        left.block = block
        left
      when AST::FieldAccess
        AST::Call.new(block.location, left.field, [] of AST::Expr, left.object, block)
      when AST::Name
        AST::Call.new(block.location, left.ident, [] of AST::Expr, nil, block)
      else
        raise CompileError.at(block.location, "trailing block must follow a call")
      end
    end

    private def parse_arg_list : Array(AST::Expr)
      expect(:l_paren)
      skip_newlines
      args = [] of AST::Expr
      unless current.kind.r_paren?
        loop do
          args << parse_expr
          skip_newlines
          break unless match?(:comma)
          skip_newlines
        end
      end
      expect(:r_paren)
      args
    end

    private def parse_integer(tok : Token) : {UInt64, String}
      raw = tok.value
      suffix = ""
      if raw.ends_with?("u64") && raw.size > 3
        suffix = "u64"
        raw = raw[0..-4]
      elsif raw.ends_with?("i64") && raw.size > 3
        suffix = "i64"
        raw = raw[0..-4]
      end
      bits = if raw.size >= 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X')
               raw[2..].to_u64(16)
             else
               raw.to_u64
             end
      {bits, suffix}
    rescue
      raise CompileError.at(tok.location, "integer literal is out of range")
    end

    private def infix_prec(kind : Token::Kind) : Int32
      case kind
      when .pipe_pipe?
        1
      when .amp_amp?
        2
      when .pipe?
        3
      when .caret?
        4
      when .amp?
        5
      when .eq_eq?, .not_eq?
        6
      when .less?, .less_eq?, .greater?, .greater_eq?
        7
      when .less_less?, .greater_greater?
        8
      when .plus?, .minus?
        9
      when .star?, .slash?, .percent?
        10
      else
        -1
      end
    end

    private def unary_prec : Int32
      11
    end

    private def skip_newlines_if_continuing : Nil
      return unless current.kind.newline?
      kind = look_ahead_non_nl.kind
      skip_newlines if infix_prec(kind) >= 0
    end

    private def skip_newlines_if_postfix : Nil
      return unless current.kind.newline?
      kind = look_ahead_non_nl.kind
      skip_newlines if kind.dot? || kind.l_bracket? || kind.question?
    end

    private def look_ahead_non_nl : Token
      i = @index
      while i < @tokens.size && @tokens[i].kind.newline?
        i += 1
      end
      @tokens[i]
    end

    private def skip_newlines : Nil
      while current.kind.newline?
        bump
      end
    end

    private def current : Token
      @tokens[@index]
    end

    private def bump : Token
      tok = current
      @index += 1 unless tok.kind.eof?
      tok
    end

    private def match?(kind : Token::Kind) : Bool
      return false unless current.kind == kind
      bump
      true
    end

    private def expect(kind : Token::Kind) : Token
      tok = current
      unless tok.kind == kind
        raise CompileError.at(tok.location, "expected #{kind}, found #{tok.kind}")
      end
      bump
      tok
    end
  end
end
