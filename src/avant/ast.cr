module Avant
  module AST
    abstract class Node
      getter location : Location

      def initialize(@location)
      end
    end

    class Program < Node
      getter structs : Array(StructDef)
      getter classes : Array(ClassDef)
      getter functions : Array(Function)
      getter libs : Array(LibDef)

      def initialize(location, @structs, @functions, @classes = [] of ClassDef, @libs = [] of LibDef)
        super(location)
      end

      def all_functions : Array(Function)
        fns = @functions.dup
        @structs.each { |s| fns.concat(s.methods) }
        @classes.each { |c| fns.concat(c.methods) }
        fns
      end
    end

    class StructDef < Node
      getter name : String
      getter fields : Array(Field)
      getter methods : Array(Function)

      def initialize(location, @name, @fields, @methods = [] of Function)
        super(location)
      end
    end

    class ClassDef < Node
      getter name : String
      getter fields : Array(Field)
      getter methods : Array(Function)

      def initialize(location, @name, @fields, @methods = [] of Function)
        super(location)
      end
    end

    class LibDef < Node
      getter name : String
      getter funs : Array(FunDecl)

      def initialize(location, @name, @funs)
        super(location)
      end
    end

    class FunDecl < Node
      getter name : String
      getter params : Array(Param)
      getter return_type : TypeName?

      def initialize(location, @name, @params, @return_type)
        super(location)
      end
    end

    class Field < Node
      getter name : String
      getter type : TypeName
      getter default : Expr?

      def initialize(location, @name, @type, @default = nil)
        super(location)
      end
    end

    class Function < Node
      getter name : String
      getter params : Array(Param)
      property return_type : TypeName?
      getter body : Array(Stmt)
      getter receiver : Param?
      property owner : String?
      property emit_name : String
      property type_params : Array(String)
      property generic : Bool
      property template : Function?

      def initialize(location, @name, @params, @return_type, @body, @receiver = nil, @owner = nil)
        super(location)
        @emit_name = @name
        @type_params = [] of String
        @generic = false
        @template = nil
      end

      def void? : Bool
        @return_type.nil?
      end

      def method? : Bool
        !@receiver.nil? || !@owner.nil?
      end

      def self_name : String
        @receiver.try(&.name) || "self"
      end
    end

    class Param < Node
      getter name : String
      property type : TypeName
      getter default : Expr?

      def initialize(location, @name, @type, @default = nil)
        super(location)
      end
    end

    class TypeName < Node
      getter name : String
      getter args : Array(TypeName)
      getter nilable : Bool
      getter members : Array(TypeName)

      def initialize(location, @name, @args = [] of TypeName, @nilable = false, @members = [] of TypeName)
        super(location)
      end

      def union?
        !@members.empty?
      end

      def self.union(location, members : Array(TypeName), nilable = false)
        new(location, "", [] of TypeName, nilable, members)
      end
    end

    abstract class Stmt < Node
    end

    class ExprStmt < Stmt
      getter expr : Expr

      def initialize(location, @expr)
        super(location)
      end
    end

    class ReturnStmt < Stmt
      getter expr : Expr?

      def initialize(location, @expr)
        super(location)
      end
    end

    class IfStmt < Stmt
      getter cond : Expr
      getter then_body : Array(Stmt)
      getter else_body : Array(Stmt)
      getter bind : String?

      def initialize(location, @cond, @then_body, @else_body, @bind = nil)
        super(location)
      end
    end

    class WhileStmt < Stmt
      getter cond : Expr
      getter body : Array(Stmt)

      def initialize(location, @cond, @body)
        super(location)
      end
    end

    class BreakStmt < Stmt
    end

    class ContinueStmt < Stmt
    end

    class AssignStmt < Stmt
      property target : Expr
      getter value : Expr
      property declared_type : TypeName?
      getter op : Token::Kind

      def initialize(location, @target, @value, @declared_type = nil, @op = Token::Kind::Eq)
        super(location)
      end

      def compound? : Bool
        !@op.eq?
      end
    end

    abstract class Expr < Node
      property type : Ty?
    end

    class SwitchCase < Node
      getter labels : Array(Int64)
      getter body : Array(Stmt)

      def initialize(location, @labels, @body)
        super(location)
      end
    end

    class SwitchExpr < Expr
      getter cond : Expr
      getter cases : Array(SwitchCase)
      getter else_body : Array(Stmt)?

      def initialize(location, @cond, @cases, @else_body = nil)
        super(location)
      end
    end

    class IntegerLiteral < Expr
      getter bits : UInt64
      getter suffix : String

      def initialize(location, @bits, @suffix = "")
        super(location)
      end

      def value : Int64
        @bits.to_i64
      end
    end

    class FloatLiteral < Expr
      getter value : Float64

      def initialize(location, @value)
        super(location)
      end
    end

    class StringLiteral < Expr
      getter value : String

      def initialize(location, @value)
        super(location)
      end
    end

    class BoolLiteral < Expr
      getter value : Bool

      def initialize(location, @value)
        super(location)
      end
    end

    class Name < Expr
      getter ident : String
      property implicit_field : Bool

      def initialize(location, @ident, @implicit_field = false)
        super(location)
      end
    end

    class Call < Expr
      getter callee : String
      getter args : Array(Expr)
      getter receiver : Expr?
      property lib_name : String?
      property block : Block?
      property spawn_captures : Array({String, Ty})
      property spawn_thunk : String?
      property resolved : String?

      def initialize(location, @callee, @args, @receiver = nil, @block = nil)
        super(location)
        @lib_name = nil
        @spawn_captures = [] of {String, Ty}
        @spawn_thunk = nil
        @resolved = nil
      end

      def method? : Bool
        !@receiver.nil?
      end
    end

    class Block < Node
      getter params : Array(String)
      getter body : Array(Stmt)
      property value_type : Ty?

      def initialize(location, @params, @body)
        super(location)
        @value_type = nil
      end
    end

    class Unary < Expr
      getter op : Token::Kind
      getter expr : Expr

      def initialize(location, @op, @expr)
        super(location)
      end
    end

    class Binary < Expr
      getter op : Token::Kind
      getter left : Expr
      getter right : Expr
      property op_method : String?

      def initialize(location, @op, @left, @right)
        super(location)
        @op_method = nil
      end
    end

    class FieldAccess < Expr
      getter object : Expr
      getter field : String
      property method_call : Bool
      property resolved : String?

      def initialize(location, @object, @field, @method_call = false)
        super(location)
        @resolved = nil
      end
    end

    class Index < Expr
      getter array : Expr
      getter index : Expr

      def initialize(location, @array, @index)
        super(location)
      end
    end

    class StructLiteral < Expr
      getter type_name : String
      getter fields : Array({String, Expr})

      def initialize(location, @type_name, @fields)
        super(location)
      end
    end

    class ArrayLiteral < Expr
      getter elements : Array(Expr)

      def initialize(location, @elements)
        super(location)
      end
    end

    class ArrayNew < Expr
      property elem_type : TypeName
      getter size : Expr

      def initialize(location, @elem_type, @size)
        super(location)
      end
    end

    class HashNew < Expr
      property key_type : TypeName
      property val_type : TypeName

      def initialize(location, @key_type, @val_type)
        super(location)
      end
    end

    class NilLiteral < Expr
    end

    class Try < Expr
      getter expr : Expr

      def initialize(location, @expr)
        super(location)
      end
    end

    class InterpString < Expr
      getter parts : Array(InterpPart)

      def initialize(location, @parts)
        super(location)
      end
    end

    record InterpPart, text : String?, expr : Expr? do
      def literal?
        !@text.nil?
      end
    end
  end
end
