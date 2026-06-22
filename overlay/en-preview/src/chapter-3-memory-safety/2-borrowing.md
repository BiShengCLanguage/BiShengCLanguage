# Borrowing

Borrowing, as an important component of BiSheng C's memory safety features, complements ownership. The previous section described the ownership feature: an entity that owns a resource is responsible for releasing that resource. This section will introduce borrowing of resources.

## Feature Overview

If we only had ownership types, code capabilities would be very limited because function calls, assignments, and other operations transfer ownership. In programming, we often need to express the concept of "borrowing a resource," which is distinct from "owning a resource." Just as in real life, if someone owns something, you can borrow it from them, and after using it, you must return it to the owner.

### Definition of Borrowing and Borrowing Operators

In BiSheng C, **a borrow is a pointer type that points to the memory address where the borrowed object is stored**. To express the concept of borrowing:

1. A new keyword `_Borrow` is introduced, and `_Borrow` is used to modify the pointer type `T*`, representing the borrow type of T. `_ArrayElem` can also be used together with `_Borrow` to modify the pointer type `T*`, representing a borrow type for array elements of T.
2. Borrowing operators `&_Mut` and `&_Const` are introduced, where `&_Mut e` represents obtaining a **mutable borrow** of expression e, and `&_Const e` represents obtaining a **read-only borrow** of expression `e`. Here, expression `e` is required to be an lvalue, similar to the address-of operator `&` in standard C. The borrow operator actually obtains the address of expression `e`.

For example, we can create a mutable borrow `p1` and an immutable borrow `p2` of the local variable `local` and use them:

```C
void use_immut(const int *_Borrow p) {}
void use_mut(int *_Borrow p) {}

void foo() {
  int local = 5;
  // p1 is a mutable borrow pointer to local
  int *_Borrow p1 = &_Mut local;
  use_mut(p1);
  // p2 is an immutable borrow pointer to local
  const int *_Borrow p2 = &_Const local;
  use_immut(p2);
}

int main() {
  foo();
  return 0;
}
```

Additionally, if expression `e` is a pointer dereference expression, `&_Mut *p` and `&_Const *p` can be seen as taking a mutable borrow and an immutable borrow of the value stored at address `p`, i.e., `*p`. **This operation does not create a temporary variable for `*p`.** Here, `p` can be a raw pointer, an `_Owned` pointer, or another borrow pointer. For example:

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation

void foo() {
  int *x1 = malloc(sizeof(int));
  *x1 = 1;
  // p1 borrows *x1
  int *_Borrow p1 = &_Mut * x1;
  int *_Owned x2 = safe_malloc(2);
  // p2 borrows *x2
  int *_Borrow p2 = &_Mut * x2;
  int local = 3;
  int *_Borrow x3 = &_Mut local;
  // p3 borrows *x3
  int *_Borrow p3 = &_Mut * x3;
  safe_free((void *_Owned)x2);
}

int main() {
  foo();
  return 0;
}
```

If expression `e` is an array subscript expression, then the result types of `&_Mut e` and `&_Const e` will carry both `_Borrow _ArrayElem` qualifiers to indicate a borrow pointer "pointing to an array element." Such borrow pointers allow subscript operations `[]`, arithmetic operations, and have additional type conversion rules. Otherwise, the rules are the same as for ordinary `_Borrow` pointers. Unless explicitly stated or `_Borrow _ArrayElem` is listed separately, the rules for `_Borrow` pointers also apply to `_Borrow _ArrayElem`.

```C
void f1(int *_Borrow _ArrayElem p) {}
void f2(const int *_Borrow _ArrayElem p) {}

void foo() {
  int arr[4] = {1, 2, 3, 4};
  int *_Borrow _ArrayElem p = &_Mut arr[0];
  const int *_Borrow _ArrayElem q = &_Const arr[1];
  f1(&_Mut arr[2]);
  f2(&_Const arr[3]);
}
```

### The Role of Borrowing

Suppose we have this requirement: create a file and call some operation functions to read and write to the file. Without the concept of borrowing, calling file operation functions would cause ownership transfer of the file pointer. To keep the file pointer usable after function calls, we would need to return ownership back to the caller:

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation
#include <stdio.h>

typedef struct {
  int file_id;
} MyFile;

MyFile *_Owned create_file(int id) {
  MyFile f = {.file_id = id};
  return safe_malloc(f);
}
void file_safe_free(MyFile *_Owned p) { safe_free((void *_Owned)p); }

MyFile *_Owned insert_str(MyFile *_Owned p, char *str) {
  // some operation to insert a string to file
  printf("%s to file %d\n", str, p->file_id);
  // Transfer ownership back to caller through return value to avoid ownership transfer
  return p;
}

MyFile *_Owned other_operation(MyFile *_Owned p) {
  // some operation
  // Transfer ownership back to caller through return value to avoid ownership transfer
  return p;
}

int main() {
  MyFile *_Owned p = create_file(0);
  char str[] = "insert str";
  // p's ownership is first moved to insert_str, then transferred back to caller through return value
  p = insert_str(p, str);
  p = other_operation(p);
  file_safe_free(p);
  return 0;
}
```

