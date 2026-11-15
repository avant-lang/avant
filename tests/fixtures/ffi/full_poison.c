/* full_poison.av's C half. C keeps the
 * address of a String's bytes, which the program then lets die, and reads
 * a byte through it after a full collection: a stale reference to a
 * mature object that died at a full collection. Under AVANT_GC_STRESS=1
 * the full sweep poisons what it frees (GC_POISON, 0xdb), so the read
 * gives 219; before this poisoning it gave the dead string's old byte. The
 * read is made only under the stress mode without the verifier (under
 * AVANT_GC_VERIFY=1 a released block is PROT_NONE until the next
 * collection; elsewhere the read is undefined), and -1 is returned
 * otherwise, so the program's stdout says which. */
#include <stdint.h>
#include <stdlib.h>
static const volatile uint8_t *fp_kept[2];
int32_t fp_keep(int32_t i, const char *s) {
  fp_kept[i] = (const volatile uint8_t *)s;
  return s[0];
}
int32_t fp_byte(int32_t i, int32_t off) {
  const char *st = getenv("AVANT_GC_STRESS");
  const char *v = getenv("AVANT_GC_VERIFY");
  if (!st || st[0] != '1' || (v && v[0] == '1')) {
    return -1;
  }
  return fp_kept[i][off];
}
