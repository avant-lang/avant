module Avant
  module Codegen
    class Myc
      @return_type : Ty
      @indent : Int32
      @temps : Int32
      @rooted : Set(String)
      @c_main : Bool

      def initialize(@program : AST::Program)
        @io = IO::Memory.new
        @indent = 0
        @temps = 0
        @c_main = false
        @return_type = VoidTy::INSTANCE.as(Ty)
        @named = {} of String => AggTy
        @opaques = {} of String => OpaqueTy
        @resolver = TypeResolver.new(@named, @opaques)
        @sigs = {} of String => {Array(Ty), Ty}
        @methods = {} of String => Hash(String, MethodSig)
        @arrays = {} of String => ArrayTy
        @self_ty = nil.as(AggTy?)
        @self_name = nil.as(String?)
        @type_ids = {} of String => Int32
        @rooted = Set(String).new
        @unions = {} of String => UnionTy
        @spawn_id = 0
        @pending_thunks = [] of {String, AST::Block, Array({String, Ty})}
        @spawn_caps = {} of String => Array({String, Ty})
      end

      def emit : String
        @named, @opaques = Avant.load_types(@program)
        @resolver = TypeResolver.new(@named, @opaques)
        assign_type_ids

        emit_runtime_decl
        emit_lib_decls

        @program.functions.each do |fn|
          next if fn.method?
          next if fn.generic
          params = fn.params.map { |p| @resolver.resolve(p.type) }
          ret = fn.return_type ? @resolver.resolve(fn.return_type.not_nil!) : VoidTy::INSTANCE
          @sigs[fn.emit_name] = {params, ret}
        end
        collect_methods

        collect_arrays
        @program.structs.each { |defn| emit_layout(@named[defn.name]) }
        @program.classes.each { |defn| emit_layout(@named[defn.name]) }
        @unions.each_value { |ty| emit_union_def(ty) }
        @arrays.each_value { |ty| emit_array_def(ty) }
        collect_spawns
        @pending_thunks.each { |name, _block, caps| emit_spawn_cap_layout(name, caps) }

        @program.all_functions.each { |fn| emit_function(fn) }
        @program.classes.each { |defn| emit_constructor(@named[defn.name].as(ClassTy)) }
        @pending_thunks.each { |name, block, caps| emit_spawn_thunk(name, block, caps) }
        emit_wrapper_main unless @sigs.has_key?("main")
        @io.to_s
      end

      private def collect_methods : Nil
        @program.all_functions.each do |fn|
          next unless fn.method?
          next if fn.generic
          owner = @named[fn.owner.not_nil!]
          params = fn.params.map { |p| @resolver.resolve(p.type) }
          ret = fn.return_type ? @resolver.resolve(fn.return_type.not_nil!) : VoidTy::INSTANCE
          bucket = @methods[owner.name] ||= {} of String => MethodSig
          bucket[fn.emit_name] = MethodSig.new(fn, owner, params, ret)
        end
      end

      private def collect_arrays : Nil
        @sigs.each_value do |params, ret|
          params.each { |t| note_ty(t) }
          note_ty(ret)
        end
        @methods.each_value do |bucket|
          bucket.each_value do |sig|
            note_ty(sig.owner)
            sig.params.each { |t| note_ty(t) }
            note_ty(sig.return_type)
          end
        end
        @named.each_value do |ty|
          ty.fields.each { |_, field_ty| note_ty(field_ty) }
        end
        @program.classes.each do |defn|
          defn.fields.each { |field| collect_expr(field.default) }
        end
        @program.all_functions.each { |fn| collect_stmts(fn.body) unless fn.generic }
      end

      private def collect_spawns : Nil
        @program.all_functions.each { |fn| collect_spawn_stmts(fn.body) unless fn.generic }
      end

      private def collect_spawn_stmts(body : Array(AST::Stmt)) : Nil
        body.each do |stmt|
          case stmt
          when AST::IfStmt
            collect_spawn_expr(stmt.cond)
            collect_spawn_stmts(stmt.then_body)
            collect_spawn_stmts(stmt.else_body)
          when AST::WhileStmt
            collect_spawn_expr(stmt.cond)
            collect_spawn_stmts(stmt.body)
          when AST::AssignStmt
            collect_spawn_expr(stmt.target)
            collect_spawn_expr(stmt.value)
          when AST::ExprStmt
            collect_spawn_expr(stmt.expr)
          when AST::ReturnStmt
            collect_spawn_expr(stmt.expr)
          end
        end
      end

      private def collect_spawn_expr(expr : AST::Expr?) : Nil
        return unless expr
        case expr
        when AST::Call
          if expr.callee == "spawn" && expr.receiver.nil? && (block = expr.block)
            name = "__spawn_#{@spawn_id}"
            @spawn_id += 1
            expr.spawn_thunk = name
            @pending_thunks << {name, block, expr.spawn_captures}
          end
          collect_spawn_expr(expr.receiver)
          expr.args.each { |a| collect_spawn_expr(a) }
          if b = expr.block
            collect_spawn_stmts(b.body)
          end
        when AST::Unary
          collect_spawn_expr(expr.expr)
        when AST::Binary
          collect_spawn_expr(expr.left)
          collect_spawn_expr(expr.right)
        when AST::FieldAccess
          collect_spawn_expr(expr.object)
        when AST::Index
          collect_spawn_expr(expr.array)
          collect_spawn_expr(expr.index)
        when AST::StructLiteral
          expr.fields.each { |_, v| collect_spawn_expr(v) }
        when AST::ArrayLiteral
          expr.elements.each { |e| collect_spawn_expr(e) }
        when AST::ArrayNew
          collect_spawn_expr(expr.size)
        when AST::Try
          collect_spawn_expr(expr.expr)
        when AST::InterpString
          expr.parts.each { |p| collect_spawn_expr(p.expr) }
        when AST::SwitchExpr
          collect_spawn_expr(expr.cond)
          expr.cases.each { |arm| collect_spawn_stmts(arm.body) }
          if else_body = expr.else_body
            collect_spawn_stmts(else_body)
          end
        end
      end

      private def collect_stmts(body : Array(AST::Stmt)) : Nil
        body.each do |stmt|
          case stmt
          when AST::ReturnStmt
            collect_expr(stmt.expr)
          when AST::IfStmt
            collect_expr(stmt.cond)
            collect_stmts(stmt.then_body)
            collect_stmts(stmt.else_body)
          when AST::WhileStmt
            collect_expr(stmt.cond)
            collect_stmts(stmt.body)
          when AST::BreakStmt, AST::ContinueStmt
          when AST::AssignStmt
            collect_expr(stmt.target)
            collect_expr(stmt.value)
          when AST::ExprStmt
            collect_expr(stmt.expr)
          end
        end
      end

      private def collect_expr(expr : AST::Expr?) : Nil
        return unless expr
        note_ty(expr.type)
        case expr
        when AST::Call
          collect_expr(expr.receiver)
          expr.args.each { |a| collect_expr(a) }
          if b = expr.block
            collect_stmts(b.body)
          end
        when AST::Unary
          collect_expr(expr.expr)
        when AST::Binary
          collect_expr(expr.left)
          collect_expr(expr.right)
        when AST::FieldAccess
          collect_expr(expr.object)
        when AST::Index
          collect_expr(expr.array)
          collect_expr(expr.index)
        when AST::StructLiteral
          expr.fields.each { |_, v| collect_expr(v) }
        when AST::ArrayLiteral
          expr.elements.each { |e| collect_expr(e) }
        when AST::ArrayNew
          collect_expr(expr.size)
          note_ty(@resolver.resolve(expr.elem_type))
        when AST::HashNew
          note_ty(@resolver.resolve(expr.key_type))
          note_ty(@resolver.resolve(expr.val_type))
        when AST::Try
          collect_expr(expr.expr)
        when AST::InterpString
          expr.parts.each { |p| collect_expr(p.expr) }
        when AST::SwitchExpr
          collect_expr(expr.cond)
          expr.cases.each { |arm| collect_stmts(arm.body) }
          if else_body = expr.else_body
            collect_stmts(else_body)
          end
        end
      end

      private def note_ty(ty : Ty?) : Nil
        return unless ty
        if ty.is_a?(ArrayTy)
          @arrays[ty.layout_myc] = ty
          note_ty(ty.elem)
        elsif ty.is_a?(UnionTy)
          @unions[ty.layout_myc] = ty
          ty.members.each { |m| note_ty(m) }
        elsif ty.is_a?(HashTy)
          note_ty(ty.key)
          note_ty(ty.val)
        end
      end

      private def emit_layout(ty : AggTy) : Nil
        line "STRUCT :#{ty.layout_myc}"
        @indent += 1
        ty.fields.each { |_, field_ty| line "TYPE :#{field_ty.myc}" }
        @indent -= 1
        line "ENDSTRUCT"
        @io << '\n'
      end

      private def emit_spawn_cap_layout(name : String, caps : Array({String, Ty})) : Nil
        return if caps.empty?
        line "STRUCT :#{name}_cap"
        @indent += 1
        caps.each { |_, ty| line "TYPE :#{ty.myc}" }
        @indent -= 1
        line "ENDSTRUCT"
        @io << '\n'
      end

      private def emit_array_def(ty : ArrayTy) : Nil
        line "STRUCT :#{ty.layout_myc}"
        @indent += 1
        line "TYPE :ptr<#{ty.elem.myc}>"
        line "TYPE :i32"
        line "TYPE :i32"
        @indent -= 1
        line "ENDSTRUCT"
        @io << '\n'
      end

      private def emit_function(fn : AST::Function) : Nil
        return if fn.generic
        @temps = 0
        @self_ty = nil
        @self_name = nil

        if fn.method?
          sig = @methods[fn.owner.not_nil!][fn.emit_name]
          @return_type = sig.return_type
          @self_ty = sig.owner
          @self_name = fn.self_name
          @c_main = false
          emit_method_func(fn, sig)
        else
          params, @return_type = @sigs[fn.emit_name]
          @c_main = fn.name == "main"
          emit_plain_func(fn, params)
          @c_main = false
        end
      end

      private def emit_plain_func(fn : AST::Function, params : Array(Ty)) : Nil
        line "FUNC :#{fn.emit_name}"
        @indent += 1
        if @c_main
          line "ARGS"
          @indent += 1
          line "TYPE :i32"
          line "TYPE :ptr<void>"
          @indent -= 1
          roots = [] of {String, Ty}
          emit_return_and_body(fn, roots) do
            emit_init_argv
          end
        else
          unless fn.params.empty?
            line "ARGS"
            @indent += 1
            params.each { |ty| line "TYPE :#{ty.myc}" }
            @indent -= 1
          end
          roots = fn.params.map_with_index { |param, i| {param.name, params[i]} }
          emit_return_and_body(fn, roots) do
            fn.params.each_with_index do |param, i|
              line "PARAM #{i}"
              line "LOCAL :#{param.name} :#{params[i].myc}"
              line "STORE"
            end
          end
        end
      end

      private def emit_method_func(fn : AST::Function, sig : MethodSig) : Nil
        line "FUNC :#{sig.mangled}"
        @indent += 1
        line "ARGS"
        @indent += 1
        line "TYPE :#{sig.owner.self_ptr_myc}"
        sig.params.each { |ty| line "TYPE :#{ty.myc}" }
        @indent -= 1
        roots = [{fn.self_name, sig.owner.as(Ty)}] of {String, Ty}
        fn.params.each_with_index { |param, i| roots << {param.name, sig.params[i]} }
        emit_return_and_body(fn, roots) do
          line "PARAM 0"
          line "LOCAL :#{fn.self_name} :#{sig.owner.self_ptr_myc}"
          line "STORE"
          fn.params.each_with_index do |param, i|
            line "PARAM #{i + 1}"
            line "LOCAL :#{param.name} :#{sig.params[i].myc}"
            line "STORE"
          end
        end
      end

      private def emit_return_and_body(fn : AST::Function, roots : Array({String, Ty}), & : ->) : Nil
        @rooted = Set(String).new
        if @c_main
          line "RETURN"
          @indent += 1
          line "TYPE :i32"
          @indent -= 1
        elsif !@return_type.void?
          line "RETURN"
          @indent += 1
          line "TYPE :#{@return_type.myc}"
          @indent -= 1
        end
        line "BODY"
        @indent += 1
        line "CALL :avant_gc_enter"
        emit_type_maps if fn.name == "main"
        yield
        roots.each { |name, ty| ensure_root(name, ty) }
        emit_stmts(fn.body, implicit_return: true)
        emit_gc_leave_if_fallthrough(fn)
        @indent -= 1
        @indent -= 1
        line "ENDFUNC"
        @io << '\n'
        @self_ty = nil
        @self_name = nil
      end

      private def emit_constructor(ty : ClassTy) : Nil
        init = initialize_sig(ty.name)
        @temps = 0
        @return_type = ty.as(Ty)
        @self_ty = nil
        @self_name = nil

        line "FUNC :#{ty.name}__new"
        @indent += 1
        if init && !init.params.empty?
          line "ARGS"
          @indent += 1
          init.params.each { |p| line "TYPE :#{p.myc}" }
          @indent -= 1
        end
        line "RETURN"
        @indent += 1
        line "TYPE :#{ty.myc}"
        @indent -= 1
        line "BODY"
        @indent += 1
        @rooted = Set(String).new
        line "CALL :avant_gc_enter"
        if init
          init.params.each_with_index do |param_ty, i|
            name = init.node.params[i].name
            line "PARAM #{i}"
            line "LOCAL :#{name} :#{param_ty.myc}"
            line "STORE"
            ensure_root(name, param_ty)
          end
        end
        emit_alloc_sizeof(ty.layout_myc, class_type_id(ty))
        line "AS :#{ty.self_ptr_myc}"
        line "LOCAL :self :#{ty.self_ptr_myc}"
        line "STORE"
        ensure_root("self", ty.as(Ty))
        emit_field_defaults(ty)
        if init
          emit_named_args(init.node.params.map(&.name), init.params)
          line "LOCAL :self"
          line "CALL :#{init.mangled}"
        end
        line "LOCAL :self"
        emit_ret
        @indent -= 1
        @indent -= 1
        line "ENDFUNC"
        @io << '\n'
      end

      private def emit_wrapper_main : Nil
        _, run_ret = @sigs["run"]
        line "FUNC :main"
        @indent += 1
        line "ARGS"
        @indent += 1
        line "TYPE :i32"
        line "TYPE :ptr<void>"
        @indent -= 1
        line "RETURN"
        @indent += 1
        line "TYPE :i32"
        @indent -= 1
        line "BODY"
        @indent += 1
        line "CALL :avant_gc_enter"
        emit_type_maps
        emit_init_argv
        line "CALL :run"
        line "STACK :drop" unless run_ret.void?
        line "CALL :avant_gc_leave"
        line "PUSH 0"
        line "RET"
        @indent -= 1
        @indent -= 1
        line "ENDFUNC"
        @io << '\n'
      end

      private def emit_init_argv : Nil
        line "PARAM 1"
        line "PARAM 0"
        line "CALL :avant_io_init_argv"
      end

      private def emit_field_defaults(ty : ClassTy) : Nil
        defn = @program.classes.find { |c| c.name == ty.name }
        return unless defn
        defn.fields.each_with_index do |field, i|
          default = field.default
          next unless default
          field_ty = ty.fields[i][1]
          emit_expr(default)
          emit_coerce(default.type.not_nil!, field_ty)
          if heap_ptr_value?(field_ty)
            val = new_temp
            line "LOCAL :#{val} :#{field_ty.myc}"
            line "STORE"
            ensure_root(val, field_ty)
            line "LOCAL :self"
            line "CALL :avant_barrier"
            line "LOCAL :#{val}"
            line "LOCAL :self"
            line "DEREF"
            line "FIELD #{i}"
            line "STORE"
          else
            line "LOCAL :self"
            line "DEREF"
            line "FIELD #{i}"
            line "STORE"
          end
        end
      end

      private def emit_switch(expr : AST::SwitchExpr) : Nil
        result_ty = expr.type.not_nil!
        emit_expr(expr.cond)
        if expr.cond.type.try(&.bool?)
          line "AS :i32"
        end
        dest = nil.as(String?)
        unless result_ty.void?
          dest = new_temp
        end
        line "SWITCH"
        @indent += 1
        expr.cases.each do |arm|
          arm.labels.each do |label|
            line "CASE #{label}"
            @indent += 1
            emit_switch_arm(arm.body, dest, result_ty)
            @indent -= 1
          end
        end
        if else_body = expr.else_body
          line "ELSE"
          @indent += 1
          emit_switch_arm(else_body, dest, result_ty)
          @indent -= 1
        end
        @indent -= 1
        line "ENDSWITCH"
        if name = dest
          line "LOCAL :#{name}"
        end
      end

      private def emit_switch_arm(body : Array(AST::Stmt), dest : String?, result_ty : Ty) : Nil
        if dest
          if body.empty?
            emit_zero(result_ty)
            line "LOCAL :#{dest} :#{result_ty.myc}"
            line "STORE"
            ensure_root(dest, result_ty)
            return
          end
          last = body.size - 1
          body.each_with_index do |stmt, i|
            if i == last && stmt.is_a?(AST::ExprStmt)
              emit_expr(stmt.expr)
              emit_coerce(stmt.expr.type.not_nil!, result_ty)
              line "LOCAL :#{dest} :#{result_ty.myc}"
              line "STORE"
              ensure_root(dest, result_ty)
            else
              emit_stmt(stmt, false)
            end
          end
        else
          emit_stmts(body, implicit_return: false)
        end
      end

      private def emit_file_read(expr : AST::Call) : Nil
        emit_opt_cstr(expr, "avant_file_read")
      end

      private def emit_opt_cstr(expr : AST::Call, cname : String) : Nil
        found = new_temp
        line "PUSH 0"
        line "LOCAL :#{found} :i32"
        line "STORE"
        line "LOCAL :#{found}"
        line "ADDR"
        emit_expr(expr.args[0])
        line "CALL :#{cname}"
        val_ty = StringTy::INSTANCE.as(Ty)
        val = new_temp
        line "LOCAL :#{val} :#{val_ty.myc}"
        line "STORE"
        ensure_root(val, val_ty)
        want = UnionTy.nilable(val_ty).as(UnionTy)
        out = new_temp
        line "LOCAL :#{found}"
        line "PUSH 1"
        line "BINARY :eq"
        line "IF"
        @indent += 1
        line "THEN"
        @indent += 1
        line "LOCAL :#{val}"
        emit_wrap_union(val_ty, want)
        line "LOCAL :#{out} :#{want.myc}"
        line "STORE"
        @indent -= 1
        line "ELSE"
        @indent += 1
        emit_zero(NilTy::INSTANCE)
        emit_wrap_union(NilTy::INSTANCE, want)
        line "LOCAL :#{out} :#{want.myc}"
        line "STORE"
        @indent -= 1
        @indent -= 1
        line "ENDIF"
        line "LOCAL :#{out}"
      end

      private def emit_process_run(expr : AST::Call) : Nil
        emit_expr(expr.args[1])
        line "AS :ptr<void>"
        args = new_temp
        line "LOCAL :#{args} :ptr<void>"
        line "STORE"
        emit_expr(expr.args[0])
        path = new_temp
        line "LOCAL :#{path} :ptr<u8>"
        line "STORE"
        ensure_root(path, StringTy::INSTANCE.as(Ty))
        line "LOCAL :#{args}"
        line "LOCAL :#{path}"
        line "CALL :avant_process_run"
      end

      private def emit_process_run_out(expr : AST::Call) : Nil
        emit_expr(expr.args[2])
        outp = new_temp
        line "LOCAL :#{outp} :ptr<u8>"
        line "STORE"
        ensure_root(outp, StringTy::INSTANCE.as(Ty))
        emit_expr(expr.args[1])
        line "AS :ptr<void>"
        args = new_temp
        line "LOCAL :#{args} :ptr<void>"
        line "STORE"
        emit_expr(expr.args[0])
        path = new_temp
        line "LOCAL :#{path} :ptr<u8>"
        line "STORE"
        ensure_root(path, StringTy::INSTANCE.as(Ty))
        line "LOCAL :#{outp}"
        line "LOCAL :#{args}"
        line "LOCAL :#{path}"
        line "CALL :avant_process_run_out"
      end

      private def emit_stmts(body : Array(AST::Stmt), implicit_return : Bool) : Nil
        body.each_with_index do |stmt, i|
          emit_stmt(stmt, implicit_return && i == body.size - 1)
        end
      end

      private def emit_stmt(stmt : AST::Stmt, at_tail : Bool) : Nil
        case stmt
        when AST::ReturnStmt
          if expr = stmt.expr
            emit_expr(expr)
            emit_coerce(expr.type.not_nil!, @return_type)
          end
          emit_ret
        when AST::IfStmt
          if bind = stmt.bind
            emit_if_assign(stmt, bind)
          else
            emit_expr(stmt.cond)
            line "IF"
            @indent += 1
            line "THEN"
            @indent += 1
            emit_stmts(stmt.then_body, implicit_return: false)
            @indent -= 1
            unless stmt.else_body.empty?
              line "ELSE"
              @indent += 1
              emit_stmts(stmt.else_body, implicit_return: false)
              @indent -= 1
            end
            @indent -= 1
            line "ENDIF"
          end
        when AST::WhileStmt
          line "LOOP"
          @indent += 1
          line "COND"
          @indent += 1
          emit_expr(stmt.cond)
          @indent -= 1
          line "BODY"
          @indent += 1
          emit_stmts(stmt.body, implicit_return: false)
          @indent -= 1
          @indent -= 1
          line "ENDLOOP"
        when AST::BreakStmt
          line "BREAK"
        when AST::ContinueStmt
          line "NEXT"
        when AST::AssignStmt
          emit_assign(stmt)
        when AST::ExprStmt
          emit_expr(stmt.expr)
          if at_tail && !@return_type.void?
            emit_coerce(stmt.expr.type.not_nil!, @return_type)
            emit_ret
          elsif !void_expr?(stmt.expr)
            line "STACK :drop"
          end
        else
          raise "unknown statement #{stmt.class}"
        end
      end

      private def emit_assign(stmt : AST::AssignStmt) : Nil
        target = stmt.target
        if !stmt.compound? && target.is_a?(AST::Index) && target.array.type.try(&.hash?)
          emit_hash_set(target, stmt.value, target.array.type.as(HashTy))
          return
        end
        unless stmt.compound?
          if target.is_a?(AST::FieldAccess)
            object_ty = target.object.type.not_nil!
            if object_ty.class? && heap_ptr_value?(target.type.not_nil!)
              emit_barrier_field_store(target, stmt.value)
              return
            end
          end
          if target.is_a?(AST::Index)
            array_ty = target.array.type.as(ArrayTy)
            if heap_ptr_value?(array_ty.elem)
              emit_barrier_index_store(target, stmt.value)
              return
            end
          end
        end

        if stmt.compound?
          emit_expr(stmt.target)
          tmp = new_temp
          ty = stmt.target.type.not_nil!
          line "LOCAL :#{tmp} :#{ty.myc}"
          line "STORE"
          emit_expr(stmt.value)
          line "LOCAL :#{tmp}"
          line "BINARY :#{myc_binop(compound_to_binop(stmt.op))}"
        else
          emit_expr(stmt.value)
          emit_coerce(stmt.value.type.not_nil!, stmt.target.type.not_nil!)
        end
        emit_lvalue(stmt.target)
        line "STORE"
        root_after_store(stmt.target)
      end

      private def emit_barrier_field_store(target : AST::FieldAccess, value : AST::Expr) : Nil
        field_ty = target.type.not_nil!
        object_ty = target.object.type.not_nil!
        emit_expr(value)
        val = new_temp
        line "LOCAL :#{val} :#{field_ty.myc}"
        line "STORE"
        ensure_root(val, field_ty)
        emit_expr(target.object)
        obj = new_temp
        line "LOCAL :#{obj} :#{object_ty.myc}"
        line "STORE"
        ensure_root(obj, object_ty)
        line "LOCAL :#{obj}"
        line "CALL :avant_barrier"
        line "LOCAL :#{val}"
        line "LOCAL :#{obj}"
        line "DEREF"
        line "FIELD #{field_index(target)}"
        line "STORE"
      end

      private def emit_barrier_index_store(target : AST::Index, value : AST::Expr) : Nil
        array_ty = target.array.type.as(ArrayTy)
        emit_expr(value)
        val = new_temp
        line "LOCAL :#{val} :#{array_ty.elem.myc}"
        line "STORE"
        ensure_root(val, array_ty.elem)
        emit_expr(target.array)
        arr = new_temp
        line "LOCAL :#{arr} :#{target.array.type.not_nil!.myc}"
        line "STORE"
        ensure_root(arr, target.array.type.not_nil!)
        emit_expr(target.index)
        ix = new_temp
        line "LOCAL :#{ix} :i32"
        line "STORE"
        line "LOCAL :#{val}"
        line "LOCAL :#{ix}"
        line "LOCAL :#{arr}"
        line "CALL :avant_array_set_ptr"
      end

      private def root_after_store(target : AST::Expr) : Nil
        return unless target.is_a?(AST::Name)
        return if target.implicit_field
        ensure_root(target.ident, target.type.not_nil!)
      end

      private def compound_to_binop(kind : Token::Kind) : Token::Kind
        case kind
        when .plus_eq?  then Token::Kind::Plus
        when .minus_eq? then Token::Kind::Minus
        when .star_eq?  then Token::Kind::Star
        when .slash_eq? then Token::Kind::Slash
        else
          raise "not a compound assignment #{kind}"
        end
      end

      private def void_expr?(expr : AST::Expr) : Bool
        expr.type.try(&.void?) || false
      end

      private def emit_expr(expr : AST::Expr) : Nil
        case expr
        when AST::IntegerLiteral
          emit_int_literal(expr)
        when AST::FloatLiteral
          line "PUSH #{expr.value}"
        when AST::StringLiteral
          line "PUSH #{myc_string(expr.value)}"
        when AST::BoolLiteral
          line "PUSH #{expr.value}"
        when AST::Name, AST::FieldAccess, AST::Index
          if expr.is_a?(AST::FieldAccess) && expr.method_call
            if expr.object.type.try(&.array?) && expr.field == "sort"
              emit_expr(expr.object)
              line "CALL :avant_array_sort_i32"
            elsif expr.object.type.try(&.array?) && expr.field == "pop"
              emit_array_pop(expr.object, expr.object.type.as(ArrayTy))
            elsif expr.object.type.try(&.array?) && expr.field == "clear"
              emit_array_clear(expr.object, expr.object.type.as(ArrayTy))
            elsif expr.object.type.try(&.join_handle?)
              emit_expr(expr.object)
              line "CALL :avant_join"
            elsif expr.object.type.try(&.float?)
              emit_float_field(expr)
            elsif expr.object.type.try(&.integer?)
              emit_int_field(expr)
            elsif expr.object.type.try(&.string?)
              emit_expr(expr.object)
              case expr.field
              when "to_f"
                line "CALL :avant_str_to_f"
              when "to_i"
                line "CALL :avant_str_to_i"
              else
                raise "unknown string field #{expr.field}"
              end
            elsif expr.object.type.try(&.buf?)
              emit_expr(expr.object)
              case expr.field
              when "to_s"
                line "CALL :avant_buf_to_str"
              when "clear"
                line "CALL :avant_buf_clear"
              else
                raise "unknown buf field #{expr.field}"
              end
            else
              emit_method_named(expr.object, expr.resolved.not_nil!, [] of AST::Expr)
            end
          elsif expr.is_a?(AST::FieldAccess) && expr.object.type.try(&.string?) && expr.field == "size"
            emit_expr(expr.object)
            line "CALL :avant_str_size"
          elsif expr.is_a?(AST::FieldAccess) && expr.object.type.try(&.string?) && expr.field == "to_f"
            emit_expr(expr.object)
            line "CALL :avant_str_to_f"
          elsif expr.is_a?(AST::FieldAccess) && expr.object.type.try(&.string?) && expr.field == "to_i"
            emit_expr(expr.object)
            line "CALL :avant_str_to_i"
          elsif expr.is_a?(AST::FieldAccess) && expr.object.type.try(&.hash?) && expr.field == "size"
            emit_expr(expr.object)
            line "CALL :avant_hash_size"
          elsif expr.is_a?(AST::FieldAccess) && expr.object.type.try(&.buf?) && expr.field == "size"
            emit_expr(expr.object)
            line "CALL :avant_buf_size"
          else
            emit_lvalue(expr)
          end
        when AST::Unary
          emit_unary(expr)
        when AST::Binary
          emit_binary(expr)
        when AST::Call
          emit_call(expr)
        when AST::StructLiteral
          emit_struct_literal(expr)
        when AST::ArrayLiteral
          emit_array_literal(expr)
        when AST::ArrayNew
          emit_array_new(expr)
        when AST::HashNew
          emit_hash_new(expr)
        when AST::NilLiteral
          line "PUSH 0"
        when AST::Try
          emit_try(expr)
        when AST::InterpString
          emit_interp(expr)
        when AST::SwitchExpr
          emit_switch(expr)
        else
          raise "unknown expression #{expr.class}"
        end
      end

      private def emit_lvalue(expr : AST::Expr) : Nil
        case expr
        when AST::Name
          if expr.implicit_field
            emit_self_field(expr.ident)
          else
            emit_name_lvalue(expr)
          end
        when AST::FieldAccess
          emit_field_lvalue(expr)
        when AST::Index
          emit_index_lvalue(expr)
        else
          emit_expr(expr)
        end
      end

      private def emit_name_lvalue(expr : AST::Name) : Nil
        ty = expr.type.not_nil!
        if struct_self?(expr.ident)
          line "LOCAL :#{expr.ident} :#{@self_ty.not_nil!.self_ptr_myc}"
          line "DEREF"
        else
          line "LOCAL :#{expr.ident} :#{ty.myc}"
        end
      end

      private def struct_self?(name : String) : Bool
        return false unless self_ty = @self_ty
        self_ty.struct? && @self_name == name
      end

      private def emit_self_field(field : String) : Nil
        ty = @self_ty.not_nil!
        line "LOCAL :#{@self_name} :#{ty.self_ptr_myc}"
        line "DEREF"
        line "FIELD #{ty.field_index(field).not_nil!}"
      end

      private def emit_field_lvalue(expr : AST::FieldAccess) : Nil
        object_ty = expr.object.type.not_nil!
        if object_ty.is_a?(ArrayTy)
          emit_expr(expr.object)
          line "DEREF"
          line "FIELD #{field_index(expr)}"
          return
        end
        if object_ty.class?
          emit_expr(expr.object)
          line "DEREF"
          line "FIELD #{field_index(expr)}"
        else
          emit_lvalue(expr.object)
          line "FIELD #{field_index(expr)}"
        end
      end

      private def field_index(expr : AST::FieldAccess) : Int32
        object_ty = expr.object.type.not_nil!
        if object_ty.is_a?(ArrayTy)
          return 1 if expr.field == "size"
          raise "bad array field"
        end
        object_ty.as(AggTy).field_index(expr.field) || raise "missing field"
      end

      private def emit_int_literal(expr : AST::IntegerLiteral) : Nil
        ty = expr.type
        if ty.try(&.float?)
          line "PUSH #{expr.bits} :f64"
        elsif ty.try(&.uint64?)
          line "PUSH #{expr.bits} :u64"
        elsif ty.try(&.int64?)
          line "PUSH #{expr.bits} :i64"
        else
          line "PUSH #{expr.bits}"
        end
      end

      private def emit_int_convert(from : Ty, name : String) : Nil
        case name
        when "to_f"
          line "AS :f64"
        when "abs"
          line "UNARY :abs"
        when "to_i"
          line "AS :i32" unless from.int?
        when "to_i64"
          if from.int?
            line "TO :i64"
          elsif from.uint64?
            line "AS :i64"
          end
        when "to_u64"
          if from.int?
            line "TO :i64"
            line "AS :u64"
          elsif from.int64?
            line "AS :u64"
          end
        else
          raise "unknown integer convert #{name}"
        end
      end

      private def emit_unary(expr : AST::Unary) : Nil
        emit_expr(expr.expr)
        case expr.op
        when .minus?
          line "UNARY :neg"
        when .bang?
          line "UNARY :lnot"
        when .tilde?
          line "UNARY :bnot"
        else
          raise "unsupported unary #{expr.op}"
        end
      end

      private def emit_index_lvalue(expr : AST::Index) : Nil
        container = expr.array.type.not_nil!
        if container.string?
          emit_expr(expr.index)
          emit_expr(expr.array)
          line "CALL :avant_str_byte"
          return
        end
        if container.is_a?(HashTy)
          emit_hash_get(expr, container)
          return
        end
        array_ty = container.as(ArrayTy)
        if heap_ptr_value?(array_ty.elem)
          emit_expr(expr.index)
          emit_expr(expr.array)
          line "CALL :avant_array_get_ptr"
          line "AS :#{array_ty.elem.myc}"
          return
        end
        emit_expr(expr.index)
        ix = new_temp
        line "LOCAL :#{ix} :i32"
        line "STORE"
        emit_expr(expr.array)
        line "DEREF"
        line "FIELD 0"
        ptr_tmp = new_temp
        line "LOCAL :#{ptr_tmp} :ptr<#{array_ty.elem.myc}>"
        line "STORE"
        emit_ptr_root(ptr_tmp)
        line "LOCAL :#{ix}"
        line "LOCAL :#{ptr_tmp}"
        line "BINARY :add"
        line "DEREF"
      end

      private def emit_binary(expr : AST::Binary) : Nil
        if name = expr.op_method
          emit_method_named(expr.left, name, [expr.right] of AST::Expr)
          return
        end
        if expr.op.amp_amp? || expr.op.pipe_pipe?
          emit_logical(expr)
          return
        end
        left_ty = expr.left.type.not_nil!
        if left_ty.string?
          emit_string_binary(expr)
          return
        end
        tmp = new_temp
        left_ty = expr.left.type.not_nil!
        emit_expr(expr.left)
        line "LOCAL :#{tmp} :#{left_ty.myc}"
        line "STORE"
        emit_expr(expr.right)
        right_ty = expr.right.type.not_nil!
        if (left_ty.uint64? || left_ty.int64?) && right_ty.int? && (expr.op.less_less? || expr.op.greater_greater?)
          line "AS :#{left_ty.myc}"
        end
        line "LOCAL :#{tmp}"
        op = if expr.op.greater_greater? && left_ty.uint64?
               "shr"
             else
               myc_binop(expr.op)
             end
        line "BINARY :#{op}"
      end

      private def emit_logical(expr : AST::Binary) : Nil
        out = new_temp
        emit_expr(expr.left)
        line "IF"
        @indent += 1
        line "THEN"
        @indent += 1
        if expr.op.amp_amp?
          emit_expr(expr.right)
        else
          line "PUSH true"
        end
        line "LOCAL :#{out} :bool"
        line "STORE"
        @indent -= 1
        line "ELSE"
        @indent += 1
        if expr.op.amp_amp?
          line "PUSH false"
        else
          emit_expr(expr.right)
        end
        line "LOCAL :#{out} :bool"
        line "STORE"
        @indent -= 1
        @indent -= 1
        line "ENDIF"
        line "LOCAL :#{out}"
      end

      private def emit_call(expr : AST::Call) : Nil
        if expr.lib_name
          emit_call_named(expr.callee, expr.args)
          return
        end

        if expr.callee == "puts" && expr.receiver.nil?
          emit_puts(expr.args[0])
          return
        end

        if expr.callee == "spawn" && expr.receiver.nil?
          emit_spawn(expr)
          return
        end

        if expr.callee == "now_ms" && expr.receiver.nil?
          line "CALL :avant_now_ms"
          return
        end

        if expr.callee == "file_read" && expr.receiver.nil?
          emit_file_read(expr)
          return
        end

        if expr.callee == "file_write" && expr.receiver.nil?
          emit_call_named("avant_file_write", expr.args)
          return
        end

        if expr.callee == "argv" && expr.receiver.nil?
          line "CALL :avant_argv"
          line "AS :#{expr.type.not_nil!.myc}"
          return
        end

        if expr.callee == "process_run" && expr.receiver.nil?
          emit_process_run(expr)
          return
        end

        if expr.callee == "process_run_out" && expr.receiver.nil?
          emit_process_run_out(expr)
          return
        end

        if expr.callee == "env_get" && expr.receiver.nil?
          emit_opt_cstr(expr, "avant_env_get")
          return
        end

        if expr.callee == "file_exists" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_file_exists"
          line "PUSH 1"
          line "BINARY :eq"
          return
        end

        if expr.callee == "dir_list" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_dir_list"
          line "AS :#{expr.type.not_nil!.myc}"
          return
        end

        if expr.callee == "now_us" && expr.receiver.nil?
          line "CALL :avant_now_us"
          return
        end

        if expr.callee == "chr" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_str_from_byte"
          return
        end

        if expr.callee == "repeat_byte" && expr.receiver.nil?
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_str_repeat_byte"
          return
        end

        if expr.callee == "bytes_to_str" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_str_from_i32_bytes"
          return
        end

        if expr.callee == "str_slice" && expr.receiver.nil?
          emit_expr(expr.args[2])
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_str_slice"
          return
        end

        if expr.callee == "fmt_float" && expr.receiver.nil?
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_str_fmt_float"
          return
        end

        if expr.callee == "buf_new" && expr.receiver.nil?
          if expr.args.empty?
            line "PUSH 0"
          else
            emit_expr(expr.args[0])
          end
          line "CALL :avant_buf_new"
          return
        end

        if expr.callee == "str_to_f_slice" && expr.receiver.nil?
          emit_expr(expr.args[2])
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_str_to_f_slice"
          return
        end

        if expr.callee == "json_free" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_json_free"
          return
        end

        if expr.callee == "json_gen_body" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_json_gen_body"
          return
        end

        if expr.callee == "json_gen_into" && expr.receiver.nil?
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_json_gen_into"
          return
        end

        if expr.callee == "b64_encode" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_b64_encode"
          return
        end

        if expr.callee == "b64_decode" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_b64_decode"
          return
        end

        if expr.callee == "b64_encode_buf" && expr.receiver.nil?
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_b64_encode_buf"
          return
        end

        if expr.callee == "b64_decode_buf" && expr.receiver.nil?
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_b64_decode_buf"
          return
        end

        if expr.callee == "zlib_compress" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_zlib_compress"
          return
        end

        if expr.callee == "zlib_uncompress" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_zlib_uncompress"
          return
        end

        if expr.callee == "sha256" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_sha256"
          return
        end

        if expr.callee == "crc32_bytes" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_crc32_i32"
          return
        end

        if expr.callee == "sha256_word0" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_sha256_word0"
          return
        end

        if expr.callee == "re_compile" && expr.receiver.nil?
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_re_compile"
          return
        end

        if expr.callee == "re_find" && expr.receiver.nil?
          emit_expr(expr.args[2])
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_re_find"
          return
        end

        if expr.callee == "re_m0" && expr.receiver.nil?
          line "CALL :avant_re_m0"
          return
        end

        if expr.callee == "re_m1" && expr.receiver.nil?
          line "CALL :avant_re_m1"
          return
        end

        if expr.callee == "re_c0" && expr.receiver.nil?
          line "CALL :avant_re_c0"
          return
        end

        if expr.callee == "re_c1" && expr.receiver.nil?
          line "CALL :avant_re_c1"
          return
        end

        if expr.callee == "re_count" && expr.receiver.nil?
          emit_expr(expr.args[2])
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_re_count"
          return
        end

        if expr.callee == "hash_last_key" && expr.receiver.nil?
          line "CALL :avant_hash_last_key"
          return
        end

        if expr.callee == "exp" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_exp"
          return
        end

        if expr.callee == "json_root" && expr.receiver.nil?
          emit_json_root(expr)
          return
        end

        if expr.callee == "json_get" && expr.receiver.nil?
          emit_json_obj_get(expr)
          return
        end

        if expr.callee == "json_len" && expr.receiver.nil?
          emit_json_len(expr)
          return
        end

        if expr.callee == "json_at" && expr.receiver.nil?
          emit_json_at(expr)
          return
        end

        if expr.callee == "json_f64" && expr.receiver.nil?
          emit_json_f64(expr)
          return
        end

        if expr.callee == "json_sum_f64" && expr.receiver.nil?
          emit_json_sum_f64(expr)
          return
        end

        if expr.callee == "json_parse" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_json_parse"
          return
        end

        if (expr.callee == "json_int" || expr.callee == "json_str") && expr.receiver.nil?
          emit_json_get(expr, expr.callee == "json_str")
          return
        end

        if expr.callee == "http_roundtrip" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_http_roundtrip"
          return
        end

        if expr.callee == "sqrt" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "UNARY :sqrt"
          return
        end

        if expr.callee == "wide_op" && expr.receiver.nil?
          emit_expr(expr.args[4])
          emit_expr(expr.args[3])
          emit_expr(expr.args[2])
          emit_expr(expr.args[1])
          emit_expr(expr.args[0])
          line "CALL :avant_wide_op"
          return
        end

        if expr.callee == "wide_hi" && expr.receiver.nil?
          line "CALL :avant_wide_hi"
          return
        end

        if expr.callee == "checksum_f64" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_checksum_f64"
          return
        end

        if expr.callee == "checksum_str" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line "CALL :avant_checksum_str"
          return
        end

        if expr.callee == "puts_u" && expr.receiver.nil?
          emit_expr(expr.args[0])
          line %(PUSH "%u\\n")
          line "PRINTF 1"
          return
        end

        if recv = expr.receiver
          if expr.callee == "new"
            emit_ctor_call(expr, recv)
          elsif recv.type.try(&.float?)
            emit_float_method(expr)
          elsif recv.type.try(&.integer?)
            emit_int_method(expr)
          elsif recv.type.try(&.string?)
            emit_string_method(expr)
          elsif recv.type.try(&.array?)
            emit_array_method(expr, recv.type.as(ArrayTy))
          elsif recv.type.try(&.hash?)
            emit_hash_method(expr, recv.type.as(HashTy))
          elsif recv.type.try(&.buf?)
            emit_buf_method(expr)
          elsif recv.type.try(&.join_handle?)
            emit_join(expr)
          else
            emit_method_named(recv, expr.resolved.not_nil!, expr.args)
          end
          return
        end

        emit_call_named(expr.resolved || expr.callee, expr.args)
      end

      private def emit_float_method(expr : AST::Call) : Nil
        emit_expr(expr.receiver.not_nil!)
        case expr.callee
        when "sqrt"
          line "UNARY :sqrt"
        when "exp"
          line "CALL :avant_exp"
        when "abs"
          line "UNARY :abs"
        when "floor"
          line "UNARY :floor"
        when "to_i"
          line "UNARY :trunc"
          line "AS :i32"
        else
          raise "unknown float method"
        end
      end

      private def emit_int_method(expr : AST::Call) : Nil
        recv = expr.receiver.not_nil!
        emit_expr(recv)
        emit_int_convert(recv.type.not_nil!, expr.callee)
      end

      private def emit_string_method(expr : AST::Call) : Nil
        emit_expr(expr.receiver.not_nil!)
        case expr.callee
        when "to_f"
          line "CALL :avant_str_to_f"
        when "to_i"
          line "CALL :avant_str_to_i"
        else
          raise "unknown string method"
        end
      end

      private def emit_float_field(expr : AST::FieldAccess) : Nil
        emit_expr(expr.object)
        case expr.field
        when "sqrt"
          line "UNARY :sqrt"
        when "abs"
          line "UNARY :abs"
        when "floor"
          line "UNARY :floor"
        when "to_i"
          line "UNARY :trunc"
          line "AS :i32"
        else
          raise "unknown float field"
        end
      end

      private def emit_int_field(expr : AST::FieldAccess) : Nil
        emit_expr(expr.object)
        emit_int_convert(expr.object.type.not_nil!, expr.field)
      end

      private def emit_ctor_call(expr : AST::Call, recv : AST::Expr) : Nil
        name = expr.resolved || begin
          ty = recv.type
          if ty.is_a?(AggTy)
            "#{ty.name}__new"
          else
            "#{recv.as(AST::Name).ident}__new"
          end
        end
        emit_call_named(name, expr.args)
      end

      private def initialize_sig(owner : String) : MethodSig?
        bucket = @methods[owner]?
        return nil unless bucket
        bucket.each_value do |sig|
          return sig if sig.node.name == "initialize"
        end
        nil
      end

      private def method_for(recv : AST::Expr, name : String) : MethodSig
        ty = recv.type.as(AggTy)
        @methods[ty.name][name]
      end

      private def emit_method_named(recv : AST::Expr, name : String, args : Array(AST::Expr)) : Nil
        emit_args(args)
        emit_self_ptr(recv)
        line "CALL :#{name}"
      end

      private def emit_method_invoke(recv : AST::Expr, sig : MethodSig, args : Array(AST::Expr)) : Nil
        emit_method_named(recv, sig.mangled, args)
      end

      private def emit_self_ptr(recv : AST::Expr) : Nil
        ty = recv.type.not_nil!
        if ty.class?
          emit_expr(recv)
        else
          emit_lvalue(recv)
          line "ADDR"
        end
      end

      private def emit_call_named(name : String, args : Array(AST::Expr)) : Nil
        emit_args(args)
        line "CALL :#{name}"
      end

      private def emit_args(args : Array(AST::Expr)) : Nil
        if args.size <= 1
          args.each { |arg| emit_expr(arg) }
          return
        end
        temps = args.map do |arg|
          emit_expr(arg)
          tmp = new_temp
          line "LOCAL :#{tmp} :#{arg.type.not_nil!.myc}"
          line "STORE"
          ensure_root(tmp, arg.type.not_nil!)
          tmp
        end
        temps.reverse_each { |tmp| line "LOCAL :#{tmp}" }
      end

      private def emit_named_args(names : Array(String), types : Array(Ty)) : Nil
        if names.size <= 1
          names.each { |name| line "LOCAL :#{name}" }
          return
        end
        temps = names.map_with_index do |name, i|
          line "LOCAL :#{name}"
          tmp = new_temp
          line "LOCAL :#{tmp} :#{types[i].myc}"
          line "STORE"
          ensure_root(tmp, types[i])
          tmp
        end
        temps.reverse_each { |tmp| line "LOCAL :#{tmp}" }
      end

      private def emit_struct_literal(expr : AST::StructLiteral) : Nil
        ty = expr.type.as(StructTy)
        temps = Array(String).new(ty.fields.size, "")
        expr.fields.each do |name, value|
          index = ty.field_index(name).not_nil!
          emit_expr(value)
          tmp = new_temp
          line "LOCAL :#{tmp} :#{ty.fields[index][1].myc}"
          line "STORE"
          temps[index] = tmp
        end
        (temps.size - 1).downto(0) { |i| line "LOCAL :#{temps[i]}" }
        line "CREATE :#{ty.myc}"
      end

      private def emit_array_new(expr : AST::ArrayNew) : Nil
        ty = expr.type.as(ArrayTy)
        len_tmp = new_temp
        ptr_tmp = new_temp
        emit_expr(expr.size)
        line "LOCAL :#{len_tmp} :i32"
        line "STORE"
        line "LOCAL :#{len_tmp}"
        emit_alloc_count(ty.elem)
        line "LOCAL :#{ptr_tmp} :ptr<#{ty.elem.myc}>"
        line "STORE"
        emit_ptr_root(ptr_tmp)
        emit_array_header(ty, ptr_tmp, len_tmp, len_tmp)
      end

      private def emit_array_literal(expr : AST::ArrayLiteral) : Nil
        ty = expr.type.as(ArrayTy)
        n = expr.elements.size
        ptr_tmp = new_temp
        line "PUSH #{n}"
        emit_alloc_count(ty.elem)
        line "LOCAL :#{ptr_tmp} :ptr<#{ty.elem.myc}>"
        line "STORE"
        emit_ptr_root(ptr_tmp)
        expr.elements.each_with_index do |el, i|
          emit_expr(el)
          emit_coerce(el.type.not_nil!, ty.elem)
          line "PUSH #{i}"
          line "LOCAL :#{ptr_tmp}"
          line "BINARY :add"
          line "DEREF"
          line "STORE"
        end
        n_tmp = new_temp
        line "PUSH #{n}"
        line "LOCAL :#{n_tmp} :i32"
        line "STORE"
        emit_array_header(ty, ptr_tmp, n_tmp, n_tmp)
      end

      private def emit_puts(arg : AST::Expr) : Nil
        emit_expr(arg)
        ty = arg.type.not_nil!
        case
        when ty.int?
          line %(PUSH "%d\\n")
          line "PRINTF 1"
        when ty.int64?
          line %(PUSH "%lld\\n")
          line "PRINTF 1"
        when ty.uint64?
          line %(PUSH "%llu\\n")
          line "PRINTF 1"
        when ty.string?
          line %(PUSH "%s\\n")
          line "PRINTF 1"
        when ty.float?
          line %(PUSH "%g\\n")
          line "PRINTF 1"
        when ty.bool?
          line "AS :i32"
          line %(PUSH "%d\\n")
          line "PRINTF 1"
        else
          raise CompileError.at(arg.location, "puts cannot print #{ty}")
        end
      end

      private def myc_binop(kind : Token::Kind) : String
        case kind
        when .plus?        then "add"
        when .minus?       then "sub"
        when .star?        then "mul"
        when .slash?       then "div"
        when .percent?     then "rem"
        when .amp?         then "and"
        when .pipe?        then "or"
        when .caret?       then "xor"
        when .less_less?   then "shl"
        when .greater_greater? then "sar"
        when .eq_eq?       then "eq"
        when .not_eq?      then "not_eq"
        when .less?        then "less"
        when .less_eq?     then "less_eq"
        when .greater?     then "more"
        when .greater_eq?  then "more_eq"
        else
          raise "unsupported binop #{kind}"
        end
      end

      private def myc_string(value : String) : String
        String.build do |io|
          io << '"'
          value.each_char do |c|
            case c
            when '\n' then io << "\\n"
            when '\t' then io << "\\t"
            when '\r' then io << "\\r"
            when '"'  then io << "\\\""
            when '\\' then io << "\\\\"
            else           io << c
            end
          end
          io << '"'
        end
      end

      private def emit_runtime_decl : Nil
        emit_ext_func("avant_alloc", ["u64", "u32"], "ptr<void>")
        emit_ext_func("avant_pin", ["ptr<void>"], nil)
        emit_ext_func("avant_gc_enter", [] of String, nil)
        emit_ext_func("avant_gc_leave", [] of String, nil)
        emit_ext_func("avant_gc_root", ["ptr<void>"], nil)
        emit_ext_func("avant_type_map", ["u32", "u64"], nil)
        emit_ext_func("avant_barrier", ["ptr<void>"], nil)
        emit_ext_func("avant_str_concat", ["ptr<u8>", "ptr<u8>"], "ptr<u8>")
        emit_ext_func("avant_str_from_int", ["i32"], "ptr<u8>")
        emit_ext_func("avant_str_from_i64", ["i64"], "ptr<u8>")
        emit_ext_func("avant_str_from_u64", ["u64"], "ptr<u8>")
        emit_ext_func("avant_str_from_float", ["f64"], "ptr<u8>")
        emit_ext_func("avant_str_from_bool", ["i32"], "ptr<u8>")
        emit_ext_func("avant_str_from_byte", ["i32"], "ptr<u8>")
        emit_ext_func("avant_str_from_i32_bytes", ["ptr<void>"], "ptr<u8>")
        emit_ext_func("avant_str_repeat_byte", ["i32", "i32"], "ptr<u8>")
        emit_ext_func("avant_str_slice", ["ptr<u8>", "i32", "i32"], "ptr<u8>")
        emit_ext_func("avant_str_fmt_float", ["f64", "i32"], "ptr<u8>")
        emit_ext_func("avant_str_eq", ["ptr<u8>", "ptr<u8>"], "i32")
        emit_ext_func("avant_str_size", ["ptr<u8>"], "i32")
        emit_ext_func("avant_str_byte", ["ptr<u8>", "i32"], "i32")
        emit_ext_func("avant_str_to_f", ["ptr<u8>"], "f64")
        emit_ext_func("avant_str_to_f_slice", ["ptr<u8>", "i32", "i32"], "f64")
        emit_ext_func("avant_str_to_i", ["ptr<u8>"], "i32")
        emit_ext_func("avant_checksum_f64", ["f64"], "i32")
        emit_ext_func("avant_checksum_str", ["ptr<u8>"], "i32")
        emit_ext_func("avant_wide_op", ["i32", "i32", "i32", "i32", "i32"], "i32")
        emit_ext_func("avant_wide_hi", [] of String, "i32")
        emit_ext_func("avant_exp", ["f64"], "f64")
        emit_ext_func("avant_hash_new", ["i32", "i32"], "ptr<void>")
        emit_ext_func("avant_hash_size", ["ptr<void>"], "i32")
        emit_ext_func("avant_hash_set_i32", ["ptr<void>", "ptr<u8>", "i32"], nil)
        emit_ext_func("avant_hash_set_str", ["ptr<void>", "ptr<u8>", "ptr<u8>"], nil)
        emit_ext_func("avant_hash_set_i32k_i32", ["ptr<void>", "i32", "i32"], nil)
        emit_ext_func("avant_hash_get_i32", ["ptr<void>", "ptr<u8>", "ptr<i32>"], "i32")
        emit_ext_func("avant_hash_get_str", ["ptr<void>", "ptr<u8>", "ptr<i32>"], "ptr<u8>")
        emit_ext_func("avant_hash_get_i32k_i32", ["ptr<void>", "i32", "ptr<i32>"], "i32")
        emit_ext_func("avant_hash_del", ["ptr<void>", "ptr<u8>"], nil)
        emit_ext_func("avant_hash_del_i32k", ["ptr<void>", "i32"], nil)
        emit_ext_func("avant_hash_inc_slice", ["ptr<void>", "ptr<u8>", "i32", "i32"], "i32")
        emit_ext_func("avant_hash_last_key", [] of String, "ptr<u8>")
        emit_ext_func("avant_hash_get_str_slice", ["ptr<void>", "ptr<u8>", "i32", "i32", "ptr<i32>"], "ptr<u8>")
        emit_ext_func("avant_hash_get_concat", ["ptr<void>", "ptr<u8>", "ptr<u8>", "ptr<i32>"], "i32")
        emit_ext_func("avant_buf_new", ["i32"], "ptr<void>")
        emit_ext_func("avant_buf_push_byte", ["ptr<void>", "i32"], nil)
        emit_ext_func("avant_buf_push_str", ["ptr<void>", "ptr<u8>"], nil)
        emit_ext_func("avant_buf_push_slice", ["ptr<void>", "ptr<u8>", "i32", "i32"], nil)
        emit_ext_func("avant_buf_push_fmt_f", ["ptr<void>", "f64", "i32"], nil)
        emit_ext_func("avant_buf_push_fmt_i", ["ptr<void>", "i32"], nil)
        emit_ext_func("avant_buf_to_str", ["ptr<void>"], "ptr<u8>")
        emit_ext_func("avant_buf_size", ["ptr<void>"], "i32")
        emit_ext_func("avant_buf_clear", ["ptr<void>"], nil)
        emit_ext_func("avant_buf_starts", ["ptr<void>", "ptr<u8>"], "i32")
        emit_ext_func("avant_b64_encode", ["ptr<u8>"], "ptr<u8>")
        emit_ext_func("avant_b64_decode", ["ptr<u8>"], "ptr<u8>")
        emit_ext_func("avant_b64_encode_buf", ["ptr<void>", "ptr<u8>"], nil)
        emit_ext_func("avant_b64_decode_buf", ["ptr<void>", "ptr<u8>"], nil)
        emit_ext_func("avant_crc32_i32", ["ptr<void>"], "i32")
        emit_ext_func("avant_sha256_word0", ["ptr<void>"], "i32")
        emit_ext_func("avant_re_compile", ["ptr<u8>", "i32"], "ptr<void>")
        emit_ext_func("avant_re_find", ["ptr<void>", "ptr<u8>", "i32"], "i32")
        emit_ext_func("avant_re_m0", [] of String, "i32")
        emit_ext_func("avant_re_m1", [] of String, "i32")
        emit_ext_func("avant_re_c0", [] of String, "i32")
        emit_ext_func("avant_re_c1", [] of String, "i32")
        emit_ext_func("avant_re_count", ["ptr<u8>", "ptr<u8>", "i32"], "i32")
        emit_ext_func("avant_array_push_slot", ["ptr<void>", "u64", "u32"], "ptr<void>")
        emit_ext_func("avant_array_push_ptr", ["ptr<void>", "ptr<void>", "u32"], nil)
        emit_ext_func("avant_array_set_ptr", ["ptr<void>", "i32", "ptr<void>"], nil)
        emit_ext_func("avant_array_get_ptr", ["ptr<void>", "i32"], "ptr<void>")
        emit_ext_func("avant_array_pop_ptr", ["ptr<void>"], "ptr<void>")
        emit_ext_func("avant_array_push_i32", ["ptr<void>", "i32"], nil)
        emit_ext_func("avant_array_clear", ["ptr<void>"], nil)
        emit_ext_func("avant_array_reserve", ["ptr<void>", "i32", "u64", "u32"], nil)
        emit_ext_func("avant_array_pop_slot", ["ptr<void>", "u64"], "ptr<void>")
        emit_ext_func("avant_array_sort_i32", ["ptr<void>"], nil)
        emit_ext_func("avant_array_fill_i32", ["ptr<void>", "i32"], nil)
        emit_ext_func("avant_zlib_compress", ["ptr<u8>"], "ptr<u8>")
        emit_ext_func("avant_zlib_uncompress", ["ptr<u8>"], "ptr<u8>")
        emit_ext_func("avant_sha256", ["ptr<u8>"], "ptr<u8>")
        emit_ext_func("avant_spawn", ["ptr<void>", "ptr<void>"], "ptr<void>")
        emit_ext_func("avant_join", ["ptr<void>"], "i32")
        emit_ext_func("avant_now_ms", [] of String, "i32")
        emit_ext_func("avant_now_us", [] of String, "i32")
        emit_ext_func("avant_io_init_argv", ["i32", "ptr<void>"], nil)
        emit_ext_func("avant_argv", [] of String, "ptr<void>")
        emit_ext_func("avant_file_read", ["ptr<u8>", "ptr<i32>"], "ptr<u8>")
        emit_ext_func("avant_file_write", ["ptr<u8>", "ptr<u8>"], "i32")
        emit_ext_func("avant_process_run", ["ptr<u8>", "ptr<void>"], "i32")
        emit_ext_func("avant_process_run_out", ["ptr<u8>", "ptr<void>", "ptr<u8>"], "i32")
        emit_ext_func("avant_env_get", ["ptr<u8>", "ptr<i32>"], "ptr<u8>")
        emit_ext_func("avant_file_exists", ["ptr<u8>"], "i32")
        emit_ext_func("avant_dir_list", ["ptr<u8>"], "ptr<void>")
        emit_ext_func("avant_json_parse", ["ptr<u8>"], "ptr<void>")
        emit_ext_func("avant_json_free", ["ptr<void>"], nil)
        emit_ext_func("avant_json_gen_body", ["i32"], "ptr<u8>")
        emit_ext_func("avant_json_gen_into", ["ptr<void>", "i32"], nil)
        emit_ext_func("avant_json_get_int", ["ptr<void>", "ptr<u8>", "ptr<i32>"], "i32")
        emit_ext_func("avant_json_get_str", ["ptr<void>", "ptr<u8>", "ptr<i32>"], "ptr<u8>")
        emit_ext_func("avant_json_root", ["ptr<void>"], "ptr<void>")
        emit_ext_func("avant_json_obj_get", ["ptr<void>", "ptr<u8>"], "ptr<void>")
        emit_ext_func("avant_json_arr_len", ["ptr<void>"], "i32")
        emit_ext_func("avant_json_arr_get", ["ptr<void>", "i32"], "ptr<void>")
        emit_ext_func("avant_json_as_f64", ["ptr<void>", "ptr<i32>"], "f64")
        emit_ext_func("avant_json_obj_f64", ["ptr<void>", "ptr<u8>", "ptr<i32>"], "f64")
        emit_ext_func("avant_json_arr_sum_f64", ["ptr<void>", "ptr<u8>"], "f64")
        emit_ext_func("avant_http_roundtrip", ["ptr<u8>"], "ptr<u8>")
      end

      private def emit_ext_func(name : String, args : Array(String), ret : String?) : Nil
        line "FUNC :#{name}"
        @indent += 1
        if ret
          line "RETURN"
          @indent += 1
          line "TYPE :#{ret}"
          @indent -= 1
        end
        unless args.empty?
          line "ARGS"
          @indent += 1
          args.each { |ty| line "TYPE :#{ty}" }
          @indent -= 1
        end
        @indent -= 1
        line "ENDFUNC"
        @io << '\n'
      end

      private def emit_lib_decls : Nil
        @program.libs.each do |lib_def|
          lib_def.funs.each { |cfun| emit_lib_fun(cfun) }
        end
      end

      private def emit_lib_fun(fn : AST::FunDecl) : Nil
        params = fn.params.map { |p| @resolver.resolve_value(p.type) }
        ret = if t = fn.return_type
                @resolver.resolve(t)
              else
                VoidTy::INSTANCE
              end

        line "FUNC :#{fn.name}"
        @indent += 1
        unless ret.void?
          line "RETURN"
          @indent += 1
          line "TYPE :#{ret.myc}"
          @indent -= 1
        end
        unless params.empty?
          line "ARGS"
          @indent += 1
          params.each { |ty| line "TYPE :#{ty.myc}" }
          @indent -= 1
        end
        @indent -= 1
        line "ENDFUNC"
        @io << '\n'
      end

      private def assign_type_ids : Nil
        @program.classes.each_with_index do |defn, i|
          @type_ids[defn.name] = Runtime::CLASS_TYPE_BASE + i
        end
      end

      private def class_type_id(ty : ClassTy) : Int32
        @type_ids[ty.name]
      end

      private def emit_alloc_sizeof(layout : String, type_id : Int32) : Nil
        line "PUSH #{type_id} :u32"
        line "SIZEOF :#{layout}"
        line "CALL :avant_alloc"
      end

      private def emit_alloc_count(elem : Ty) : Nil
        line "AS :u64"
        line "SIZEOF :#{elem.myc}"
        line "BINARY :mul"
        line "PUSH #{array_type_id(elem)} :u32"
        line "STACK :swap2"
        line "CALL :avant_alloc"
        line "AS :ptr<#{elem.myc}>"
      end

      private def array_type_id(elem : Ty) : Int32
        heap_ptr_value?(elem) ? Runtime::ARRAY_PTR_TYPE_ID : Runtime::ARRAY_TYPE_ID
      end

      private def emit_ret : Nil
        line "CALL :avant_gc_leave"
        if @c_main
          line "PUSH 0"
        end
        line "RET"
      end

      private def emit_gc_leave_if_fallthrough(fn : AST::Function) : Nil
        last = fn.body.last?
        return if last.is_a?(AST::ReturnStmt)
        if last.is_a?(AST::ExprStmt) && !@return_type.void?
          return
        end
        emit_ret
      end

      private def emit_type_maps : Nil
        line "PUSH 1 :u64"
        line "PUSH #{Runtime::ARRAY_OBJ_TYPE_ID} :u32"
        line "CALL :avant_type_map"
        line "PUSH 1 :u64"
        line "PUSH #{Runtime::BUF_TYPE_ID} :u32"
        line "CALL :avant_type_map"
        line "PUSH 7 :u64"
        line "PUSH #{Runtime::HASH_TYPE_ID} :u32"
        line "CALL :avant_type_map"
        @type_ids.each do |name, id|
          ty = @named[name].as(ClassTy)
          bits = pointer_word_bits(ty)
          line "PUSH #{bits} :u64"
          line "PUSH #{id} :u32"
          line "CALL :avant_type_map"
        end
      end

      private def pointer_word_bits(ty : ClassTy) : UInt64
        bits = 0_u64
        offset = 0_u64
        ty.fields.each do |_, fty|
          offset = align_up_u64(offset, abi_align(fty))
          bits = add_ptr_bits(fty, offset, bits)
          offset += abi_size(fty)
        end
        bits
      end

      private def add_ptr_bits(ty : Ty, offset : UInt64, bits : UInt64) : UInt64
        case ty
        when ClassTy
          word_bit(offset, bits)
        when ArrayTy, HashTy, JoinHandleTy, StringTy, BufTy
          word_bit(offset, bits)
        when UnionTy
          off = offset + 4_u64
          off = align_up_u64(off, 8_u64)
          ty.payload_members.reduce(bits) do |acc, m|
            off = align_up_u64(off, abi_align(m))
            acc = add_ptr_bits(m, off, acc)
            off += abi_size(m)
            acc
          end
        when StructTy
          off = offset
          ty.fields.reduce(bits) do |acc, (_, fty)|
            off = align_up_u64(off, abi_align(fty))
            acc = add_ptr_bits(fty, off, acc)
            off += abi_size(fty)
            acc
          end
        else
          bits
        end
      end

      private def word_bit(offset : UInt64, bits : UInt64) : UInt64
        word = offset // 8
        raise "object pointer map exceeds 64 words" if word >= 64
        bits | (1_u64 << word)
      end

      private def abi_align(ty : Ty) : UInt64
        case ty
        when IntTy, NilTy
          4_u64
        when Int64Ty, UInt64Ty
          8_u64
        when BoolTy
          1_u64
        when Float64Ty, ClassTy, PtrTy, StringTy, HashTy, JoinHandleTy, ArrayTy, BufTy
          8_u64
        when UnionTy
          aligns = [4_u64]
          ty.payload_members.each { |m| aligns << abi_align(m) }
          aligns.max
        when StructTy
          ty.fields.empty? ? 1_u64 : ty.fields.max_of { |_, f| abi_align(f) }
        else
          8_u64
        end
      end

      private def abi_size(ty : Ty) : UInt64
        case ty
        when IntTy, NilTy
          4_u64
        when Int64Ty, UInt64Ty
          8_u64
        when BoolTy
          1_u64
        when Float64Ty, ClassTy, PtrTy, StringTy, HashTy, JoinHandleTy, ArrayTy, BufTy
          8_u64
        when UnionTy
          offset = 4_u64
          ty.payload_members.each do |m|
            offset = align_up_u64(offset, abi_align(m))
            offset += abi_size(m)
          end
          align_up_u64(offset, abi_align(ty))
        when StructTy
          offset = 0_u64
          ty.fields.each do |_, fty|
            offset = align_up_u64(offset, abi_align(fty))
            offset += abi_size(fty)
          end
          align_up_u64(offset, abi_align(ty))
        else
          8_u64
        end
      end

      private def align_up_u64(n : UInt64, align : UInt64) : UInt64
        (n + align - 1) // align * align
      end

      private def heap_ptr_value?(ty : Ty) : Bool
        ty.class? || ty.array? || ty.string? || ty.hash? || ty.buf? || ty.join_handle?
      end

      private def contains_heap_ptr?(ty : Ty) : Bool
        case ty
        when ClassTy, ArrayTy, HashTy, JoinHandleTy, StringTy, BufTy
          true
        when UnionTy
          ty.payload_members.any? { |m| contains_heap_ptr?(m) }
        when StructTy
          ty.fields.any? { |_, fty| contains_heap_ptr?(fty) }
        else
          false
        end
      end

      private def ensure_root(name : String, ty : Ty) : Nil
        return if @rooted.includes?(name)
        return unless contains_heap_ptr?(ty)
        @rooted << name
        if struct_self?(name)
          emit_struct_ptr_roots(name, @self_ty.as(StructTy))
        else
          emit_root_ty(name, ty)
        end
      end

      private def emit_root_ty(name : String, ty : Ty) : Nil
        case ty
        when ClassTy, HashTy, JoinHandleTy, StringTy, BufTy
          emit_ptr_root(name)
        when ArrayTy
          emit_ptr_root(name)
        when UnionTy
          emit_union_value_roots(name, ty)
        when StructTy
          emit_struct_value_roots(name, ty, [] of Int32)
        end
      end

      private def emit_ptr_root(name : String) : Nil
        @rooted << name
        line "LOCAL :#{name}"
        line "ADDR"
        line "CALL :avant_gc_root"
      end

      private def emit_struct_value_roots(name : String, ty : StructTy, prefix : Array(Int32)) : Nil
        ty.fields.each_with_index do |(_, fty), i|
          path = prefix + [i]
          case fty
          when ClassTy
            emit_field_root(name, path)
          when ArrayTy
            emit_field_root(name, path + [0])
          when StructTy
            emit_struct_value_roots(name, fty, path) if contains_heap_ptr?(fty)
          end
        end
      end

      private def emit_struct_ptr_roots(name : String, ty : StructTy) : Nil
        ty.fields.each_with_index do |(_, fty), i|
          next unless contains_heap_ptr?(fty)
          line "LOCAL :#{name}"
          line "DEREF"
          case fty
          when ClassTy
            line "FIELD #{i}"
            line "ADDR"
            line "CALL :avant_gc_root"
          when ArrayTy
            line "FIELD #{i}"
            line "FIELD 0"
            line "ADDR"
            line "CALL :avant_gc_root"
          when StructTy
            line "FIELD #{i}"
            emit_nested_struct_fields(fty)
          end
        end
      end

      private def emit_nested_struct_fields(ty : StructTy) : Nil
        ty.fields.each_with_index do |(_, fty), i|
          next unless contains_heap_ptr?(fty)
          line "FIELD #{i}"
          case fty
          when ClassTy
            line "ADDR"
            line "CALL :avant_gc_root"
          when ArrayTy
            line "FIELD 0"
            line "ADDR"
            line "CALL :avant_gc_root"
          when StructTy
            emit_nested_struct_fields(fty)
          end
        end
      end

      private def emit_field_root(name : String, fields : Array(Int32)) : Nil
        line "LOCAL :#{name}"
        fields.each { |i| line "FIELD #{i}" }
        line "ADDR"
        line "CALL :avant_gc_root"
      end

      private def new_temp : String
        @temps += 1
        "__t#{@temps}"
      end

      private def line(text : String) : Nil
        @indent.times { @io << "  " }
        @io << text
        @io << '\n'
      end
    end
  end
end