This approach causes frequent ownership transfers of the file pointer, which is error-prone when code logic becomes complex. Moreover, if ownership is transferred away and not returned, the file pointer can no longer be used. With borrowing, we can pass a borrow of the file pointer as a parameter to operation functions. After the function returns, the file pointer can still be used for subsequent operations, without needing to pass in ownership through function parameters and then pass out ownership through function returns as in the example above. The code becomes cleaner:

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation
#include <stdio.h>

typedef struct {
  int file_id;
} MyFile;

MyFile *_Owned create_file(int id) {
  MyFile f = {.file_id = id};
  return safe_malloc(f);
}
void file_safe_free(MyFile *_Owned p) { safe_free((void *_Owned)p); }

void insert_str(MyFile *_Borrow p, char *str) {
  // some operation to insert a string to file
  printf("%s to file %d\n", str, p->file_id);
  // No need to return ownership
}

void other_operation(MyFile *_Borrow p) {
  // some operation
  // No need to return ownership
}

int main() {
  MyFile *_Owned p = create_file(0);
  char str[] = "insert str";
  // Ownership is not moved
  insert_str(&_Mut * p, str);
  // Ownership is not moved
  other_operation(&_Mut * p);
  file_safe_free(p);
  return 0;
}
```

## Lifetimes of Borrow Variables and Borrowed Objects

### Lifetime and Its Role

We can take borrows of different kinds of objects: `_Owned` variables, non-`_Owned` local variables, global variables, temporary anonymous variables, parameters, etc., or even part of a composite variable. To correctly represent the valid scope of borrow variables and different kinds of borrowed objects, we introduce the concept of lifetime.

The main purpose of lifetime checking is to avoid dangling pointers, which cause programs to use data they shouldn't. The following C code is a typical example using a dangling pointer:

```C
int main() {
  int *p;
  {
    int local = 5;
    p = &local;
  }
  *p = 1;
  return 0;
}
```

This C code has two points worth noting:

1. The declaration of `int *p` has the risk of being used with `NULL`;
2. `p` points to the `local` variable in the inner block, but `local` will be released when the block ends, so after returning to the outer block, `p` will point to an invalid address and is a dangling pointer. It points to the prematurely released variable `local`, and we can predict that `*p = 1` will cause undefined behavior at runtime. When code logic is complex, such abnormal behavior is hard to discover.

For the second point, BiSheng C stipulates: **any borrow of a resource cannot have a longer lifetime than the resource's owner**. That is: the lifetime of a borrow variable cannot be longer than the lifetime of the borrowed object.

Next, we rewrite the above C code using BiSheng C's borrow feature. By checking the lifetimes of the borrow variable and the borrowed object, potential memory safety risks can be identified at compile time:

```C
int main() {
  int local1 = 1;
  // Borrow pointer variable p must be initialized before use, otherwise an error will be reported
  int *_Borrow p = &_Mut local1;
  {
    int local2 = 2;
    // After reassigning p, p no longer borrows local1, but borrows local2
    p = &_Mut local2;
  }
  *p = 3; // error: local2's lifetime is not long enough
  return 0;
}
```

### Borrow Variables and Borrowed Objects

Each borrow variable (i.e., _Borrow pointer variable) has one or more borrowed objects, for example:

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation

struct S {
  int a;
};

int *_Borrow bar(int *_Borrow, int *_Borrow);

int g = 5;
void foo(int a, int *_Owned b, int *c, struct S d) {
  // Borrowed object is an ordinary local variable
  int local = 5;
  int *_Borrow p1 = &_Mut local; // p1's borrowed object is local
  int *_Borrow p2 = &_Mut * p1;  // p2's borrowed object is *p1
  int *_Borrow p3 = p1;         // p3's borrowed object is *p1

  // Borrowed object is an owned variable
  int *_Owned x1 = safe_malloc<int>(2);
  int *_Borrow p4 = &_Mut * x1; // p4's borrowed object is *x1

  // Borrowed object is a raw pointer variable
  int *x2 = malloc(sizeof(int));
  int *_Borrow p5 = &_Mut * x2; // p5's borrowed object is *x2

  // Borrowed object is a field of a struct
  struct S s = {.a = 5};
  int *_Borrow p6 = &_Mut s.a; // p6's borrowed object is s.a

  // Borrowed object is a function return value, same as the "borrowed object" of the callee's borrow-type parameters
  int local1 = 10, local2 = 20;
  // The callee function bar has two borrow-type parameters, so p7's borrowed objects are local1 and local2
  int *_Borrow p7 = bar(&_Mut local1, &_Mut local2);

  // Borrowed object is a global variable
  const int *_Borrow p8 = &_Const g; // p8's borrowed object is g

  // Borrowed object is a function parameter
  int *_Borrow p9 = &_Mut a;    // p9's borrowed object is a
  int *_Borrow p10 = &_Mut * b; // p10's borrowed object is *b
  int *_Borrow p11 = &_Mut * c; // p11's borrowed object is *c
  int *_Borrow p12 = &_Mut d.a; // p12's borrowed object is d.a

  safe_free((void *_Owned)b);
  safe_free((void *_Owned)x1);
}

int main() {
  int a = 42;
  int *_Owned b = safe_malloc(73);
  int *c = &a;
  struct S d = {.a = 31};
  foo(a, b, c, d);
  return 0;
}
```

Note: If the borrowed object is obtained from taking an address or casting to a raw pointer and then getting a borrow, it will not be recorded as a borrowed object. For example:

