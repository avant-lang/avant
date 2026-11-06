; Not valid IR: the parser must report it (test_llvm_glue.av).
define i32 @main(i32 %argc, ptr %argv) {
entry:
  %x = frobnicate i32 1, 2
  ret i32 %x
}
