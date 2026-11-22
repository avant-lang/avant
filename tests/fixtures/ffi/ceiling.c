/* The size ceiling's probes
 * (tests/cases/test_ceiling.av): each builds a Buf, an Array or a Hash in
 * C memory whose size and capacity sit at the ceiling, with tiny storage,
 * and asks the runtime to grow it; the runtime must trap before it
 * allocates or writes anything (a write would land gigabytes past the
 * storage). The layouts are the runtime's (avant_buf.c, avant_array.c,
 * avant_hash.c). ceil_concat makes two small strings whose headers
 * say 1.5 GiB and concatenates them (3 GiB, past the String ceiling; the
 * trap comes before the allocation, so nothing reads the false lengths); ceil_sparse makes a sparse file of N bytes for
 * file_read. Compiled with
 * `--cc`; no runtime header (the prototypes are below). */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

void avant_buf_append(void *buf, const void *p, int32_t n);
void avant_buf_push_byte(void *buf, int64_t c);
void avant_array_push_i32(void *arr, int32_t v);
void *avant_array_push_slot(void *arr, uint64_t elem_size, uint32_t buf_type_id);
void *avant_hash_put_ik(void *hp, int64_t key);
void avant_buf_fill_appends(void *buf, void *dst_offs, void *src_offs, void *lens, void *extras, int64_t n);

typedef struct {
  uint8_t *data;
  int32_t size;
  int32_t cap;
} CeilBuf;

typedef struct {
  void *buf;
  int32_t size;
  int32_t cap;
} CeilArr;

typedef struct {
  void *keys;
  void *vals;
  uint8_t *meta;
  int32_t size;
  int32_t cap;
  uint32_t desc;
  uint32_t seed;
  int32_t used;
  uint8_t kw;
  uint8_t vw;
  uint8_t vshift;
  uint8_t compact;
  uint32_t changes;
  int32_t frozen;
} CeilHash;

#define CEIL_MAX 2147483647

static CeilBuf *fake_buf(int32_t size) {
  CeilBuf *b = (CeilBuf *)calloc(1, sizeof(CeilBuf));
  b->data = (uint8_t *)calloc(1, 16);
  b->size = size;
  b->cap = size;
  return b;
}

static CeilArr *fake_arr(int32_t size) {
  CeilArr *a = (CeilArr *)calloc(1, sizeof(CeilArr));
  a->buf = calloc(1, 16);
  a->size = size;
  a->cap = size;
  return a;
}

/* mode: 1 Buf append past the ceiling, 2 Buf push_byte at the ceiling,
 * 3 Array push of an Int at the ceiling, 4 Array push of a slot (any
 * element) at the ceiling, 5 Hash growth past its slot ceiling, 7 Buf
 * ranges whose end passes the ceiling, 8 Array push of a byte-wide slot
 * at the ceiling, 9 Array push of an 8-byte slot at one object's storage. Returns only if the runtime did
 * not trap (a failure). */
int64_t ceil_probe(int64_t mode) {
  static const char text[] = "abcd";
  if (mode == 1) {
    avant_buf_append(fake_buf(CEIL_MAX - 2), text, 4);
  } else if (mode == 2) {
    avant_buf_push_byte(fake_buf(CEIL_MAX), 65);
  } else if (mode == 3) {
    avant_array_push_i32(fake_arr(CEIL_MAX), 7);
  } else if (mode == 4) {
    (void)avant_array_push_slot(fake_arr(CEIL_MAX), 8, 0);
  } else if (mode == 8) {
    (void)avant_array_push_slot(fake_arr(CEIL_MAX), 1, 0);
  } else if (mode == 9) {
    /* An Array of 8-byte elements full at one object's storage
     * (536870911 elements, 4294967288 bytes): its next push is past the
     * ceiling, which the element count alone would not say. */
    (void)avant_array_push_slot(fake_arr(536870911), 8, 0);
  } else if (mode == 5) {
    CeilHash *h = (CeilHash *)calloc(1, sizeof(CeilHash));
    h->keys = calloc(1, 16);
    h->vals = calloc(1, 16);
    h->meta = (uint8_t *)calloc(1, 16);
    h->cap = 1073741824;
    h->size = 805306368;
    h->used = 805306368;
    h->kw = 8;
    h->vw = 8;
    h->vshift = 3;
    (void)avant_hash_put_ik(h, 1);
  } else if (mode == 7) {
    /* One range: copy 16 bytes from 0 to CEIL_MAX - 4 (an end past the
     * ceiling, which 32-bit arithmetic wrapped negative). The offset
     * arrays are Array(Int) layouts (64-bit elements) of one element
     * each. */
    static int64_t dst[1] = {CEIL_MAX - 4};
    static int64_t src[1] = {0};
    static int64_t len[1] = {16};
    static int64_t ext[1] = {0};
    CeilArr d = {dst, 1, 1}, s = {src, 1, 1}, l = {len, 1, 1}, e = {ext, 1, 1};
    CeilBuf *b = fake_buf(16);
    avant_buf_fill_appends(b, &d, &s, &l, &e, 1);
  }
  return mode;
}

char *avant_str_concat(const char *a, const char *b);

/* Two distinct strings outside the heap, with the header a literal has
 * (its size, the bytes type, the string tag, no heap flag) and a false
 * length: the runtime's length cache would check a cached length's
 * terminator, which a false length puts out of reach. They are this
 * file's own, not the program's: the program's strings may lie in a run
 * of young objects the check modes walk by their headers (a runtime
 * string is allocated inline), where a false size would end the walk
 * past the cursor. A and B are the program's strings, unused. */
typedef struct {
  uint32_t size;
  uint16_t type_id;
  uint16_t flags;
  char bytes[8];
} __attribute__((aligned(8))) FakeStr;
static FakeStr fake_a = {0x60000001u, 0, 0x4000u, "a"};
static FakeStr fake_b = {0x60000001u, 0, 0x4000u, "b"};

int64_t ceil_concat(char *a, char *b) {
  (void)a;
  (void)b;
  (void)avant_str_concat(fake_a.bytes, fake_b.bytes);
  return 0;
}

/* A sparse file of n bytes (no memory, no disk blocks): 0, or -1. */
int64_t ceil_sparse(const char *path, int64_t n) {
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (fd < 0) {
    return -1;
  }
  int rc = ftruncate(fd, (off_t)n);
  close(fd);
  return rc == 0 ? 0 : -1;
}
