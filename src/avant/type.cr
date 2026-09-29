module Avant
  abstract class Ty
    def int?
      false
    end

    def int64?
      false
    end

    def uint64?
      false
    end

    def integer?
      int? || int64? || uint64?
    end

    def bool?
      false
    end

    def float?
      false
    end

    def string?
      false
    end

    def void?
      false
    end

    def array?
      false
    end

    def struct?
      false
    end

    def class?
      false
    end

    def ptr?
      false
    end

    def nil_type?
      false
    end

    def union?
      false
    end

    def hash?
      false
    end

    def buf?
      false
    end

    def join_handle?
      false
    end

    def opaque?
      false
    end

    def type_var?
      false
    end

    def module_ref?
      false
    end

    def aggregate?
      struct? || class?
    end

    def field_type(_name : String) : Ty?
      nil
    end

    def field_index(_name : String) : Int32?
      nil
    end

    def numeric?
      integer? || float?
    end

    abstract def myc : String
    abstract def same?(other : Ty) : Bool
    abstract def to_s(io : IO) : Nil

    def ==(other : Ty)
      same?(other)
    end

    def hash(hasher)
      myc.hash(hasher)
    end
  end

  class IntTy < Ty
    INSTANCE = new

    def int?
      true
    end

    def myc : String
      "i32"
    end

    def same?(other : Ty) : Bool
      other.int?
    end

    def to_s(io : IO) : Nil
      io << "Int"
    end
  end

  class Int64Ty < Ty
    INSTANCE = new

    def int64?
      true
    end

    def myc : String
      "i64"
    end

    def same?(other : Ty) : Bool
      other.int64?
    end

    def to_s(io : IO) : Nil
      io << "Int64"
    end
  end

  class UInt64Ty < Ty
    INSTANCE = new

    def uint64?
      true
    end

    def myc : String
      "u64"
    end

    def same?(other : Ty) : Bool
      other.uint64?
    end

    def to_s(io : IO) : Nil
      io << "UInt64"
    end
  end

  class BoolTy < Ty
    INSTANCE = new

    def bool?
      true
    end

    def myc : String
      "bool"
    end

    def same?(other : Ty) : Bool
      other.bool?
    end

    def to_s(io : IO) : Nil
      io << "Bool"
    end
  end

  class Float64Ty < Ty
    INSTANCE = new

    def float?
      true
    end

    def myc : String
      "f64"
    end

    def same?(other : Ty) : Bool
      other.float?
    end

    def to_s(io : IO) : Nil
      io << "Float64"
    end
  end

  class StringTy < Ty
    INSTANCE = new

    def string?
      true
    end

    def myc : String
      "ptr<u8>"
    end

    def same?(other : Ty) : Bool
      other.string?
    end

    def to_s(io : IO) : Nil
      io << "String"
    end
  end

  class TypeVar < Ty
    getter name : String

    def initialize(@name)
    end

    def type_var?
      true
    end

    def myc : String
      raise "type parameter #{@name} is not concrete"
    end

    def same?(other : Ty) : Bool
      other.is_a?(TypeVar) && other.name == @name
    end

    def to_s(io : IO) : Nil
      io << @name
    end
  end

  class VoidTy < Ty
    INSTANCE = new

    def void?
      true
    end

    def myc : String
      "void"
    end

    def same?(other : Ty) : Bool
      other.void?
    end

    def to_s(io : IO) : Nil
      io << "Void"
    end
  end

  class ArrayTy < Ty
    getter elem : Ty

    def initialize(@elem)
    end

    def array?
      true
    end

    def myc : String
      self_ptr_myc
    end

    def layout_myc : String
      "Array_#{elem_id}"
    end

    def self_ptr_myc : String
      "ptr<#{layout_myc}>"
    end

    def elem_id : String
      @elem.to_s.gsub(/[^A-Za-z0-9]/, "_")
    end

    def same?(other : Ty) : Bool
      other.is_a?(ArrayTy) && other.elem.same?(@elem)
    end

    def to_s(io : IO) : Nil
      io << "Array(" << @elem << ')'
    end
  end

  class OpaqueTy < Ty
    getter name : String

    def initialize(@name)
    end

    def opaque?
      true
    end

    def myc : String
      raise "opaque type #{@name} is only valid behind Ptr"
    end

    def same?(other : Ty) : Bool
      other.is_a?(OpaqueTy) && other.name == @name
    end

    def to_s(io : IO) : Nil
      io << @name
    end
  end

  class ModuleTy < Ty
    getter name : String

    def initialize(@name)
    end

    def module_ref?
      true
    end

    def myc : String
      raise "module #{@name} is not a value"
    end

    def same?(other : Ty) : Bool
      other.is_a?(ModuleTy) && other.name == @name
    end

    def to_s(io : IO) : Nil
      io << "module " << @name
    end
  end

  class PtrTy < Ty
    getter inner : Ty

    def initialize(@inner)
    end

    def ptr?
      true
    end

    def myc : String
      case inner = @inner
      when OpaqueTy, VoidTy
        "ptr<void>"
      else
        "ptr<#{inner.myc}>"
      end
    end

    def same?(other : Ty) : Bool
      other.is_a?(PtrTy) && other.inner.same?(@inner)
    end

    def to_s(io : IO) : Nil
      io << "Ptr(" << @inner << ')'
    end
  end

  abstract class AggTy < Ty
    getter name : String
    property fields : Array({String, Ty})

    def initialize(@name, @fields)
    end

    def layout_myc : String
      @name
    end

    def self_ptr_myc : String
      "ptr<#{@name}>"
    end

    def field_index(name : String) : Int32?
      @fields.index { |n, _| n == name }
    end

    def field_type(name : String) : Ty?
      @fields.find { |n, _| n == name }.try(&.[1])
    end

    def to_s(io : IO) : Nil
      io << @name
    end
  end

  class StructTy < AggTy
    def struct?
      true
    end

    def myc : String
      layout_myc
    end

    def same?(other : Ty) : Bool
      other.is_a?(StructTy) && other.name == @name
    end
  end

  class ClassTy < AggTy
    def class?
      true
    end

    def myc : String
      self_ptr_myc
    end

    def same?(other : Ty) : Bool
      other.is_a?(ClassTy) && other.name == @name
    end
  end

  class NilTy < Ty
    INSTANCE = new

    def nil_type?
      true
    end

    def myc : String
      "i32"
    end

    def same?(other : Ty) : Bool
      other.nil_type?
    end

    def to_s(io : IO) : Nil
      io << "Nil"
    end
  end

  class UnionTy < Ty
    getter members : Array(Ty)

    def initialize(@members)
    end

    def union?
      true
    end

    def myc : String
      layout_myc
    end

    def layout_myc : String
      "U_" + @members.map { |m| m.to_s.gsub(/[^A-Za-z0-9]/, "_") }.join("_")
    end

    def includes?(other : Ty) : Bool
      @members.any?(&.same?(other))
    end

    def includes_nil? : Bool
      @members.any?(&.nil_type?)
    end

    def without_nil : Ty
      UnionTy.build(@members.reject(&.nil_type?))
    end

    def nilable? : Bool
      includes_nil? && @members.size == 2
    end

    def tag_of(member : Ty) : Int32
      @members.index { |m| m.same?(member) } || raise "not a member of #{self}"
    end

    def payload_index(member : Ty) : Int32?
      i = 0
      @members.each do |m|
        next if m.nil_type?
        return i + 1 if m.same?(member)
        i += 1
      end
      nil
    end

    def payload_members : Array(Ty)
      @members.reject(&.nil_type?)
    end

    def same?(other : Ty) : Bool
      return false unless other.is_a?(UnionTy)
      return false unless other.members.size == @members.size
      @members.each_with_index do |m, i|
        return false unless m.same?(other.members[i])
      end
      true
    end

    def to_s(io : IO) : Nil
      if nilable?
        non_nil = without_nil
        if non_nil.union?
          io << '(' << non_nil << ")?"
        else
          io << non_nil << '?'
        end
      else
        @members.each_with_index do |m, i|
          io << " | " if i > 0
          io << m
        end
      end
    end

    def self.build(types : Array(Ty)) : Ty
        flat = [] of Ty
        types.each { |t| flatten_into(t, flat) }
      uniq = [] of Ty
      flat.each do |t|
        uniq << t unless uniq.any?(&.same?(t))
      end
      uniq.sort_by! { |t| t.to_s }
      case uniq.size
      when 0
        NilTy::INSTANCE
      when 1
        uniq[0]
      else
        new(uniq)
      end
    end

    def self.nilable(inner : Ty) : Ty
      build([inner, NilTy::INSTANCE.as(Ty)])
    end

    def self.flatten_into(ty : Ty, into : Array(Ty)) : Nil
      if ty.is_a?(UnionTy)
        ty.members.each { |m| flatten_into(m, into) }
      else
        into << ty
      end
    end
  end

  class HashTy < Ty
    getter key : Ty
    getter val : Ty

    def initialize(@key, @val)
    end

    def hash?
      true
    end

    def myc : String
      "ptr<void>"
    end

    def same?(other : Ty) : Bool
      other.is_a?(HashTy) && other.key.same?(@key) && other.val.same?(@val)
    end

    def to_s(io : IO) : Nil
      io << "Hash(" << @key << ", " << @val << ')'
    end
  end

  class BufTy < Ty
    INSTANCE = new

    def buf?
      true
    end

    def myc : String
      "ptr<void>"
    end

    def same?(other : Ty) : Bool
      other.is_a?(BufTy)
    end

    def to_s(io : IO) : Nil
      io << "Buf"
    end
  end

  class JoinHandleTy < Ty
    getter result : Ty

    def initialize(@result)
    end

    def join_handle?
      true
    end

    def myc : String
      "ptr<void>"
    end

    def same?(other : Ty) : Bool
      other.is_a?(JoinHandleTy) && other.result.same?(@result)
    end

    def to_s(io : IO) : Nil
      io << "JoinHandle(" << @result << ')'
    end
  end

  def self.emit_op_name(name : String) : String
    case name
    when "+"
      "plus"
    when "-"
      "minus"
    when "*"
      "star"
    when "/"
      "slash"
    when "%"
      "percent"
    when "=="
      "eq"
    when "!="
      "ne"
    when "<"
      "lt"
    when "<="
      "le"
    when ">"
      "gt"
    when ">="
      "ge"
    else
      name
    end
  end

  def self.mangle_ty(ty : Ty) : String
    ty.to_s.gsub(/[^A-Za-z0-9]+/, "_").gsub(/^_|_$/, "")
  end

  def self.operator_method?(name : String) : Bool
    case name
    when "+", "-", "*", "/", "%", "==", "!=", "<", "<=", ">", ">="
      true
    else
      false
    end
  end
end
