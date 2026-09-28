module Avant
  class Checker
    private record FunSig,
      node : AST::FunDecl,
      params : Array(Ty),
      return_type : Ty

    private record FuncSig,
      node : AST::Function,
      params : Array(Ty),
      return_type : Ty

    def initialize(@source : Source, @program : AST::Program)
      @named = {} of String => AggTy
      @opaques = {} of String => OpaqueTy
      @functions = {} of String => FuncSig
      @libs = {} of String => Hash(String, FunSig)
      @c_symbols = {} of String => String
      @methods = {} of String => Hash(String, MethodSig)
      @mangled = {} of String => AST::Function
      @scopes = [] of Hash(String, Ty)
      @resolver = TypeResolver.new(@named)
      @self_ty = nil.as(AggTy?)
      @self_name = nil.as(String?)
      @return_type = VoidTy::INSTANCE.as(Ty)
      @loop_depth = 0
    end

    def check : AST::Program
      @named, @opaques = Avant.load_types(@program)
      @resolver = TypeResolver.new(@named, @opaques)

      @program.libs.each { |lib_def| register_lib(lib_def) }
      @program.functions.each do |fn|
        if fn.receiver
          register_method(fn)
        else
          register_function(fn)
        end
      end
      @program.structs.each { |defn| defn.methods.each { |fn| register_method(fn) } }
      @program.classes.each { |defn| defn.methods.each { |fn| register_method(fn) } }
      reserve_constructors
      check_field_defaults

      unless @functions.has_key?("main") || @functions.has_key?("run")
        raise CompileError.at(@program.location, "program needs fn main or fn run")
      end

      @program.all_functions.each { |fn| check_function(fn) }
      @program
    end

    private def register_lib(lib_def : AST::LibDef) : Nil
      if @libs.has_key?(lib_def.name)
        raise CompileError.at(lib_def.location, "lib #{lib_def.name} is already defined")
      end
      if @named.has_key?(lib_def.name)
        raise CompileError.at(lib_def.location, "lib #{lib_def.name} collides with a type")
      end

      funs = {} of String => FunSig
      lib_def.funs.each do |fn|
        if funs.has_key?(fn.name)
          raise CompileError.at(fn.location, "fun #{fn.name} is already defined in lib #{lib_def.name}")
        end
        if @functions.has_key?(fn.name) || @mangled.has_key?(fn.name)
          raise CompileError.at(fn.location, "C function #{fn.name} collides with an Avant function")
        end
        if existing = @c_symbols[fn.name]?
          raise CompileError.at(fn.location, "C function #{fn.name} is already bound as #{existing}")
        end
        if Runtime.reserved_symbol?(fn.name)
          raise CompileError.at(fn.location, "C function #{fn.name} is reserved")
        end
        params = fn.params.map { |p| @resolver.resolve_value(p.type) }
        ret = if t = fn.return_type
                ty = @resolver.resolve(t)
                if ty.is_a?(OpaqueTy)
                  raise CompileError.at(t.location, "#{ty} is opaque; use Ptr(#{ty})")
                end
                ty
              else
                VoidTy::INSTANCE
              end
        funs[fn.name] = FunSig.new(fn, params, ret)
        @c_symbols[fn.name] = "#{lib_def.name}.#{fn.name}"
      end
      @libs[lib_def.name] = funs
    end

    private def register_function(fn : AST::Function) : Nil
      if @functions.has_key?(fn.name)
        raise CompileError.at(fn.location, "function #{fn.name} is already defined")
      end
      if @libs.has_key?(fn.name)
        raise CompileError.at(fn.location, "function #{fn.name} collides with a lib")
      end
      if @c_symbols.has_key?(fn.name)
        raise CompileError.at(fn.location, "function #{fn.name} collides with a C function")
      end
      if @named.has_key?(fn.name)
        raise CompileError.at(fn.location, "function #{fn.name} collides with a type")
      end
      params = fn.params.map { |p| @resolver.resolve_value(p.type) }
      ret = if t = fn.return_type
              ty = @resolver.resolve(t)
              if ty.is_a?(OpaqueTy)
                raise CompileError.at(t.location, "#{ty} is opaque; use Ptr(#{ty})")
              end
              ty
            else
              VoidTy::INSTANCE
            end
      @functions[fn.name] = FuncSig.new(fn, params, ret)
      note_mangled(fn.name, fn)
    end

    private def register_method(fn : AST::Function) : Nil
      owner_ty = if owner = fn.owner
                   @named[owner]? || raise CompileError.at(fn.location, "unknown type #{owner}")
                 else
                   recv = fn.receiver.not_nil!
                   ty = @resolver.resolve(recv.type)
                   unless ty.is_a?(AggTy)
                     raise CompileError.at(recv.location, "methods can only extend a struct or class, not #{ty}")
                   end
                   fn.owner = ty.name
                   ty
                 end

      if fn.name == "new"
        raise CompileError.at(fn.location, "new is reserved; write initialize")
      end
      if owner_ty.field_type(fn.name)
        raise CompileError.at(fn.location, "method #{owner_ty.name}.#{fn.name} collides with a field")
      end

      bucket = @methods[owner_ty.name] ||= {} of String => MethodSig
      if bucket.has_key?(fn.name)
        raise CompileError.at(fn.location, "method #{owner_ty.name}.#{fn.name} is already defined")
      end

      params = fn.params.map { |p| @resolver.resolve_value(p.type) }
      ret = if t = fn.return_type
              ty = @resolver.resolve(t)
              if ty.is_a?(OpaqueTy)
                raise CompileError.at(t.location, "#{ty} is opaque; use Ptr(#{ty})")
              end
              ty
            else
              VoidTy::INSTANCE
            end
      if fn.name == "initialize" && !ret.void?
        raise CompileError.at(fn.location, "initialize cannot return a value")
      end
      sig = MethodSig.new(fn, owner_ty, params, ret)
      bucket[fn.name] = sig
      note_mangled(sig.mangled, fn)
    end

    private def reserve_constructors : Nil
      @program.classes.each do |defn|
        name = "#{defn.name}__new"
        if @mangled.has_key?(name) || @functions.has_key?(name)
          raise CompileError.at(defn.location, "#{name} collides with the synthesized constructor")
        end
      end
    end

    private def check_field_defaults : Nil
      saved_self = @self_ty
      saved_self_name = @self_name
      @self_ty = nil
      @self_name = nil
      @program.classes.each do |defn|
        ty = @named[defn.name]
        defn.fields.each do |field|
          default = field.default
          next unless default
          unless trivial_default?(default)
            raise CompileError.at(default.location, "field default must be a literal or nil")
          end
          want = ty.field_type(field.name) || raise "missing field type"
          push_scope
          got = check_expr(default, want)
          unless assignable?(got, want)
            raise CompileError.at(default.location, "cannot assign #{got} to #{want}")
          end
          pop_scope
        end
      end
      @self_ty = saved_self
      @self_name = saved_self_name
    end

    private def trivial_default?(expr : AST::Expr) : Bool
      case expr
      when AST::IntegerLiteral, AST::FloatLiteral, AST::StringLiteral, AST::BoolLiteral, AST::NilLiteral
        true
      when AST::Unary
        expr.op.minus? && expr.expr.is_a?(AST::IntegerLiteral)
      else
        false
      end
    end

    private def note_mangled(name : String, fn : AST::Function) : Nil
      if existing = @mangled[name]?
        raise CompileError.at(fn.location, "emitted name #{name} collides with #{existing.name}")
      end
      if @c_symbols.has_key?(name)
        raise CompileError.at(fn.location, "emitted name #{name} collides with a C function")
      end
      @mangled[name] = fn
    end

    private def check_function(fn : AST::Function) : Nil
      ret, params = function_types(fn)
      @return_type = ret
      if !ret.void? && fn.body.empty?
        raise CompileError.at(fn.location, "missing return value")
      end

      @self_ty = nil
      @self_name = nil
      push_scope
      if fn.method?
        owner = @named[fn.owner.not_nil!]
        @self_ty = owner
        @self_name = fn.self_name
        if lookup(fn.self_name)
          raise CompileError.at(fn.location, "duplicate parameter #{fn.self_name}")
        end
        bind(fn.self_name, owner)
      end
      fn.params.each_with_index do |param, i|
        if lookup(param.name)
          raise CompileError.at(param.location, "duplicate parameter #{param.name}")
        end
        bind(param.name, params[i])
      end
      check_body(fn.body, ret, at_tail: true)
      pop_scope
      @self_ty = nil
      @self_name = nil
    end

    private def function_types(fn : AST::Function) : {Ty, Array(Ty)}
      if fn.method?
        sig = method_sig(fn.owner.not_nil!, fn.name).not_nil!
        {sig.return_type, sig.params}
      else
        sig = @functions[fn.name]
        {sig.return_type, sig.params}
      end
    end

    private def check_body(body : Array(AST::Stmt), expected : Ty, at_tail : Bool) : Nil
      body.each_with_index do |stmt, i|
        last = at_tail && i == body.size - 1
        check_stmt(stmt, expected, last)
      end
    end

    private def check_block(body : Array(AST::Stmt), expected : Ty, at_tail : Bool) : Nil
      push_scope
      check_body(body, expected, at_tail)
      pop_scope
    end

    private def check_stmt(stmt : AST::Stmt, expected : Ty, at_tail : Bool) : Nil
      case stmt
      when AST::ReturnStmt
        if expr = stmt.expr
          got = check_expr(expr, expected)
          unless assignable?(got, expected)
            raise CompileError.at(stmt.location, "return type is #{got}, expected #{expected}")
          end
        else
          unless expected.void?
            raise CompileError.at(stmt.location, "return needs a value of type #{expected}")
          end
        end
      when AST::IfStmt
        if bind = stmt.bind
          check_if_assign(stmt, bind, expected, at_tail)
        else
          cond = check_expr(stmt.cond)
          unless cond.bool?
            raise CompileError.at(stmt.cond.location, "if condition must be Bool, got #{cond}")
          end
          check_block(stmt.then_body, expected, at_tail: false)
          check_block(stmt.else_body, expected, at_tail: false) unless stmt.else_body.empty?
          if at_tail && !expected.void?
            unless !stmt.else_body.empty? && !fallthrough?(stmt.then_body) && !fallthrough?(stmt.else_body)
              raise CompileError.at(stmt.location, "missing return value")
            end
          end
        end
      when AST::WhileStmt
        cond = check_expr(stmt.cond)
        unless cond.bool?
          raise CompileError.at(stmt.cond.location, "while condition must be Bool, got #{cond}")
        end
        @loop_depth += 1
        check_block(stmt.body, expected, at_tail: false)
        @loop_depth -= 1
        if at_tail && !expected.void?
          raise CompileError.at(stmt.location, "missing return value")
        end
      when AST::BreakStmt, AST::ContinueStmt
        if @loop_depth == 0
          word = stmt.is_a?(AST::BreakStmt) ? "break" : "continue"
          raise CompileError.at(stmt.location, "#{word} is only valid inside while")
        end
        if at_tail && !expected.void?
          raise CompileError.at(stmt.location, "missing return value")
        end
      when AST::AssignStmt
        check_assign(stmt)
        if at_tail && !expected.void?
          raise CompileError.at(stmt.location, "missing return value")
        end
      when AST::ExprStmt
        got = check_expr(stmt.expr)
        if at_tail && !expected.void?
          unless assignable?(got, expected)
            raise CompileError.at(stmt.location, "last expression is #{got}, expected #{expected}")
          end
        elsif at_tail && expected.void? && !got.void?
          raise CompileError.at(stmt.location, "unused #{got} value at the end of a void function")
        end
      else
        raise "unknown statement #{stmt.class}"
      end
    end

    private def check_assign(stmt : AST::AssignStmt) : Nil
      if stmt.compound?
        check_compound_assign(stmt)
        return
      end

      case target = stmt.target
      when AST::Name
        if target.ident == "self"
          raise CompileError.at(stmt.location, "cannot assign to self")
        end
        if declared = stmt.declared_type
          want = @resolver.resolve(declared)
          if lookup(target.ident) || implicit_field(target.ident)
            raise CompileError.at(stmt.location, "cannot shadow #{target.ident}")
          end
          got = check_expr(stmt.value, want)
          unless assignable?(got, want)
            raise CompileError.at(stmt.value.location, "cannot assign #{got} to #{want}")
          end
          bind(target.ident, want)
          target.type = want
        elsif existing = lookup(target.ident)
          got = check_expr(stmt.value, existing)
          unless assignable?(got, existing)
            raise CompileError.at(stmt.value.location, "cannot assign #{got} to #{existing}")
          end
          target.type = existing
        elsif field_ty = implicit_field(target.ident)
          target.implicit_field = true
          target.type = field_ty
          got = check_expr(stmt.value, field_ty)
          unless assignable?(got, field_ty)
            raise CompileError.at(stmt.value.location, "cannot assign #{got} to #{field_ty}")
          end
        else
          got = check_expr(stmt.value)
          if got.void?
            raise CompileError.at(stmt.location, "cannot bind a Void value")
          end
          bind(target.ident, got)
          target.type = got
        end
      when AST::FieldAccess, AST::Index
        if stmt.declared_type
          raise CompileError.at(stmt.location, "type annotation only belongs on a new name")
        end
        if target.is_a?(AST::Index)
          container = check_expr(target.array)
          if container.is_a?(HashTy)
            key = check_expr(target.index, container.key)
            unless assignable?(key, container.key)
              raise CompileError.at(target.index.location, "hash key must be #{container.key}, got #{key}")
            end
            got = check_expr(stmt.value, container.val)
            unless assignable?(got, container.val)
              raise CompileError.at(stmt.value.location, "cannot assign #{got} to #{container.val}")
            end
            target.type = container.val
            return
          end
        end
        place = check_expr(target)
        if target.is_a?(AST::FieldAccess) && target.method_call
          raise CompileError.at(stmt.location, "cannot assign to a method")
        end
        got = check_expr(stmt.value, place)
        unless assignable?(got, place)
          raise CompileError.at(stmt.value.location, "cannot assign #{got} to #{place}")
        end
      else
        raise CompileError.at(stmt.location, "cannot assign to this expression")
      end
    end

    private def check_compound_assign(stmt : AST::AssignStmt) : Nil
      if stmt.declared_type
        raise CompileError.at(stmt.location, "type annotation only belongs on a new name")
      end
      place = check_expr(stmt.target)
      if (target = stmt.target).is_a?(AST::Name) && target.ident == "self"
        raise CompileError.at(stmt.location, "cannot assign to self")
      end
      if (target = stmt.target).is_a?(AST::FieldAccess) && target.method_call
        raise CompileError.at(stmt.location, "cannot assign to a method")
      end
      got = check_expr(stmt.value, place)
      unless got.same?(place)
        raise CompileError.at(stmt.value.location, "cannot assign #{got} to #{place}")
      end
      unless place.numeric?
        raise CompileError.at(stmt.location, "#{stmt.op} expects a numeric target, got #{place}")
      end
    end

    private def fallthrough?(body : Array(AST::Stmt)) : Bool
      return true if body.empty?
      last = body.last
      case last
      when AST::ReturnStmt
        false
      when AST::IfStmt
        !last.else_body.empty? && !fallthrough?(last.then_body) && !fallthrough?(last.else_body)
      else
        true
      end
    end

    private def check_expr(expr : AST::Expr, expected : Ty? = nil) : Ty
      kind = case expr
             when AST::IntegerLiteral
               check_integer(expr, expected)
             when AST::FloatLiteral
               Float64Ty::INSTANCE
             when AST::StringLiteral
               StringTy::INSTANCE
             when AST::BoolLiteral
               BoolTy::INSTANCE
             when AST::Name
               check_name(expr)
             when AST::Call
               check_call(expr)
             when AST::Unary
               check_unary(expr)
             when AST::Binary
               check_binary(expr)
             when AST::FieldAccess
               check_field(expr)
             when AST::Index
               check_index(expr)
             when AST::StructLiteral
               check_struct_literal(expr)
             when AST::ArrayLiteral
               check_array_literal(expr, expected)
             when AST::ArrayNew
               check_array_new(expr)
             when AST::HashNew
               check_hash_new(expr)
             when AST::NilLiteral
               NilTy::INSTANCE
             when AST::Try
               check_try(expr)
             when AST::InterpString
               check_interp(expr)
             when AST::SwitchExpr
               check_switch(expr, expected)
             else
               raise "unknown expression #{expr.class}"
             end
      if expected && !assignable?(kind, expected)
        raise CompileError.at(expr.location, "expected #{expected}, got #{kind}")
      end
      expr.type = kind
      kind
    end

    private def check_name(expr : AST::Name) : Ty
      if expr.ident == "self" && @self_ty.nil?
        raise CompileError.at(expr.location, "self is only valid in a method")
      end
      if existing = lookup(expr.ident)
        return existing
      end
      if field_ty = implicit_field(expr.ident)
        expr.implicit_field = true
        return field_ty
      end
      raise CompileError.at(expr.location, "unknown name #{expr.ident}")
    end

    private def check_integer(expr : AST::IntegerLiteral, expected : Ty?) : Ty
      if expected && expected.float?
        return Float64Ty::INSTANCE
      end
      case expr.suffix
      when "u64"
        return UInt64Ty::INSTANCE
      when "i64"
        return Int64Ty::INSTANCE
      end
      if expected.try(&.uint64?)
        return UInt64Ty::INSTANCE
      end
      if expected.try(&.int64?)
        return Int64Ty::INSTANCE
      end
      unless expr.bits <= Int32::MAX.to_u64
        raise CompileError.at(expr.location, "integer literal does not fit in Int; use #{expr.bits}i64 or #{expr.bits}u64")
      end
      IntTy::INSTANCE
    end

    private def check_unary(expr : AST::Unary) : Ty
      inner = check_expr(expr.expr)
      case expr.op
      when .minus?
        unless inner.numeric?
          raise CompileError.at(expr.location, "unary minus expects Int or Float64")
        end
        inner
      when .bang?
        unless inner.bool?
          raise CompileError.at(expr.location, "! expects Bool")
        end
        BoolTy::INSTANCE
      when .tilde?
        unless inner.integer?
          raise CompileError.at(expr.location, "~ expects an integer")
        end
        inner
      else
        raise CompileError.at(expr.location, "unsupported unary operator")
      end
    end

    private def check_binary(expr : AST::Binary) : Ty
      if expr.op.amp_amp? || expr.op.pipe_pipe?
        left = check_expr(expr.left)
        right = check_expr(expr.right)
        unless left.bool? && right.bool?
          raise CompileError.at(expr.location, "#{expr.op} expects Bool operands")
        end
        return BoolTy::INSTANCE
      end

      left = check_expr(expr.left)
      right = check_expr(expr.right, (left.integer? || left.float? || left.string?) ? left : nil)
      if left.string? && right.string?
        case expr.op
        when .plus?
          return StringTy::INSTANCE
        when .eq_eq?, .not_eq?
          return BoolTy::INSTANCE
        else
          raise CompileError.at(expr.location, "unsupported operator #{expr.op} on String")
        end
      end
      if expr.op.percent?
        unless left.integer? && right.same?(left)
          raise CompileError.at(expr.location, "% expects matching integer operands")
        end
        return left
      end

      if bitwise?(expr.op)
        if shift?(expr.op) && (left.uint64? || left.int64?) && right.int?
          return left
        end
        unless left.integer? && right.same?(left)
          raise CompileError.at(expr.location, "#{expr.op} expects matching integer operands")
        end
        return left
      end

      if left.integer? && right.integer?
        unless left.same?(right)
          raise CompileError.at(expr.location, "binary operator expects matching types, got #{left} and #{right}")
        end
        return int_binary(expr.op, left)
      end

      if left.numeric? && right.numeric?
        unless left.float?
          expr.left.type = Float64Ty::INSTANCE
        end
        unless right.float?
          expr.right.type = Float64Ty::INSTANCE
        end
        return float_binary(expr.op, expr.location)
      end

      raise CompileError.at(expr.location, "binary operator expects numeric operands, got #{left} and #{right}")
    end

    private def bitwise?(op : Token::Kind) : Bool
      op.amp? || op.pipe? || op.caret? || op.less_less? || op.greater_greater?
    end

    private def shift?(op : Token::Kind) : Bool
      op.less_less? || op.greater_greater?
    end

    private def int_binary(op : Token::Kind, ty : Ty = IntTy::INSTANCE.as(Ty)) : Ty
      case op
      when .plus?, .minus?, .star?, .slash?
        ty
      when .eq_eq?, .not_eq?, .less?, .less_eq?, .greater?, .greater_eq?
        BoolTy::INSTANCE
      else
        raise "unsupported operator #{op}"
      end
    end

    private def float_binary(op : Token::Kind, loc : Location) : Ty
      case op
      when .plus?, .minus?, .star?, .slash?
        Float64Ty::INSTANCE
      when .eq_eq?, .not_eq?, .less?, .less_eq?, .greater?, .greater_eq?
        BoolTy::INSTANCE
      else
        raise CompileError.at(loc, "unsupported operator #{op} on Float64")
      end
    end

    private def check_field(expr : AST::FieldAccess) : Ty
      object = check_expr(expr.object)
      if object.float?
        return check_float_field(expr)
      end
      if object.int? || object.int64? || object.uint64?
        return check_int_field(expr, object)
      end
      if object.string?
        case expr.field
        when "size"
          return IntTy::INSTANCE
        when "to_f"
          expr.method_call = true
          return Float64Ty::INSTANCE
        when "to_i"
          expr.method_call = true
          return IntTy::INSTANCE
        else
          raise CompileError.at(expr.location, "String has no field #{expr.field} (did you mean size?)")
        end
      end
      if object.is_a?(ArrayTy)
        case expr.field
        when "size"
          return IntTy::INSTANCE
        when "sort"
          unless object.elem.int?
            raise CompileError.at(expr.location, "Array.sort is Int-only in Stage 7")
          end
          expr.method_call = true
          return VoidTy::INSTANCE
        when "pop"
          expr.method_call = true
          return object.elem
        when "clear"
          expr.method_call = true
          return VoidTy::INSTANCE
        else
          raise CompileError.at(expr.location, "#{object} has no field #{expr.field} (did you mean size?)")
        end
      end
      if object.is_a?(HashTy)
        unless expr.field == "size"
          raise CompileError.at(expr.location, "#{object} has no field #{expr.field} (did you mean size?)")
        end
        return IntTy::INSTANCE
      end
      if object.is_a?(BufTy)
        case expr.field
        when "size"
          return IntTy::INSTANCE
        when "to_s"
          expr.method_call = true
          return StringTy::INSTANCE
        when "clear"
          expr.method_call = true
          return VoidTy::INSTANCE
        else
          raise CompileError.at(expr.location, "Buf has no field #{expr.field}")
        end
      end
      if object.is_a?(JoinHandleTy)
        unless expr.field == "join"
          raise CompileError.at(expr.location, "JoinHandle has no field #{expr.field} (did you mean join?)")
        end
        expr.method_call = true
        return object.result
      end
      unless object.is_a?(AggTy)
        raise CompileError.at(expr.location, "cannot read field #{expr.field} on #{object}")
      end
      if field_ty = object.field_type(expr.field)
        if method_sig(object.name, expr.field)
          raise CompileError.at(expr.location, "#{object.name}.#{expr.field} is both a field and a method")
        end
        return field_ty
      end
      if sig = method_sig(object.name, expr.field)
        unless sig.params.empty?
          raise CompileError.at(expr.location, "#{object.name}.#{expr.field} takes #{sig.params.size} arguments")
        end
        expr.method_call = true
        return sig.return_type
      end
      raise CompileError.at(expr.location, "no field #{expr.field} on #{object.name}")
    end

    private def check_float_field(expr : AST::FieldAccess) : Ty
      case expr.field
      when "sqrt", "abs", "floor"
        expr.method_call = true
        Float64Ty::INSTANCE
      when "to_i"
        expr.method_call = true
        IntTy::INSTANCE
      else
        raise CompileError.at(expr.location, "Float64 has no field #{expr.field}")
      end
    end

    private def check_int_field(expr : AST::FieldAccess, object : Ty) : Ty
      case expr.field
      when "to_f"
        expr.method_call = true
        Float64Ty::INSTANCE
      when "abs"
        expr.method_call = true
        object
      when "to_i"
        expr.method_call = true
        IntTy::INSTANCE
      when "to_i64"
        expr.method_call = true
        Int64Ty::INSTANCE
      when "to_u64"
        expr.method_call = true
        UInt64Ty::INSTANCE
      else
        raise CompileError.at(expr.location, "#{object} has no field #{expr.field}")
      end
    end

    private def check_index(expr : AST::Index) : Ty
      array = check_expr(expr.array)
      if array.string?
        index = check_expr(expr.index, IntTy::INSTANCE)
        unless index.int?
          raise CompileError.at(expr.index.location, "string index must be Int, got #{index}")
        end
        return IntTy::INSTANCE
      end
      if array.is_a?(HashTy)
        key = check_expr(expr.index, array.key)
        unless assignable?(key, array.key)
          raise CompileError.at(expr.index.location, "hash key must be #{array.key}, got #{key}")
        end
        return UnionTy.nilable(array.val)
      end
      unless array.is_a?(ArrayTy)
        raise CompileError.at(expr.location, "cannot index #{array}")
      end
      index = check_expr(expr.index, IntTy::INSTANCE)
      unless index.int?
        raise CompileError.at(expr.index.location, "array index must be Int, got #{index}")
      end
      array.elem
    end

    private def check_struct_literal(expr : AST::StructLiteral) : Ty
      ty = @named[expr.type_name]? || raise CompileError.at(expr.location, "unknown type #{expr.type_name}")
      if ty.class?
        raise CompileError.at(expr.location, "classes are constructed with #{ty.name}.new, not #{ty.name} { }")
      end
      seen = Set(String).new
      expr.fields.each do |name, value|
        if seen.includes?(name)
          raise CompileError.at(value.location, "duplicate field #{name}")
        end
        seen << name
        field_ty = ty.field_type(name) || raise CompileError.at(value.location, "no field #{name} on #{ty.name}")
        got = check_expr(value, field_ty)
        unless assignable?(got, field_ty)
          raise CompileError.at(value.location, "field #{name} is #{got}, expected #{field_ty}")
        end
      end
      ty.fields.each do |name, _|
        unless seen.includes?(name)
          raise CompileError.at(expr.location, "missing field #{name}")
        end
      end
      ty
    end

    private def check_array_literal(expr : AST::ArrayLiteral, expected : Ty?) : Ty
      if expr.elements.empty?
        unless expected.is_a?(ArrayTy)
          raise CompileError.at(expr.location, "empty array needs a type, e.g. xs: Array(Int) = []")
        end
        return expected
      end
      elem_ty = if expected.is_a?(ArrayTy)
                  expected.elem
                else
                  check_expr(expr.elements[0])
                end
      expr.elements.each do |el|
        got = check_expr(el, elem_ty)
        unless assignable?(got, elem_ty)
          raise CompileError.at(el.location, "array element is #{got}, expected #{elem_ty}")
        end
      end
      ArrayTy.new(elem_ty)
    end

    private def check_array_new(expr : AST::ArrayNew) : Ty
      elem = @resolver.resolve(expr.elem_type)
      size = check_expr(expr.size, IntTy::INSTANCE)
      unless size.int?
        raise CompileError.at(expr.size.location, "Array(T).new expects Int size")
      end
      ArrayTy.new(elem)
    end

    private def check_call(expr : AST::Call) : Ty
      if recv = expr.receiver
        return check_method_call(expr, recv)
      end

      if expr.callee == "puts"
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "puts takes one argument")
        end
        if expr.block
          raise CompileError.at(expr.location, "puts does not take a block")
        end
        arg = check_expr(expr.args[0])
        unless arg.integer? || arg.string? || arg.float? || arg.bool?
          raise CompileError.at(expr.args[0].location, "puts cannot print #{arg}")
        end
        return VoidTy::INSTANCE
      end

      if expr.callee == "spawn"
        return check_spawn(expr)
      end

      if expr.callee == "now_ms"
        unless expr.args.empty? && expr.block.nil?
          raise CompileError.at(expr.location, "now_ms takes no arguments")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "file_read"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "file_read takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "file_read expects String")
        end
        return UnionTy.nilable(StringTy::INSTANCE)
      end

      if expr.callee == "file_write"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "file_write takes a path and a body")
        end
        path = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(path, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "file_write path must be String")
        end
        body = check_expr(expr.args[1], StringTy::INSTANCE)
        unless assignable?(body, StringTy::INSTANCE)
          raise CompileError.at(expr.args[1].location, "file_write body must be String")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "argv"
        unless expr.args.empty? && expr.block.nil?
          raise CompileError.at(expr.location, "argv takes no arguments")
        end
        return ArrayTy.new(StringTy::INSTANCE)
      end

      if expr.callee == "process_run"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "process_run takes a path and args")
        end
        path = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(path, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "process_run path must be String")
        end
        args_ty = ArrayTy.new(StringTy::INSTANCE)
        got = check_expr(expr.args[1], args_ty)
        unless assignable?(got, args_ty)
          raise CompileError.at(expr.args[1].location, "process_run args must be Array(String)")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "process_run_out"
        unless expr.args.size == 3 && expr.block.nil?
          raise CompileError.at(expr.location, "process_run_out takes a path, args, and an output path")
        end
        path = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(path, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "process_run_out path must be String")
        end
        args_ty = ArrayTy.new(StringTy::INSTANCE)
        got = check_expr(expr.args[1], args_ty)
        unless assignable?(got, args_ty)
          raise CompileError.at(expr.args[1].location, "process_run_out args must be Array(String)")
        end
        outp = check_expr(expr.args[2], StringTy::INSTANCE)
        unless assignable?(outp, StringTy::INSTANCE)
          raise CompileError.at(expr.args[2].location, "process_run_out output path must be String")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "env_get"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "env_get takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "env_get expects String")
        end
        return UnionTy.nilable(StringTy::INSTANCE)
      end

      if expr.callee == "file_exists"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "file_exists takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "file_exists expects String")
        end
        return BoolTy::INSTANCE
      end

      if expr.callee == "dir_list"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "dir_list takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "dir_list expects String")
        end
        return ArrayTy.new(StringTy::INSTANCE)
      end

      if expr.callee == "json_parse"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "json_parse takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "json_parse expects String")
        end
        return PtrTy.new(VoidTy::INSTANCE)
      end

      if expr.callee == "json_int"
        return check_json_get(expr, IntTy::INSTANCE)
      end

      if expr.callee == "json_str"
        return check_json_get(expr, StringTy::INSTANCE)
      end

      if expr.callee == "json_root"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "json_root takes a document")
        end
        got = check_expr(expr.args[0])
        unless got.ptr?
          raise CompileError.at(expr.args[0].location, "json_root expects Ptr(Void)")
        end
        return PtrTy.new(VoidTy::INSTANCE)
      end

      if expr.callee == "json_get"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "json_get takes a value and a key")
        end
        got = check_expr(expr.args[0])
        unless got.ptr?
          raise CompileError.at(expr.args[0].location, "json_get expects Ptr(Void)")
        end
        key = check_expr(expr.args[1], StringTy::INSTANCE)
        unless assignable?(key, StringTy::INSTANCE)
          raise CompileError.at(expr.args[1].location, "key must be String")
        end
        return UnionTy.nilable(PtrTy.new(VoidTy::INSTANCE))
      end

      if expr.callee == "json_len"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "json_len takes an array value")
        end
        got = check_expr(expr.args[0])
        unless got.ptr?
          raise CompileError.at(expr.args[0].location, "json_len expects Ptr(Void)")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "json_at"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "json_at takes an array and an index")
        end
        got = check_expr(expr.args[0])
        unless got.ptr?
          raise CompileError.at(expr.args[0].location, "json_at expects Ptr(Void)")
        end
        idx = check_expr(expr.args[1], IntTy::INSTANCE)
        unless idx.int?
          raise CompileError.at(expr.args[1].location, "index must be Int")
        end
        return UnionTy.nilable(PtrTy.new(VoidTy::INSTANCE))
      end

      if expr.callee == "json_f64"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "json_f64 takes an object and a key")
        end
        got = check_expr(expr.args[0])
        unless got.ptr?
          raise CompileError.at(expr.args[0].location, "json_f64 expects Ptr(Void)")
        end
        key = check_expr(expr.args[1], StringTy::INSTANCE)
        unless assignable?(key, StringTy::INSTANCE)
          raise CompileError.at(expr.args[1].location, "key must be String")
        end
        return UnionTy.nilable(Float64Ty::INSTANCE)
      end

      if expr.callee == "json_sum_f64"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "json_sum_f64 takes an array and a key")
        end
        got = check_expr(expr.args[0])
        unless got.ptr?
          raise CompileError.at(expr.args[0].location, "json_sum_f64 expects Ptr(Void)")
        end
        key = check_expr(expr.args[1], StringTy::INSTANCE)
        unless assignable?(key, StringTy::INSTANCE)
          raise CompileError.at(expr.args[1].location, "key must be String")
        end
        return Float64Ty::INSTANCE
      end

      if expr.callee == "chr"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "chr takes one Int")
        end
        got = check_expr(expr.args[0], IntTy::INSTANCE)
        unless got.int?
          raise CompileError.at(expr.args[0].location, "chr expects Int")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "repeat_byte"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "repeat_byte takes a byte and a count")
        end
        b = check_expr(expr.args[0], IntTy::INSTANCE)
        n = check_expr(expr.args[1], IntTy::INSTANCE)
        unless b.int?
          raise CompileError.at(expr.args[0].location, "repeat_byte expects Int byte")
        end
        unless n.int?
          raise CompileError.at(expr.args[1].location, "repeat_byte expects Int count")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "bytes_to_str"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "bytes_to_str takes Array(Int)")
        end
        got = check_expr(expr.args[0])
        unless got.is_a?(ArrayTy) && got.elem.int?
          raise CompileError.at(expr.args[0].location, "bytes_to_str expects Array(Int)")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "str_slice"
        unless expr.args.size == 3 && expr.block.nil?
          raise CompileError.at(expr.location, "str_slice takes String, start, stop")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "str_slice expects String")
        end
        start = check_expr(expr.args[1], IntTy::INSTANCE)
        stop = check_expr(expr.args[2], IntTy::INSTANCE)
        unless start.int?
          raise CompileError.at(expr.args[1].location, "start must be Int")
        end
        unless stop.int?
          raise CompileError.at(expr.args[2].location, "stop must be Int")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "fmt_float"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "fmt_float takes Float64 and Int")
        end
        got = check_expr(expr.args[0])
        unless got.numeric?
          raise CompileError.at(expr.args[0].location, "fmt_float expects Float64")
        end
        unless got.float?
          expr.args[0].type = Float64Ty::INSTANCE
        end
        prec = check_expr(expr.args[1], IntTy::INSTANCE)
        unless prec.int?
          raise CompileError.at(expr.args[1].location, "precision must be Int")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "buf_new"
        unless expr.block.nil?
          raise CompileError.at(expr.location, "buf_new does not take a block")
        end
        if expr.args.empty?
          return BufTy::INSTANCE
        end
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "buf_new takes an optional Int capacity")
        end
        cap = check_expr(expr.args[0], IntTy::INSTANCE)
        unless cap.int?
          raise CompileError.at(expr.args[0].location, "capacity must be Int")
        end
        return BufTy::INSTANCE
      end

      if expr.callee == "str_to_f_slice"
        unless expr.args.size == 3 && expr.block.nil?
          raise CompileError.at(expr.location, "str_to_f_slice takes String, start, stop")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "str_to_f_slice expects String")
        end
        start = check_expr(expr.args[1], IntTy::INSTANCE)
        stop = check_expr(expr.args[2], IntTy::INSTANCE)
        unless start.int? && stop.int?
          raise CompileError.at(expr.location, "start and stop must be Int")
        end
        return Float64Ty::INSTANCE
      end

      if expr.callee == "json_free"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "json_free takes a document")
        end
        got = check_expr(expr.args[0])
        unless got.ptr?
          raise CompileError.at(expr.args[0].location, "json_free expects Ptr(Void)")
        end
        return VoidTy::INSTANCE
      end

      if expr.callee == "json_gen_body"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "json_gen_body takes Int")
        end
        n = check_expr(expr.args[0], IntTy::INSTANCE)
        unless n.int?
          raise CompileError.at(expr.args[0].location, "json_gen_body expects Int")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "json_gen_into"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "json_gen_into takes Buf and Int")
        end
        buf = check_expr(expr.args[0])
        unless buf.buf?
          raise CompileError.at(expr.args[0].location, "json_gen_into expects Buf")
        end
        n = check_expr(expr.args[1], IntTy::INSTANCE)
        unless n.int?
          raise CompileError.at(expr.args[1].location, "json_gen_into expects Int")
        end
        return VoidTy::INSTANCE
      end

      if expr.callee == "b64_encode" || expr.callee == "b64_decode"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "#{expr.callee} takes one String")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "#{expr.callee} expects String")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "b64_encode_buf" || expr.callee == "b64_decode_buf"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "#{expr.callee} takes Buf and String")
        end
        buf = check_expr(expr.args[0])
        unless buf.buf?
          raise CompileError.at(expr.args[0].location, "#{expr.callee} expects Buf")
        end
        s = check_expr(expr.args[1], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[1].location, "#{expr.callee} expects String")
        end
        return VoidTy::INSTANCE
      end

      if expr.callee == "crc32_bytes" || expr.callee == "sha256_word0"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "#{expr.callee} takes Array(Int)")
        end
        got = check_expr(expr.args[0])
        unless got.is_a?(ArrayTy) && got.elem.int?
          raise CompileError.at(expr.args[0].location, "#{expr.callee} expects Array(Int)")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "re_compile"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "re_compile takes a pattern and flags")
        end
        pat = check_expr(expr.args[0], StringTy::INSTANCE)
        flags = check_expr(expr.args[1], IntTy::INSTANCE)
        unless assignable?(pat, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "pattern must be String")
        end
        unless flags.int?
          raise CompileError.at(expr.args[1].location, "flags must be Int")
        end
        return PtrTy.new(VoidTy::INSTANCE)
      end

      if expr.callee == "re_find"
        unless expr.args.size == 3 && expr.block.nil?
          raise CompileError.at(expr.location, "re_find takes regex, string, and start")
        end
        re = check_expr(expr.args[0])
        unless re.ptr?
          raise CompileError.at(expr.args[0].location, "re_find expects Ptr(Void)")
        end
        s = check_expr(expr.args[1], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[1].location, "subject must be String")
        end
        from = check_expr(expr.args[2], IntTy::INSTANCE)
        unless from.int?
          raise CompileError.at(expr.args[2].location, "start must be Int")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "re_m0" || expr.callee == "re_m1" || expr.callee == "re_c0" || expr.callee == "re_c1"
        unless expr.args.empty? && expr.block.nil?
          raise CompileError.at(expr.location, "#{expr.callee} takes no arguments")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "re_count"
        unless expr.args.size == 3 && expr.block.nil?
          raise CompileError.at(expr.location, "re_count takes pattern, text, and caseless")
        end
        pat = check_expr(expr.args[0], StringTy::INSTANCE)
        text = check_expr(expr.args[1], StringTy::INSTANCE)
        flags = check_expr(expr.args[2], IntTy::INSTANCE)
        unless assignable?(pat, StringTy::INSTANCE) && assignable?(text, StringTy::INSTANCE)
          raise CompileError.at(expr.location, "re_count expects String pattern and text")
        end
        unless flags.int?
          raise CompileError.at(expr.args[2].location, "caseless must be Int")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "hash_last_key"
        unless expr.args.empty? && expr.block.nil?
          raise CompileError.at(expr.location, "hash_last_key takes no arguments")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "zlib_compress" || expr.callee == "zlib_uncompress" || expr.callee == "sha256"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "#{expr.callee} takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "#{expr.callee} expects String")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "now_us"
        unless expr.args.empty? && expr.block.nil?
          raise CompileError.at(expr.location, "now_us takes no arguments")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "exp"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "exp takes one Float64")
        end
        got = check_expr(expr.args[0])
        unless got.numeric?
          raise CompileError.at(expr.args[0].location, "exp expects Float64")
        end
        unless got.float?
          expr.args[0].type = Float64Ty::INSTANCE
        end
        return Float64Ty::INSTANCE
      end

      if expr.callee == "http_roundtrip"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "http_roundtrip takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "http_roundtrip expects String")
        end
        return StringTy::INSTANCE
      end

      if expr.callee == "sqrt"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "sqrt takes one Float64")
        end
        got = check_expr(expr.args[0])
        unless got.numeric?
          raise CompileError.at(expr.args[0].location, "sqrt expects Float64")
        end
        unless got.float?
          expr.args[0].type = Float64Ty::INSTANCE
        end
        return Float64Ty::INSTANCE
      end

      if expr.callee == "wide_op"
        unless expr.args.size == 5 && expr.block.nil?
          raise CompileError.at(expr.location, "wide_op takes five Ints")
        end
        expr.args.each do |arg|
          got = check_expr(arg, IntTy::INSTANCE)
          unless got.int?
            raise CompileError.at(arg.location, "wide_op expects Int")
          end
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "wide_hi"
        unless expr.args.empty? && expr.block.nil?
          raise CompileError.at(expr.location, "wide_hi takes no arguments")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "checksum_f64"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "checksum_f64 takes one Float64")
        end
        got = check_expr(expr.args[0])
        unless got.numeric?
          raise CompileError.at(expr.args[0].location, "checksum_f64 expects Float64")
        end
        unless got.float?
          expr.args[0].type = Float64Ty::INSTANCE
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "checksum_str"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "checksum_str takes one String")
        end
        got = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(got, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "checksum_str expects String")
        end
        return IntTy::INSTANCE
      end

      if expr.callee == "puts_u"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "puts_u takes one Int")
        end
        got = check_expr(expr.args[0], IntTy::INSTANCE)
        unless got.int?
          raise CompileError.at(expr.args[0].location, "puts_u expects Int")
        end
        return VoidTy::INSTANCE
      end

      sig = @functions[expr.callee]?
      unless sig
        raise CompileError.at(expr.location, "unknown function #{expr.callee}")
      end
      unless expr.args.size == sig.params.size
        raise CompileError.at(expr.location, "#{expr.callee} takes #{sig.params.size} arguments, got #{expr.args.size}")
      end
      expr.args.each_with_index do |arg, i|
        got = check_expr(arg, sig.params[i])
        unless assignable?(got, sig.params[i])
          raise CompileError.at(arg.location, "argument #{i + 1} of #{expr.callee} is #{got}, expected #{sig.params[i]}")
        end
      end
      sig.return_type
    end

    private def check_method_call(expr : AST::Call, recv : AST::Expr) : Ty
      if recv.is_a?(AST::Name) && lookup(recv.ident).nil?
        if funs = @libs[recv.ident]?
          return check_lib_call(expr, recv.ident, funs)
        end
      end

      if expr.callee == "new"
        return check_constructor(expr, recv)
      end

      object = check_expr(recv)
      if object.float?
        return check_float_method(expr, object)
      end
      if object.int? || object.int64? || object.uint64?
        return check_int_method(expr, object)
      end
      if object.string?
        return check_string_method(expr)
      end
      if object.is_a?(ArrayTy)
        return check_array_method(expr, object)
      end
      if object.is_a?(HashTy)
        return check_hash_method(expr, object)
      end
      if object.is_a?(BufTy)
        return check_buf_method(expr)
      end
      if object.is_a?(JoinHandleTy)
        return check_join(expr, object)
      end
      unless object.is_a?(AggTy)
        raise CompileError.at(expr.location, "cannot call #{expr.callee} on #{object}")
      end
      if object.field_type(expr.callee)
        raise CompileError.at(expr.location, "#{object.name}.#{expr.callee} is a field, not a method")
      end
      sig = method_sig(object.name, expr.callee)
      unless sig
        raise CompileError.at(expr.location, "no method #{expr.callee} on #{object.name}")
      end
      check_args(expr, sig.params, "#{object.name}.#{expr.callee}")
      sig.return_type
    end

    private def check_float_method(expr : AST::Call, _object : Ty) : Ty
      if expr.block
        raise CompileError.at(expr.location, "Float64.#{expr.callee} does not take a block")
      end
      case expr.callee
      when "sqrt", "abs", "floor", "exp"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "Float64.#{expr.callee} takes no arguments")
        end
        Float64Ty::INSTANCE
      when "to_i"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "Float64.to_i takes no arguments")
        end
        IntTy::INSTANCE
      else
        raise CompileError.at(expr.location, "no method #{expr.callee} on Float64")
      end
    end

    private def check_int_method(expr : AST::Call, object : Ty) : Ty
      if expr.block
        raise CompileError.at(expr.location, "#{object}.#{expr.callee} does not take a block")
      end
      case expr.callee
      when "to_f"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "#{object}.to_f takes no arguments")
        end
        Float64Ty::INSTANCE
      when "abs"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "#{object}.abs takes no arguments")
        end
        object
      when "to_i"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "#{object}.to_i takes no arguments")
        end
        IntTy::INSTANCE
      when "to_i64"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "#{object}.to_i64 takes no arguments")
        end
        Int64Ty::INSTANCE
      when "to_u64"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "#{object}.to_u64 takes no arguments")
        end
        UInt64Ty::INSTANCE
      else
        raise CompileError.at(expr.location, "no method #{expr.callee} on #{object}")
      end
    end

    private def check_string_method(expr : AST::Call) : Ty
      if expr.block
        raise CompileError.at(expr.location, "String.#{expr.callee} does not take a block")
      end
      case expr.callee
      when "to_f"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "String.to_f takes no arguments")
        end
        Float64Ty::INSTANCE
      when "to_i"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "String.to_i takes no arguments")
        end
        IntTy::INSTANCE
      else
        raise CompileError.at(expr.location, "no method #{expr.callee} on String")
      end
    end

    private def check_lib_call(expr : AST::Call, lib_name : String, funs : Hash(String, FunSig)) : Ty
      sig = funs[expr.callee]?
      unless sig
        raise CompileError.at(expr.location, "unknown function #{lib_name}.#{expr.callee}")
      end
      check_args(expr, sig.params, "#{lib_name}.#{expr.callee}")
      expr.lib_name = lib_name
      sig.return_type
    end

    private def check_constructor(expr : AST::Call, recv : AST::Expr) : Ty
      unless recv.is_a?(AST::Name)
        raise CompileError.at(expr.location, "new is a type constructor")
      end
      ty = @named[recv.ident]? || raise CompileError.at(recv.location, "unknown type #{recv.ident}")
      unless ty.class?
        raise CompileError.at(expr.location, "structs are constructed with #{ty.name} { fields }, not .new")
      end
      recv.type = ty
      if sig = method_sig(ty.name, "initialize")
        check_args(expr, sig.params, "#{ty.name}.new")
      else
        unless expr.args.empty?
          raise CompileError.at(expr.location, "#{ty.name}.new takes no arguments")
        end
      end
      ty
    end

    private def check_args(expr : AST::Call, params : Array(Ty), label : String) : Nil
      unless expr.args.size == params.size
        raise CompileError.at(expr.location, "#{label} takes #{params.size} arguments, got #{expr.args.size}")
      end
      expr.args.each_with_index do |arg, i|
        got = check_expr(arg, params[i])
        unless assignable?(got, params[i])
          raise CompileError.at(arg.location, "argument #{i + 1} of #{label} is #{got}, expected #{params[i]}")
        end
      end
    end

    private def method_sig(owner : String, name : String) : MethodSig?
      @methods[owner]?.try(&.[name]?)
    end

    private def implicit_field(name : String) : Ty?
      @self_ty.try(&.field_type(name))
    end

    private def lookup(name : String) : Ty?
      @scopes.reverse_each do |scope|
        return scope[name] if scope.has_key?(name)
      end
      nil
    end

    private def bind(name : String, ty : Ty) : Nil
      @scopes.last[name] = ty
    end

    private def push_scope : Nil
      @scopes << {} of String => Ty
    end

    private def pop_scope : Nil
      @scopes.pop
    end

    private def assignable?(got : Ty, want : Ty) : Bool
      return true if got.same?(want)
      if want.is_a?(UnionTy) && want.includes?(got)
        return true
      end
      if got.is_a?(UnionTy) && want.is_a?(UnionTy)
        return got.members.all? { |m| want.includes?(m) }
      end
      false
    end

    private def check_switch(expr : AST::SwitchExpr, expected : Ty?) : Ty
      cond = check_expr(expr.cond)
      unless cond.int? || cond.bool?
        raise CompileError.at(expr.cond.location, "switch discriminant must be Int or Bool, got #{cond}")
      end
      seen = Set(Int64).new
      expr.cases.each do |arm|
        if arm.labels.empty?
          raise CompileError.at(arm.location, "case needs a label")
        end
        arm.labels.each do |label|
          if seen.includes?(label)
            raise CompileError.at(arm.location, "duplicate case #{label}")
          end
          seen << label
        end
      end

      branch_tys = [] of Ty
      expr.cases.each do |arm|
        branch_tys << check_switch_branch(arm.body, expected)
      end
      if else_body = expr.else_body
        branch_tys << check_switch_branch(else_body, expected)
      end

      if branch_tys.all?(&.void?)
        return VoidTy::INSTANCE
      end
      if branch_tys.any?(&.void?)
        raise CompileError.at(expr.location, "switch cases must all produce a value, or none")
      end
      unless expr.else_body
        raise CompileError.at(expr.location, "switch expression needs else")
      end

      if expected && !expected.void?
        branch_tys.each do |ty|
          unless assignable?(ty, expected)
            raise CompileError.at(expr.location, "switch branch is #{ty}, expected #{expected}")
          end
        end
        return expected
      end

      result = branch_tys[0]
      branch_tys.each do |ty|
        unless ty.same?(result)
          raise CompileError.at(expr.location, "switch branches have different types #{result} and #{ty}")
        end
      end
      result
    end

    private def check_switch_branch(body : Array(AST::Stmt), expected : Ty?) : Ty
      push_scope
      if body.empty?
        pop_scope
        return VoidTy::INSTANCE
      end
      last = body.last
      body.each_with_index do |stmt, i|
        if i == body.size - 1
          if last.is_a?(AST::ExprStmt)
            want = expected && !expected.void? ? expected : nil
            ty = check_expr(last.expr, want)
            pop_scope
            return ty
          else
            check_stmt(stmt, VoidTy::INSTANCE, false)
            pop_scope
            return VoidTy::INSTANCE
          end
        else
          check_stmt(stmt, VoidTy::INSTANCE, false)
        end
      end
      pop_scope
      VoidTy::INSTANCE
    end

    private def check_if_assign(stmt : AST::IfStmt, bind_name : String, expected : Ty, at_tail : Bool) : Nil
      got = check_expr(stmt.cond)
      success = if_assign_success(got, stmt.cond.location)
      push_scope
      bind(bind_name, success)
      check_body(stmt.then_body, expected, at_tail: false)
      pop_scope
      check_block(stmt.else_body, expected, at_tail: false) unless stmt.else_body.empty?
      if at_tail && !expected.void?
        unless !stmt.else_body.empty? && !fallthrough?(stmt.then_body) && !fallthrough?(stmt.else_body)
          raise CompileError.at(stmt.location, "missing return value")
        end
      end
    end

    private def if_assign_success(got : Ty, loc : Location) : Ty
      unless got.is_a?(UnionTy) && got.includes_nil?
        raise CompileError.at(loc, "if-assign expects T?, got #{got}")
      end
      success = got.without_nil
      if success.is_a?(UnionTy)
        raise CompileError.at(loc, "if-assign needs a single success type, got #{got}")
      end
      success
    end

    private def check_try(expr : AST::Try) : Ty
      got = check_expr(expr.expr)
      success, _fails = split_try(got, expr.location)
      success
    end

    private def split_try(got : Ty, loc : Location) : {Ty, Array(Ty)}
      unless got.is_a?(UnionTy)
        raise CompileError.at(loc, "postfix ? expects a union, got #{got}")
      end
      if got.includes_nil?
        success = got.without_nil
        return {success, [NilTy::INSTANCE.as(Ty)]}
      end
      ret = @return_type
      unless ret.is_a?(UnionTy)
        raise CompileError.at(loc, "postfix ? needs the function to return a union that includes the failure")
      end
      fails = got.members.select { |m| ret.as(UnionTy).includes?(m) }
      succ = got.members.reject { |m| fails.any?(&.same?(m)) }
      if succ.size != 1 || fails.empty?
        raise CompileError.at(loc, "cannot use postfix ? on #{got} in a function returning #{ret}")
      end
      {succ[0], fails}
    end

    private def check_interp(expr : AST::InterpString) : Ty
      expr.parts.each do |part|
        if inner = part.expr
          ty = check_expr(inner)
          unless ty.integer? || ty.string? || ty.float? || ty.bool?
            raise CompileError.at(inner.location, "cannot interpolate #{ty}")
          end
        end
      end
      StringTy::INSTANCE
    end

    private def check_hash_new(expr : AST::HashNew) : Ty
      key = @resolver.resolve_value(expr.key_type)
      val = @resolver.resolve_value(expr.val_type)
      unless key.string? || key.int?
        raise CompileError.at(expr.location, "Hash keys must be String or Int")
      end
      unless val.int? || val.string?
        raise CompileError.at(expr.location, "Hash values must be Int or String")
      end
      if key.int? && val.string?
        raise CompileError.at(expr.location, "Hash(Int, String) is not in this stage")
      end
      HashTy.new(key, val)
    end

    private def check_spawn(expr : AST::Call) : Ty
      unless expr.args.empty?
        raise CompileError.at(expr.location, "spawn takes a block, not arguments")
      end
      block = expr.block || raise CompileError.at(expr.location, "spawn needs a block")
      unless block.params.empty?
        raise CompileError.at(block.location, "spawn { } does not take block parameters; use items.parallel")
      end
      outer = snapshot_bindings
      result = check_block_value(block, {} of String => Ty)
      unless result.int?
        raise CompileError.at(block.location, "spawn result must be Int")
      end
      expr.spawn_captures = collect_spawn_captures(block.body, outer)
      JoinHandleTy.new(result)
    end

    private def snapshot_bindings : Hash(String, Ty)
      merged = {} of String => Ty
      @scopes.each do |scope|
        scope.each { |name, ty| merged[name] = ty }
      end
      merged
    end

    private def copyable_for_spawn?(ty : Ty) : Bool
      ty.int? || ty.int64? || ty.uint64? || ty.bool? || ty.float?
    end

    private def collect_spawn_captures(body : Array(AST::Stmt), outer : Hash(String, Ty)) : Array({String, Ty})
      captures = [] of {String, Ty}
      seen = Set(String).new
      walk_spawn_stmts(body, outer, captures, seen)
      captures
    end

    private def walk_spawn_stmts(body : Array(AST::Stmt), outer : Hash(String, Ty), captures : Array({String, Ty}), seen : Set(String)) : Nil
      body.each { |s| walk_spawn_stmt(s, outer, captures, seen) }
    end

    private def walk_spawn_stmt(stmt : AST::Stmt, outer : Hash(String, Ty), captures : Array({String, Ty}), seen : Set(String)) : Nil
      case stmt
      when AST::ReturnStmt
        walk_spawn_expr(stmt.expr, outer, captures, seen)
      when AST::IfStmt
        walk_spawn_expr(stmt.cond, outer, captures, seen)
        walk_spawn_stmts(stmt.then_body, outer, captures, seen)
        walk_spawn_stmts(stmt.else_body, outer, captures, seen)
      when AST::WhileStmt
        walk_spawn_expr(stmt.cond, outer, captures, seen)
        walk_spawn_stmts(stmt.body, outer, captures, seen)
      when AST::AssignStmt
        if target = stmt.target.as?(AST::Name)
          if outer.has_key?(target.ident)
            raise CompileError.at(target.location, "spawn cannot assign to #{target.ident}; copy at send is read-only (D35)")
          end
        else
          walk_spawn_expr(stmt.target, outer, captures, seen)
        end
        walk_spawn_expr(stmt.value, outer, captures, seen)
      when AST::ExprStmt
        walk_spawn_expr(stmt.expr, outer, captures, seen)
      when AST::BreakStmt, AST::ContinueStmt
      end
    end

    private def walk_spawn_expr(expr : AST::Expr?, outer : Hash(String, Ty), captures : Array({String, Ty}), seen : Set(String)) : Nil
      return unless expr
      case expr
      when AST::Name
        note_spawn_capture(expr, outer, captures, seen)
      when AST::Call
        walk_spawn_expr(expr.receiver, outer, captures, seen)
        expr.args.each { |a| walk_spawn_expr(a, outer, captures, seen) }
        if b = expr.block
          inner = outer.dup
          b.params.each { |p| inner.delete(p) }
          walk_spawn_stmts(b.body, inner, captures, seen)
        end
      when AST::Unary
        walk_spawn_expr(expr.expr, outer, captures, seen)
      when AST::Binary
        walk_spawn_expr(expr.left, outer, captures, seen)
        walk_spawn_expr(expr.right, outer, captures, seen)
      when AST::FieldAccess
        walk_spawn_expr(expr.object, outer, captures, seen)
      when AST::Index
        walk_spawn_expr(expr.array, outer, captures, seen)
        walk_spawn_expr(expr.index, outer, captures, seen)
      when AST::StructLiteral
        expr.fields.each { |_, v| walk_spawn_expr(v, outer, captures, seen) }
      when AST::ArrayLiteral
        expr.elements.each { |e| walk_spawn_expr(e, outer, captures, seen) }
      when AST::ArrayNew
        walk_spawn_expr(expr.size, outer, captures, seen)
      when AST::Try
        walk_spawn_expr(expr.expr, outer, captures, seen)
      when AST::InterpString
        expr.parts.each { |p| walk_spawn_expr(p.expr, outer, captures, seen) }
      when AST::SwitchExpr
        walk_spawn_expr(expr.cond, outer, captures, seen)
        expr.cases.each { |arm| walk_spawn_stmts(arm.body, outer, captures, seen) }
        if else_body = expr.else_body
          walk_spawn_stmts(else_body, outer, captures, seen)
        end
      end
    end

    private def note_spawn_capture(expr : AST::Name, outer : Hash(String, Ty), captures : Array({String, Ty}), seen : Set(String)) : Nil
      return unless ty = outer[expr.ident]?
      unless copyable_for_spawn?(ty)
        raise CompileError.at(expr.location, "spawn cannot capture #{expr.ident}: #{ty} is not copied at send (D35)")
      end
      return if seen.includes?(expr.ident)
      seen << expr.ident
      captures << {expr.ident, ty}
    end

    private def check_json_get(expr : AST::Call, val : Ty) : Ty
      unless expr.args.size == 2 && expr.block.nil?
        raise CompileError.at(expr.location, "#{expr.callee} takes a document and a key")
      end
      doc = check_expr(expr.args[0])
      unless doc.ptr?
        raise CompileError.at(expr.args[0].location, "#{expr.callee} expects Ptr(Void)")
      end
      key = check_expr(expr.args[1], StringTy::INSTANCE)
      unless assignable?(key, StringTy::INSTANCE)
        raise CompileError.at(expr.args[1].location, "key must be String")
      end
      UnionTy.nilable(val)
    end

    private def check_join(expr : AST::Call, handle : JoinHandleTy) : Ty
      unless expr.callee == "join"
        raise CompileError.at(expr.location, "no method #{expr.callee} on JoinHandle")
      end
      unless expr.args.empty? && expr.block.nil?
        raise CompileError.at(expr.location, "join takes no arguments")
      end
      handle.result
    end

    private def check_array_method(expr : AST::Call, array : ArrayTy) : Ty
      case expr.callee
      when "push"
        if expr.block
          raise CompileError.at(expr.location, "push does not take a block")
        end
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "push takes one argument")
        end
        got = check_expr(expr.args[0], array.elem)
        unless assignable?(got, array.elem)
          raise CompileError.at(expr.args[0].location, "push expects #{array.elem}, got #{got}")
        end
        VoidTy::INSTANCE
      when "sort"
        if expr.block
          raise CompileError.at(expr.location, "sort does not take a block")
        end
        unless expr.args.empty?
          raise CompileError.at(expr.location, "sort takes no arguments")
        end
        unless array.elem.int?
          raise CompileError.at(expr.location, "Array.sort is Int-only in Stage 7")
        end
        VoidTy::INSTANCE
      when "pop"
        if expr.block
          raise CompileError.at(expr.location, "pop does not take a block")
        end
        unless expr.args.empty?
          raise CompileError.at(expr.location, "pop takes no arguments")
        end
        array.elem
      when "fill"
        if expr.block
          raise CompileError.at(expr.location, "fill does not take a block")
        end
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "fill takes one argument")
        end
        unless array.elem.int?
          raise CompileError.at(expr.location, "Array.fill is Int-only")
        end
        got = check_expr(expr.args[0], IntTy::INSTANCE)
        unless got.int?
          raise CompileError.at(expr.args[0].location, "fill expects Int")
        end
        VoidTy::INSTANCE
      when "clear"
        if expr.block
          raise CompileError.at(expr.location, "clear does not take a block")
        end
        unless expr.args.empty?
          raise CompileError.at(expr.location, "clear takes no arguments")
        end
        VoidTy::INSTANCE
      when "reserve"
        if expr.block
          raise CompileError.at(expr.location, "reserve does not take a block")
        end
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "reserve takes one Int")
        end
        n = check_expr(expr.args[0], IntTy::INSTANCE)
        unless n.int?
          raise CompileError.at(expr.args[0].location, "reserve expects Int")
        end
        VoidTy::INSTANCE
      when "each"
        block = expr.block || raise CompileError.at(expr.location, "each needs a block")
        unless expr.args.empty?
          raise CompileError.at(expr.location, "each takes a block, not arguments")
        end
        unless block.params.size == 1
          raise CompileError.at(block.location, "each { |x| ... } takes one block parameter")
        end
        check_block_void(block, {block.params[0] => array.elem})
        VoidTy::INSTANCE
      when "parallel"
        block = expr.block || raise CompileError.at(expr.location, "parallel needs a block")
        unless expr.args.empty?
          raise CompileError.at(expr.location, "parallel takes a block, not arguments")
        end
        unless block.params.size == 1
          raise CompileError.at(block.location, "parallel { |x| ... } takes one block parameter")
        end
        result = check_block_value(block, {block.params[0] => array.elem})
        ArrayTy.new(result)
      else
        raise CompileError.at(expr.location, "no method #{expr.callee} on Array")
      end
    end

    private def check_hash_method(expr : AST::Call, hash : HashTy) : Ty
      case expr.callee
      when "get"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "get takes one key")
        end
        got = check_expr(expr.args[0], hash.key)
        unless assignable?(got, hash.key)
          raise CompileError.at(expr.args[0].location, "hash key must be #{hash.key}, got #{got}")
        end
        return UnionTy.nilable(hash.val)
      when "delete"
        unless expr.args.size == 1 && expr.block.nil?
          raise CompileError.at(expr.location, "delete takes one key")
        end
        got = check_expr(expr.args[0], hash.key)
        unless assignable?(got, hash.key)
          raise CompileError.at(expr.args[0].location, "hash key must be #{hash.key}, got #{got}")
        end
        return VoidTy::INSTANCE
      when "inc_slice"
        unless expr.args.size == 3 && expr.block.nil?
          raise CompileError.at(expr.location, "inc_slice takes string, start, stop")
        end
        unless hash.key.string? && hash.val.int?
          raise CompileError.at(expr.location, "inc_slice is Hash(String, Int) only")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "inc_slice expects String")
        end
        start = check_expr(expr.args[1], IntTy::INSTANCE)
        stop = check_expr(expr.args[2], IntTy::INSTANCE)
        unless start.int? && stop.int?
          raise CompileError.at(expr.location, "start and stop must be Int")
        end
        return IntTy::INSTANCE
      when "get_slice"
        unless expr.args.size == 3 && expr.block.nil?
          raise CompileError.at(expr.location, "get_slice takes string, start, stop")
        end
        unless hash.key.string? && hash.val.string?
          raise CompileError.at(expr.location, "get_slice is Hash(String, String) only")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "get_slice expects String")
        end
        start = check_expr(expr.args[1], IntTy::INSTANCE)
        stop = check_expr(expr.args[2], IntTy::INSTANCE)
        unless start.int? && stop.int?
          raise CompileError.at(expr.location, "start and stop must be Int")
        end
        return UnionTy.nilable(StringTy::INSTANCE)
      when "get_concat"
        unless expr.args.size == 2 && expr.block.nil?
          raise CompileError.at(expr.location, "get_concat takes two Strings")
        end
        unless hash.key.string? && hash.val.int?
          raise CompileError.at(expr.location, "get_concat is Hash(String, Int) only")
        end
        a = check_expr(expr.args[0], StringTy::INSTANCE)
        b = check_expr(expr.args[1], StringTy::INSTANCE)
        unless assignable?(a, StringTy::INSTANCE) && assignable?(b, StringTy::INSTANCE)
          raise CompileError.at(expr.location, "get_concat expects String keys")
        end
        return UnionTy.nilable(IntTy::INSTANCE)
      else
        raise CompileError.at(expr.location, "no method #{expr.callee} on Hash")
      end
    end

    private def check_buf_method(expr : AST::Call) : Ty
      if expr.block
        raise CompileError.at(expr.location, "Buf.#{expr.callee} does not take a block")
      end
      case expr.callee
      when "push_byte"
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "push_byte takes one Int")
        end
        b = check_expr(expr.args[0], IntTy::INSTANCE)
        unless b.int?
          raise CompileError.at(expr.args[0].location, "byte must be Int")
        end
        VoidTy::INSTANCE
      when "push_str"
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "push_str takes one String")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "push_str expects String")
        end
        VoidTy::INSTANCE
      when "push_slice"
        unless expr.args.size == 3
          raise CompileError.at(expr.location, "push_slice takes string, start, stop")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "push_slice expects String")
        end
        start = check_expr(expr.args[1], IntTy::INSTANCE)
        stop = check_expr(expr.args[2], IntTy::INSTANCE)
        unless start.int? && stop.int?
          raise CompileError.at(expr.location, "start and stop must be Int")
        end
        VoidTy::INSTANCE
      when "push_fmt_f"
        unless expr.args.size == 2
          raise CompileError.at(expr.location, "push_fmt_f takes Float64 and Int")
        end
        v = check_expr(expr.args[0])
        unless v.numeric?
          raise CompileError.at(expr.args[0].location, "push_fmt_f expects Float64")
        end
        unless v.float?
          expr.args[0].type = Float64Ty::INSTANCE
        end
        prec = check_expr(expr.args[1], IntTy::INSTANCE)
        unless prec.int?
          raise CompileError.at(expr.args[1].location, "precision must be Int")
        end
        VoidTy::INSTANCE
      when "push_fmt_i"
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "push_fmt_i takes one Int")
        end
        n = check_expr(expr.args[0], IntTy::INSTANCE)
        unless n.int?
          raise CompileError.at(expr.args[0].location, "push_fmt_i expects Int")
        end
        VoidTy::INSTANCE
      when "to_s"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "to_s takes no arguments")
        end
        StringTy::INSTANCE
      when "clear"
        unless expr.args.empty?
          raise CompileError.at(expr.location, "clear takes no arguments")
        end
        VoidTy::INSTANCE
      when "starts_with"
        unless expr.args.size == 1
          raise CompileError.at(expr.location, "starts_with takes one String")
        end
        s = check_expr(expr.args[0], StringTy::INSTANCE)
        unless assignable?(s, StringTy::INSTANCE)
          raise CompileError.at(expr.args[0].location, "starts_with expects String")
        end
        IntTy::INSTANCE
      else
        raise CompileError.at(expr.location, "no method #{expr.callee} on Buf")
      end
    end

    private def check_block_void(block : AST::Block, params : Hash(String, Ty)) : Nil
      push_scope
      params.each { |name, ty| bind(name, ty) }
      check_body(block.body, VoidTy::INSTANCE, at_tail: false)
      pop_scope
      block.value_type = VoidTy::INSTANCE
    end

    private def check_block_value(block : AST::Block, params : Hash(String, Ty)) : Ty
      if block.body.empty?
        raise CompileError.at(block.location, "block needs a value")
      end
      push_scope
      params.each { |name, ty| bind(name, ty) }
      last = block.body.last
      block.body.each_with_index do |stmt, i|
        if i == block.body.size - 1
          unless last.is_a?(AST::ExprStmt)
            raise CompileError.at(stmt.location, "block must end with a value")
          end
          ty = check_expr(last.expr)
          if ty.void?
            raise CompileError.at(stmt.location, "block must end with a value")
          end
          pop_scope
          block.value_type = ty
          return ty
        else
          check_stmt(stmt, VoidTy::INSTANCE, false)
        end
      end
      pop_scope
      raise CompileError.at(block.location, "block needs a value")
    end
  end
end
