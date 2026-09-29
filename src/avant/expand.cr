module Avant
  class Expander
    def initialize(@source : Source, @program : AST::Program)
      @next_id = 0
      @ctx_var = nil.as(String?)
      @ctx_field = nil.as(AST::Field?)
    end

    def expand : AST::Program
      expand_quotes(@program.quotes, "")
      @program.quotes.clear
      @program.structs.each do |s|
        expand_quotes(s.quotes, s.name)
        s.quotes.clear
        expand_walks(s.comptimes, s.name)
        s.comptimes.clear
      end
      @program.classes.each do |c|
        expand_quotes(c.quotes, c.name)
        c.quotes.clear
        expand_walks(c.comptimes, c.name)
        c.comptimes.clear
      end
      expand_walks(@program.comptimes, "")
      @program.comptimes.clear
      @program.all_functions.each do |fn|
        fn.body = expand_stmt_list(fn.body)
      end
      @program
    end

    private def expand_quotes(quotes : Array(AST::QuoteDecl), owner : String) : Nil
      quotes.each do |q|
        q.functions.each do |fn|
          inject_function(fill_and_hygiene_fn(AST.clone_function(fn)), owner.empty? ? q.owner : owner, q.module_name)
        end
      end
    end

    private def expand_walks(walks : Array(AST::ComptimeWalk), owner : String) : Nil
      walks.each do |walk|
        fields = find_fields(walk.type_name, walk.location)
        fields.each do |field|
          @ctx_var = walk.var_name
          @ctx_field = field
          walk.quote.functions.each do |tmpl|
            inject_function(fill_and_hygiene_fn(AST.clone_function(tmpl)), owner.empty? ? walk.owner : owner, walk.module_name)
          end
          @ctx_var = nil
          @ctx_field = nil
        end
      end
    end

    private def fill_and_hygiene_fn(fn : AST::Function) : AST::Function
      fill_function(fn)
      hygiene_function(fn)
      fn.body = expand_stmt_list(fn.body)
      fn
    end

    private def inject_function(fn : AST::Function, owner : String, mod_name : String) : Nil
      fn.module_name = mod_name unless mod_name.empty?
      if owner.empty?
        fn.owner = nil
        @program.functions << fn
      else
        fn.owner = owner
        if s = @program.structs.find { |x| x.name == owner }
          s.methods << fn
          return
        end
        if c = @program.classes.find { |x| x.name == owner }
          c.methods << fn
          return
        end
        raise CompileError.at(fn.location, "quote owner #{owner} is not a type")
      end
    end

    private def find_fields(type_name : String, loc : Location) : Array(AST::Field)
      structs = @program.structs.select { |s| s.name == type_name }
      classes = @program.classes.select { |c| c.name == type_name }
      total = structs.size + classes.size
      if total == 0
        raise CompileError.at(loc, "unknown type #{type_name} for fields")
      end
      if total > 1
        raise CompileError.at(loc, "ambiguous type #{type_name} for fields")
      end
      structs.empty? ? classes[0].fields : structs[0].fields
    end

    private def fill_function(fn : AST::Function) : Nil
      if sp = fn.name_splice
        fn.name = splice_to_ident(sp)
        fn.emit_name = fn.name
        fn.name_splice = nil
      end
      if r = fn.receiver
        r.type = fill_type_name(r.type)
      end
      if t = fn.return_type
        fn.return_type = fill_type_name(t)
      end
      fn.params.each do |p|
        p.type = fill_type_name(p.type)
      end
      fn.body = fn.body.map { |s| fill_stmt(s) }
    end

    private def fill_type_name(tn : AST::TypeName) : AST::TypeName
      if sp = tn.splice
        return splice_to_type(sp)
      end
      filled_args = tn.args.map { |a| fill_type_name(a) }
      tn.args.clear
      filled_args.each { |a| tn.args << a }
      filled_members = tn.members.map { |m| fill_type_name(m) }
      tn.members.clear
      filled_members.each { |m| tn.members << m }
      tn
    end

    private def fill_stmt(s : AST::Stmt) : AST::Stmt
      case s
      when AST::ExprStmt
        AST::ExprStmt.new(s.location, fill_expr(s.expr))
      when AST::ReturnStmt
        AST::ReturnStmt.new(s.location, s.expr.try { |e| fill_expr(e) })
      when AST::IfStmt
        AST::IfStmt.new(s.location, fill_expr(s.cond), s.then_body.map { |x| fill_stmt(x) }, s.else_body.map { |x| fill_stmt(x) }, s.bind)
      when AST::WhileStmt
        AST::WhileStmt.new(s.location, fill_expr(s.cond), s.body.map { |x| fill_stmt(x) })
      when AST::AssignStmt
        declared = s.declared_type
        if declared
          declared = fill_type_name(declared)
        end
        AST::AssignStmt.new(s.location, fill_expr(s.target), fill_expr(s.value), declared, s.op)
      when AST::QuoteStmt
        AST::QuoteStmt.new(s.location, s.body.map { |x| fill_stmt(x) })
      else
        s
      end
    end

    private def fill_expr(e : AST::Expr) : AST::Expr
      case e
      when AST::Splice
        splice_to_expr(e)
      when AST::Name, AST::IntegerLiteral, AST::FloatLiteral, AST::StringLiteral, AST::BoolLiteral, AST::NilLiteral
        e
      when AST::Call
        if sp = e.callee_splice
          e.callee = splice_to_ident(sp)
          e.callee_splice = nil
        end
        e.receiver = e.receiver.try { |r| fill_expr(r) }
        args = e.args.map { |a| fill_expr(a) }
        e.args.clear
        args.each { |a| e.args << a }
        if b = e.block
          b.body = b.body.map { |s| fill_stmt(s) }
        end
        e
      when AST::Unary
        AST::Unary.new(e.location, e.op, fill_expr(e.expr))
      when AST::Binary
        AST::Binary.new(e.location, e.op, fill_expr(e.left), fill_expr(e.right))
      when AST::FieldAccess
        if sp = e.field_splice
          e.field = splice_to_ident(sp)
          e.field_splice = nil
        end
        AST::FieldAccess.new(e.location, fill_expr(e.object), e.field, e.method_call)
      when AST::Index
        AST::Index.new(e.location, fill_expr(e.array), fill_expr(e.index))
      when AST::StructLiteral
        fields = [] of {String, AST::Expr}
        e.fields.each { |n, v| fields << {n, fill_expr(v)} }
        AST::StructLiteral.new(e.location, e.type_name, fields, e.qualifier)
      when AST::ArrayLiteral
        AST::ArrayLiteral.new(e.location, e.elements.map { |x| fill_expr(x) })
      when AST::ArrayNew
        AST::ArrayNew.new(e.location, fill_type_name(e.elem_type), fill_expr(e.size))
      when AST::HashNew
        AST::HashNew.new(e.location, fill_type_name(e.key_type), fill_type_name(e.val_type))
      when AST::Try
        AST::Try.new(e.location, fill_expr(e.expr))
      when AST::InterpString
        AST::InterpString.new(e.location, e.parts.map { |p| AST::InterpPart.new(p.text, p.expr.try { |x| fill_expr(x) }) })
      when AST::SwitchExpr
        AST::SwitchExpr.new(e.location, fill_expr(e.cond), e.cases.map { |c| AST::SwitchCase.new(c.location, c.labels.dup, c.body.map { |s| fill_stmt(s) }) }, e.else_body.try { |b| b.map { |s| fill_stmt(s) } })
      else
        e
      end
    end

    private def splice_payload(sp : AST::Expr) : AST::Expr
      case sp
      when AST::Splice
        sp.inner
      else
        sp
      end
    end

    private def splice_to_ident(sp : AST::Expr) : String
      inner = splice_payload(sp)
      case inner
      when AST::StringLiteral
        ident_from_string(inner.value, inner.location)
      when AST::Name
        inner.ident
      when AST::FieldAccess
        walk_field_name(inner)
      else
        raise CompileError.at(inner.location, "splice is a name here")
      end
    end

    private def splice_to_type(sp : AST::Expr) : AST::TypeName
      inner = splice_payload(sp)
      case inner
      when AST::StringLiteral
        AST::TypeName.new(inner.location, ident_from_string(inner.value, inner.location))
      when AST::Name
        AST::TypeName.new(inner.location, inner.ident)
      when AST::FieldAccess
        unless walk_field?(inner)
          raise CompileError.at(inner.location, "splice f.type needs a field walk")
        end
        unless inner.field == "type"
          raise CompileError.at(inner.location, "f.type is the field type in a walk")
        end
        field = @ctx_field.not_nil!
        AST.clone_type_name(field.type)
      else
        raise CompileError.at(inner.location, "splice is a type here")
      end
    end

    private def splice_to_expr(sp : AST::Splice) : AST::Expr
      inner = sp.inner
      case inner
      when AST::IntegerLiteral, AST::FloatLiteral, AST::StringLiteral, AST::BoolLiteral, AST::NilLiteral
        AST.clone_expr(inner)
      when AST::Name
        AST::Name.new(inner.location, inner.ident)
      when AST::FieldAccess
        unless walk_field?(inner)
          raise CompileError.at(inner.location, "splice f.name needs a field walk")
        end
        field = @ctx_field.not_nil!
        if inner.field == "name"
          AST::StringLiteral.new(inner.location, field.name)
        elsif inner.field == "type"
          raise CompileError.at(inner.location, "f.type is a type, not an expression")
        else
          raise CompileError.at(inner.location, "field walk splice is f.name or f.type")
        end
      else
        raise CompileError.at(inner.location, "splice is a literal, a name, or f.name / f.type")
      end
    end

    private def walk_field?(fa : AST::FieldAccess) : Bool
      obj = fa.object
      return false unless obj.is_a?(AST::Name)
      return false unless var = @ctx_var
      obj.ident == var && (fa.field == "name" || fa.field == "type")
    end

    private def walk_field_name(fa : AST::FieldAccess) : String
      unless walk_field?(fa)
        raise CompileError.at(fa.location, "splice f.name needs a field walk")
      end
      unless fa.field == "name"
        raise CompileError.at(fa.location, "f.name is the field name in a walk")
      end
      @ctx_field.not_nil!.name
    end

    private def ident_from_string(s : String, loc : Location) : String
      unless ident?(s)
        raise CompileError.at(loc, "splice is not an identifier #{s}")
      end
      s
    end

    private def ident?(s : String) : Bool
      return false if s.empty?
      c = s[0]
      return false unless c.ascii_letter? || c == '_'
      s.each_char { |ch| return false unless ch.ascii_alphanumeric? || ch == '_' }
      true
    end

    private def hygiene_function(fn : AST::Function) : Nil
      introduced = [] of String
      fn.params.each { |p| introduced << p.name unless p.name == "self" }
      if r = fn.receiver
        introduced << r.name unless r.name == "self"
      end
      collect_introduced(fn.body, introduced)
      map = rename_map(introduced)
      return if map.empty?
      fn.params.each { |p| p.name = map[p.name] if map.has_key?(p.name) }
      if r = fn.receiver
        r.name = map[r.name] if map.has_key?(r.name)
      end
      fn.body = fn.body.map { |s| rename_stmt(s, map) }
    end

    private def hygiene_stmts(body : Array(AST::Stmt)) : Array(AST::Stmt)
      introduced = [] of String
      collect_introduced(body, introduced)
      map = rename_map(introduced)
      return body if map.empty?
      body.map { |s| rename_stmt(s, map) }
    end

    private def rename_map(introduced : Array(String)) : Hash(String, String)
      map = {} of String => String
      return map if introduced.empty?
      id = @next_id
      @next_id += 1
      introduced.uniq.each do |name|
        next if name == "self"
        map[name] = "__q#{id}_#{name}"
      end
      map
    end

    private def collect_introduced(body : Array(AST::Stmt), into : Array(String)) : Nil
      body.each do |s|
        case s
        when AST::AssignStmt
          t = s.target
          if t.is_a?(AST::Name)
            into << t.ident
          end
          collect_introduced_expr(s.value, into)
        when AST::IfStmt
          if bind = s.bind
            into << bind
          end
          collect_introduced_expr(s.cond, into)
          collect_introduced(s.then_body, into)
          collect_introduced(s.else_body, into)
        when AST::WhileStmt
          collect_introduced_expr(s.cond, into)
          collect_introduced(s.body, into)
        when AST::QuoteStmt
          collect_introduced(s.body, into)
        when AST::ExprStmt
          collect_introduced_expr(s.expr, into)
        when AST::ReturnStmt
          s.expr.try { |e| collect_introduced_expr(e, into) }
        else
        end
      end
    end

    private def collect_introduced_expr(e : AST::Expr, into : Array(String)) : Nil
      case e
      when AST::Call
        if b = e.block
          b.params.each { |p| into << p }
          collect_introduced(b.body, into)
        end
        e.receiver.try { |r| collect_introduced_expr(r, into) }
        e.args.each { |a| collect_introduced_expr(a, into) }
      when AST::Unary
        collect_introduced_expr(e.expr, into)
      when AST::Binary
        collect_introduced_expr(e.left, into)
        collect_introduced_expr(e.right, into)
      when AST::FieldAccess
        collect_introduced_expr(e.object, into)
      when AST::Index
        collect_introduced_expr(e.array, into)
        collect_introduced_expr(e.index, into)
      when AST::StructLiteral
        e.fields.each { |_n, v| collect_introduced_expr(v, into) }
      when AST::ArrayLiteral
        e.elements.each { |x| collect_introduced_expr(x, into) }
      when AST::ArrayNew
        collect_introduced_expr(e.size, into)
      when AST::Try
        collect_introduced_expr(e.expr, into)
      when AST::InterpString
        e.parts.each { |p| p.expr.try { |x| collect_introduced_expr(x, into) } }
      when AST::SwitchExpr
        collect_introduced_expr(e.cond, into)
        e.cases.each { |c| collect_introduced(c.body, into) }
        e.else_body.try { |b| collect_introduced(b, into) }
      else
      end
    end

    private def rename_stmt(s : AST::Stmt, map : Hash(String, String)) : AST::Stmt
      case s
      when AST::ExprStmt
        AST::ExprStmt.new(s.location, rename_expr(s.expr, map))
      when AST::ReturnStmt
        AST::ReturnStmt.new(s.location, s.expr.try { |e| rename_expr(e, map) })
      when AST::IfStmt
        bind = s.bind
        bind = map[bind] if bind && map.has_key?(bind)
        AST::IfStmt.new(s.location, rename_expr(s.cond, map), s.then_body.map { |x| rename_stmt(x, map) }, s.else_body.map { |x| rename_stmt(x, map) }, bind)
      when AST::WhileStmt
        AST::WhileStmt.new(s.location, rename_expr(s.cond, map), s.body.map { |x| rename_stmt(x, map) })
      when AST::AssignStmt
        AST::AssignStmt.new(s.location, rename_expr(s.target, map), rename_expr(s.value, map), s.declared_type, s.op)
      when AST::QuoteStmt
        AST::QuoteStmt.new(s.location, s.body.map { |x| rename_stmt(x, map) })
      else
        s
      end
    end

    private def rename_expr(e : AST::Expr, map : Hash(String, String)) : AST::Expr
      case e
      when AST::Name
        if mapped = map[e.ident]?
          AST::Name.new(e.location, mapped, e.implicit_field)
        else
          e
        end
      when AST::Call
        callee = map[e.callee]? || e.callee
        recv = e.receiver.try { |r| rename_expr(r, map) }
        args = e.args.map { |a| rename_expr(a, map) }
        block = e.block.try do |b|
          params = b.params.map { |p| map[p]? || p }
          AST::Block.new(b.location, params, b.body.map { |s| rename_stmt(s, map) })
        end
        c = AST::Call.new(e.location, callee, args, recv, block)
        c.lib_name = e.lib_name
        c
      when AST::Unary
        AST::Unary.new(e.location, e.op, rename_expr(e.expr, map))
      when AST::Binary
        AST::Binary.new(e.location, e.op, rename_expr(e.left, map), rename_expr(e.right, map))
      when AST::FieldAccess
        AST::FieldAccess.new(e.location, rename_expr(e.object, map), e.field, e.method_call)
      when AST::Index
        AST::Index.new(e.location, rename_expr(e.array, map), rename_expr(e.index, map))
      when AST::StructLiteral
        fields = [] of {String, AST::Expr}
        e.fields.each { |n, v| fields << {n, rename_expr(v, map)} }
        AST::StructLiteral.new(e.location, e.type_name, fields, e.qualifier)
      when AST::ArrayLiteral
        AST::ArrayLiteral.new(e.location, e.elements.map { |x| rename_expr(x, map) })
      when AST::ArrayNew
        AST::ArrayNew.new(e.location, e.elem_type, rename_expr(e.size, map))
      when AST::Try
        AST::Try.new(e.location, rename_expr(e.expr, map))
      when AST::InterpString
        AST::InterpString.new(e.location, e.parts.map { |p| AST::InterpPart.new(p.text, p.expr.try { |x| rename_expr(x, map) }) })
      when AST::SwitchExpr
        AST::SwitchExpr.new(e.location, rename_expr(e.cond, map), e.cases.map { |c| AST::SwitchCase.new(c.location, c.labels.dup, c.body.map { |s| rename_stmt(s, map) }) }, e.else_body.try { |b| b.map { |s| rename_stmt(s, map) } })
      else
        e
      end
    end

    private def expand_stmt_list(body : Array(AST::Stmt)) : Array(AST::Stmt)
      out = [] of AST::Stmt
      body.each do |s|
        case s
        when AST::QuoteStmt
          filled = s.body.map { |x| fill_stmt(x) }
          hygiened = hygiene_stmts(filled)
          out.concat(expand_stmt_list(hygiened))
        when AST::IfStmt
          out << AST::IfStmt.new(s.location, s.cond, expand_stmt_list(s.then_body), expand_stmt_list(s.else_body), s.bind)
        when AST::WhileStmt
          out << AST::WhileStmt.new(s.location, s.cond, expand_stmt_list(s.body))
        when AST::SwitchExpr
          out << s
        else
          if s.is_a?(AST::ExprStmt)
            expr = s.expr
            if expr.is_a?(AST::SwitchExpr)
              cases = expr.cases.map { |c| AST::SwitchCase.new(c.location, c.labels.dup, expand_stmt_list(c.body)) }
              else_body = expr.else_body.try { |b| expand_stmt_list(b) }
              sw = AST::SwitchExpr.new(expr.location, expr.cond, cases, else_body)
              out << AST::ExprStmt.new(s.location, sw)
              next
            end
          end
          out << expand_nested_stmt(s)
        end
      end
      out
    end

    private def expand_nested_stmt(s : AST::Stmt) : AST::Stmt
      case s
      when AST::IfStmt
        AST::IfStmt.new(s.location, s.cond, expand_stmt_list(s.then_body), expand_stmt_list(s.else_body), s.bind)
      when AST::WhileStmt
        AST::WhileStmt.new(s.location, s.cond, expand_stmt_list(s.body))
      else
        s
      end
    end
  end
end
