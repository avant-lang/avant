/* An empty unit, kept because the compiler that builds the next
 * compiler (the seed) links runtime/build/avant_codec.o by name: the
 * compiler's own SHA-256 it held left for std.digest's
 * (runtime/avant_digest.c), and the link table of every compiler built
 * from this tree no longer names it. It goes when the seed is a release
 * built from such a tree. */
typedef int avant_codec_unit_is_empty;