```C
void f1() {
  int a = 1;
  int *p = &a;
  int *_Borrow p1 = (int *_Borrow)(int *)(&_Mut *p); // The borrowed object *p is not recorded
  int *_Borrow p2 = &_Mut *&a; // The borrowed object a is not recorded
}
```

### Non-Lexical Lifetime of Borrow Variables

A variable's lifetime starts from its declaration to the end of the current entire statement block. This design is called Lexical Lifetime because the variable's lifetime is strictly bound to the scope range in the lexicon. This strategy is very simple to implement, but it may be too conservative. In some cases, the scope of borrow variables is overly extended, so that some code that is actually safe is also prevented, which to some extent limits the code programmers can write. Therefore, BiSheng C introduces Non-Lexical Lifetime (abbreviated as NLL) for borrow variables, using more refined means to calculate the range where borrow variables actually take effect. **The NLL range of a borrow variable is: from the borrow point, continuing until the last place it is used**. Specifically, it is **from the definition or reassignment of the borrow variable to the end of the last use before being reassigned**.

The following scenarios are considered uses of borrow variable p:

1. Function calls, such as `use(p)` or `use(&_Mut *p)`
2. Function returns `return p` or `return &_Mut *p`
3. Dereference `*p`
4. Member access `p->field`

For example:

```C
void use(int *_Borrow p) {}
void other_op() {}

// In this example, p's NLL is segmented, each NLL segment has one borrowed object
void foo() {
  int local1 = 1, local2 = 2;  //#1
  int *_Borrow p = &_Mut local1; //#2, p's first NLL segment starts, borrowed object is local1
  other_op();                  //#3
  use(p);                      //#4, p's first NLL segment ends
  other_op();                  //#5
  p = &_Mut local2;             //#6, p's second NLL segment starts, borrowed object is local2, since there's no further use of p afterwards, p's NLL ends
  other_op();     //#7
}
// p's NLL is: [2,4]->local1, [6,6]->local2

int main() {
  foo();
  return 0;
}
```

### Lexical Lifetime of Borrowed Objects

Unlike borrow variables, the lifetime of borrowed objects is Lexical Lifetime. For the lifetimes of different kinds of borrowed objects, we give specific definitions:

| Borrowed Object Type | | Lifetime Definition |
| ---- | ---- | ---- |
| Global variable | | The lifetime of a global variable is the entire program, existing from program start to exit |
| Local variable | owned variable | Starts from variable definition, ends when it is moved away (for _Owned struct types, if not moved, the lifetime ends when the current block ends) |
| | non-owned non-borrow variable | Starts from variable definition, ends when the current block ends |
| Local literal | `"string literal"` | Starts from the point of use, ends when the current block ends |
| | `(struct S) { ... }` | Starts from the point of use, ends when the current block ends |
| `e->field` | | Lifetime of `*e` |
| `e.field` | | Lifetime of `e` |
| `e[index]` or `*e` (`e` is an array) | | Lifetime of `e` |
| `e[index]` or `*e` (`e` is a pointer) | | Lifetime of `*e` |

### Lifetime Constraints of Borrowing

In section 2.1, we mentioned that for borrowing, we have the following lifetime constraint: **the lifetime of a borrow variable cannot be longer than the lifetime of the borrowed object**.

For example:

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation

void use(int *_Borrow p) {}
int *_Borrow call(int *_Borrow p, int *_Borrow q) { return p; }

// In this example, p's lifetime is [2,4], the borrowed object local's lifetime is [1,4], satisfying the lifetime constraint
void test1() {
  int local = 5;              //#1
  int *_Borrow p = &_Mut local; //#2
  use(p);                     //#3
} //#4

// In this example, p's lifetime has two segments:
// ok: The first segment is [2,2], the borrowed object local1's lifetime is [1,8], satisfying the lifetime constraint
// error: The second segment is [5,7], the borrowed object local2's lifetime is [4,6], not satisfying the lifetime constraint
void test2() {
  int local1 = 5;              //#1
  int *_Borrow p = &_Mut local1; //#2
  {                            //#3
    int local2 = 5;            //#4
    p = &_Mut local2;           //#5
  }                            //#6
  use(p);                      //#7
} //#8

// In this example, p's lifetime has two segments:
// ok: The first segment is [2,2], the borrowed object local1's lifetime is [1, 8], satisfying the lifetime constraint
// error: The second segment is [5,7], the borrowed object has two: local1 and local2, where local2's lifetime is [4, 6], not satisfying the lifetime constraint
void test3() {
  int local1 = 5;                       //#1
  int *_Borrow p = &_Mut local1;          //#2
  {                                     //#3
    int local2 = 5;                     //#4
    p = call(&_Mut local1, &_Mut local2); //#5
  }                                     //#6
  use(p);                               //#7
} //#8

// In this example, the if branch reassigns p, at #10
// error: When use(p) is called, the lifetime of p's borrowed object local2 has already ended, not satisfying the lifetime constraint
// ok: The else branch satisfies the lifetime constraint
void test4() {
  int local = 5;              //#1
  int *_Borrow p = &_Mut local; //#2
  int local1 = 5;             //#3
  if (rand()) {               //#4
    int local2 = 5;           //#5
    p = &_Mut local2;          //#6
  } else {                    //#7
    p = &_Mut local1;          //#8
  }                           //#9
  use(p);                     //#10
}

