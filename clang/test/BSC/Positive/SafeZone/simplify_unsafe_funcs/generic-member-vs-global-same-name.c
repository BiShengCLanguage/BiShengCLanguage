// RUN: %clang_cc1 -fbsc-experimental -fsyntax-only -verify -x bsc %s
// expected-no-diagnostics
struct Vec<T> {
  T *buf;
  unsigned long len;
};

_Safe T Vec<T>::remove(struct Vec<T> *_Borrow this, unsigned long index) {
  _Unsafe {
    T ret = this->buf[index];
    return ret;
  }
}

// This declaration (like stdio's remove) must not conflict with Vec<T>::remove.
extern int remove(const char *filename);

int use_remove(void) {
  return remove("/tmp/file");
}
