require "spec"
require "../src/avant"

def tokenize(text : String, path = "<test>")
  Avant::Lexer.new(Avant::Source.new(path, text)).tokenize
end

def token_kinds(text : String)
  tokenize(text).map(&.kind)
end

def parse(text : String, path = "<test>")
  source = Avant::Source.new(path, text)
  tokens = Avant::Lexer.new(source).tokenize
  Avant::Parser.new(source, tokens).parse
end

def compile(text : String, path = "<test>")
  Avant.compile(text, path)
end

def compile_error(text : String, path = "<test>")
  expect_raises(Avant::CompileError) do
    compile(text, path)
  end
end

def run_ir(ir : String, extra_objects = [] of String, linker_flags = [] of String) : String
  output = IO::Memory.new
  error = IO::Memory.new
  status = Avant.run_ir(ir, output, error, extra_objects, linker_flags)
  unless status.success?
    raise "myc-llvm failed (#{status.exit_code}): #{error}#{output}"
  end
  output.to_s
end

def run_src(text : String, path = "<test>") : String
  run_ir(compile(text, path))
end

def run_av(path : String, extra_objects = [] of String, linker_flags = [] of String) : String
  run_ir(Avant.compile_file(path), extra_objects, linker_flags)
end

def compile_bin(text : String, path = "<test>") : String
  compile_ir_bin(compile(text, path))
end

def compile_av_bin(paths : Array(String)) : String
  compile_ir_bin(Avant.compile_files(paths))
end

def compile_ir_bin(ir : String) : String
  bin = File.tempname("avant-bin")
  output = IO::Memory.new
  error = IO::Memory.new
  status = Avant.compile_ir(ir, bin, output, error)
  unless status.success?
    File.delete(bin) if File.exists?(bin)
    raise "myc-llvm compile failed (#{status.exit_code}): #{error}#{output}"
  end
  File.chmod(bin, 0o755)
  bin
end

def dump_host_tokens(text : String, path = "<test>") : String
  tokenize(text, path).map do |t|
    val = t.value.gsub("\\", "\\\\").gsub("\n", "\\n").gsub("\t", "\\t")
    "#{t.kind.value}\t#{t.location.line}\t#{t.location.column}\t#{val}"
  end.join('\n') + "\n"
end

def port_compiler_root : String
  File.expand_path("../compiler/lex_dump.av", __DIR__)
end

def port_driver_root : String
  File.expand_path("../compiler/main.av", __DIR__)
end

def port_compiler_files : Array(String)
  [port_compiler_root]
end

def port_driver_files : Array(String)
  [port_driver_root]
end

def port_lexer_files : Array(String)
  port_compiler_files
end

def dump_host_escape(s : String) : String
  s.gsub("\\", "\\\\").gsub("\n", "\\n").gsub("\t", "\\t")
end

def dump_host_type(t : Avant::AST::TypeName) : String
  if sp = t.splice
    return dump_host_expr(sp)
  end
  if t.union?
    inner = t.members.map { |m| dump_host_type(m) }.join("|")
    t.nilable ? "#{inner}?" : inner
  else
    s = t.name.dup
    unless t.args.empty?
      s += "(" + t.args.map { |a| dump_host_type(a) }.join(",") + ")"
    end
    t.nilable ? "#{s}?" : s
  end
end

def dump_host_expr(e : Avant::AST::Expr) : String
  case e
  when Avant::AST::IntegerLiteral
    "(int #{e.bits.to_i64}#{e.suffix})"
  when Avant::AST::FloatLiteral
    "(float)"
  when Avant::AST::StringLiteral
    "(string #{dump_host_escape(e.value)})"
  when Avant::AST::BoolLiteral
    e.value ? "(true)" : "(false)"
  when Avant::AST::NilLiteral
    "(nil)"
  when Avant::AST::Name
    "(name #{e.ident})"
  when Avant::AST::Call
    buf = String.build do |io|
      callee = e.callee
      if sp = e.callee_splice
        callee = dump_host_expr(sp)
      end
      io << "(call " << callee
      if recv = e.receiver
        io << " " << dump_host_expr(recv)
      end
      e.args.each { |a| io << " " << dump_host_expr(a) }
      if b = e.block
        io << " (block"
        b.params.each { |p| io << " |" << p }
        b.body.each { |s| io << " " << dump_host_stmt(s) }
        io << ")"
      end
      io << ")"
    end
    buf
  when Avant::AST::Unary
    inner = e.expr
    "(unary #{e.op.value} #{dump_host_expr(inner)})"
  when Avant::AST::Binary
    "(binary #{e.op.value} #{dump_host_expr(e.left)} #{dump_host_expr(e.right)})"
  when Avant::AST::FieldAccess
    field = e.field
    if sp = e.field_splice
      field = dump_host_expr(sp)
    end
    "(field #{dump_host_expr(e.object)} #{field})"
  when Avant::AST::Index
    "(index #{dump_host_expr(e.array)} #{dump_host_expr(e.index)})"
  when Avant::AST::Try
    "(try #{dump_host_expr(e.expr)})"
  when Avant::AST::ArrayLiteral
    "(array#{e.elements.map { |x| " " + dump_host_expr(x) }.join})"
  when Avant::AST::SwitchExpr
    buf = String.build do |io|
      io << "(switch " << dump_host_expr(e.cond)
      e.cases.each do |arm|
        io << " (case"
        arm.labels.each { |n| io << " " << n }
        arm.body.each { |s| io << " " << dump_host_stmt(s) }
        io << ")"
      end
      if else_body = e.else_body
        io << " (else"
        else_body.each { |s| io << " " << dump_host_stmt(s) }
        io << ")"
      end
      io << ")"
    end
    buf
  when Avant::AST::Splice
    "(splice #{dump_host_expr(e.inner)})"
  else
    "(expr)"
  end
