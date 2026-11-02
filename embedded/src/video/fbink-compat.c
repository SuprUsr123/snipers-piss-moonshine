#include <stdlib.h>

/* New FBink headers reference this glibc 2.38 symbol; Kindle firmware has the legacy one. */
extern unsigned long fbink_legacy_strtoul(const char *, char **, int) __asm__("strtoul");

unsigned long __isoc23_strtoul(const char *text, char **end, int base) {
  return fbink_legacy_strtoul(text, end, base);
}
