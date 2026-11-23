; IR that parses but LLVM's verifier rejects: %x, defined on one path
; only, is read where both paths meet ("Instruction does not dominate all
; uses"). The verify mode (AVANT_VERIFY=1) refuses it; tests/cases/test_verify_mode.av.
define i64 @pick(i1 %c) {
entry:
  br i1 %c, label %one, label %join
one:
  %x = add i64 %v, 2
  br label %join
join:
  %v = add i64 1, 1
  ret i64 %x
}
