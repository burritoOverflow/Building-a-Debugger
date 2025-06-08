#include <cstdio>
#include <numeric>
#include <signal.h>
#include <unistd.h>

// only if "nobody is watching"
void AnInnocentFunction() { std::puts("Putting pineapple on pizza..."); }

void AnInnocentFunctionEnd() {}

// The checksum function sums up all the bytes of 'AnInnocentFunction' into
// a single integer. Simplified version of 'section hashing'
int checksum() {
  const auto start =
      reinterpret_cast<volatile const char*>(&AnInnocentFunction);
  const auto end =
      reinterpret_cast<volatile const char*>(&AnInnocentFunctionEnd);
  return std::accumulate(start, end, 0);
}

int main() {
  const auto safe = checksum();

  const auto ptr = reinterpret_cast<void*>(&AnInnocentFunction);
  write(STDOUT_FILENO, &ptr, sizeof(void*));
  fflush(stdout);

  raise(SIGTRAP);

  while (true) {
    if (checksum() == safe) {
      // nothing has changed, so we can call the 'innocent' function
      AnInnocentFunction();
    } else {
      // otherwise, a breakpoint has probably been set, so...
      puts("Putting pepperoni on pizza...");
    }

    fflush(stdout);
    raise(SIGTRAP);
  }
}
