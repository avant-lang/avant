module Avant
  module Codegen
    class Myc
      private def emit_union_def(ty : UnionTy) : Nil
        line "STRUCT :#{ty.layout_myc}"
        @indent += 1
        line "TYPE :i32"
        ty.payload_members.each { |m| line "TYPE :#{m.myc}" }
        @indent -= 1
        line "ENDSTRUCT"
        @io << '\n'
      end

      private def emit_union_value_roots(name : String, ty : UnionTy) : Nil
        ty.payload_members.each_with_index do |m, i|
          next unless contains_heap_ptr?(m)
          line "LOCAL :#{name}"
          line "FIELD #{i + 1}"
          line "ADDR"
          line "CALL :avant_gc_root"
        end
      end

      private def emit_array_header(ty : ArrayTy, buf : String, size : String, cap : String) : Nil
        emit_alloc_sizeof(ty.layout_myc, Runtime::ARRAY_OBJ_TYPE_ID)
        line "AS :#{ty.myc}"
        arr = new_temp
        line "LOCAL :#{arr} :#{ty.myc}"
        line "STORE"
        ensure_root(arr, ty.as(Ty))
        line "LOCAL :#{buf}"
        line "LOCAL :#{arr}"
        line "DEREF"
        line "FIELD 0"
        line "STORE"
        line "LOCAL :#{size}"
        line "LOCAL :#{arr}"
        line "DEREF"
        line "FIELD 1"
        line "STORE"
        line "LOCAL :#{cap}"
        line "LOCAL :#{arr}"
        line "DEREF"
        line "FIELD 2"
        line "STORE"
        line "LOCAL :#{arr}"
        line "CALL :avant_barrier"
        line "LOCAL :#{arr}"
      end

      private def emit_coerce(got : Ty, want : Ty) : Nil
        return if got.same?(want)
        if want.is_a?(UnionTy) && want.includes?(got)
          emit_wrap_union(got, want)
        end
      end

      private def emit_wrap_union(got : Ty, want : UnionTy) : Nil
        val = new_temp
        line "LOCAL :#{val} :#{got.myc}"
        line "STORE"
        ensure_root(val, got)
        payloads = want.payload_members
        (payloads.size - 1).downto(0) do |i|
          m = payloads[i]
          if m.same?(got)
            line "LOCAL :#{val}"
          else
            emit_zero(m)
          end
        end
        line "PUSH #{want.tag_of(got)}"
        line "CREATE :#{want.layout_myc}"
      end

      private def emit_zero(ty : Ty) : Nil
        case ty
        when Float64Ty
          line "PUSH 0 :f64"
        when Int64Ty
          line "PUSH 0 :i64"
        when UInt64Ty
          line "PUSH 0 :u64"
        when BoolTy
          line "PUSH false"
        else
          line "PUSH 0"
          line "AS :#{ty.myc}" unless ty.int? || ty.nil_type?
        end
      end

      private def emit_try(expr : AST::Try) : Nil
        inner = expr.expr
        uty = inner.type.as(UnionTy)
        emit_expr(inner)
        tmp = new_temp
        line "LOCAL :#{tmp} :#{uty.myc}"
        line "STORE"
        ensure_root(tmp, uty.as(Ty))
        success, fails = try_split(uty)
        fail_tag = uty.tag_of(fails[0])
        unwrapped = new_temp
        line "LOCAL :#{tmp}"
        line "FIELD 0"
        line "PUSH #{fail_tag}"
        line "BINARY :eq"
        line "IF"
        @indent += 1
        line "THEN"
        @indent += 1
        emit_failure_return(tmp, uty, fails[0])
        @indent -= 1
        line "ELSE"
        @indent += 1
        emit_unwrap(tmp, uty, success)
        line "LOCAL :#{unwrapped} :#{success.myc}"
        line "STORE"
        ensure_root(unwrapped, success)
        @indent -= 1
        @indent -= 1
        line "ENDIF"
        line "LOCAL :#{unwrapped}"
      end

      private def try_split(got : UnionTy) : {Ty, Array(Ty)}
        if got.includes_nil?
          return {got.without_nil, [NilTy::INSTANCE.as(Ty)]}
        end
        ret = @return_type.as(UnionTy)
        fails = got.members.select { |m| ret.includes?(m) }
        succ = got.members.reject { |m| fails.any?(&.same?(m)) }
        {succ[0], fails}
      end

      private def emit_failure_return(tmp : String, got : UnionTy, fail : Ty) : Nil
        if @return_type.same?(got)
          line "LOCAL :#{tmp}"
        elsif fail.nil_type?
          emit_zero(fail)
          emit_wrap_union(fail, @return_type.as(UnionTy))
        else
          emit_unwrap(tmp, got, fail)
          emit_wrap_union(fail, @return_type.as(UnionTy))
        end
        emit_ret
      end

      private def emit_unwrap(tmp : String, uty : UnionTy, success : Ty) : Nil
        if success.nil_type?
          line "PUSH 0"
          return
        end
        idx = uty.payload_index(success) || raise "missing payload"
        line "LOCAL :#{tmp}"
        line "FIELD #{idx}"
      end

      private def emit_if_assign(stmt : AST::IfStmt, bind : String) : Nil
        uty = stmt.cond.type.as(UnionTy)
        success = uty.without_nil
        emit_expr(stmt.cond)
        tmp = new_temp
        line "LOCAL :#{tmp} :#{uty.myc}"
        line "STORE"
        ensure_root(tmp, uty.as(Ty))
        nil_tag = uty.tag_of(NilTy::INSTANCE)
        line "LOCAL :#{tmp}"
        line "FIELD 0"
        line "PUSH #{nil_tag}"
        line "BINARY :not_eq"
        line "IF"
        @indent += 1
        line "THEN"
        @indent += 1
        emit_unwrap(tmp, uty, success)
        line "LOCAL :#{bind} :#{success.myc}"
        line "STORE"
        ensure_root(bind, success)
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

      private def emit_interp(expr : AST::InterpString) : Nil
        pieces = [] of String
        expr.parts.each do |part|
          if text = part.text
            next if text.empty?
            line %(PUSH #{myc_string(text)})
          elsif inner = part.expr
            emit_expr(inner)
            emit_to_string(inner.type.not_nil!)
          else
            next
          end
          tmp = new_temp
          line "LOCAL :#{tmp} :ptr<u8>"
          line "STORE"
          ensure_root(tmp, StringTy::INSTANCE.as(Ty))
          pieces << tmp
        end
        if pieces.empty?
          line %(PUSH "")
          return
        end
        acc = pieces[0]
        pieces[1..].each do |p|
          line "LOCAL :#{p}"
          line "LOCAL :#{acc}"
          line "CALL :avant_str_concat"
          acc = new_temp
          line "LOCAL :#{acc} :ptr<u8>"
          line "STORE"
          ensure_root(acc, StringTy::INSTANCE.as(Ty))
        end
        line "LOCAL :#{acc}"
      end

      private def emit_to_string(ty : Ty) : Nil
        case
        when ty.string?
        when ty.int?
          line "CALL :avant_str_from_int"
        when ty.int64?
          line "CALL :avant_str_from_i64"
        when ty.uint64?
          line "CALL :avant_str_from_u64"
        when ty.float?
          line "CALL :avant_str_from_float"
        when ty.bool?
          line "AS :i32"
          line "CALL :avant_str_from_bool"
        else
          raise "cannot interpolate #{ty}"
        end
      end

      private def emit_string_binary(expr : AST::Binary) : Nil
        case expr.op
        when .plus?
          emit_expr(expr.left)
          a = new_temp
          line "LOCAL :#{a} :ptr<u8>"
          line "STORE"
          ensure_root(a, StringTy::INSTANCE.as(Ty))
          emit_expr(expr.right)
          line "LOCAL :#{a}"
          line "CALL :avant_str_concat"
        when .eq_eq?, .not_eq?
          emit_expr(expr.left)
          a = new_temp
          line "LOCAL :#{a} :ptr<u8>"
          line "STORE"
          emit_expr(expr.right)
          line "LOCAL :#{a}"
          line "CALL :avant_str_eq"
          if expr.op.not_eq?
            line "PUSH 0"
            line "BINARY :eq"
          else
            line "AS :bool"
          end
        else
          raise "unsupported string op"
        end
      end

      private def emit_hash_new(expr : AST::HashNew) : Nil
        ty = expr.type.as(HashTy)
        key_kind = ty.key.string? ? 1 : 0
        val_kind = ty.val.string? ? 1 : 0
        line "PUSH #{val_kind}"
        line "PUSH #{key_kind}"
        line "CALL :avant_hash_new"
      end

      private def hash_set_func(ty : HashTy) : String
        if ty.key.int?
          "avant_hash_set_i32k_i32"
        elsif ty.val.string?
          "avant_hash_set_str"
        else
          "avant_hash_set_i32"
        end
      end

      private def hash_get_func(ty : HashTy) : String
        if ty.key.int?
          "avant_hash_get_i32k_i32"
        elsif ty.val.string?
          "avant_hash_get_str"
        else
          "avant_hash_get_i32"
        end
      end

      private def hash_del_func(ty : HashTy) : String
        ty.key.int? ? "avant_hash_del_i32k" : "avant_hash_del"
      end

      private def emit_hash_set(target : AST::Index, value : AST::Expr, ty : HashTy) : Nil
        emit_expr(value)
        val = new_temp
        line "LOCAL :#{val} :#{ty.val.myc}"
        line "STORE"
        ensure_root(val, ty.val)
        emit_expr(target.index)
        key = new_temp
        line "LOCAL :#{key} :#{ty.key.myc}"
        line "STORE"
        ensure_root(key, ty.key)
        emit_expr(target.array)
        h = new_temp
        line "LOCAL :#{h} :ptr<void>"
        line "STORE"
        ensure_root(h, ty.as(Ty))
        line "LOCAL :#{val}"
        line "LOCAL :#{key}"
        line "LOCAL :#{h}"
        line "CALL :#{hash_set_func(ty)}"
      end

      private def emit_hash_get(expr : AST::Index, ty : HashTy) : Nil
        found = new_temp
        line "PUSH 0"
        line "LOCAL :#{found} :i32"
        line "STORE"
        emit_expr(expr.index)
        key = new_temp
        line "LOCAL :#{key} :#{ty.key.myc}"
        line "STORE"
        emit_expr(expr.array)
        h = new_temp
        line "LOCAL :#{h} :ptr<void>"
        line "STORE"
        line "LOCAL :#{found}"
        line "ADDR"
        line "LOCAL :#{key}"
        line "LOCAL :#{h}"
        line "CALL :#{hash_get_func(ty)}"
        emit_hash_get_wrap(ty, found)
      end

      private def emit_hash_get_wrap(ty : HashTy, found : String) : Nil
        val = new_temp
        line "LOCAL :#{val} :#{ty.val.myc}"
        line "STORE"
        ensure_root(val, ty.val)
        want = UnionTy.nilable(ty.val).as(UnionTy)
        out = new_temp
        line "LOCAL :#{found}"
        line "PUSH 1"
        line "BINARY :eq"
        line "IF"
        @indent += 1
        line "THEN"
        @indent += 1
        line "LOCAL :#{val}"
        emit_wrap_union(ty.val, want)
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

      private def emit_hash_method(expr : AST::Call, ty : HashTy) : Nil
        case expr.callee
        when "get"
          idx = AST::Index.new(expr.location, expr.receiver.not_nil!, expr.args[0])
          idx.type = UnionTy.nilable(ty.val)
          emit_hash_get(idx, ty)
        when "delete"
          emit_expr(expr.args[0])
          key = new_temp
          line "LOCAL :#{key} :#{ty.key.myc}"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          h = new_temp
          line "LOCAL :#{h} :ptr<void>"
          line "STORE"
          line "LOCAL :#{key}"
          line "LOCAL :#{h}"
          line "CALL :#{hash_del_func(ty)}"
        when "inc_slice"
          emit_expr(expr.args[2])
          stop = new_temp
          line "LOCAL :#{stop} :i32"
          line "STORE"
          emit_expr(expr.args[1])
          start = new_temp
          line "LOCAL :#{start} :i32"
          line "STORE"
          emit_expr(expr.args[0])
          s = new_temp
          line "LOCAL :#{s} :ptr<u8>"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          h = new_temp
          line "LOCAL :#{h} :ptr<void>"
          line "STORE"
          line "LOCAL :#{stop}"
          line "LOCAL :#{start}"
          line "LOCAL :#{s}"
          line "LOCAL :#{h}"
          line "CALL :avant_hash_inc_slice"
        when "get_slice"
          found = new_temp
          line "PUSH 0"
          line "LOCAL :#{found} :i32"
          line "STORE"
          emit_expr(expr.args[2])
          stop = new_temp
          line "LOCAL :#{stop} :i32"
          line "STORE"
          emit_expr(expr.args[1])
          start = new_temp
          line "LOCAL :#{start} :i32"
          line "STORE"
          emit_expr(expr.args[0])
          s = new_temp
          line "LOCAL :#{s} :ptr<u8>"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          h = new_temp
          line "LOCAL :#{h} :ptr<void>"
          line "STORE"
          line "LOCAL :#{found}"
          line "ADDR"
          line "LOCAL :#{stop}"
          line "LOCAL :#{start}"
          line "LOCAL :#{s}"
          line "LOCAL :#{h}"
          line "CALL :avant_hash_get_str_slice"
          emit_hash_get_wrap(ty, found)
        when "get_concat"
          found = new_temp
          line "PUSH 0"
          line "LOCAL :#{found} :i32"
          line "STORE"
          emit_expr(expr.args[1])
          b = new_temp
          line "LOCAL :#{b} :ptr<u8>"
          line "STORE"
          emit_expr(expr.args[0])
          a = new_temp
          line "LOCAL :#{a} :ptr<u8>"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          h = new_temp
          line "LOCAL :#{h} :ptr<void>"
          line "STORE"
          line "LOCAL :#{found}"
          line "ADDR"
          line "LOCAL :#{b}"
          line "LOCAL :#{a}"
          line "LOCAL :#{h}"
          line "CALL :avant_hash_get_concat"
          emit_hash_get_wrap(ty, found)
        else
          raise "unknown hash method"
        end
      end

      private def emit_buf_method(expr : AST::Call) : Nil
        case expr.callee
        when "push_byte"
          emit_expr(expr.args[0])
          b = new_temp
          line "LOCAL :#{b} :i32"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          buf = new_temp
          line "LOCAL :#{buf} :ptr<void>"
          line "STORE"
          line "LOCAL :#{b}"
          line "LOCAL :#{buf}"
          line "CALL :avant_buf_push_byte"
        when "push_str"
          emit_expr(expr.args[0])
          s = new_temp
          line "LOCAL :#{s} :ptr<u8>"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          buf = new_temp
          line "LOCAL :#{buf} :ptr<void>"
          line "STORE"
          line "LOCAL :#{s}"
          line "LOCAL :#{buf}"
          line "CALL :avant_buf_push_str"
        when "push_slice"
          emit_expr(expr.args[2])
          stop = new_temp
          line "LOCAL :#{stop} :i32"
          line "STORE"
          emit_expr(expr.args[1])
          start = new_temp
          line "LOCAL :#{start} :i32"
          line "STORE"
          emit_expr(expr.args[0])
          s = new_temp
          line "LOCAL :#{s} :ptr<u8>"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          buf = new_temp
          line "LOCAL :#{buf} :ptr<void>"
          line "STORE"
          line "LOCAL :#{stop}"
          line "LOCAL :#{start}"
          line "LOCAL :#{s}"
          line "LOCAL :#{buf}"
          line "CALL :avant_buf_push_slice"
        when "push_fmt_f"
          emit_expr(expr.args[1])
          prec = new_temp
          line "LOCAL :#{prec} :i32"
          line "STORE"
          emit_expr(expr.args[0])
          v = new_temp
          line "LOCAL :#{v} :f64"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          buf = new_temp
          line "LOCAL :#{buf} :ptr<void>"
          line "STORE"
          line "LOCAL :#{prec}"
          line "LOCAL :#{v}"
          line "LOCAL :#{buf}"
          line "CALL :avant_buf_push_fmt_f"
        when "push_fmt_i"
          emit_expr(expr.args[0])
          n = new_temp
          line "LOCAL :#{n} :i32"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          buf = new_temp
          line "LOCAL :#{buf} :ptr<void>"
          line "STORE"
          line "LOCAL :#{n}"
          line "LOCAL :#{buf}"
          line "CALL :avant_buf_push_fmt_i"
        when "to_s"
          emit_expr(expr.receiver.not_nil!)
          line "CALL :avant_buf_to_str"
        when "clear"
          emit_expr(expr.receiver.not_nil!)
          line "CALL :avant_buf_clear"
        when "starts_with"
          emit_expr(expr.args[0])
          s = new_temp
          line "LOCAL :#{s} :ptr<u8>"
          line "STORE"
          emit_expr(expr.receiver.not_nil!)
          buf = new_temp
          line "LOCAL :#{buf} :ptr<void>"
          line "STORE"
          line "LOCAL :#{s}"
          line "LOCAL :#{buf}"
          line "CALL :avant_buf_starts"
        else
          raise "unknown buf method"
        end
      end

      private def emit_array_method(expr : AST::Call, ty : ArrayTy) : Nil
        case expr.callee
        when "push"
          emit_array_push(expr, ty)
        when "sort"
          emit_array_sort(expr)
        when "pop"
          emit_array_pop(expr.receiver.not_nil!, ty)
        when "fill"
          emit_expr(expr.args[0])
          emit_expr(expr.receiver.not_nil!)
          line "CALL :avant_array_fill_i32"
        when "clear"
          emit_array_clear(expr.receiver.not_nil!, ty)
        when "reserve"
          emit_array_reserve(expr, ty)
        when "each"
          emit_array_each(expr, ty)
        when "parallel"
          emit_array_parallel(expr, ty)
        else
          raise "unknown array method"
        end
      end

      private def emit_array_push(expr : AST::Call, ty : ArrayTy) : Nil
        if ty.elem.int?
          emit_expr(expr.args[0])
          emit_coerce(expr.args[0].type.not_nil!, ty.elem)
          emit_expr(expr.receiver.not_nil!)
          line "CALL :avant_array_push_i32"
          return
        end
        # Evaluate the element first. push_slot returns an interior
        # pointer into the buffer; a collecting CALL before the store
        # would leave the slot pointing at a moved buffer.
        emit_expr(expr.args[0])
        emit_coerce(expr.args[0].type.not_nil!, ty.elem)
        val = new_temp
        line "LOCAL :#{val} :#{ty.elem.myc}"
        line "STORE"
        ensure_root(val, ty.elem)
        emit_expr(expr.receiver.not_nil!)
        arr = new_temp
        line "LOCAL :#{arr} :#{ty.myc}"
        line "STORE"
        ensure_root(arr, ty.as(Ty))
        if heap_ptr_value?(ty.elem)
          line "PUSH #{array_type_id(ty.elem)} :u32"
          line "LOCAL :#{val}"
          line "LOCAL :#{arr}"
          line "CALL :avant_array_push_ptr"
          return
        end
        line "PUSH #{array_type_id(ty.elem)} :u32"
        line "SIZEOF :#{ty.elem.myc}"
        line "AS :u64"
        line "LOCAL :#{arr}"
        line "CALL :avant_array_push_slot"
        line "AS :ptr<#{ty.elem.myc}>"
        line "LOCAL :#{val}"
        line "STACK :swap2"
        line "DEREF"
        line "STORE"
      end

      private def emit_array_clear(receiver : AST::Expr, ty : ArrayTy) : Nil
        emit_expr(receiver)
        arr = new_temp
        line "LOCAL :#{arr} :#{ty.myc}"
        line "STORE"
        ensure_root(arr, ty.as(Ty))
        line "PUSH 0"
        line "LOCAL :#{arr}"
        line "DEREF"
        line "FIELD 1"
        line "STORE"
      end

      private def emit_array_reserve(expr : AST::Call, ty : ArrayTy) : Nil
        emit_expr(expr.receiver.not_nil!)
        arr = new_temp
        line "LOCAL :#{arr} :#{ty.myc}"
        line "STORE"
        ensure_root(arr, ty.as(Ty))
        line "PUSH #{array_type_id(ty.elem)} :u32"
        line "SIZEOF :#{ty.elem.myc}"
        line "AS :u64"
        emit_expr(expr.args[0])
        line "LOCAL :#{arr}"
        line "CALL :avant_array_reserve"
      end

      private def emit_array_each(expr : AST::Call, ty : ArrayTy) : Nil
        block = expr.block.not_nil!
        pname = block.params[0]
        emit_expr(expr.receiver.not_nil!)
        arr = new_temp
        line "LOCAL :#{arr} :#{ty.myc}"
        line "STORE"
        ensure_root(arr, ty.as(Ty))
        i = new_temp
        line "PUSH 0"
        line "LOCAL :#{i} :i32"
        line "STORE"
        line "LOOP"
        @indent += 1
        line "COND"
        @indent += 1
        line "LOCAL :#{arr}"
        line "DEREF"
        line "FIELD 1"
        line "LOCAL :#{i}"
        line "BINARY :less"
        @indent -= 1
        line "BODY"
        @indent += 1
        if heap_ptr_value?(ty.elem)
          line "LOCAL :#{i}"
          line "LOCAL :#{arr}"
          line "CALL :avant_array_get_ptr"
          line "AS :#{ty.elem.myc}"
        else
          buf = new_temp
          line "LOCAL :#{arr}"
          line "DEREF"
          line "FIELD 0"
          line "LOCAL :#{buf} :ptr<#{ty.elem.myc}>"
          line "STORE"
          line "LOCAL :#{i}"
          line "LOCAL :#{buf}"
          line "BINARY :add"
          line "DEREF"
        end
        line "LOCAL :#{pname} :#{ty.elem.myc}"
        line "STORE"
        ensure_root(pname, ty.elem)
        emit_stmts(block.body, implicit_return: false)
        line "PUSH 1"
        line "LOCAL :#{i}"
        line "BINARY :add"
        line "LOCAL :#{i} :i32"
        line "STORE"
        @indent -= 1
        @indent -= 1
        line "ENDLOOP"
      end

      private def emit_array_parallel(expr : AST::Call, ty : ArrayTy) : Nil
        # Prototype: sequential each that builds a result array (spawn per item is a later tightening).
        # The language surface is `.parallel`; the first implementation is honest about doing the
        # work, then spawn is used for whole-thunk Matmul workers.
        block = expr.block.not_nil!
        result_ty = expr.type.as(ArrayTy)
        emit_expr(expr.receiver.not_nil!)
        src = new_temp
        line "LOCAL :#{src} :#{ty.myc}"
        line "STORE"
        ensure_root(src, ty.as(Ty))
        line "LOCAL :#{src}"
        line "DEREF"
        line "FIELD 1"
        emit_alloc_count(result_ty.elem)
        dst = new_temp
        line "LOCAL :#{dst} :ptr<#{result_ty.elem.myc}>"
        line "STORE"
        emit_ptr_root(dst)
        i = new_temp
        line "PUSH 0"
        line "LOCAL :#{i} :i32"
        line "STORE"
        pname = block.params[0]
        line "LOOP"
        @indent += 1
        line "COND"
        @indent += 1
        line "LOCAL :#{src}"
        line "DEREF"
        line "FIELD 1"
        line "LOCAL :#{i}"
        line "BINARY :less"
        @indent -= 1
        line "BODY"
        @indent += 1
        buf = new_temp
        line "LOCAL :#{src}"
        line "DEREF"
        line "FIELD 0"
        line "LOCAL :#{buf} :ptr<#{ty.elem.myc}>"
        line "STORE"
        line "LOCAL :#{i}"
        line "LOCAL :#{buf}"
        line "BINARY :add"
        line "DEREF"
        line "LOCAL :#{pname} :#{ty.elem.myc}"
        line "STORE"
        ensure_root(pname, ty.elem)
        last = block.body.last.as(AST::ExprStmt)
        block.body.each_with_index do |s, idx|
          if idx == block.body.size - 1
            emit_expr(last.expr)
            emit_coerce(last.expr.type.not_nil!, result_ty.elem)
          else
            emit_stmt(s, false)
          end
        end
        line "LOCAL :#{i}"
        line "LOCAL :#{dst}"
        line "BINARY :add"
        line "DEREF"
        line "STORE"
        line "PUSH 1"
        line "LOCAL :#{i}"
        line "BINARY :add"
        line "LOCAL :#{i} :i32"
        line "STORE"
        @indent -= 1
        @indent -= 1
        line "ENDLOOP"
        len = new_temp
        line "LOCAL :#{src}"
        line "DEREF"
        line "FIELD 1"
        line "LOCAL :#{len} :i32"
        line "STORE"
        emit_array_header(result_ty, dst, len, len)
      end

      private def emit_spawn(expr : AST::Call) : Nil
        block = expr.block.not_nil!
        name = expr.spawn_thunk || raise "spawn thunk was not collected"
        caps = expr.spawn_captures
        if caps.empty?
          line "PUSH 0"
          line "AS :ptr<void>"
        else
          emit_alloc_sizeof("#{name}_cap", Runtime::ARRAY_TYPE_ID)
          line "AS :ptr<#{name}_cap>"
          cap = new_temp
          line "LOCAL :#{cap} :ptr<#{name}_cap>"
          line "STORE"
          line "LOCAL :#{cap}"
          line "AS :ptr<void>"
          line "CALL :avant_pin"
          caps.each_with_index do |(cname, cty), i|
            line "LOCAL :#{cname} :#{cty.myc}"
            line "LOCAL :#{cap}"
            line "DEREF"
            line "FIELD #{i}"
            line "STORE"
          end
          line "LOCAL :#{cap}"
          line "AS :ptr<void>"
        end
        line "ADDR :#{name}"
        line "AS :ptr<void>"
        line "CALL :avant_spawn"
      end

      private def emit_spawn_thunk(name : String, block : AST::Block, caps : Array({String, Ty})) : Nil
        saved_indent = @indent
        saved_temps = @temps
        saved_ret = @return_type
        saved_rooted = @rooted
        saved_self_ty = @self_ty
        saved_self_name = @self_name
        @indent = 0
        @temps = 0
        @return_type = IntTy::INSTANCE
        @self_ty = nil
        @self_name = nil
        line "FUNC :#{name}"
        @indent += 1
        line "ARGS"
        @indent += 1
        line "TYPE :ptr<void>"
        @indent -= 1
        line "RETURN"
        @indent += 1
        line "TYPE :i32"
        @indent -= 1
        line "BODY"
        @indent += 1
        @rooted = Set(String).new
        line "CALL :avant_gc_enter"
        line "PARAM 0"
        unless caps.empty?
          line "AS :ptr<#{name}_cap>"
          cap = new_temp
          line "LOCAL :#{cap} :ptr<#{name}_cap>"
          line "STORE"
          caps.each_with_index do |(cname, cty), i|
            line "LOCAL :#{cap}"
            line "DEREF"
            line "FIELD #{i}"
            line "LOCAL :#{cname} :#{cty.myc}"
            line "STORE"
          end
        else
          line "STACK :drop"
        end
        last = block.body.last.as(AST::ExprStmt)
        block.body.each_with_index do |s, i|
          if i == block.body.size - 1
            emit_expr(last.expr)
            emit_ret
          else
            emit_stmt(s, false)
          end
        end
        @indent -= 1
        @indent -= 1
        line "ENDFUNC"
        @io << '\n'
        @indent = saved_indent
        @temps = saved_temps
        @return_type = saved_ret
        @rooted = saved_rooted
        @self_ty = saved_self_ty
        @self_name = saved_self_name
      end

      private def emit_join(expr : AST::Call) : Nil
        emit_expr(expr.receiver.not_nil!)
        line "CALL :avant_join"
      end

      private def emit_json_get(expr : AST::Call, as_str : Bool) : Nil
        found = new_temp
        line "PUSH 0"
        line "LOCAL :#{found} :i32"
        line "STORE"
        emit_expr(expr.args[1])
        key = new_temp
        line "LOCAL :#{key} :ptr<u8>"
        line "STORE"
        emit_expr(expr.args[0])
        doc = new_temp
        line "LOCAL :#{doc} :ptr<void>"
        line "STORE"
        line "LOCAL :#{found}"
        line "ADDR"
        line "LOCAL :#{key}"
        line "LOCAL :#{doc}"
        if as_str
          line "CALL :avant_json_get_str"
          val_ty = StringTy::INSTANCE.as(Ty)
        else
          line "CALL :avant_json_get_int"
          val_ty = IntTy::INSTANCE.as(Ty)
        end
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

      private def emit_array_sort(expr : AST::Call) : Nil
        emit_expr(expr.receiver.not_nil!)
        line "CALL :avant_array_sort_i32"
      end

      private def emit_array_pop(receiver : AST::Expr, ty : ArrayTy) : Nil
        emit_expr(receiver)
        arr = new_temp
        line "LOCAL :#{arr} :#{ty.myc}"
        line "STORE"
        ensure_root(arr, ty.as(Ty))
        if heap_ptr_value?(ty.elem)
          line "LOCAL :#{arr}"
          line "CALL :avant_array_pop_ptr"
          line "AS :#{ty.elem.myc}"
          return
        end
        line "SIZEOF :#{ty.elem.myc}"
        line "AS :u64"
        line "LOCAL :#{arr}"
        line "CALL :avant_array_pop_slot"
        line "AS :ptr<#{ty.elem.myc}>"
        slot = new_temp
        line "LOCAL :#{slot} :ptr<#{ty.elem.myc}>"
        line "STORE"
        emit_ptr_root(slot)
        line "LOCAL :#{slot}"
        line "DEREF"
      end

      private def emit_nullable_ptr : Nil
        ptr_ty = PtrTy.new(VoidTy::INSTANCE).as(Ty)
        want = UnionTy.nilable(ptr_ty).as(UnionTy)
        val = new_temp
        line "LOCAL :#{val} :ptr<void>"
        line "STORE"
        ensure_root(val, ptr_ty)
        out = new_temp
        line "LOCAL :#{val}"
        line "PUSH 0"
        line "AS :ptr<void>"
        line "BINARY :not_eq"
        line "IF"
        @indent += 1
        line "THEN"
        @indent += 1
        line "LOCAL :#{val}"
        emit_wrap_union(ptr_ty, want)
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

      private def emit_json_root(expr : AST::Call) : Nil
        emit_expr(expr.args[0])
        line "CALL :avant_json_root"
      end

      private def emit_json_obj_get(expr : AST::Call) : Nil
        emit_expr(expr.args[1])
        key = new_temp
        line "LOCAL :#{key} :ptr<u8>"
        line "STORE"
        emit_expr(expr.args[0])
        obj = new_temp
        line "LOCAL :#{obj} :ptr<void>"
        line "STORE"
        line "LOCAL :#{key}"
        line "LOCAL :#{obj}"
        line "CALL :avant_json_obj_get"
        emit_nullable_ptr
      end

      private def emit_json_len(expr : AST::Call) : Nil
        emit_expr(expr.args[0])
        line "CALL :avant_json_arr_len"
      end

      private def emit_json_at(expr : AST::Call) : Nil
        emit_expr(expr.args[1])
        idx = new_temp
        line "LOCAL :#{idx} :i32"
        line "STORE"
        emit_expr(expr.args[0])
        arr = new_temp
        line "LOCAL :#{arr} :ptr<void>"
        line "STORE"
        line "LOCAL :#{idx}"
        line "LOCAL :#{arr}"
        line "CALL :avant_json_arr_get"
        emit_nullable_ptr
      end

      private def emit_json_f64(expr : AST::Call) : Nil
        found = new_temp
        line "PUSH 0"
        line "LOCAL :#{found} :i32"
        line "STORE"
        emit_expr(expr.args[1])
        key = new_temp
        line "LOCAL :#{key} :ptr<u8>"
        line "STORE"
        emit_expr(expr.args[0])
        obj = new_temp
        line "LOCAL :#{obj} :ptr<void>"
        line "STORE"
        line "LOCAL :#{found}"
        line "ADDR"
        line "LOCAL :#{key}"
        line "LOCAL :#{obj}"
        line "CALL :avant_json_obj_f64"
        val_ty = Float64Ty::INSTANCE.as(Ty)
        val = new_temp
        line "LOCAL :#{val} :f64"
        line "STORE"
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

      private def emit_json_sum_f64(expr : AST::Call) : Nil
        emit_expr(expr.args[1])
        key = new_temp
        line "LOCAL :#{key} :ptr<u8>"
        line "STORE"
        emit_expr(expr.args[0])
        arr = new_temp
        line "LOCAL :#{arr} :ptr<void>"
        line "STORE"
        line "LOCAL :#{key}"
        line "LOCAL :#{arr}"
        line "CALL :avant_json_arr_sum_f64"
      end
    end
  end
end
