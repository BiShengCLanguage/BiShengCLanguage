// RUN: %clang -fbsc-experimental -x bsc %s -o %t.output
// RUN: %t.output
// RUN: %clang -fbsc-experimental -x bsc -rewrite-bsc %s -o %t-rw.c
// RUN: %clang -fbsc-experimental %t-rw.c -o %t-rw.output
// RUN: %t-rw.output

struct S {
  int a;
  int b;
};

int struct S::retA(struct S *this) {
  return this->a;
}

int struct S::retB(This *this) {
  return this->b;
}

int main() {
  struct S s = { 2, 3 };
  int x = struct S::retA(&s);
  int y = struct S::retB(&s);
  return 0;
}