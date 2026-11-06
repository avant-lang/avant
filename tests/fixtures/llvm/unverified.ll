; Parses, but fails the verifier: %x does not dominate its use in %join
; (test_llvm_glue.av).
define i32 @main(i32 %argc, ptr %argv) {
entry:
  %c = icmp eq i32 %argc, 1
  br i1 %c, label %then, label %join

then:
  %x = add i32 %argc, 1
  br label %join

join:
  ret i32 %x
}