end

def dump_host_stmt(s : Avant::AST::Stmt) : String
  case s
  when Avant::AST::ExprStmt
    "(expr #{dump_host_expr(s.expr)})"
  when Avant::AST::ReturnStmt
    if e = s.expr
      "(return #{dump_host_expr(e)})"
    else
      "(return)"
    end
  when Avant::AST::IfStmt
    buf = String.build do |io|
      io << "(if"
      if bind = s.bind
        io << "-assign " << bind
      end
      io << " " << dump_host_expr(s.cond)
      s.then_body.each { |t| io << " " << dump_host_stmt(t) }
      unless s.else_body.empty?
        io << " (else"
        s.else_body.each { |t| io << " " << dump_host_stmt(t) }
        io << ")"
      end
      io << ")"
    end
    buf
  when Avant::AST::WhileStmt
    buf = String.build do |io|
      io << "(while " << dump_host_expr(s.cond)
      s.body.each { |t| io << " " << dump_host_stmt(t) }
      io << ")"
    end
    buf
  when Avant::AST::BreakStmt
    "(break)"
  when Avant::AST::ContinueStmt
    "(continue)"
  when Avant::AST::AssignStmt
    "(assign #{s.op.value} #{dump_host_expr(s.target)} #{dump_host_expr(s.value)})"
  when Avant::AST::QuoteStmt
    buf = String.build do |io|
      io << "(quote"
      s.body.each { |t| io << " " << dump_host_stmt(t) }
      io << ")"
    end
    buf
  else
    "(stmt)"
  end
end

def dump_host_fn(fn : Avant::AST::Function) : String
  String.build do |io|
    io << "(fn "
    if sp = fn.name_splice
      io << dump_host_expr(sp)
    else
      io << fn.name
    end
    if recv = fn.receiver
      io << " (recv " << recv.name << " " << dump_host_type(recv.type) << ")"
    end
    fn.params.each do |p|
      io << " (param " << p.name << " " << dump_host_type(p.type) << ")"
    end
    if rt = fn.return_type
      io << " " << dump_host_type(rt)
    end
    fn.body.each { |s| io << " " << dump_host_stmt(s) }
    io << ")"
  end
end

def dump_host_ast(text : String, path = "<test>") : String
  program = parse(text, path)
  String.build do |io|
    io << "(program"
    program.structs.each { |s| io << " (struct " << s.name << ")" }
    program.classes.each { |c| io << " (class " << c.name << ")" }
    program.functions.each { |fn| io << " " << dump_host_fn(fn) }
    program.quotes.each do |q|
      io << " (quote"
      q.functions.each { |fn| io << " " << dump_host_fn(fn) }
      io << ")"
    end
    program.comptimes.each do |c|
      io << " (comptime " << c.type_name << " " << c.var_name
      io << " (quote"
      c.quote.functions.each { |fn| io << " " << dump_host_fn(fn) }
      io << "))"
    end
    io << ")\n"
  end
end

def dump_host_expr_ty(e : Avant::AST::Expr) : String
  if t = e.type
    t.to_s
  else
    "?"
  end
end

