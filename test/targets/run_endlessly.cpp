int main() {
  volatile int i;
  // empty infinite loops are UB so we'll use a volatile variable
  while (true) i = 42;
}
