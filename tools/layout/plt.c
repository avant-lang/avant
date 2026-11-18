/* Four PLT entries a program does not otherwise have, in a function
 * nothing calls: linked in with `avant compile --cc`, it moves the
 * program's code (.text starts 64 bytes later on x86-64 with the
 * runtime's 64-byte alignment) and changes nothing else. The shift
 * tools/layout-retime.sh times a task with. */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
void layout_plt_unused(void) {
  unsigned char v;
  char b[1];
  (void)sigaltstack(0, 0);
  (void)mincore(0, 0, &v);
  (void)getauxval(AT_PHDR);
  (void)pread(-1, b, 1, 0);
}
