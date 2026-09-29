module Avant
  module AST
    def self.clone_type_name(tn : TypeName) : TypeName
      TypeName.new(
        tn.location,
        tn.name,
        tn.args.map { |a| clone_type_name(a) },
        tn.nilable,
        tn.members.map { |m| clone_type_name(m) }
      )
    end

    def self.subst_type_name(tn : TypeName, from : Array(String), to : Array(TypeName)) : TypeName
      if tn.union?
        return TypeName.union(tn.location, tn.members.map { |m| subst_type_name(m, from, to) }, tn.nilable)
      end
      if tn.args.empty?
        from.each_with_index do |name, i|
          if tn.name == name
            rep = clone_type_name(to[i])
            if tn.nilable && !rep.nilable
              if rep.union?
                return TypeName.union(tn.location, [rep], true)
              end
              return TypeName.new(tn.location, rep.name, rep.args.dup, true, rep.members.map { |m| clone_type_name(m) })
            end
            return rep
          end
        end
      end
      TypeName.new(tn.location, tn.name, tn.args.map { |a| subst_type_name(a, from, to) }, tn.nilable)
    end

    def self.clone_param(p : Param) : Param
      Param.new(p.location, p.name, clone_type_name(p.type), p.default.try { |d| clone_expr(d) })
    end

    def self.clone_block(b : Block) : Block
      Block.new(b.location, b.params.dup, b.body.map { |s| clone_stmt(s) })
    end

    def self.clone_function(fn : Function) : Function
      out = Function.new(
        fn.location,
        fn.name,
        fn.params.map { |p| clone_param(p) },
        fn.return_type.try { |t| clone_type_name(t) },
        fn.body.map { |s| clone_stmt(s) },
        fn.receiver.try { |r| clone_param(r) },
        fn.owner
      )
      out.emit_name = fn.emit_name
      out.type_params = fn.type_params.dup
      out.generic = fn.generic
      out.template = fn.template
      out
    end

    def self.subst_function_types(fn : Function, from : Array(String), to : Array(TypeName)) : Nil
      if r = fn.receiver
        r.type = subst_type_name(r.type, from, to)
      end
      if t = fn.return_type
        fn.return_type = subst_type_name(t, from, to)
      end
      fn.params.each do |p|
        p.type = subst_type_name(p.type, from, to)
        if d = p.default
          subst_type_in_expr(d, from, to)
        end
      end
      subst_type_in_stmts(fn.body, from, to)
    end

    def self.clone_expr(e : Expr) : Expr
      out = case e
      when IntegerLiteral
        IntegerLiteral.new(e.location, e.bits, e.suffix)
      when FloatLiteral
        FloatLiteral.new(e.location, e.value)
      when StringLiteral
        StringLiteral.new(e.location, e.value)
      when BoolLiteral
        BoolLiteral.new(e.location, e.value)
      when NilLiteral
        NilLiteral.new(e.location)
      when Name
        n = Name.new(e.location, e.ident, e.implicit_field)
        n
      when Call
        c = Call.new(e.location, e.callee, e.args.map { |a| clone_expr(a) }, e.receiver.try { |r| clone_expr(r) }, e.block.try { |b| clone_block(b) })
        c.lib_name = e.lib_name
        c
      when Unary
        Unary.new(e.location, e.op, clone_expr(e.expr))
      when Binary
        Binary.new(e.location, e.op, clone_expr(e.left), clone_expr(e.right))
      when FieldAccess
        FieldAccess.new(e.location, clone_expr(e.object), e.field, e.method_call)
      when Index
        Index.new(e.location, clone_expr(e.array), clone_expr(e.index))
      when StructLiteral
        fields = [] of {String, Expr}
        e.fields.each { |n, v| fields << {n, clone_expr(v)} }
        StructLiteral.new(e.location, e.type_name, fields)
      when ArrayLiteral
        ArrayLiteral.new(e.location, e.elements.map { |x| clone_expr(x) })
      when ArrayNew
        ArrayNew.new(e.location, clone_type_name(e.elem_type), clone_expr(e.size))
      when HashNew
        HashNew.new(e.location, clone_type_name(e.key_type), clone_type_name(e.val_type))
      when Try
        Try.new(e.location, clone_expr(e.expr))
      when InterpString
        InterpString.new(e.location, e.parts.map { |p| InterpPart.new(p.text, p.expr.try { |x| clone_expr(x) }) })
      when SwitchExpr
        sw = SwitchExpr.new(e.location, clone_expr(e.cond), e.cases.map { |c| clone_switch_case(c) }, e.else_body.try { |b| b.map { |s| clone_stmt(s) } })
        sw
      else
        raise "cannot clone #{e.class}"
      end
      out
    end

    def self.clone_switch_case(c : SwitchCase) : SwitchCase
      SwitchCase.new(c.location, c.labels.dup, c.body.map { |s| clone_stmt(s) })
    end

    def self.clone_stmt(s : Stmt) : Stmt
      case s
      when ExprStmt
        ExprStmt.new(s.location, clone_expr(s.expr))
      when ReturnStmt
        ReturnStmt.new(s.location, s.expr.try { |e| clone_expr(e) })
      when IfStmt
        IfStmt.new(s.location, clone_expr(s.cond), s.then_body.map { |x| clone_stmt(x) }, s.else_body.map { |x| clone_stmt(x) }, s.bind)
      when WhileStmt
        WhileStmt.new(s.location, clone_expr(s.cond), s.body.map { |x| clone_stmt(x) })
      when BreakStmt
        BreakStmt.new(s.location)
      when ContinueStmt
        ContinueStmt.new(s.location)
      when AssignStmt
        AssignStmt.new(s.location, clone_expr(s.target), clone_expr(s.value), s.declared_type.try { |t| clone_type_name(t) }, s.op)
      else
        raise "cannot clone #{s.class}"
      end
    end

    def self.subst_type_in_expr(e : Expr, from : Array(String), to : Array(TypeName)) : Nil
      case e
      when Call
        e.receiver.try { |r| subst_type_in_expr(r, from, to) }
        e.args.each { |a| subst_type_in_expr(a, from, to) }
        if b = e.block
          subst_type_in_stmts(b.body, from, to)
        end
      when Unary
        subst_type_in_expr(e.expr, from, to)
      when Binary
        subst_type_in_expr(e.left, from, to)
        subst_type_in_expr(e.right, from, to)
      when FieldAccess
        subst_type_in_expr(e.object, from, to)
      when Index
        subst_type_in_expr(e.array, from, to)
        subst_type_in_expr(e.index, from, to)
      when StructLiteral
        e.fields.each { |_, v| subst_type_in_expr(v, from, to) }
      when ArrayLiteral
        e.elements.each { |x| subst_type_in_expr(x, from, to) }
      when ArrayNew
        e.elem_type = subst_type_name(e.elem_type, from, to)
        subst_type_in_expr(e.size, from, to)
      when HashNew
        e.key_type = subst_type_name(e.key_type, from, to)
        e.val_type = subst_type_name(e.val_type, from, to)
      when Try
        subst_type_in_expr(e.expr, from, to)
      when InterpString
        e.parts.each { |p| p.expr.try { |x| subst_type_in_expr(x, from, to) } }
      when SwitchExpr
        subst_type_in_expr(e.cond, from, to)
        e.cases.each { |c| subst_type_in_stmts(c.body, from, to) }
        e.else_body.try { |b| subst_type_in_stmts(b, from, to) }
      end
    end

    def self.subst_type_in_stmts(body : Array(Stmt), from : Array(String), to : Array(TypeName)) : Nil
      body.each { |s| subst_type_in_stmt(s, from, to) }
    end

    def self.subst_type_in_stmt(s : Stmt, from : Array(String), to : Array(TypeName)) : Nil
      case s
      when ExprStmt
        subst_type_in_expr(s.expr, from, to)
      when ReturnStmt
        s.expr.try { |e| subst_type_in_expr(e, from, to) }
      when IfStmt
        subst_type_in_expr(s.cond, from, to)
        subst_type_in_stmts(s.then_body, from, to)
        subst_type_in_stmts(s.else_body, from, to)
      when WhileStmt
        subst_type_in_expr(s.cond, from, to)
        subst_type_in_stmts(s.body, from, to)
      when AssignStmt
        subst_type_in_expr(s.target, from, to)
        subst_type_in_expr(s.value, from, to)
        if t = s.declared_type
          s.declared_type = subst_type_name(t, from, to)
        end
      end
    end

    def self.rewrite_names(e : Expr, map : Hash(String, Expr)) : Expr
      if e.is_a?(Name)
        if rep = map[e.ident]?
          return clone_expr(rep)
        end
        return clone_expr(e)
      end
      case e
      when Call
        blk = e.block
        if blk
          blk = Block.new(blk.location, blk.params.dup, blk.body.map { |s| rewrite_stmt_names(s, map) })
        end
        Call.new(e.location, e.callee, e.args.map { |a| rewrite_names(a, map) }, e.receiver.try { |r| rewrite_names(r, map) }, blk)
      when Unary
        Unary.new(e.location, e.op, rewrite_names(e.expr, map))
      when Binary
        Binary.new(e.location, e.op, rewrite_names(e.left, map), rewrite_names(e.right, map))
      when FieldAccess
        FieldAccess.new(e.location, rewrite_names(e.object, map), e.field, e.method_call)
      when Index
        Index.new(e.location, rewrite_names(e.array, map), rewrite_names(e.index, map))
      when StructLiteral
        fields = [] of {String, Expr}
        e.fields.each { |n, v| fields << {n, rewrite_names(v, map)} }
        StructLiteral.new(e.location, e.type_name, fields)
      when ArrayLiteral
        ArrayLiteral.new(e.location, e.elements.map { |x| rewrite_names(x, map) })
      when ArrayNew
        ArrayNew.new(e.location, clone_type_name(e.elem_type), rewrite_names(e.size, map))
      when HashNew
        HashNew.new(e.location, clone_type_name(e.key_type), clone_type_name(e.val_type))
      when Try
        Try.new(e.location, rewrite_names(e.expr, map))
      when InterpString
        InterpString.new(e.location, e.parts.map { |p| InterpPart.new(p.text, p.expr.try { |x| rewrite_names(x, map) }) })
      when SwitchExpr
        SwitchExpr.new(e.location, rewrite_names(e.cond, map), e.cases.map { |c|
          SwitchCase.new(c.location, c.labels.dup, c.body.map { |s| rewrite_stmt_names(s, map) })
        }, e.else_body.try { |b| b.map { |s| rewrite_stmt_names(s, map) } })
      else
        e
      end
    end

    def self.rewrite_stmt_names(s : Stmt, map : Hash(String, Expr)) : Stmt
      case s
      when ExprStmt
        ExprStmt.new(s.location, rewrite_names(s.expr, map))
      when ReturnStmt
        ReturnStmt.new(s.location, s.expr.try { |e| rewrite_names(e, map) })
      when IfStmt
        IfStmt.new(s.location, rewrite_names(s.cond, map), s.then_body.map { |x| rewrite_stmt_names(x, map) }, s.else_body.map { |x| rewrite_stmt_names(x, map) }, s.bind)
      when WhileStmt
        WhileStmt.new(s.location, rewrite_names(s.cond, map), s.body.map { |x| rewrite_stmt_names(x, map) })
      when AssignStmt
        AssignStmt.new(s.location, rewrite_names(s.target, map), rewrite_names(s.value, map), s.declared_type.try { |t| clone_type_name(t) }, s.op)
      else
        s
      end
    end
  end
end