// In this example, p's lifetime is [2,4], the borrowed object *x's lifetime is [1,3], not satisfying the lifetime constraint, error
void test5() {
  int *_Owned x = safe_malloc<int>(5); //#1
  int *_Borrow p = &_Mut * x;           //#2
  safe_free((void *_Owned)x);          //#3
  use(p);                             //#4
} //#5

int main() {
  test1();
  test2();
  test3();
  test4();
  test5();
  return 0;
}
```

## Mutable Borrows and Immutable Borrows

BiSheng C classifies the permissions of borrow pointers into mutable (mut) borrows and immutable (immut) borrows. We can read and write the content of the borrowed object through mutable borrow pointers. Through immutable borrow pointers, we can only read the content of the borrowed object but cannot modify it. For example:

```C
// Mutable borrow pointer type is T *_Borrow
void use_mut(int *_Borrow p) {
  // Through mutable borrow pointer, can modify the value of borrowed object
  *p = 5;
  // Through mutable borrow pointer, can read the value of borrowed object
  int a = *p;
}

// Immutable borrow pointer type is const T *_Borrow
void use_immut(const int *_Borrow p) {
  *p = 5; // error: Cannot modify the value of borrowed object through immutable borrow pointer
  // Through immutable borrow pointer, can read the value of borrowed object
  int a = *p;
}

int main() {
  int i = 1;
  int *_Borrow pmi = &_Mut i;
  use_mut(pmi);
  const int *_Borrow pimi = &_Const i;
  use_immut(pimi);
  return 0;
}
```

### `&_Mut e` Requires e to Be Modifiable

We mentioned in section 1.1 that `&_Mut e` and `&_Const e` require expression e to be an lvalue, meaning e can have its address taken. For the mutable borrow expression `&_Mut e`, we also require e to be mutable. Specifically:

| lvalue expression | Is it modifiable |
| ---- | ---- |
| ident | Variable ident is not modified by const, and ident cannot be a function name |
| "string literal" | Not allowed, because string literals are stored in the constant area and cannot be written. Attempting to take a mutable borrow of a string literal (such as `&_Mut "hello"` or `&_Mut * "hello"`) will result in a compilation error |
| (struct S) { ... } | Allowed |
| `e->field` | Requires `e` to be a mutable borrow pointer, or an _Owned pointer pointing to a modifiable type, or a raw pointer pointing to a modifiable type, and the field is not modified by const. For multi-level fields, each level's field must not be modified by const |
| `e.field` | Requires `e` to be mutable, and the field is not modified by const. For multi-level fields, each level's field must not be modified by const |
| `e[index]` or `*e` (`e` is an array) | Requires `e` to be mutable |
| `e[index]` or `*e` (`e` is a pointer) | Requires `e` to be a mutable borrow pointer, or an _Owned pointer pointing to a modifiable type, or a raw pointer pointing to a modifiable type |

### Only One Mutable Borrow Can Exist at a Time

If two or more pointers simultaneously access the same data, and at least one pointer is used to write data, it may lead to undefined behavior. For example:

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation
#include <stdio.h>

void free_a(int *a) { free(a); }
void read_a(int *a) { printf("%d\n", *a); }

void test() {
  int *a = malloc(sizeof(int));
  *a = 42;
  int *p1 = a;
  int *p2 = a;
  // This function will free the memory pointed to by a
  free_a(p1);
  // This function will read the memory pointed to by a
  read_a(p2); // Prints a dirty value
}

int main() {
  test();
  return 0;
}
```

Since borrowing is essentially a pointer, to avoid the above problems, BiSheng C stipulates that **at any given moment, for the same object, there can either be only one mutable borrow, or any number of immutable borrows**.

```C
void write(int *_Borrow p) {}
void read(const int *_Borrow p) {}

void test1() {
  int local = 1;
  int *_Borrow p1 = &_Mut local;
  int *_Borrow p2 = &_Mut local; // error: At most one mutable borrow variable pointing to local can exist at the same time
  write(p1);
  write(p2);
}

void test2() {
  int local = 1;
  int *_Borrow p1 = &_Mut local;
  const int *_Borrow p2 = &_Const local; // error: Mutable and immutable borrows pointing to local cannot exist simultaneously
  write(p1);
  read(p2);
}

void test3() {
  int local = 1;
  const int *_Borrow p1 = &_Const local;
  int *_Borrow p2 = &_Mut local; // error: Mutable and immutable borrows pointing to local cannot exist simultaneously
  read(p1);
  write(p2);
}

int main() {
  test1();
  test2();
  test3();
  return 0;
}
```

If both a mutable borrow and an immutable borrow of a variable exist simultaneously, it may happen that the borrowed object's memory state is modified through the mutable borrow, and then the modified memory is accessed using the immutable borrow, leading to undefined behavior.

For example:

```C
#include <stdio.h>

struct A {
  int *p;
};

const int *_Borrow struct A::get_p(This *_Borrow this) {
  return &_Const * (this->p);
}

void struct A::free_p(This *_Borrow this) { free(this->p); }

int main() {
  struct A a = {.p = malloc(sizeof(int))};
  // q borrows a.p
  const int *_Borrow q = a.get_p();
  // The memory pointed to by a.p is freed
  a.free_p(); // error: a is borrowed mutably multiple times
  // *q operation may lead to undefined behavior
  printf("%d\n", *q);
  return 0;
}
```

