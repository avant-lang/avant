module Avant
  record MethodSig,
    node : AST::Function,
    owner : AggTy,
    params : Array(Ty),
    return_type : Ty do
    def mangled : String
      "#{owner.name}__#{node.name}"
    end
  end

  def self.load_types(program : AST::Program) : {Hash(String, AggTy), Hash(String, OpaqueTy)}
    named = {} of String => AggTy

    program.structs.each do |defn|
      if named.has_key?(defn.name)
        raise CompileError.at(defn.location, "#{defn.name} is already defined")
      end
      named[defn.name] = StructTy.new(defn.name, [] of {String, Ty})
    end

    program.classes.each do |defn|
      if named.has_key?(defn.name)
        raise CompileError.at(defn.location, "#{defn.name} is already defined")
      end
      named[defn.name] = ClassTy.new(defn.name, [] of {String, Ty})
    end

    program.libs.each do |lib_def|
      if named.has_key?(lib_def.name)
        raise CompileError.at(lib_def.location, "lib #{lib_def.name} collides with a type")
      end
    end

    opaques = collect_opaques(program, named)
    resolver = TypeResolver.new(named, opaques)
    fill_fields = ->(name : String, fields : Array(AST::Field)) {
      resolved = [] of {String, Ty}
      seen = Set(String).new
      fields.each do |field|
        if seen.includes?(field.name)
          raise CompileError.at(field.location, "duplicate field #{field.name}")
        end
        seen << field.name
        resolved << {field.name, resolver.resolve_value(field.type)}
      end
      named[name].fields = resolved
    }

    program.structs.each { |defn| fill_fields.call(defn.name, defn.fields) }
    program.classes.each { |defn| fill_fields.call(defn.name, defn.fields) }
    {named, opaques}
  end

  def self.type_map(program : AST::Program) : Hash(String, AggTy)
    named, _ = load_types(program)
    named
  end

  def self.collect_opaques(program : AST::Program, named : Hash(String, AggTy)) : Hash(String, OpaqueTy)
    opaques = {} of String => OpaqueTy
    note = ->(tn : AST::TypeName) { note_ptr_opaque(tn, named, opaques) }

    program.libs.each do |lib_def|
      lib_def.funs.each do |cfun|
        cfun.params.each { |p| note.call(p.type) }
        if ret = cfun.return_type
          note.call(ret)
        end
      end
    end
    program.structs.each { |defn| defn.fields.each { |f| note.call(f.type) } }
    program.classes.each { |defn| defn.fields.each { |f| note.call(f.type) } }
    program.all_functions.each do |fn|
      fn.params.each { |p| note.call(p.type) }
      if recv = fn.receiver
        note.call(recv.type)
      end
      if ret = fn.return_type
        note.call(ret)
      end
    end
    opaques
  end

  def self.note_ptr_opaque(tn : AST::TypeName, named : Hash(String, AggTy), opaques : Hash(String, OpaqueTy)) : Nil
    tn.args.each { |a| note_ptr_opaque(a, named, opaques) }
    return unless tn.name == "Ptr" && tn.args.size == 1
    inner = tn.args[0]
    return unless inner.args.empty?
    return if TypeResolver.builtin?(inner.name)
    return if named.has_key?(inner.name)
    opaques[inner.name] ||= OpaqueTy.new(inner.name)
  end

  def self.struct_map(program : AST::Program) : Hash(String, StructTy)
    types = {} of String => StructTy
    type_map(program).each do |name, ty|
      types[name] = ty if ty.is_a?(StructTy)
    end
    types
  end

  class TypeResolver
    BUILTINS = {"Int", "Int64", "UInt64", "Bool", "Float64", "String", "Void", "Array", "Ptr", "Hash", "Buf", "Nil", "JoinHandle"}

    def self.builtin?(name : String) : Bool
      BUILTINS.includes?(name)
    end

    def initialize(@named : Hash(String, AggTy), @opaques = {} of String => OpaqueTy)
    end

    def resolve_value(tn : AST::TypeName) : Ty
      ty = resolve(tn)
      case ty
      when OpaqueTy
        raise CompileError.at(tn.location, "#{ty} is opaque; use Ptr(#{ty})")
      when VoidTy
        raise CompileError.at(tn.location, "Void is not a value; use Ptr(Void) or omit a return type")
      else
        ty
      end
    end

    def resolve(tn : AST::TypeName) : Ty
      inner = resolve_core(tn)
      tn.nilable ? UnionTy.nilable(inner) : inner
    end

    private def resolve_core(tn : AST::TypeName) : Ty
      if tn.union?
        return UnionTy.build(tn.members.map { |m| resolve(m) })
      end
      case tn.name
      when "Int"
        reject_args(tn)
        IntTy::INSTANCE
      when "Int64"
        reject_args(tn)
        Int64Ty::INSTANCE
      when "UInt64"
        reject_args(tn)
        UInt64Ty::INSTANCE
      when "Bool"
        reject_args(tn)
        BoolTy::INSTANCE
      when "Float64"
        reject_args(tn)
        Float64Ty::INSTANCE
      when "String"
        reject_args(tn)
        StringTy::INSTANCE
      when "Buf"
        reject_args(tn)
        BufTy::INSTANCE
      when "Nil"
        reject_args(tn)
        NilTy::INSTANCE
      when "Void"
        reject_args(tn)
        VoidTy::INSTANCE
      when "Array"
        unless tn.args.size == 1
          raise CompileError.at(tn.location, "Array takes one type argument")
        end
        ArrayTy.new(resolve_value(tn.args[0]))
      when "Hash"
        unless tn.args.size == 2
          raise CompileError.at(tn.location, "Hash takes two type arguments")
        end
        HashTy.new(resolve_value(tn.args[0]), resolve_value(tn.args[1]))
      when "JoinHandle"
        unless tn.args.size == 1
          raise CompileError.at(tn.location, "JoinHandle takes one type argument")
        end
        JoinHandleTy.new(resolve_value(tn.args[0]))
      when "Ptr"
        unless tn.args.size == 1
          raise CompileError.at(tn.location, "Ptr takes one type argument")
        end
        inner = tn.args[0]
        if inner.name == "Void"
          reject_args(inner)
          PtrTy.new(VoidTy::INSTANCE)
        else
          PtrTy.new(resolve(inner))
        end
      else
        reject_args(tn)
        @named[tn.name]? || @opaques[tn.name]? || raise CompileError.at(tn.location, "unknown type #{tn.name}")
      end
    end

    private def reject_args(tn : AST::TypeName) : Nil
      unless tn.args.empty?
        raise CompileError.at(tn.location, "#{tn.name} does not take type arguments")
      end
    end
  end
end
