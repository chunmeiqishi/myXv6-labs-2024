#include "kernel/param.h"
#include "kernel/memlayout.h"
#include <stdio.h>
int main() {
  printf("SUPERPGSIZE = 0x%x\n", (1<<21));
  printf("4 * SUPERPGSIZE = 0x%x\n", 4 * (1<<21));
  return 0;
}