In the above code, `a.free_p()` actually uses a mutable borrow pointing to a. This mutable borrow invalidates the previously defined borrow q. Since `printf("%d\n", *q)` uses the invalidated q, the BiSheng C compiler will report an error, thus preventing unsafe behavior.

Since immutable borrows do not cause the borrowed object to be modified, any number of immutable borrows can exist at the same moment. For example:

```C
void read(const int *_Borrow p) {}

void test() {
  int local = 5;
  const int *_Borrow p1 = &_Const local;
  const int *_Borrow p2 = &_Const local;
  read(p1);
  read(p2);
}

int main() {
  test();
  return 0;
}
```

## The Effect of Borrowing on the Borrowed Object

### The Effect of Immutable Borrow on the Borrowed Object

Taking an immutable borrow of expression e, i.e., `&_Const e`, before the lifetime of this immutable borrow ends, e can only be read and cannot be modified, nor can a mutable borrow be created for e, but an immutable borrow can still be taken for e.

| Immutable borrow expression | State of the borrowed object |
| ---- | ---- |
| `&_Const ident` | Variable `ident` can only be read, not modified, and no mutable borrow can be created for variable `ident`. Creating immutable borrows for variable `ident` is allowed |
| `&_Const "string literal"` | Temporary variable is always in "read-only" state |
| `&_Const (struct S) { ... }` | Temporary variable is always in "read-only" state |
| `&_Const e->field` | `e->field` enters "read-only" state, and modifying `*e` as a whole is also not allowed. But modifying other members pointed to by `e`, or taking mutable borrows of other members is allowed |
| `&_Const e.field` | `e.field` enters "read-only" state, and modifying `e` as a whole is also not allowed. But modifying other members of `e`, or taking mutable borrows of other members is allowed |
| `&_Const e[index]` or `&_Const *e` (`e` is an array) | `e` enters "read-only" state, modifying `e` and its direct or indirect members is not allowed, nor is taking mutable borrows of other members |
| `&_Const e[index]` or `&_Const *e` (`e` is a pointer) | `*e` enters "read-only" state, directly modifying `*e` and its direct or indirect members is not allowed, nor is taking mutable borrows of `*e` and its direct or indirect members. If `e` is an _Owned pointer type, then `e` also enters read-only state. If `e` is a _Borrow pointer type (i.e., this is an immutable reborrow of `e`), then modifying what `e` points to is allowed, and after modifying the pointer target, `e`'s read/write attributes are restored to the state before the reborrow |

### The Effect of Mutable Borrow on the Borrowed Object

Taking a mutable borrow of expression e, i.e., `&_Mut e`, expression e enters a "frozen" state. Before the lifetime of this mutable borrow ends, e cannot be read, cannot be modified (including being moved), and cannot be borrowed.

| Mutable borrow expression | State of the borrowed object |
| ---- | ---- |
| `&_Mut ident` | Variable `ident` is frozen |
| `&_Mut "string literal"` | Compilation error (string literals are immutable and cannot be mutably borrowed) |
| `&_Mut (struct S) { ... }` | Temporary variable is frozen |
| `&_Mut e->field` | `e->field` is frozen, reading and writing `e->field` is not allowed, modifying `*e` as a whole is not allowed, but modifying other members pointed to by `e`, or taking mutable borrows of other members is allowed |
| `&_Mut e.field` | `e.field` is frozen, reading and writing `e.field` is not allowed, modifying `e` as a whole is not allowed, but modifying other members of `e`, or taking mutable borrows of other members is allowed |
| `&_Mut e[index]` or `&_Mut *e` (`e` is an array) | `e` is frozen, reading and writing `e` and its members is not allowed |
| `&_Mut e[index]` or `&_Mut *e` (`e` is a pointer) | `*e` is frozen, reading, writing, and taking borrows of `*e` and its members is not allowed. If `e` is an _Owned pointer type, then reading and writing `e` is also not allowed; if `e` is a _Borrow pointer type (i.e., this is a mutable reborrow of `e`), then modifying what `e` points to is allowed, and after modifying the pointer target, reading, writing, and taking borrows of `*e` and its direct or indirect members is allowed |

## Borrow Types in Function Definitions

1. It is not allowed for a function to have no borrow-type parameters but have a borrow-type return value.

2. If a function has one borrow-type parameter and the function return is a borrow type, then we directly consider that the borrow of this return value comes from this borrow-type parameter, and the "borrowed object" of the returned borrow is the same as the "borrowed object" of this borrow-type parameter. This returned borrow should also satisfy the borrowing rules mentioned earlier.

3. If a function has multiple borrow-type parameters and the function return is a borrow type, then we directly consider that the borrow of this return value simultaneously contains "borrowed variables" passed from multiple borrow-type parameters. This returned borrow should also satisfy the borrowing rules mentioned earlier.

