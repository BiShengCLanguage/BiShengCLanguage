// RUN: %clang_cc1 -xbsc -fsyntax-only -verify -Wno-deprecated-non-prototype %s

typedef int * _Owned OwnedIntPtr;
typedef int * _Borrow BorrowIntPtr;
typedef OwnedIntPtr * OwnedIntPtrPtr;
typedef BorrowIntPtr * BorrowIntPtrPtr;
typedef OwnedIntPtrPtr * OwnedIntPtrPtrPtr;
typedef BorrowIntPtrPtr * BorrowIntPtrPtrPtr;

struct BorrowInner {
  int * _Borrow p;
};

struct BorrowOuter {
  struct BorrowInner inner;
};
typedef struct BorrowOuter BorrowOuterAlias;
typedef BorrowOuterAlias BorrowOuterAlias2;

struct OwnedInner {
  int * _Owned p;
};

struct OwnedOuter {
  struct OwnedInner inner;
};
typedef struct OwnedOuter OwnedOuterAlias;
typedef OwnedOuterAlias OwnedOuterAlias2;

struct BorrowPair {
  int * _Borrow first;
  int * _Borrow second;
};

struct OwnedPair {
  int * _Owned first;
  int * _Owned second;
};

struct BorrowMixedInner {
  int * _Borrow nested;
};

struct BorrowMixedOuter {
  int * _Borrow direct;
  struct BorrowMixedInner inner;
};

struct OwnedMixedInner {
  int * _Owned nested;
};

struct OwnedMixedOuter {
  int * _Owned direct;
  struct OwnedMixedInner inner;
};

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static int * _Owned knr_owned(p)
  int * _Owned p;
{
  return p;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int * _Borrow knr_borrow(p)
  int * _Borrow p;
{
  return p;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static int knr_owned_param_only(p)
  int * _Owned p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static int knr_owned_param_const_pointer(p)
  int * const _Owned p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_borrow_param_const(p)
  const int * _Borrow p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_borrow_param_pointer_const(p)
  int * const _Borrow p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static int knr_owned_multi_level_pointer(p)
  int * _Owned *p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_borrow_multi_level_pointer(p)
  int * _Borrow *p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static OwnedIntPtr knr_owned_typedef_return(p)
  OwnedIntPtr p;
{
  return p;
}

static BorrowIntPtr knr_borrow_typedef_return(p) // expected-error {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
  BorrowIntPtr p;
{
  return p;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static int knr_owned_typedef_param(p)
  OwnedIntPtr p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_borrow_typedef_param(p)
  BorrowIntPtr p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static int knr_owned_typedef_multi_level_param(p)
  OwnedIntPtrPtr p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_borrow_typedef_multi_level_param(p)
  BorrowIntPtrPtr p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static int knr_owned_typedef_three_level_param(p)
  OwnedIntPtrPtrPtr p;
{
  return p != 0;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_borrow_typedef_three_level_param(p)
  BorrowIntPtrPtrPtr p;
{
  return p != 0;
}

static int knr_owned_array_param(p) // expected-error {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
// expected-error@-1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
  OwnedIntPtr p[4];
{
  return p[0] != 0;
}

static int knr_borrow_array_param(p) // expected-error {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
  BorrowIntPtr p[4];
{
  return p[0] != 0;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_nested_borrow_record(box)
  struct BorrowOuter box;
{
  return box.inner.p != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static struct OwnedOuter knr_nested_owned_record(box)
  struct OwnedOuter box;
{
  return box;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_typedef_nested_borrow_record(box)
  BorrowOuterAlias2 box;
{
  return box.inner.p != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static OwnedOuterAlias2 knr_typedef_nested_owned_record(box)
  OwnedOuterAlias2 box;
{
  return box;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_multi_borrow_record(box)
  struct BorrowPair box;
{
  return box.first != 0 || box.second != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static struct OwnedPair knr_multi_owned_record(box)
  struct OwnedPair box;
{
  return box;
}

// expected-error@+1 {{type with '_Borrow' semantics is not allowed in a K&R-style function definition}}
static int knr_mixed_borrow_record(box)
  struct BorrowMixedOuter box;
{
  return box.direct != 0 || box.inner.nested != 0;
}

// expected-error@+1 {{type with '_Owned' semantics is not allowed in a K&R-style function definition}}
static struct OwnedMixedOuter knr_mixed_owned_record(box)
  struct OwnedMixedOuter box;
{
  return box;
}
