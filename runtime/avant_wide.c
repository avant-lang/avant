/* An empty unit, kept because the compiler that builds the next
 * compiler (the seed) links runtime/build/avant_wide.o by name: the
 * 32-bit-halves arithmetic it held left the language, and std.bits
 * (lib/std/bits.av) has the 64-bit operations as single instructions.
 * The link table of every compiler built from this tree no longer names
 * it; it goes when the seed is a release built from such a tree. */
typedef int avant_wide_unit_is_empty;