```C
int *_Borrow f1(int *_Borrow p) { return p; }
int *_Borrow f2(int *_Borrow p1, int *_Borrow p2) { return p1; }

void test() {
  int local = 5;
  int *_Borrow p1 = f1(&_Mut local);
  /* Function f1's parameter created a mutable borrow of local, which was passed
     to return value p1, causing p1 to be equivalent to a mutable borrow of local,
     so the borrowed object of return value p1 is local, and before p1's lifetime
     ends, local will remain frozen.*/

  int local1, local2;
  int *_Borrow p2 = f2(&_Mut local1, &_Mut local2);
  /* Function f2's parameters created mutable borrows of local1 and local2,
     these two borrows were passed to return value p2, causing p2 to be
     equivalent to a mutable borrow of local1 and local2, so the borrowed
     objects of return value p2 are local1 and local2, and before p2's lifetime
     ends, local1 and local2 remain frozen.*/
}

int main() {
  test();
  return 0;
}
```

## Borrow Types in struct Definitions

1. If a struct contains multiple borrow members, then this struct simultaneously has multiple "borrowed objects," and these borrow members should also satisfy the borrowing rules mentioned earlier.

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation

struct R {
  int *_Borrow m1;
  int *_Borrow m2;
};

void test() {
  int local1, local2;
  struct R r = {.m1 = &_Mut local1, .m2 = &_Mut local2};
  // Before r's lifetime ends, local1 and local2 remain frozen.
  // Because variable r created a mutable borrow of local1 and local2 during initialization,
  // causing r to simultaneously contain a mutable borrow of local1 and a mutable borrow of local2.
}

int main() {
  test();
  return 0;
}
```

## Dereferencing Borrow Variables

Dereferencing of borrow pointer variables is allowed, consistent with the dereference operation in standard C: the syntax for dereferencing borrow pointer variable `p` is `*p`.
Dereferencing a borrow variable `e` of type `const T * _Borrow` as `*e` results in type `T`.
Dereferencing a borrow variable `e` of type `T * _Borrow` as `*e` results in type `T`.
If `p` is a borrow pointing to type `T`, and `o` is an lvalue of type `T`, for the expression `*p`, there are the following restrictions:

| | T is Copy semantics | T is Move semantics |
| ---- | ---- | ---- |
| p is immut borrow | *p = expr; not allowed | *p = expr; not allowed |
| | o = *p; allowed | o = *p; not allowed |
| p is mut borrow | *p = expr; allowed | *p = expr; allowed |
| | o = *p; allowed | o = *p; not allowed |

In the above table, move/copy semantics refer to: `T` is a type modified by `_Owned` and `T` is other types, respectively.

Note: The permissions for assignment operations in the above table also apply to function parameter passing and return scenarios.

## Member Access of Borrow Variables

Borrow pointer variables are allowed to access member variables or call member functions, consistent with the arrow operator in standard C: the syntax for accessing member variable `field` of pointer variable `p` is `p->field`, and the syntax for calling member method `method()` of pointer variable `p` is `p->method()`.

### Accessing Member Variables

When accessing member variables through a borrow, the expression's type depends on the member variable's own type. The type of the `p->field` expression is the same as the type defined for the `field` member.
If the type of `p->field` is `T`, and `o` is an lvalue of type `T`, for the `p->field` expression, there are the following restrictions:

| | T is Copy semantics | T is Move semantics |
| ---- | ---- | ---- |
| p is immut borrow | p->field = expr; not allowed | p->field = expr; not allowed |
| | o = p->field; allowed | o = p->field; not allowed |
| p is mut borrow | p->field = expr; allowed | p->field = expr; allowed |
| | o = p->field; allowed | o = p->field; not allowed |

In the above table, move/copy semantics refer to: T is a type modified by _Owned and T is other types, respectively.

Note: The permissions for assignment operations in the above table also apply to function parameter passing and return scenarios.

### Calling Member Functions

When calling member functions through a borrow, i.e., the `p->method()` scenario, the rules between actual arguments and formal parameters are as follows:

| | void method(const This * _Borrow this) | void method(This * _Borrow this) | |
| --- | ---- | ---- | --- |
| p is immut borrow | allowed | not allowed, immut borrow cannot create mut borrow | |
| p is mut borrow | allowed, creating immut borrow from mut borrow is allowed | allowed | |

For example:

```C
void int ::method1(const This *_Borrow this) {}
void int ::method2(This *_Borrow this) {}

void test() {
  int local = 5;
  const int *_Borrow p1 = &_Const local;
  int *_Borrow p2 = &_Mut local;
  p1->method1(); // ok: formal parameter type and actual parameter type are consistent, both are immutable borrows
  p1->method2(); // error: formal parameter is mutable borrow type, actual parameter is immutable borrow type, immutable borrow cannot create mutable borrow
  p2->method1(); // ok: formal parameter is immutable borrow type, actual parameter is mutable borrow type, creating immutable borrow from mutable borrow is allowed
  p2->method2(); // ok: formal parameter type and actual parameter type are consistent, both are mutable borrows
}

int main() {
  test();
  return 0;
}
```

## Type Conversions for Borrows

1. For any type T, if T implements _Trait TR, then a borrow pointing to type T is allowed to be upcast to a borrow pointing to type TR; conversely, conversion from a borrow of type TR to a borrow of type T is not allowed.

```C
#include <stdio.h>

_Trait TR { void print(This * _Borrow this); };
void int ::print(int *this) { printf("%d\n", *this); }

_Impl _Trait TR for int;

void test() {
  int x = 10;
  int *_Borrow r = &_Mut x;
  _Trait TR *_Borrow p = r; // ok: Supports upcasting from int* borrow type to _Trait TR* borrow type
  p->print();
  int *_Borrow px = (int *_Borrow)p; // error: Downcasting from _Trait TR* is prohibited
}