def dump_host_typed_expr(e : Avant::AST::Expr) : String
  ty = dump_host_expr_ty(e)
  case e
  when Avant::AST::IntegerLiteral
    "(int #{e.bits.to_i64}#{e.suffix} : #{ty})"
  when Avant::AST::FloatLiteral
    "(float : #{ty})"
  when Avant::AST::StringLiteral
    "(string #{dump_host_escape(e.value)} : #{ty})"
  when Avant::AST::BoolLiteral
    e.value ? "(true : #{ty})" : "(false : #{ty})"
  when Avant::AST::NilLiteral
    "(nil : #{ty})"
  when Avant::AST::Name
    "(name #{e.ident} : #{ty})"
  when Avant::AST::Call
    buf = String.build do |io|
      io << "(call " << e.callee
      if recv = e.receiver
        io << " " << dump_host_typed_expr(recv)
      end
      e.args.each { |a| io << " " << dump_host_typed_expr(a) }
      if b = e.block
        io << " (block"
        b.params.each { |p| io << " |" << p }
        b.body.each { |s| io << " " << dump_host_typed_stmt(s) }
        io << ")"
      end
      io << " : " << ty << ")"
    end
    buf
  when Avant::AST::Unary
    inner = e.expr
    "(unary #{e.op.value} #{dump_host_typed_expr(inner)} : #{ty})"
  when Avant::AST::Binary
    "(binary #{e.op.value} #{dump_host_typed_expr(e.left)} #{dump_host_typed_expr(e.right)} : #{ty})"
  when Avant::AST::FieldAccess
    "(field #{dump_host_typed_expr(e.object)} #{e.field} : #{ty})"
  when Avant::AST::Index
    "(index #{dump_host_typed_expr(e.array)} #{dump_host_typed_expr(e.index)} : #{ty})"
  when Avant::AST::Try
    "(try #{dump_host_typed_expr(e.expr)} : #{ty})"
  when Avant::AST::ArrayLiteral
    "(array#{e.elements.map { |x| " " + dump_host_typed_expr(x) }.join} : #{ty})"
  when Avant::AST::SwitchExpr
    buf = String.build do |io|
      io << "(switch " << dump_host_typed_expr(e.cond)
      e.cases.each do |arm|
        io << " (case"
        arm.labels.each { |n| io << " " << n }
        arm.body.each { |s| io << " " << dump_host_typed_stmt(s) }
        io << ")"
      end
      if else_body = e.else_body
        io << " (else"
        else_body.each { |s| io << " " << dump_host_typed_stmt(s) }
        io << ")"
      end
      io << " : " << ty << ")"
    end
    buf
  else
    "(expr : #{ty})"
  end
end

def dump_host_typed_stmt(s : Avant::AST::Stmt) : String
  case s
  when Avant::AST::ExprStmt
    "(expr #{dump_host_typed_expr(s.expr)})"
  when Avant::AST::ReturnStmt
    if e = s.expr
      "(return #{dump_host_typed_expr(e)})"
    else
      "(return)"
    end
  when Avant::AST::IfStmt
    buf = String.build do |io|
      io << "(if"
      if bind = s.bind
        io << "-assign " << bind
      end
      io << " " << dump_host_typed_expr(s.cond)
      s.then_body.each { |t| io << " " << dump_host_typed_stmt(t) }
      unless s.else_body.empty?
        io << " (else"
        s.else_body.each { |t| io << " " << dump_host_typed_stmt(t) }
        io << ")"
      end
      io << ")"
    end
    buf
  when Avant::AST::WhileStmt
    buf = String.build do |io|
      io << "(while " << dump_host_typed_expr(s.cond)
      s.body.each { |t| io << " " << dump_host_typed_stmt(t) }
      io << ")"
    end
    buf
  when Avant::AST::BreakStmt
    "(break)"
  when Avant::AST::ContinueStmt
    "(continue)"
  when Avant::AST::AssignStmt
    "(assign #{s.op.value} #{dump_host_typed_expr(s.target)} #{dump_host_typed_expr(s.value)})"
  else
    "(stmt)"
  end
end

def dump_host_typed_fn(fn : Avant::AST::Function) : String
  String.build do |io|
    io << "(fn " << fn.name << " : "
    if rt = fn.return_type
      io << dump_host_type(rt)
    else
      io << "Void"
    end
    if recv = fn.receiver
      io << " (recv " << recv.name << " : " << dump_host_type(recv.type) << ")"
    end
    fn.params.each do |p|
      io << " (param " << p.name << " : " << dump_host_type(p.type) << ")"
    end
    fn.body.each { |s| io << " " << dump_host_typed_stmt(s) }
    io << ")"
  end
end

def dump_host_typed_program(program : Avant::AST::Program) : String
  String.build do |io|
    io << "(program"
    program.structs.each { |s| io << " (struct " << s.name << ")" }
    program.classes.each { |c| io << " (class " << c.name << ")" }
    program.functions.each { |fn| io << " " << dump_host_typed_fn(fn) }
    io << ")"
  end
end

def dump_host_typed(text : String, path = "<test>") : String
  source = Avant::Source.new(path, text)
  begin
    tokens = Avant::Lexer.new(source).tokenize
    program = Avant::Parser.new(source, tokens).parse
    Avant::Expander.new(source, program).expand
    Avant::Checker.new(source, program).check
    dump_host_typed_program(program) + "\n"
  rescue e : Avant::CompileError
    "#{e.message}\n"
  end
end

def run_bin(bin : String, args = [] of String) : {Int32, String}
  output = IO::Memory.new
  error = IO::Memory.new
  status = Process.run(bin, args, output: output, error: error)
  {status.exit_code, output.to_s}
end

def with_env(vars : Hash(String, String?))
  old = {} of String => String?
  vars.each do |key, value|
    old[key] = ENV[key]?
    if value
      ENV[key] = value
    else
      ENV.delete(key)
    end
  end
  begin
    yield
  ensure
    old.each do |key, value|
      if value
        ENV[key] = value
      else
        ENV.delete(key)
      end
    end
  end
end
