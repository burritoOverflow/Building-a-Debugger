#include <csignal>
#include <cstdio>
#include <unistd.h>

int main() {
  unsigned long long a         = 0xcafecafe;
  auto               a_address = &a;
  // write the addr to stdout
  write(STDOUT_FILENO, &a_address, sizeof(void*));
  // and flush so the debugger can see this value
  fflush(stdout);
  raise(SIGTRAP);

  char b[12]     = {0};
  auto b_address = &b;
  write(STDOUT_FILENO, &b_address, sizeof(void*));
  fflush(stdout);
  raise(SIGTRAP);

  printf("%s", b);
}