int main() {
  test();
  return 0;
}
```

2. When pointing to different types, implicit conversion from a borrow pointing to T to a borrow pointing to void type is allowed; conversely, conversion from a borrow of type void to a borrow of type T must be done explicitly. Type conversions between borrow pointers pointing to different types are not allowed in other cases.

```C
void test() {
  int x = 10;
  int *_Borrow r = &_Mut x;
  void *_Borrow p = r;
  int *_Borrow t1 = p; // error: Implicit conversion to void *_Borrow type is not allowed
  int *_Borrow t2 = (int *_Borrow)p; // ok: Explicit cast to void *_Borrow type is allowed
}

int main() {
  test();
  return 0;
}
```

3. Conversion between `T * _Borrow` and `T *` is only allowed in unsafe zones.

```C
int main() {
  int *_Borrow p = (int *_Borrow)NULL; // ok: Unsafe zone allows conversion between T * _Borrow and T *
  int *q = p; // error: Type conversion must be explicit, implicit type conversion is prohibited
  _Safe { int *_Borrow p = (int *_Borrow)NULL; } // error: Safe zone prohibits conversion between T * _Borrow and T *
  return 0;
}
```

4. C-style casts between `T *_Owned` and `T *_Borrow` pointers are not allowed

```C
int *_Owned test(int *_Owned p) {
  int *_Borrow q = (int *_Borrow) p; // error: Should use &_Mut *p instead of cast
  int *_Owned r = (int *_Owned) q; // error: Cannot create a T *_Owned copy from T *_Borrow through cast
  return r;
}
```

5. Mutable borrow `T *_Borrow` type can be implicitly converted to immutable borrow `const T *_Borrow` type, with the compiler automatically inserting the `&_Const *` operator. Explicit casts between mutable borrows and read-only borrows are not allowed.

   The following scenarios can have implicit conversion from mutable borrow to immutable borrow:
   1. Variable initialization and assignment
   2. Function call expression parameter passing
   3. Function return statements

```C
void foo(const int *_Borrow);

void test1() {
  int a = 1;
  int *_Borrow p = &_Mut a;
  const int *_Borrow q = p; // ok
  q = p; // ok
  foo(p); // ok
}

const int *_Borrow test2(int *_Borrow p) {
  return p; // ok
}
```

```C
#include <stdio.h>

int main() {
  int local = 10;
  int *_Borrow p = &_Mut local;
  const int *_Borrow b = (const int *_Borrow)p; // error: Casting mutable borrow to read-only borrow is not allowed
  printf("%d\n", *b);  // Read b
  *p = 1;              // Modify p (if conversion were allowed, this would violate borrow rules)
  printf("%d\n", *b);  // Read b again

  const int *_Borrow c = &_Const local;
  int *_Borrow m = (int *_Borrow)c; // error: Casting read-only borrow to mutable borrow is not allowed
  *m = 20;  // If conversion were allowed, this would violate const safety

  return 0;
}
```

For `_ArrayElem` borrows, implicit conversion from `T *_Borrow _ArrayElem` to `T *_Borrow` is also allowed, which is equivalent to "re-borrowing at the current position to obtain an ordinary borrow pointer"; the reverse cast from `T *_Borrow -> T *_Borrow _ArrayElem` is not allowed.

```C
void test(int arr[4], int local) {
  int *_Borrow _ArrayElem p = &_Mut arr[0];
  int *_Borrow q = p; // ok, equivalent to &_Mut *p

  int *_Borrow plain = &_Mut local;
  int *_Borrow _ArrayElem bad = (int *_Borrow _ArrayElem)plain; // error: Reverse conversion is not allowed
}
```

6. Implicit conversion of `_Borrow` pointers to `_Bool` is allowed in variable initialization, variable assignment, function parameter passing, and return

```c
void foo(int *_Borrow _Nullable p) {
  _Bool flag = p; // equivalent: _Bool flag = p != nullptr;
}
void bar(int *_Borrow _Nullable p, _Bool flag) {
  flag = p; // equivalent: flag = p != nullptr;
}
void use(_Bool);
void baz(int *_Borrow _Nullable p) {
  use(p); // equivalent: use(p != nullptr);
}
_Bool foobar (int *_Borrow _Nullable p) {
  return p; // equivalent: return p != nullptr;
}
```

## Other Rules for Borrowing

Besides the rules above, we have the following rules for borrowing:

1. For global variables, we cannot track in function signatures which function reads the global variable and which function modifies the global variable. To ensure safety, BiSheng C stipulates: in safe zones, only read-only borrows can be taken of global variables; mutable borrows are not allowed. If borrowing a function name, from a lifetime perspective, it can be treated as borrowing a global variable.

2. Borrow variables must be initialized before use.

```C
void test() {
  int *_Borrow p; 
  use(p); // error: Must be initialized
}

int main() {
  test();
  return 0;
}
```

3. Initializing or reassigning a borrow-type lvalue with a borrow-type expression, i.e., `p = e`, requires `p` and `e` to be borrow types of the same type, and `e`'s lifetime must be greater than p's lifetime.

```C
#include <stdio.h>

void test() {
  int x = 1;
  int *_Borrow p = &_Mut x;
  {
    int y = 2;
    int *_Borrow pp = &_Mut y;
    p = pp; // error: pp's lifetime is less than p's
    printf("%d\n", *p);
  }
  printf("%d\n", *p);
}

int main() {
  test();
  return 0;
}
```

Based on this rule, a `_Borrow` pointer member inside a `struct` cannot borrow from that `struct` or its other members.

```C
struct S {
  int m;
  const int *_Borrow p;
};

void test() {
  struct S s = {.m = 0, .p = &_Const s.m}; // error: Because s.p's lifetime is the same as s.m's lifetime
}

int main() {
  test();
  return 0;
}
```

4. Borrow variables cannot be global variables; they can only be local variables.

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation

int g = 5
int *_Borrow p = &_Mut g; // error: Borrow variables cannot be global variables
void test() { int *_Borrow p = &_Mut g; }

int main() {
  test();
  return 0;
}
```

5. Taking a borrow of an expression that contains a borrow is not allowed. Similarly, in borrow type `T* _Borrow`, `T` itself and its members cannot be borrow types.

```C
#include "bishengc_safety.hbs" // Header file provided by BiSheng C for safe memory allocation and deallocation

struct R {
  int *_Borrow p;
};

void test() {
  int local = 5;
  int *_Borrow *_Borrow p = &_Mut(&_Mut local); // error: Multi-level borrow pointers are not allowed

  struct R r1 = {.p = &_Mut local};
  struct R *_Borrow r2 = &_Mut r1; // error: r1 already contains a borrow
}

int main() {
  test();
  return 0;
}
```

6. Implementing _Trait for borrow types is not allowed.

```C
_Trait TR{};

_Impl _Trait TR for int *_Borrow; // error: Implementing _Trait for borrow types is not allowed

int main() { return 0; }
```

7. Adding member functions to borrow types is not allowed.

```C
void int *_Borrow::f() {} // error: Adding member functions to borrow types is not allowed

int main() { return 0; }
```

8. Union members cannot be borrow types.

```C
union U {
  int *_Borrow p; // error: Borrow pointers cannot be union members
};

int main() { return 0; }
```

9. Borrow pointer types cannot be generic arguments.

10. Ordinary borrow pointer variables do not support subscript operations; `_Borrow _ArrayElem` pointers support subscript operations.

11. Ordinary borrow pointer variables do not support arithmetic operations; `_Borrow _ArrayElem` pointers support `+`, `-`, `+=`, `-=`, `++`, `--` operations.

12. Comparison operators such as `==`, `!=`, `>`, `<`, `<=`, `>=` are allowed between borrow variables of the same type.

13. `sizeof` and `alignof` operators are allowed on borrow types, and:
    `sizeof(T* _Borrow) == sizeof(T*)`
    `_Alignof(T* _Borrow) == _Alignof(T*)`
    `sizeof(T* _Borrow _ArrayElem) == sizeof(T*)`
    `_Alignof(T* _Borrow _ArrayElem) == _Alignof(T*)`

14. Unary `&`, `!` and binary `&&`, `||` operators are allowed on borrow types.

15. Unary `-`, `~`, `&_Const`, `&_Mut`, `[]`, `++`, `--` operators are not allowed on ordinary borrow types, and binary `*`, `/`, `%`, `&`, `|`, `<<`, `>>`, `+`, `-` operators are not allowed on ordinary borrow types either. For `_Borrow _ArrayElem` borrows, `[]`, `+`, `-`, `+=`, `-=`, `++`, `--` are allowed, while other restrictions remain unchanged.

```C
_Safe int foo(void) {
  int arr[4] = {1, 2, 3, 4};
  int *_Borrow _ArrayElem p = &_Mut arr[0];
  p = p + 1; // ok: _Borrow _ArrayElem supports +
  p += 1; // ok: _Borrow _ArrayElem supports +=
  ++p; // ok: _Borrow _ArrayElem supports ++
  int x = p[0]; // ok: _Borrow _ArrayElem supports []

  int *_Borrow q = p; // ok: Downgrade to ordinary borrow
  // q = q + 1; // error: Ordinary borrow does not support +
  // q += 1; // error: Ordinary borrow does not support +=
  // ++q; // error: Ordinary borrow does not support ++
  // int y = q[0]; // error: Ordinary borrow does not support []
  return x;
}
```

16. If a borrow pointer variable points to a function, then the function can be called through that borrow pointer variable.

```C
#include <stdio.h>

void f() { printf("f()\n"); }

void test() {
  void (*_Borrow const p)() = &_Const f; // ok: Taking an immutable borrow of a function
  p();
}

int main() {
  test();
  return 0;
}
```

17. Mutable borrows of functions are not allowed; only read-only borrows are allowed.

18. _Borrow pointers are allowed as conditions for `if`, `while`, `do-while`, `for` statements and ternary expressions, but not as conditions for `switch` statements

```c
void foo(int *_Borrow _Nullable p) {
  if (p) { // equivalent: p != nullptr
  }
  while (p) { // equivalent: p != nullptr
  }
  do {
  } while (p); // equivalent: p != nullptr

  for (;p;) { // equivalent: p != nullptr
  }
  switch (p) { // error
  default: 
    break;
  }
  int x = p ? 2 : 1; // equivalent: p != nullptr ? 2 : 1;
}
```
