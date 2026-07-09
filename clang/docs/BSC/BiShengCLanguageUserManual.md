<div>
    <div style='height:1000px;width:90%'>
        <div style='border-top:1px solid'> </div>
        <div style='margin-top:10%;font-size:28px;width:100%;heigth:80%;font-width:600;letter-spacing:2px;text-align:center'>
            <span style=''>毕昇 C 用户手册</span>
        </div>
    </div>
</div>
<div style=“package-break-after:always;“></div>

## 简介

在系统编程领域，C/C++ 是应用最广泛的编程语言。在硬件资源十分受限的嵌入式场景下，C 语言使用的最多，但使用 C 语言编码存在很多痛点问题，比如 C 语言中指针使用带来的内存安全问题，C 语言缺乏原生的并发支持，以及一些基础的编程抽象(如泛型等)。近年来，在系统编程语言领域有不少探索的工作，比如 Rust，主打内存安全(所有权，生命周期，borrow checker 等)和并行并发(无栈协程)。Rust 是一门全新的编程语言，采用了和 C/C++ 完全不同的语言设计，学习曲线陡峭，也无法解决存量代码开发的问题。

在这个背景下，毕昇 C 采用了不同的策略，它基于 C 语言做了很多增强的设计，比如更强的内存安全特性，语言层面支持并发等，且可以在存量代码中渐进式的使用这些特性而不用完全重写已有代码。可以认为，毕昇 C 是 C 语言的一个超集。这本用户手册，将从以下三个方面介绍毕昇 C ：
- 基础编程抽象：成员函数，_Trait，泛型
- 内存安全：所有权，借用
- 并发：无栈协程

## 1. 入门指南

### 1.1. 构建与安装

在使用毕昇C语言开发程序之前，需要先构建并安装毕昇C语言的编译器。毕昇C语言的编译器基于LLVM项目，因此其构建与安装过程与LLVM相同。

首先，克隆毕昇C编译器项目仓库：

```shell
$ git clone https://gitee.com/bisheng_c_language_dep/llvm-project.git
$ cd llvm-project
```

然后，使用`cmake`和`ninja`构建毕昇C编译器：

```shell
$ mkdir build && cd build
$ cmake -G "Ninja" -DLLVM_ENABLE_PROJECTS="clang" -DCMAKE_BUILD_TYPE=Release -DLLVM_USE_LINKER=lld -DBUILD_SHARED_LIBS=OFF -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_INSTALL_PREFIX=<install_dir> ../llvm
$ ninja
```

最后，使用`ninja install`安装上一步构建的毕昇C编译器，安装目录为 `<install_dir>`：

```shell
$ ninja install
```

可选构建: 毕昇C编译器项目中还提供了毕昇C标准库`libcbs`，用于支持毕昇C语言的开发。

```shell
$ cd llvm-project
$ mkdir build_libcbs && cd build_libcbs
$ cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=<install_dir>/bin/clang -DCMAKE_INSTALL_PREFIX=<install_dir> ../libcbs
$ ninja stdcbs
$ ninja install
```

至此，毕昇C编译器的构建与安装就完成了。可以将 `<install_dir>/bin` 添加到环境变量 `PATH` 中，方便后续使用。

### 1.2. 第一个毕昇C程序

如下示例中，我们创建一个名为 bsc_project 的目录，创建 demo.cbs 源程序文件，并键入以下内容。

注：毕昇 C 的源码文件以 .cbs 为后缀。

```shell
$ mkdir -p bsc_project && cd bsc_project
$ touch demo.cbs
```

向demo.cbs写入如下内容：

```c
#include <stdio.h>

struct Foo {
    int a;
};

int struct Foo::getA(struct Foo* this) {
    return this->a;
}

int main() {
    struct Foo foo = {.a = 1};
    printf("foo.getA() = %d\n", foo.getA());// expected result: 3
    return 0;
}
```

使用编译命令编译该文件，得到可执行文件，如下：

```shell
$ clang demo.cbs -o demo
$ ./demo
foo.getA() = 1
```

输出如上结果，说明你已经成功应用了毕昇 C 的成员函数特性。

## 2. 开发效率

### 2.1. 成员函数

#### 2.1.1. 概述

在 C 语言里，如果我们想表达某个类型的数据(data)有对应的某个方法(operation)，一般使用全局函数，让这个数据类型作为入参，如下：
```c
#include<stdio.h>
struct Data{
  int x;
};

void print_data(struct Data* data) {
    // 提供 print data 的实现逻辑
    printf("print data\n");
}

int main() {
    struct Data data = {.x = 1};
    print_data(&data);// expected result: print data
    return 0;
}
```

类似的，针对 int 类型我们可能需要 `print_int` 的函数，针对 float 类型需要 `print_float` 的函数，这不是一件令人愉快的事情，我们希望有一种更简洁的方式去表达类型与方法关联这件事，这就是为C语言引入成员函数的部分动机。引入成员函数后，上面例子的代码可以这么写：
```c
#include<stdio.h>
struct Data{
  int x;
};

void struct Data::print(struct Data* this) {
    // 提供 print data 的实现逻辑
    printf("print data\n");
}

void int::print(int* this) {
    // 提供 print int 的实现逻辑
    printf("print int\n");
}

void float::print(float* this) {
    // 提供 print float 的实现逻辑
    printf("print float\n");
}

int main() {
    struct Data data = {.x = 1};
    int a = 1;
    float b = 1.0;
    data.print();// expected result: print data
    a.print();// expected result: print int
    b.print();// expected result: print float
}

```

如果，我们想表达某些类型具有一组相似的行为，比如上面例子中的 `print` ，我们可以定义一个 `_Trait`，然后让 `struct Data` `int` `float` 等类型实现这个 `_Trait` 。成员函数和 `_Trait` 相结合，非常有表达力。关于 `_Trait` 的介绍，参考后续章节。

下面，我们简单介绍下毕昇 C 成员函数的一些具体规则：

#### 2.1.2. 基本语法

当我们想为某个类型增加一个成员函数时，我们只需要在普通函数定义的语法基础上，在函数名如 foo 前，增加 `typename::foo`，如下所示：
```c
void foo1() {
  // do nothing
};

void int::foo2(int* this) { // 实例成员函数，第一个入参是 This 指针，指向当前int类型的实例
  // do something
}

void int::foo3(int this) { // 实例成员函数，第一个入参是 This 实例，不是 This 指针
  // do something
}

void int::foo4() { // 静态成员函数
  // do something
}

```

其中，type-name 可以是基础类型如 `int`, `float` 等，也可以是用户自定义的结构体等，符合 C 语言对类型的定义规则，此外可以使用`This` 来简化表示当前类型。下面是更多的用法示例：

```c
// case 1
void int::print(int* this); // 声明

void int::print(int* this){ // 定义
    printf("int::print");
}

// case 2
struct S1{};
// 错误使用 S，在 C 语言里 struct S 才是一个类型
void S1::print(struct S1* this); // error: must use 'struct' tag to refer to type 'S'
void struct S1::print(struct S1* this); // Ok，修正后的声明

// case 3
typedef struct {
}S2;
void S2::print(S2* this); // Ok, S2 是 typedef 后的 struct S2

// case 4
void S2::print(This this); // Ok, This 表示当前类型 struct S2
void S2::print(This* this); // Ok, This* 表示指向当前类型 struct S2 的指针

```

采用这样的语法设计有一个好处，那就是我们很方便就可以给已有类型增加成员函数而不用侵入式修改源码。

#### 2.1.3. 关于 `this`

在上面的成员函数的例子中，如果函数参数中以 `this` 开头，则该函数为实例成员函数，并且此时 `this` 必须为第一个参数，它表示该成员函数对应类型实例(This this)、或指向该实例的指针(This* this)，它是一个“实例成员函数”。如果成员函数参数列表中，没有 `this` 存在，则表示这是一个“静态成员函数”。
```c
typedef struct {/*...*/} M;
void M::f(M* this, int i) {} // 实例成员函数

typedef struct {/*...*/} N;
void N::f() {} // 静态成员函数

int main() {
    M x;
    x.f(1); // Ok
    M::f(&x, 1); // Ok
    M* x1 = &x;
    x1->f(1); // Ok

    N y;
    y.f(); // Err: y does not have instance member function, use N::f instead.
    N::f(); // Ok
    return 0;
}

```
对于实例成员函数(第一个入参为`this`)，其调用方式有两种：

(1) 和访问成员变量类似，实例类型调用用 `.` 符号，如 `x.f(1)`；指针类型调用，用 `->` 符号，如 `x1->f(1)`。

(2) 普通函数调用方式，如 `M::f(&x, 1)`。

对于静态成员函数，其调用方式和调用普通全局函数类似，区别只是函数名变成 `type-name::func-name`，如 `N::f()`。

#### 2.1.4. 其他规则

- 成员函数支持声明和定义分开，如下：
```c
// 声明
const char* int::to_string(const int* this) ;

// 定义
const char* int::to_string(const int* this){
    // to_string 的实现，略
}

```
- 新增成员函数不影响原类型的 layout 包括 size 和 alignment
- 成员函数不支持重载和重定义
- 成员函数的名字不允许与成员变量相同，适用于 struct, union, enum 等
- 成员函数允许赋值给函数指针。
- 禁止对 cv-qualified type (被类型修饰符修饰的类型，如 const int 等) 添加成员函数
- 禁止对 “函数类型” "数组类型" "指针类型" 添加成员函数
- `this`指针允许被const/volatile等修饰符修饰
- 不允许给 incomplete type 扩展成员函数，incomplete type 即为那些声明了但未完整定义的类型
- 不允许给`void`类型扩展成员函数
- 如果两个头文件中对同一个类型扩展了同名的成员函数，那么在一个编译单元中包含这两个头文件，会导致编译错误
- 当前暂时禁止通过整数字面量、浮点数字面量、复合类型字面量直接调用成员函数

### 2.2. 泛型

#### 2.2.1. 概述

泛型是一种编程技术，允许将类型（如整数、字符串等基本类型，或者用户自定义的类型）作为参数传递给函数或结构体，从而实现代码复用、提升灵活性，并在保持类型安全的前提下，提高开发效率。

毕昇 C 的泛型是一种编译时的泛型机制，它可以定义一个通用的函数或类，然后根据不同的类型参数生成不同的实例。

目前，毕昇 C 已支持**泛型函数**、**泛型结构体**和**泛型类型别名**。

##### 2.2.1.1. 实现动机

泛型的目的是为了提高代码的**效率**和**重用性**而实现的，其优点在于：

- 避免了代码冗余
- 提高了代码的可读性和可维护性
- 实现了类型安全和编译时检查
- ...

泛型编程使开发者能够编写适用于任意数据类型的通用算法，无需为不同类型（如整数、字符串或自定义类型）分别实现相同逻辑。

通过泛型，用户可以为函数或结构体定义通用模板，使得类中的某些数据成员或成员函数的参数、返回值取得任意类型。

##### 2.2.1.2. 示例

以下方的代码为例：

```c
int sum_int(int a, int b) {
    int c = a + b;
    return c;
}

float sum_float(float a, float b) {
    float c = a + b;
    return c;
}

int main() {
    int sum1 = sum_int(1, 2);
    float sum2 = sum_float(1.2, 2.5);
}
```

可以看出，对于sum方法，如果要实现返回值分别为 **float** 和 **int** 两种情况，普通的 C 语言需要为同样实现的方法定义两次。

但是如果使用毕昇 C 的泛型语法，则只需定义一次，即可在实例化时重复使用，如下：

```c
#include<stdio.h>
T sum_t<T>(T a, T b) {
    T c = a + b;
    return c;
}

int main() {
    int sum1 = sum_t<int>(1, 2);
    float sum2 = sum_t<float>(1.2, 2.5);
    printf("%d\n",sum1); // expected result: 3
    printf("%f\n",sum2); // expected result: 3.700000
    return 0;
}
```

可以看出，泛型功能的引入，对于用了相同算法的声明场景，其代码量有明显的减少。

#### 2.2.2. 语法规则

对于毕昇 C 泛型，我们设计了如下的语法规则：

- 在声明泛型函数、结构体或类型别名时，泛型参数列表要用尖括号 '<>' 包裹起来，尖括号内部的类型一般是 identifier ，如 'T1', 'T2' 之类 。


- 在实例化时，既可以在尖括号内部传入具体的类型，如 int, float, struct S 等，也可以省略尖括号的书写，编译器会根据实际传入的参数进行隐式的类型推导。此外，如果将现有类型 typedef 为其他别名后，同样可以在实例化时用作泛型实参。


- 同时，毕昇 C 的泛型函数、泛型结构体和泛型类型别名也支持使用**常量参数**作为泛型的形参。

下面我们将分为五个部分详细说明。

##### 2.2.2.1. 泛型函数

对于泛型函数，我们对语法规则的设计，主要体现在两个方面：

1. 声明时，区别于普通函数声明，我们需要在**函数名**和**函数入参**之间，添加一对尖括号 '<>' ，并在尖括号中写明泛型函数的**泛型形参**，其中形参可以为任何合法的名字（此处的合法指的是不会导致语义冲突的情况）；

   同时，对于函数返回值的类型，可以是普通的 builtin 类型，可以是用户已经定义过的结构体，也可以是泛型形参中的一个（如 T）。

2. 实例化时，类型既可以在尖括号中显式指定，也可以省略尖括号以及其中的内容，由编译器进行隐式推导：

   1. 显式指定类型时，区别于普通的函数调用，同样需要在被调用的**函数名**与**函数入参**之间添加一对尖括号 '<>' ，且中间无空格；然后在尖括号中传入**泛型实参**，此处的实参可以是 builtin 类型，也可以是用户已经定义过的结构体。
   2. 隐式推导时，写法与普通的函数调用相同，毕昇 C 编译器会根据函数调用传入的参数类型，自动推导**泛型实参**的类型。不过为了代码可读性等，推荐使显式指定类型。

下面是一些用法示例：

```c
typedef long int LT;

// 泛型函数
T max<T>(T a, T b) {						// 开头的'T'为函数返回值类型，而'<>'中的'T'泛型函数'max'的泛型形参
    T Max = a > b ? a : b;
    return Max;
}

int main() {
    int a = 3;
    int b = 5;
    int c = 4;

    // 泛型函数 实例化
    int max1 = max<int>(a, b);				// 显示指定泛型实参类型
    int max3 = max(a, c);				    // 隐式推导，编译器自动推导'T'为 int 类型

	return 0;
}
```

##### 2.2.2.2. 泛型结构体

对于泛型结构体，我们对语法规则的设计，同样体现在两个方面：

1. 声明时，区别于普通结构体声明，我们需要在**结构体名**的后面之间，添加一对尖括号 '<>' ，并在尖括号中写明泛型结构体的**泛型形参**。
2. 实例化时，泛型结构体仅支持显式指定类型，即：显示指定类型时，区别于普通的结构体的构造，同样需要在被构造的**结构体名**后面添加一对尖括号 '<>' ，且中间无空格；然后在尖括号中传入**泛型实参**，此处的实参可以是 builtin 类型，也可以是用户已经定义过的结构体。
3. 在使用泛型结构体类型时，如声明泛型结构体类型的变量、声明泛型结构体类型的成员、声明参数、声明返回值、扩展成员函数时，可以省略 struct。即在使用形如`struct S<T>`的类型时，可以简写为`S<T>`，但在声明该类型时不允许省略。

下面是一些用法示例：

```c
typedef long int LT;

// 泛型struct
struct S<T, B>{
    T a;
    B b;
};

// 泛型union
union MyUnion<T1, T2> {
    T1 u1;
    T2 u2;
};

// 返回类型为'泛型union'的泛型函数'foo_union'
union MyUnion<T1, T2> foo_union<T1, T2>(union MyUnion<T1, T2> *this) {
    return *this;
}

int main() {
    // 泛型struct 实例化
    struct S<int, LT> s1 = {.a = 42, .b = 5};		// 使用 typedef 后的类型作为泛型实参

    // 泛型union 实例化
    union MyUnion<int, float> p;
    foo_union(&p);		// 泛型函数 foo_union 的隐式类型推导

    return 0;
}
```

##### 2.2.2.3. 泛型成员函数

在支持定义泛型 struct 和泛型 union 类型的基础上，还可以为它们扩展普通成员函数。

如下是一个用法示例，该示例包含了声明泛型成员函数、定义泛型成员函数及实例化调用泛型成员函数：

```c
struct MyStruct<T> {
    T res;
};

union MyUnion<T> {
    T res;
};

T struct MyStruct<T>::foo(struct MyStruct<T>* this, T a) { // 定义泛型成员函数
    this->res = this->res + a;
    return this->res;
}

T union MyUnion<T>::foo(union MyUnion<T>* this, T a) {
    this->res = this->res + a;
    return this->res;
}

int main() {
    struct MyStruct<int> s = { .res = 1 };
    union MyUnion<int> u = { .res = 5 };
    int res1 = s.foo(2); // 调用泛型成员函数
    int res2 = u.foo(6);
    return 0;
}
```

关于泛型成员函数，还有以下几点需要注意：

1. 泛型成员函数可以是静态函数，即第一个参数可以不为`this`；
2. 不允许泛型成员函数的重载，这与成员函数的规则相同；

```
   
   struct MyStruct<T> {
       T val;
   };
   
   T static struct MyStruct<T>::static_add(T a, T b) {
       return a + b;
   }
   
   
   T struct MyStruct<T>::static_add(T a);  // expected result: error: conflicting types for 'static_add'
   
   
   int main() {
       int sum = MyStruct<int>::static_add(10, 20);   // 静态泛型成员函数
       return 0;
   }
```


3. 泛型成员函数的返回类型可以是泛型 struct 或泛型 union类型。

```
   
   struct MyStruct<T> {
       T val;
   };
    
   struct MyStruct<T> struct MyStruct<T>::make_struct(T x) {
       struct MyStruct<T> result = { .val = x };
       return result;
   }
   
   
   int main() {
       MyStruct<int> s = MyStruct<int>::make_struct(42);
       return 0;
   }
```

##### 2.2.2.4. 常量泛型

除了基础泛型的实现，毕昇 C 还引入了常量泛型的功能。具体来说，常量泛型是一种允许程序项在常量值上泛型化的特性。也就是说，常量可以作为泛型参数传递到泛型变量中，代码会根据常量参数而进行特化，从而确保无开销，并可以直接在代码中作为常量来使用。

例如，在 毕昇 C  中，可以定义一个泛型结构体，其中一个泛型参数是一个**常量泛型参数**，该参数可以用于表示结构体内定义的数组的大小。

这样，通过在实例化时传入不同的常量值，便可以生成多个不同大小的数组对象，如：

```c
struct Array<T, int N> {
    T data[N];
};

int main() {
    struct Array<int, 5> arr1;
    struct Array<int, 10> arr2;
    return 0;
}
```

如上，这里的 '10' 和 '5' 就是常量泛型参数 'int N' 的实参，它们决定了数组的大小。

目前，泛型常量的规则如下：

- 常量泛型的形参只支持“可编译时计算的类型”，目前只支持整数类型
- 常量泛型的实参只能是“编译时可计算”的常量表达式
- 语法上，如果只是 int 字面量、constexpr 常量，那么可以不需要小括号，其它常量表达式一律需要用小括号

目前，毕昇 C 对于常量泛型的实现仅限于整形的“整数常量”：

- 对于声明时，形参列表仅接受 int 及其修饰符 long、short、unsigned、signed，以及上述关键字的各种组合。同时也支持将上述关键字 typedef 成其他别名再做为形参。
- 对于实例化时，目前泛型实参列表支持 IntegerLiteral （即常量1，2之类），同时也支持 constexpr修饰的变量与常量表达式。

下面是一些用法示例：

```c
#include <stdio.h>

typedef long long int LLInt;

// 使用了泛型常量的泛型函数
int print_dataSize<T, int B>()
{
    T data[B];
    printf("the size of data is %d\n", B);
  	return B;
}

// 使用了 typedef别名 作为泛型常量的泛型函数
void print_const<LLInt B>() {
    printf("the const is %d\n", B);
}

// 使用了泛型常量的泛型struct
struct Array_1<T, int N>
{
  	T data[N];
};

// 使用了 typedef别名 作为泛型常量的泛型struct
struct Array_2<LLInt B, int C, T>
{
  	LLInt data1[B];
  	int data2[C];
  	T a;
};

int main() {
  	int a1 = print_dataSize<int, 5>();
  	print_const<20>();

  	struct Array_1<int, 5> arr1;
  	struct Array_2<5, 6, int> arr2 = {.a = 1};

    return 0;
}
```

##### 2.2.2.5. 泛型类型别名

标准 C 已有类型别名的功能，语法为：
```c
typedef OldType NewType;
```
为了使类型别名能够与泛型一起配合使用，BSC 引入与标准 C 语法不同的类型别名语法：
```c
typedef NewType = OldType ;
```
加上泛型参数就形成了泛型类型别名，例如：
```c
typedef MyPointerType<T> = T*;
```
泛型类型别名的使用可以简化类型的书写，还可以为类型赋予更具描述性的名称，使代码更易于阅读和理解，举例来说：

我们可以使用 HashMap 来记录某年级学生的各科成绩，Key 表示学生的学号，Value 表示学生的各科成绩，根据实际情况的不同，学生的学号可以用 int、string 等类型表示，成绩可以用 int、float 等类型表示，科目数量也可能发生变化，使用泛型类型别名有助于我们根据实际需求进行定制：

```C
//辅助类及成员方法
struct HashMap<K, V> {
   //省略实现
};
void struct HashMap<K, V>::insert(This* this, K key, V value) {
   //省略实现
}
struct Array<T, int N> {
    T a[N];
};
//使用泛型类型别名定义一个特殊的HashMap类型，它的key是T1类型，value是一个长度为N，元素类型为T2的数组类型：
typedef GenericGrade<T1, T2, int N> = struct HashMap<T1, struct Array<T2, N>>;
//继续使用类型别名，针对不同的年级进行定制：
typedef Grade1 = GenericGrade<int, int, 3>;  //一年级学生学号为int，成绩为int，科目数量为3
typedef Grade3 = GenericGrade<int, float, 4>;//三年级学生学号为int，成绩为float，科目数量为4
typedef Grade6 = GenericGrade<int, float, 5>;//六年级学生学号为int，成绩为float，科目数量为5

int main() {
    Grade1 grade1;
    grade1.insert(10, {80, 90, 95});
    grade1.insert(11, {80, 95, 90});
    Grade3 grade3;
    grade3.insert(12, {90.0, 95.5, 90.0, 85.0});
    grade3.insert(13, {80.0, 90.0, 95.5, 90.0});
    Grade6 grade6;
    grade6.insert(15, {80.0, 90.0, 95.5, 90.0, 85.0});
    grade6.insert(16, {80.0, 90.0, 95.5, 90.0, 85.0});
    return 0;
}
```

对于类型别名，有以下规则：
1. 不允许为泛型类型别名扩展成员函数。
```C
struct S<T> {};
typedef MyS<T> = struct S<T>;

void MyS<T>::foo(This* this) {} //error: extended type of a BSC member function cannot be a generic typealias
```
2. 不允许在结构体类型内部定义类型别名。
```C
struct S<T> {
    typedef type = T;    //error: type name does not allow storage class to be specified
    typedef Int64 = int; //error: expected ';' at end of declaration list
};
```
3. 不允许在函数体内定义泛型类型别名，但可以定义普通类型别名。
```C
void foo<T>() {
    typedef type = T;
    typedef Int64 = int;

    typedef MyType1<T> = T;   //error: no template named 'MyType1'
    typedef MyType2<T1> = T1; //error: BSC generic typealias cannot be declared within a function
}
```

下面是一些用法示例：
```C
struct S<T> {};
struct V<T1, T2> {
    T1 a;
    T2 b;
};

// 普通类型别名
typedef Int64 = long int;   //等价于 typedef long int Int64;
typedef MyS = struct S<int>;//等价于 typedef struct S<int> MyS;

// 泛型类型别名
typedef MyPointerType<T> = T*;
typedef Array_3<T> = T[3];
typedef Array_N<T, int N> = T[N]; //支持带常量泛型参数

typedef MyS_T_T<T> = struct V<T, T>;
typedef MyS_T_int<T> = struct V<T, int>;

int main() {
    Int64 a = 5;  //等价于int a = 5;
    MyS s;        //等价于struct S<int> s;
    int b = 2;
    MyPointerType<int> c = &b;  //等价于int* c = &b;
    Array_3<int> d = {1,2,3};   //等价于int d[3] = {1,2,3};
    Array_N<int, 3> e= {1,2,3}; //等价于int e[3] = {1,2,3};
    MyS_T_T<int> s2;
    MyS_T_int<int> s3;
    return 0;
}
```

###### conditional 泛型类型别名

在 bsc_conditional.hbs 这一 BSC 标准库中提供了 conditional 泛型类型别名，可以实现类型层面的“分支逻辑”：
```c
// bsc_conditional.hbs
typedef conditional<int C, T, F> = __conditional(int C, T, F);
```
当 C 非 0 时，conditional 类型别名指代类型 T ，否则指代类型 F，条件表达式必须是可以编译期求值的常量表达式：
```c
#include<bsc_conditional.hbs>  //使用conditional需要导入头文件
int main() {
    conditional<1, int, double> a = 1;   //等价于int a = 1;
    conditional<0, int, double> b = 1.0; //等价于double b = 1.0;
    return 0;
}
```
使用conditional，不仅可以简化书写，可以在编译时根据条件选择不同的类型，避免了运行时的条件分支，提高代码的效率，以下是一个关于选择函数返回类型的使用案例：

定义一个泛型函数，它的返回值类型取决于泛型参数 T ，如果 T 是指针类型，则返回值类型仍然是 T ，否则，返回 T 的指针类型。

C++ 中需要借助泛型特化和 concept 来实现：
```cpp
// T 是指针类型时，std::is_pointer_v<T> == true，匹配该版本:
template<typename T> requires std::is_pointer_v<T>
T foo() { ... }

// T 不是指针类型时，匹配该版本:
template<typename T>
T* foo() { ... }
```

在 BSC 中，我们可以借助 conditional 很方便地实现这样的功能，相比C++更加方便易用：
```c
typedef PointerType<T> = conditional<is_pointer<T>(), T, T*>;

PointerType<T> foo<T>() { ... }
```

### 2.3. 常量计算

#### 2.3.1. constexpr

##### 2.3.1.1. 概述

在 C 语言中，虽然有 `const` 关键字可以表示变量只读，但它并**不能保证值在编译期间就确定**。为了明确表示“这个值必须在编译时就能计算出来”，我们引入了 `constexpr` 关键字。它既可以用于定义**编译时常量**，也可以用于定义**在编译阶段就能执行的函数**，从而支持更强的类型检查和泛型能力。

定义“编译时计算”的类型：
bool,char(signed char, unsigned char), 整数类型（包括 int 以及被 short/signed/unsigned/long/long long 等修饰的 int 类型，不包括 enum 类型），以及这些类型的别名。

定义“常量计算”上下文，也就是 constexpr 修饰的变量和函数可以作为常量使用的场景：
- 可以用于 static_assert 中，第一个条件参数属于“常量计算”上下文
- 可以用于定义定长数组，数组长度属于“常量计算”上下文
- 可以用于初始化其它 constexpr 常量
- 可以用于常量泛型的实参

下面举例说明什么是“常量计算”上下文。
```c
void bar<int N>(){}

constexpr int foo() {
    return 5;
}

int main() {
    constexpr int a = 5;

    //可以用于常量泛型的实参
    bar<a>();
    bar<foo()>();

    //可以用于定义定长数组
    int arr1[a] = {0};
    int arr2[foo()] = {0};

    //可以用于初始化其它 constexpr 常量
    constexpr int b = a;
    constexpr int c = foo();

    //可以用于 static_assert 中
    _Static_assert(a == 5, "fail");
    _Static_assert(foo() == 5, "fail");

    return 0;
}
```

##### 2.3.1.2. 使用规则

###### constexpr 修饰变量

1. constexpr 可以修饰一个常量定义，且必须在定义时被初始化，否则要报错
```c
constexpr int a = 5;
constexpr int b; //error: constexpr variable 'b' must be initialized by a constant expression
```
2. constexpr 修饰的常量在定义之后，不可被修改
```c
constexpr int a = 5;
a = 10; //error: redefinition of 'a' with a different type: 'int' vs 'const int'
```
3. constexpr 修饰常量的类型只能是上述“编译时计算”的类型
```c
constexpr float a = 5.0;//error: BSC constexpr variable does not support type 'const float'
```
4. constexpr 修饰的常量的初始化表达式必须可以在编译时求值，否则要报错。可编译时求值的常量表达式可以是：
- 字面量
- constexpr 修饰的常量
- sizeof,_Alignof 表达式
- 以可编译时求值的常量表达式作为实参，调用 constexpr 函数
- 由以下运算符组合起来的常量表达式，也是常量表达式：+,-,*,/,%,>,<,==,!=,<=,>=,&,|,^,~,!,&&,||,<<,>>,?:

举例说明：
```c
//场景1
int a = 10;
constexpr int b = a;//error
//场景2
constexpr int a = 10;
constexpr int b = a;
//场景3
constexpr int a = sizeof(int);
constexpr int b = sizeof(int);
//场景4
constexpr int foo(int a) {
    return 5;
};
constexpr int a = 10;
constexpr int b = foo(a);
//场景5
constexpr int b = 1 == 1.0;
```

5. 函数指针可以使用 constexpr 修饰，函数指针可以指向 constexpr 函数
6. constexpr 可以修饰指针变量，但其只能指向全局变量或静态变量

###### constexpr 修饰函数

1. constexpr 可以修饰一个函数声明或者定义，但必须保证声明和定义中同时有 constexpr 修饰或都没有 constexpr 修饰，否则会导致编译错误
```c
constexpr int foo();
constexpr int foo() {
    return 5;
}
```
2. constexpr 修饰的函数，参数和返回类型，只能是上述“编译时计算”的类型
```c
constexpr void foo(); //error: BSC constexpr function does not support type 'void'
```
3. constexpr 可以修饰泛型函数
```c
constexpr int foo<T>();
```
4. constexpr 函数体内的所有语句，都是编译期可求值的
- constexpr 函数体内不允许定义 static 变量
- constexpr 函数体内不允许调用非 constexpr 函数
- constexpr 函数体内不允许访问外部的非 constexpr 变量
- constexpr 函数体内不允许内嵌汇编
- constexpr 函数体内允许定义不使用 constexpr 修饰的局部变量，这些变量也只能是“编译时计算”的类型

5. 在非“常量计算”的上下文中，constexpr 修饰的函数可以当作普通函数使用，实参不需要是常量，返回值也不需要是常量。在“常量计算”的上下文中，实参和返回值都要求是常量表达式，否则会报错
```c
constexpr int foo(int a) {
    return 5;
};

int a = 10;
constexpr int b = foo(a);// error: constexpr variable 'b' must be initialized by a constant expression
int c = foo(a);//ok，foo 函数处于非“常量计算“上下文中
int main(){
    int a = 10;
    constexpr int b = foo(a);// error: constexpr variable 'b' must be initialized by a constant expression
    int c = foo(a);//ok，foo 函数处于非“常量计算“上下文中
    return 0;
}
```
6. constexpr 可以修饰成员函数，包括普通成员函数和静态成员函数
```c
//普通成员函数，参数 This* this 不属于编译时计算类型
constexpr int int::foo1(This* this) { //error
    return 5;
}

//静态成员函数
constexpr int int::foo2() {
    return 5;
}

int main() {
    constexpr int c = int::foo2();//ok，可编译期求值
    return 0;
}
```
6. constexpr 不允许修饰 _Async 函数
7. constexpr 不允许支持变长参数
8. 函数的形参不能用 constexpr 修饰
```c
int foo1(constexpr int a) { //error
    return 5;
}

constexpr int foo2(constexpr int a) { //error
    return 5;
}
```

#### 2.3.2. type trait

type trait 可以看作是一个编译期计算返回值的 constexpr 函数。
BSC标准库中提供了一系列 type trait 泛型函数，使用时需要导入头文件 bsc_type_traits.hbs

目前实现的 type trait 函数有：
```c
// 判断类型的分类
constexpr bool is_integral<T>();
constexpr bool is_floating_point<T>();
constexpr bool is_pointer<T>();
constexpr bool is_function<T>();
constexpr bool is_array<T>();
constexpr bool is_struct<T>();
constexpr bool is_union<T>();
constexpr bool is_enum<T>();
constexpr bool is_void<T>();
//判断类型的属性
constexpr bool is_signed<T>();
constexpr bool is_unsigned<T>();
constexpr bool is_const<T>();
constexpr bool is_volatile<T>();
constexpr bool is_move_semantic<T>();
constexpr bool is_owned_pointer<T>();
constexpr bool is_owned_struct<T>();
constexpr bool is_borrow<T>();
constexpr bool is_immut_borrow<T>();
constexpr bool is_mut_borrow<T>();
constexpr bool is_trivial_data<T>();
constexpr size_t rank<T>();
constexpr size_t extent<T, size_t N>();
//判断类型的关系
constexpr bool is_same<T1, T2>();
constexpr bool is_convertible<From, To>();
```
针对其中一些作出说明和举例：
```c
constexpr bool is_move_semantic<T>(); //判断类型 T 是否是 move 语义的类型
// 我们认为具有 move 语义的类型包括：
//   1. owned指针类型
//   2. _Owned struct类型
//   3. 含有 move 语义字段的结构体类型
is_move_semantic<int *_Owned>() == true;
_Owned struct S {};
is_move_semantic<S>() == true;
struct S1 { int a; };
struct S2 { int *_Owned p; };
is_move_semantic<struct S1>() == false;
is_move_semantic<struct S2>() == true;
// 不完整的结构体的 move 语义无意义，报错
struct I;
is_move_semantic<struct I>(); // error: incomplete type 'struct I' used in type trait expression

constexpr bool is_trivial_data<T>(); // 判断类型 T 是否是平凡数据类型
// 我们认为平凡数据类型是不包含以下内容的类型：
//   1. 裸指针、_Owned 指针、_Borrow 指针
//   2. _Owned 结构体
is_trivial_data<int *_Owned>() == false;
_Owned struct S {};
is_trivial_data<S>() == false;
struct S1 { int a; };
struct S2 { int *_Owned p; };
struct S3 { int *_Borrow p; };
struct S4 { int * p; };
is_trivial_data<struct S1>() == true;
is_trivial_data<struct S2>() == false;
is_trivial_data<struct S3>() == false;
is_trivial_data<struct S4>() == false;
// 不完整的结构体的是否是平凡数据无意义，报错
struct I;
is_trivial_data<struct I>(); // error: incomplete type 'struct I' used in type trait expression

constexpr size_t rank<T>(); //可用于计算数组的维数，如
rank<int>() == 0;
rank<int[5]>() == 1;
rank<int[5][5]>() == 2;

constexpr size_t extent<T, size_t N>();//可用于计算数组第 N 维元素的个数，如
extent<int[3],0>() == 3;
extent<int[3],1>() == 0;
extent<int[3][4],0>() == 3;
extent<int[3][4],1>() == 4;
extent<int[3][4],2>() == 0;

constexpr bool is_same<T1, T2>();//判断类型 T1，T2 是否一样，忽略类型别名

constexpr bool is_convertible<From, To>();//判断类型 From 是否可以隐式转换为类型 To，如
is_convertible<int, float>() == true;
is_convertible<int, const int>() == true;
is_convertible<int, volatile int>() == true;
is_convertible<int, signed>() == true;
is_convertible<int, void>() == false;
is_convertible<int, int*>() == false;
is_convertible<int, void*>() == false;
is_convertible<struct S, struct G>() == false;
```

使用时就像普通泛型函数一样
```c
#include<stdio.h>
#include<bsc_type_traits.hbs>

int main() {
    printf("%d\n",is_integral<int>()); //expected result: 1
    printf("%d\n",is_integral<float>()); //expected result: 0
    return 0;
}
```

type trait 函数可以在泛型函数和泛型结构体的成员函数中使用
```c
#include<stdio.h>
#include<bsc_type_traits.hbs>

struct S<T> {};
void struct S<T>::foo(struct S<T>* this) {
    if (is_integral<T>()) {
        printf("integral\n");
    } else {
        printf("not integral\n");
    }
}

void bar<T>() {
    if (is_integral<T>()) {
        printf("integral\n");
    } else {
        printf("not integral\n");
    }
}

int main() {
    struct S<int> s1;
    struct S<float> s2;
    s1.foo(); //print "integral"
    s2.foo(); //print "not integral"
    bar<int>();  //print "integral"
    bar<float>(); //print "not integral"
    return 0;
}
```

type trait 函数也可用于静态断言中
```c
#include<bsc_type_traits.hbs>

int main() {
    _Static_assert(is_integral<int>() == true, "fail");
    _Static_assert(is_integral<float>() == false, "fail");
    return 0;
}
```

#### 2.3.3. constexpr if

##### 2.3.3.1. 概述

以 if constexpr 开头的语句被称为 constexpr if 语句，允许用户使用 constexpr 表达式作为 if 语句的条件，保证条件是编译时计算的常量，使得编译器在编译时就能够做分支判断，并对 false 分支进行死代码消除，减少运行时开销。

constexpr if 语句与普通 if 语句的区别在于，cond 条件表达式之前使用 constexpr 关键字进行修饰，即：
```c
if constexpr (<cond>) {
    <then-statement>
} else {
    <else-statement>
}
```

我们以计算阶乘为例来认识 constexpr if，先写出一个不使用 constexpr if 的版本来计算阶乘：
```c
int factorial(int n) {
    return (n == 1) ? 1 : n * factorial(n - 1);
}

int main() {
    printf("%d", factorial(5));
    return 0;
}
```
由于 BSC 不支持泛型特化的功能，故想要实现阶乘只能使用普通函数，在运行期进行求值。使用 constexpr if，我们可以实现类似于泛型特化的功能，实现编译期计算阶乘结果。
```c
constexpr int factorial<int N>() {
    if constexpr (N == 1) {
        return 1;
    } else {
        return N * factorial<N - 1>();
    }
}

int main() {
    _Static_assert(factorial<5>() == 120, "fail");
    return 0;
}
```

##### 2.3.3.2. 用法

constexpr if 语句中的 cond 表达式在使用时需要满足以下约束：
1. cond 表达式必须是可编译时求值的常量表达式；
2. cond 表达式的类型必须是可以隐式转换为 bool 的类型，包括 bool、整型和 char。
例如：
```c
int foo1() {
    return 5;
}

constexpr int foo2(int a) {
    return a;
}

int main() {
    int a = 1;
    if constexpr(5.0) { //error: BSC constexpr if condition expression does not support type 'double'
        a = 6;
    }
    if constexpr(a) {  //error: constexpr if condition is not a constant expression
        a = 6;
    }
    if constexpr(foo1()) {  //error: constexpr if condition is not a constant expression
        a = 6;
    }
    if constexpr(foo2(a)) {  //error: constexpr if condition is not a constant expression
        a = 6;
    }
    return 0;
}
```

constexpr if 语句中，因为条件表达式是编译时计算的常量表达式，求值为 false 的分支会被定义为 “discarded statement”，会在编译期作为死代码被消除。
例如：
```c
if constexpr (<cond>) {  //如果<cond>的值为true
    <true-statement>
} else {
    <false-statement>  //那么else分支内的语句成为discarded statement，会在编译期作为死代码被消除
}
```

对于 discarded statement，有如下规则：
1. 在非泛型上下文内，discarded statement 仍然需要做完整的语法语义检查。
2. 在泛型上下文内，discarded statement 不会被实例化，也就不会进行实例化之后的语义检查。
例如：
```c
#include <stdio.h>
#include <bsc_type_traits.hbs>

void foo<T>(T a) {
    if constexpr (is_pointer<T>()) {
    //如果T不是指针，实例化的时候，下面这个block会被当作死代码，不会被实例化，也就不会进行实例化之后的语义检查
        printf("T is pointer\n");
        void* p = (void*) a;
    } else {
        printf("T is a generic case\n");
    }
}

int main() {
    int b = 5;
    foo<int>(5);
    foo<int*>(&b);
    return 0;
}
```

### 2.4. _Trait

#### 2.4.1. 概述

_Trait 是一种定义行为的方式，它类似于其它语言中的接口或抽象类，目的是定义一个实现某些目的所必须的行为集合。_Trait 定义了一组方法签名，这些方法可以被多个结构体、枚举体或内置类型共享。主要作用是为了实现代码的复用和抽象，从而提高代码的可维护性和可扩展性。

#### 2.4.2. 语法规则

毕昇 C 引入关键字 `_Trait` 与 `_Impl`，通过关键字 `_Trait` 来定义，通过 `_Impl` 可以为一个类型实现一个或多个 _Trait。下面通过一些代码示例来了解 _Trait 的使用方法。

##### 2.4.2.1. _Trait 的定义

**语法：**

```c
_Trait TraitName {
  // 定义 _Trait 中的方法签名
};
```

其中，`TraitName` 是 _Trait 的名称，后面跟着一对花括号，里面可以定义一些方法的签名。_Trait 中定义的方法不支持默认实现，必须由实现该 _Trait 的类型提供具体实现。下面我们来看一个简单的例子：

```c
_Trait T {
    void doSomeThing1(This* this);
    void doSomeThing2(This* this);
};
```

**规则：**

1. _Trait 定义只能出现在 top-level

```c
void test() {
    _Trait T {}; //不能在函数体中定义 _Trait. error: _Trait is only allowed to be defined at top-level
}

struct MyStruct {
    _Trait T {}; //不能在结构体中定义 _Trait. error: _Trait is only allowed to be defined at top-level
};
```

2. _Trait 内要求函数首个入参且只有首个入参类型为 `This` 指针，命名为 `this`; `This` 指代实现了 _Trait 的具体类型

```c
_Trait T {
    void doSomeThing1(This* this); // ok
    void doSomeThing2(This* a); // error: the first parameter of _Trait member function must be 'This* this'
    void doSomeThing3(int a, This* this); // error: the first parameter of _Trait member function must be 'This* this'
};
```

3. _Trait 内只允许声明函数

```c
_Trait T {
    void doSomeThing1(This* this) { // error: expected member name or ';' after declaration specifiers
    }
};
```

4. _Trait 内可以没有方法签名

```c
_Trait T {}; // ok
```

5. 不允许给 _Trait 类型扩展成员函数

```c
void _Trait T::getArea(_Trait T* this) { // error: cannot combine with previous 'void' declaration specifier
	...
}
```

##### 2.4.2.2. 实现 _Trait

**语法：**

```c
_Impl <_Trait> for <type>;
```

我们可以通过 `_Impl` 关键字来为一个类型实现一个 _Trait。直观的，我们来看一个简单的例子：

```c
_Trait T {
    void f(This* this);
};

struct S {};
void struct S::f(struct S* this);
void int::f(int* this);

_Impl _Trait T for int;
_Impl _Trait T for struct S;
```

**规则：**

1. 在 _Impl 语句之前，一定存在 `<_Trait>` 和 `<type>` 的定义
2. 在 _Impl 语句之前，`<type>` 必须已经声明并实现 `<_Trait>` 中所有成员函数

```c
_Trait T {
    int f1(This* this);
    int f2(This* this);
};

struct S{};

int struct S::f1(struct S* this);
//_Impl _Trait T for struct S; // error: function 'f1' in '_Trait T' is not implemented for 'struct S
int struct S::f2(struct S* this);
//_Impl _Trait T for struct S; // error: function 'f1' in '_Trait T' is not implemented for 'struct S'
int struct S::f2(struct S* this){
    return 1;
};
int struct S::f1(struct S* this){
    return 1;
};

_Impl _Trait T for struct S; // ok，struct S 已扩展声明了 _Trait 中所有成员函数
```

3. `<type>` 类型不允许是 _Trait

```c
_Impl _Trait T for struct S; // ok，支持通过 _Impl 对已有 struct/union/enum 或内置类型实现 _Trait
_Impl _Trait T for _Trait T; // error: function 'f1' in '_Trait T' is not implemented for '_Trait T'
```

4. 支持对 `typedef` 类型实现 _Trait

```c
typedef struct S S1;
_Impl _Trait T for S1;
```

##### 2.4.2.3. _Trait 类型的变量

我们可以定义 _Trait 指针类型的变量，并可以通过该指针变量进行函数调用等，具体使用方法如下：

```c
#include <stdio.h>

struct S {
    float num;
};

typedef _Trait Print {
    void print(This* this);
}P;
void struct S::print(struct S* this) {
    printf("This is a struct instance, valued %f\n", this->num);
}
void int::print(int* this) {
    printf("This is an int instance, valued %d\n", *this);
}

_Impl P for struct S;
_Impl P for int;

void test() {
    struct S s = { 0.0 };
    int a = 1;
    float b = 1.0;

    _Trait Print* p;
    p = &s; // ok，隐式转换
    p->print(); // expected result: This is a struct instance, valued 0.000000
    p = &a;
    p->print(); // expected result: This is an int instance, valued 1
    p = &b; // error: expected a pointer type which has implemented '_Trait Print', found 'float'
}
```

**规则：**

1. 支持对 _Trait 进行 `typedef`

```c
typedef _Trait Print {
    ...
}P;
```

2. 只允许声明 _Trait 指针类型的变量

```c
_Trait Print* p1; // ok
_Trait Print p2; // error: only _Trait pointer type is allowed to be declared
```

3. 如果 `<type>` 实现了 `<_Trait>`，那么指向这个 `<type>` 的指针可以被转换为 `<_Trait>` 类型的变量

```c
_Impl _Trait Print for struct S;

struct S s;
_Trait Print* t1 = &s; // 隐式转换
_Trait Print* t2 = (_Trait Print*)&s; // 显式转换
```

4. _Trait 指针类型的变量可以通过 `->` 方式调用该 _Trait 中的成员方法

```c
struct S s;
_Trait Print* t = &s;
t->print();
```

5. _Trait 指针类型的变量，允许用 `NULL` 赋值

```c
_Trait Print* p = NULL;
```

6. 允许指向 _Trait 的多级指针，但这种类型不能直接调用成员函数。不允许结构体的二级指针直接隐式转换成 _Trait 的二级指针

```c
_Trait Print* p;
p->print();
_Trait Print** q = &p; // ok
q->print(); // error: use of address-of-label extension outside of a function body
(*q)->print(); // ok
struct S s;
q = &&s; // error: tentative definition has type 'struct S' that is never completed
```

7. _Trait 指针类型的变量，不可以解引用

```c
_Trait Print *p;
int main(){
    *p; // error: use of undeclared identifier 'p'
}
```

8. _Trait 指针类型变量可以用 `const` / `volatile` 修饰

```c
const _Trait Print* p1;
volatile _Trait Print* p2;
```

9. 支持 _Trait 指针类型作为函数参数及返回值类型

```c
_Trait Print* get(_Trait Print* t) {
    return t;
}
```

10. 支持 _Trait 指针变量和 `NULL` 做比较（这里的比较仅包含 `==` 和`!=`，下同）

```c
_Trait Print* p = NULL;
if (p == NULL) {} // ok
if (p != NULL) {} // ok
```

11. 支持 _Trait 指针变量和非 _Trait 指针变量比较

```c
struct S {
    float num;
};
typedef _Trait Print {
    void print(This* this);
}P;
void struct S::print(struct S* this) {
    printf("This is a struct instance, valued %f\n", this->num);
}
// struct S 类型实现 _Trait F<int>，但 int 类型没有
_Impl _Trait Print for struct S;
int main(){
    int a = 1;
    int *p1 = &a;
    struct S s;
    struct S *p2 = &s;
    _Trait Print* p;
    if (p == p1) {}; // warning: expected a pointer type which has implemented '_Trait Print', found 'int *'
    if (p == p2) {}; // ok
    if (p == a) {};  // error: expected a pointer type which has implemented '_Trait Print', found 'int'
}

```

12. 支持 _Trait 指针变量和 _Trait 指针变量比较

```c
_Trait Print* t1 = NULL;
_Trait Print* t2 = NULL;
_Trait G *g = NULL;
if (t1 == g) {} // warning: 如果 _Trait 类型不同会报 warning
if (t1 == t2) {} // ok
```

##### 2.4.2.4. 类型转换

_Trait 类型转换只能在实现了对应 _Trait 的类型之间进行，将一个类型转换为另一个类型，同时保留原有类型的特性和方法。

**规则：**

1. _Trait 指针类型的变量，不允许强制转换为非指针类型

2. 支持 _Trait 指针类型强制转为非 _Trait 指针类型，但不支持隐式转换

```c
int a = 0;
_Trait T *p = &a; // 假设 int 类型实现了 _Trait T
int *q1 = p; // error: initializing 'int *' with an expression of incompatible type '_Trait T *'
int *q2 = (int*)p; // ok
struct S *q3 = (struct S*)p;
```

3.  _Trait 类型支持强制转换为 `void *`类型，但`void *` 指针无法转换为 _Trait 指针类型

```c
int a = 1;
_Trait T *p = &a; // 假设 int 类型实现了 _Trait T
void * q = (void *)p; // ok: _Trait 类型支持强制转换为 `void *`类型
_Trait T *p1 = (_Trait T *)q; // error: expected a pointer type which has implemented '_Trait T', found 'void'
_Trait T *p2 = q; // error: expected a pointer type which has implemented '_Trait T', found 'void'
```

#### 2.4.3. 泛型 _Trait 语法规则

泛型 _Trait 是指在 _Trait 中使用泛型类型参数，从而使 _Trait 的方法可以适用于不同类型，避免代码的重复编写。下面通过一些代码示例来了解泛型 _Trait 的使用方法。

##### 2.4.3.1. 泛型 _Trait 的定义

**语法：**

```c
_Trait TraitName<T1,T2,...,Tn> {
  // 定义泛型 _Trait 中的方法签名，可以使用泛型类型 T1,T2,...,Tn
};
```

与 _Trait 定义类似，`TraitName` 是泛型 _Trait 的名称，后面跟着一对尖括号，里面可以包含一个或多个泛型参数，在花括号里面可以定义一些方法的签名。同样，泛型 _Trait 中定义的方法不支持默认实现，必须由实现该泛型 _Trait 的类型提供具体实现。下面我们来看一个简单的例子：

```c
_Trait F<T1, T2> {
    T1 doSomeThing1(This* this);
    T1 doSomeThing2(This* this, T2 param);
};
```



**规则：**

1. 泛型 _Trait 定义只能出现在 top-level

```c
void test<T>() {
    _Trait F<T> {}; // error: _Trait is only allowed to be defined at top-level
}

struct MyStruct<T> {
    _Trait F<T> {}; // error: _Trait is only allowed to be defined at top-level
};
```

2. 泛型 _Trait 内要求函数首个入参且只有首个入参类型为 `This` 指针，命名为 `this`; `This` 指代实现了 _Trait 的具体类型

```c
_Trait F<T> {
    T doSomeThing1(This* this); // ok
    T doSomeThing2(This* a); // error: the first parameter of _Trait member function must be 'This* this'
    void doSomeThing3(T a, This* this); // error: the first parameter of _Trait member function must be 'This* this'
};
```

3. 泛型 _Trait 内只允许声明函数

```c
_Trait F<T> {
    void doSomeThing1(This* this) { // error: expected member name or ';' after declaration specifiers
        ...
    }
};
```

4. 泛型 _Trait 内可以没有方法签名

```c
_Trait F<T> {}; // ok
```

5. 不允许给泛型 _Trait 类型扩展成员函数

```c
void _Trait F<T>::getArea(_Trait F<T>* this) { // error: cannot combine with previous 'void' declaration specifier
	...
}

void _Trait F<int>::getArea(_Trait F<int>* this) { // error: cannot combine with previous 'void' declaration specifier
	...
}
```

##### 2.4.3.2. 实现泛型 _Trait

**语法：**

```c
_Impl <_Trait<SpecializationType>> for <type>;
```

我们可以通过 `_Impl` 关键字对已有 `struct`/`union`/`enum` 类型和内置类型实现 `_Trait<int>`/`_Trait<float>`等，需要注意的是，我们目前仅支持对实例化的 _Trait 类型进行 _Impl。直观的，我们来看一个简单的例子：

```c
_Trait F<T> {
    T f(This* this);
};

struct S {};
int struct S::f(struct S* this);
int int::f(int* this);

_Impl _Trait F<int> for int;
_Impl _Trait F<int> for struct S;
```

**规则：**

1. 在 _Impl 语句之前，一定存在泛型 `<_Trait>` 和 `<type>` 的定义
2. 仅支持 `_Impl` 实例化的 _Trait 类型

```c
_Impl _Trait F<T> for int; // error: use of undeclared identifier 'T'
```

3. 在 _Impl 语句之前，`<type>` 必须已经声明了 `<_Trait>` 中所有成员函数

```c
_Trait F<T> {
    T f1(This* this);
    T f2(This* this);
};

struct S{};

int struct S::f1(struct S* this){};
_Impl _Trait F<int> for struct S; // error: function 'f2' in '_Trait F<int>' is not implemented for 'struct S'
int struct S::f2(struct S* this){};
_Impl _Trait F<int> for struct S; // ok，struct S 已扩展声明了 _Trait 中所有成员函数
```

4. 支持通过 `_Impl` 关键字对已有 `struct`/`union`/`enum` 类型和内置类型实现 `_Trait<int>`/`_Trait<float>`，但`struct`/`union` 类型类型不能是泛型的

```c
struct S {};
struct G<T> {};
_Trait F<T> {};
_Impl _Trait F<int> for int; // ok
_Impl _Trait F<int> for struct S; // ok
_Impl _Trait F<int> for struct G<int>; // error: cannot _Impl _Trait for an instantiated type
```

5. `<type>` 类型不允许是 _Trait/泛型 _Trait

```c
_Impl _Trait F<int>  for _Trait S; // error: unexpected token for ImplTraitDecl
_Impl _Trait F<int>  for _Trait F<int>; // error: unexpected token for ImplTraitDecl
```

6. 支持对 `typedef` 类型实现泛型 _Trait

```c
typedef struct S S1;
_Impl _Trait F<int> for S1;
```

##### 2.4.3.3. 泛型 _Trait 类型的变量

我们可以定义泛型 _Trait 实例化后的指针类型变量，并可以通过该指针变量进行函数调用等，具体使用方法如下：

```c
#include <stdio.h>

struct S {};

_Trait F<T> {
    T foo(This* this);
};
int struct S::foo(struct S* this) {
    return 1;
}
int int::foo(int* this) {
    return 0;
}

_Impl _Trait F<int> for struct S;
_Impl _Trait F<int> for int;

void test() {
    struct S s;
    int a = 1;
    float b = 1.0;

    _Trait F<int>* p;
    p = &s; // ok，隐式转换
    p->foo(); // return 1
    p = &a;
    p->foo(); // return 0
    p = &b; // error: expected a pointer type which has implemented '_Trait F<int>', found 'float'
}
```

**规则：**

1. 只允许声明泛型 _Trait 实例化后的指针类型变量

```c
_Trait F<int>* p1; // ok
_Trait F<int> p2; // error: only _Trait pointer type is allowed to be declared
```

2. 如果 `<type>` 实现了 `<_Trait>`，那么指向这个 `<type>` 的指针可以被转换为 `<_Trait>` 类型的变量

```c
_Impl _Trait F<int> for struct S;

struct S s;
_Trait F<int>* t1 = &s; // 隐式转换
_Trait F<int>* t2 = (_Trait F<int>*)&s; // 显式转换
```

3. 泛型 _Trait 实例化后的指针类型变量可以通过 `->` 方式调用该泛型 _Trait 中的成员函数

```c
struct S s;
_Trait F<int>* t = &s;
t->foo();
```

4. 泛型 _Trait 实例化后的指针类型变量，允许用 `NULL` 赋值

```c
_Trait F<int>* p = NULL;
```

5. 泛型 _Trait 实例化后的指针类型变量，不可以解引用

```c
_Trait F<int> *p;
*p; // error: use of undeclared identifier 'p'
```

6. 允许指向泛型 _Trait 的多级指针，但这种类型不能直接调用成员函数。不允许结构体的二级指针直接隐式转换成泛型 _Trait 的二级指针

```c
_Trait F<int>* p;
p->foo();
_Trait F<int>** q = &p; // ok
q->foo(); // error: use of address-of-label extension outside of a function body
(*q)->foo(); // ok
struct S s;
q = &&s; // error: tentative definition has type 'struct S' that is never completed
```

7. 泛型 _Trait 实例化后的指针类型变量可以用 `const` / `volatile` 修饰

```c
const _Trait F<int>* p1;
volatile _Trait F<int>* p2;
```

8. 支持泛型 _Trait 实例化后的指针类型作为函数参数及返回值类型

```c
_Trait F<int>* get(_Trait F<int>* t) {
    return t;
}
```

9. 支持泛型 _Trait 实例化后的指针变量和 `NULL` 做比较（这里的比较仅包含 `==` 和`!=`，下同）

```c
_Trait F<int>* p = NULL;
p == NULL; // ok
p != NULL; // ok
```

10. 支持泛型 _Trait 实例化后的指针变量和非 _Trait 指针变量比较

```c
int a = 1;
int *p1 = &a;
struct S s;
struct S *p2 = &s;
_Trait F<int> *t = NULL;
// 假设struct S 类型实现 _Trait F<int>，但 int 类型没有
if (t == p1) {}; // warning
if (t == p2) {}; // ok
if (t == a) {}; // error: expected a pointer type which has implemented '_Trait F<int>', found 'int'
```

11. 支持指针变量和 _Trait 指针变量比较

```c
_Trait F<int> *t1 = NULL;
_Trait F<int> *t2 = NULL;
_Trait F<float> *t3 = NULL;
if (t1 == t2) {} // ok
if (t1 == t3) {} // warning: comparison of distinct pointer types ('_Trait F<int> *' and '_Trait F<float> *')
```

##### 2.4.3.4. 类型转换

泛型 _Trait 类型转换只能在实现了对应泛型 _Trait 的类型之间进行，将一个类型转换为另一个类型，同时保留原有类型的特性和方法。

**规则：**

1. 泛型 _Trait 实例化后的指针变量，不允许强制转换为非指针类型

```c
struct S s;
_Trait F<int> *p = &s; // 假设 struct S 类型实现了 _Trait F<int>
(struct S)p; // error: used type 'struct S' where arithmetic or pointer type is required
```

2. 支持泛型 _Trait 指针类型强制转为非 _Trait 指针类型，但不支持隐式转换

```c
struct S s;
_Trait F<int> *p = &s; // 假设 struct S 类型实现了 _Trait F<int>
struct S *q1 = p; // error: initializing 'struct S *' with an expression of incompatible type '_Trait F<int> *'
struct S *q2 = (struct S*)p; // ok
int *q3 = (int*)p; // ok
```

3. 泛型 _Trait 类型支持强制转换为 `void *`类型，但`void *` 指针无法转换为泛型 _Trait 指针类型

```c
struct S s;
_Trait F<int> *p = &s;
void * q = (void *)p; // ok: 泛型 _Trait 类型支持强制转换为 `void *`类型
_Trait F<int> *p1 = (_Trait F<int> *)q; // // error: expected a pointer type which has implemented '_Trait F<int>'', found 'void'
_Trait F<int> *p2 = q; // // error: expected a pointer type which has implemented '_Trait F<int>'', found 'void'
```

#### 2.4.4. 应用

```c
#include <stdio.h>

// 定义 _Trait
_Trait Shape {
    int getArea(This* this);
    int getSideLen(This* this);
};

struct Square {
    int side;
};

struct Rectangle {
    int width;
    int height;
};

int struct Square::getArea(struct Square* this) {
    int area = this->side * this->side;
    printf("the area of this square is %d.\n", area);
    return area;
}

int struct Square::getSideLen(struct Square* this) {
    int length = this->side * 4;
    printf("the side length of this square is %d.\n", length);
    return length;
}

int struct Rectangle::getArea(struct Rectangle* this) {
    int area = this->width * this->height;
    printf("the area of this rectangle is %d.\n", area);
    return area;
}

int struct Rectangle::getSideLen(struct Rectangle* this) {
    int length = (this->width + this->height) * 2;
    printf("the side length of this rectangle is %d.\n", length);
    return length;
}

// 为结构体类型实现 _Trait
_Impl _Trait Shape for struct Square;
_Impl _Trait Shape for struct Rectangle;

// _Trait 指针类型作为函数参数及返回值类型
_Trait Shape* get(_Trait Shape* s) {
    return s;
}

void test() {
    struct Square s = {.side = 5};
    struct Rectangle r = {.width = 2, .height = 3};
    _Trait Shape* shape = &s;
    // _Trait 指针变量调用方法
    shape->getArea(); // the area of this square is 25.
    // 强制转换
    ((_Trait Shape*)&s)->getSideLen(); // the side length of this square is 20.
    // 将指针赋值给 _Trait 指针类型的变量
    shape = &r;
    shape->getArea(); // the area of this rectangle is 6.
    // 隐式转换，将 struct Rectangle* 转为 _Trait Shape*
    _Trait Shape* shape2 = get(&r);
    shape2->getSideLen(); // the side length of this rectangle is 10
}
```

### 2.5. 运算符重载

#### 2.5.1. 概述

通过给函数增加 operator 属性来定义一个函数，并告诉编译器，对于符合函数入参规则的该运算符需调用此函数来行使运算符功能。这个函数也叫做运算符的重载函数。

#### 2.5.2. 代码示例

```c
#include <assert.h>

struct square {
  int width;
  int height;
};

// 运算符‘+’号重载函数
__attribute__((operator+)) struct square squareAdd(struct square s1,
                                                   struct square s2) {
  struct square s = {s1.width + s2.width, s1.height + s2.height};
  return s;
}

int main() {
  struct square s1 = {100, 50};
  struct square s2 = {60, 110};
  // 在之前我们必须主动调用函数来完成结构体运算操作。
  struct square s3 = squareAdd(s1, s2);
  assert(s3.width == 160);
  assert(s3.height == 160);
  // 将函数加上重载标记后，现在我们可以直接对该结构体进行运算操作。
  struct square s4 = s1 + s2;
  assert(s4.width == 160);
  assert(s4.height == 160);
  return 0;
}
```

#### 2.5.3. 语法规则

1、在普通函数基础上，通过 `__attribute__((operator Op))` 属性标记该函数为运算符重载函数。

```c
__attribute__((operator OP)) function-declaration
```

代码示例：

```c
// 定义了一个“+”号运算符重载函数 
__attribute__((operator +))
struct square squareAdd(struct square s1, struct square s2){
    struct square s = {s1.width + s2.width, s1.height + s2.height};
    return s;
}
```
2、运算符重载函数只能是全局函数，不允许标记成员函数、`_Trait`声明的函数为重载函数。

3、支持重载的运算符列表。

| 类别           | 运算符                                                       |
| -------------- | ------------------------------------------------------------ |
| 双目算术运算符 | + (加)，- (减)，* (乘)，/ (除)，% (取模)                     |
| 关系运算符     | == (等于)，!= (不等于)，< (小于)，> (大于)，<= (小于等于)，>= (大于等于) |
| 位运算符       | \| (按位或)，& (按位与)，~ (按位取反)，^ (按位异或)，<< (左移)，>> (右移) |
| 单目运算符     | \+ (正)，- (负)，* (解引用)                                  |
| 成员访问运算符 | ->                                                           |
| 索引运算符     | []                                                           |

4、运算符重载函数入参和返回值要求。

| 运算符                        | 入参要求                                               | 返回值要求                                   |
| -------------------------- | -------------------------------------------------- | --------------------------------------- |
| 关系运算符                      | 只允许有两个入参, 且至少有一个参数是用户自定义类型，如结构体、枚举。                | 返回值类型必须是_Bool类型                         |
| *(解引用), <br />-> (成员访问运算符) | 只允许有一个入参，且第一个参数为用户自定义类型的指针类型，包括裸指针、可变借用指针、不可变借用指针。 | 返回值类型必须是指针类型，包括裸指针、可变借用指针、不可变借用指针、Rc指针。 |
| [] (索引运算符)                 | 只允许有两个入参，且第一个参数为用户自定义类型的指针类型，包括裸指针、可变借用指针、不可变借用指针。 | 返回值类型必须是指针类型，包括裸指针、可变借用指针、不可变借用指针、Rc指针。 |
| 其他                         | 单目运算符只允许有一个入参, 双目运算符值只允许有两个入参。函数至少有一个入参是用户自定义类型。   | 无                                       |

- 关系运算符重载代码示例

```c
#include <assert.h>

struct Foo {
  int x;
  int y;
};

// 参数至少有一个是用户自定义类型
__attribute__((operator>))
_Bool FooCompare(struct Foo A, struct Foo B) {
  return A.y > B.y;
};

int main() {
  struct Foo f1 = {180, 18};
  struct Foo f2 = {166, 22};
  assert(f2 > f1 && "compare error");
  return 0;
}
```

- 解引用运算符重载代码示例

```c
#include <assert.h>

struct MyPoint<T> {
  T *data;
};

// 入参和返回值必须是指针类型
__attribute__((operator*))
T * derefMyPoint<T>(struct MyPoint<T> *_Borrow p) {
  return p->data;
}

int main() {
  int data = 100;
  struct MyPoint<int> p = {
    &data
  };
  // 编译器匹配到重载函数后，自动对p取地址并传入重载函数，并对函数返回值做解引用。此处等效为(*derefMypoint(&_Mut p)) == 100
  assert(*p == 100);
  *p = 10;
  assert(*p == 10);
  return 0;
}
```

- 成员访问运算符重载代码示例

```c
#include <assert.h>

struct MyData<T> {
  T a;
};
struct MyPoint<T> {
  MyData<T> *data;
};

// 入参和返回值必须是指针类型
__attribute__((operator->))
MyData<T> * mDerefMyPoint<T>(struct MyPoint<T> *_Borrow p) {
  return p->data;
}

int main() {
  struct MyData<int> d = {
    100
  };
  struct MyPoint<int> p = {
    &d
  };
  // 编译器匹配到重载函数后，自动对p取地址并传入重载函数。此处等效为mDerefMyPoint(&_Mut p)->a == 100
  assert(p->a == 100);
  p->a = 10;
  assert(p->a == 10);
  return 0;
}
```

-  索引运算符重载代码示例

```c
#include <assert.h>
#define ARRLEN 10

struct MyArray<T> {
  T data[ARRLEN];
};

__attribute__((operator[]))
T *GetMyArrayData<T>(struct MyArray<T> *p, int index) {
  return &p->data[index];
}

int main() {
  struct MyArray<int> array;
  for (int i = 0; i < ARRLEN; i++) {
    // 编译器匹配到重载函数后，自动对p取地址并传入重载函数,并对函数返回值做解引用。此处等效为*GetMyArrayData(&_Mut p, i) = i
    array[i] = i;
  }
  assert(array[5] == 5);
  return 0;
}
```

-  运算符重载错误代码示例

```c
// error: 不允许定义无用户自定义入参类型的运算符重载函数。
__attribute__((operator+))
int squareAdd(int a, int b) { return a + b; }
```

5、运算符重载函数名不能与普通函数名冲突。对同一个运算符定义多个重载函数时，如果函数名不同并且参数类型不完全一致，允许同时存在。

```c
__attribute__((operator +))
struct square squareAdd(struct square s1, struct square s2) {
  /* code */
}
// 支持对同一个运算符多次重载
__attribute__((operator +))
struct oblong oblongAdd(struct oblong s1, struct oblong s2) {
  /* code */
}
```

7、运算符重载函数可以是泛型函数。

```c
#include <stdio.h>

struct Point<T> {
  T x;
  T y;
};

__attribute__((operator+))
struct Point<T> Add<T>(struct Point<T> lhs, struct Point<T> rhs) {
  T x1 = lhs.x + rhs.x;
  T y1 = lhs.y + rhs.y;
  struct Point<T> p = {
    .x = x1, .y = y1
  };
  return p;
}

__attribute__((operator*)) struct Point<T>
Mul<T>(struct Point<T> lhs, struct Point<T> rhs) {
  T x1 = lhs.x * rhs.x;
  T y1 = lhs.y * rhs.y;
  struct Point<T> p = {
    .x = x1, .y = y1
  };
  return p;
}

void test1() {
  struct Point<int> p1 = {
    .x = 1, .y = 2
  };
  struct Point<int> p2 = {
    .x = 3, .y = 4
  };
  // {.x = 4, .y = 6}
  struct Point<int> p3 = p1 + p2;
  printf("p3.x: %d, p3.y :%d\n", p3.x, p3.y);
}

void test2() {
  struct Point<int> p1 = {
    .x = 1, .y = 2
  };
  struct Point<int> p2 = {
    .x = 3, .y = 4
  };
  // {.x = 3, .y = 8}
  struct Point<int> p3 = p1 * p2;
  printf("p3.x: %d, p3.y :%d\n", p3.x, p3.y);
}

int main() {
  test1();
  test2();
  return 0;
}
```

## 3. 内存安全

毕昇 C 内存管理的目标是将常见的时间类内存安全问题，如悬挂引用/内存泄漏/重复释放堆内存/解引用空指针等常见的内存安全问题在编译阶段暴露出来。为此，毕昇 C 引入了以下机制与规则：
1. [所有权](#31-所有权)：引入 `_Owned` 关键字，确保对象分配、释放正确。
2. [借用](#32-借用)：引入 `_Borrow` 关键字，与所有权结合以避免指针悬垂等问题。
3. [数组与安全指针](#33-数组与安全指针)：引入 `_ArrayElem` 关键字配合 `_Owned` 与 `_Borrow`，明确区分指向单个对象的指针与指向数组元素的指针。
4. [非空指针](#34-非空指针)：引入 `_Nonnull` 与 `_Nullable` 关键字以明确指针是否允许为空、阻止潜在的空指针解引用。
5. [_Owned struct](#35-_owned-struct-类型)：代表带有析构函数、带所有权的结构体，使用户能用所有权管理非指针类型的资源。
6. [安全区](#36-安全区)：引入 `_Safe` 与 `_Unsafe` 关键字，在 `_Safe` 的安全区中使用更严格的语法检查以阻止不安全操作。
7. [初始化分析](#37-初始化分析)：通过更准确、更严格的初始化分析以更好地发现对未初始化内存的读取。

毕昇C将 `_Owned` 或 `_Borrow` 修饰的指针称为安全指针，没有 `_Owned` 或 `_Borrow` 修饰的指针称为裸指针。C中所有时间类内存安全问题都是由裸指针的不慎使用导致的。比起裸指针，安全指针有更严格的类型检查和使用限制，可能导致悬垂引用、内层泄露等问题的代码均无法通过所有权和借用机制的检查，因此使用安全指针替代裸指针可以避免时间类内存安全问题。由于安全区中禁止解引用裸指针、只能解引用安全指针，且安全区有更严格的语法检查，因此毕昇C能够保证安全区内的代码不会导致时间类内存安全错误。其他机制在此基础上补充了额外的安全检查和保护覆盖。

毕昇C的语言特性目前无法提供对空间内存安全的保护，空间安全将会通过运行时检查机制来保证。

### 3.1. 所有权

#### 3.1.1. 前言

C 语言作为一种系统级编程语言，
提供了对指针的高度灵活的直接操作以及利用内存管理函数使开发者手动精细控制和管理内存的能力，
因而被广泛地应用于各种需要直接与硬件或内存等系统资源交互的领域和场景。
然而，这种内存管理模式存在容易导致内存泄漏、释放后使用、空指针解引用、缓冲区溢出和越界读写等内存安全问题。
内存安全问题不仅会造成资源的浪费，也可能导致程序行为错误，甚至导致程序崩溃，对程序的稳定性造成威胁。
内存安全问题可以划分为**时间内存安全**和**空间内存安全**两大类，其中时间内存安全包含内存泄漏、释放后使用、空指针解引用等，空间内存安全包括缓冲区溢出、越界读写等。
BiShengC 语言的内存管理为解决程序的时间内存安全问题，利用了**所有权特性**在编译期对潜在的内存安全问题进行检查，识别潜在的时间内存安全错误。

#### 3.1.2. 特性简介

BiShengC 语言的所有权特性被用于确保程序中的指针及其指向内存空间能被正确地管理。
在 BiShengC 语言中，使用`_Owned`关键字用来修饰一个指针类型，表明该指针拥有其指向的内存的所有权。
拥有所有权的指针必须确保其指向的内存在指针作用域结束前被显式释放，否则存在潜在的内存泄漏错误；
此外，一块堆内存只能同时被一个`_Owned`指针所拥有，`_Owned`指针为移动语义，这样避免了释放后使用等内存安全问题的发生。
以下是一段使用了所有权特性的 BiShengC 语言代码，用于了解所有权特性：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

int *_Owned foo(int *_Owned p) { return p; }

_Safe void bar(void) {
  // 通过提供的 safe_malloc 申请一块大小为 sizeof(int) 的堆内存，并将值设置为2
  int *_Owned p = safe_malloc(2);
  // 将 p 指向的堆内存转移给 q，后续不可再使用 p 访问这块内存，否则编译报错
  int *_Owned q = p;
  _Unsafe {
    // 通过函数参数转移走 q 的所有权，但通过函数返回值归还所有权
    q = foo(q);
    // 在 q 的作用域结束前调用 safe_free
    // 安全释放堆内存，此处不释放则会报内存泄漏错误
    safe_free((void *_Owned)q);
  }
  return;
}

int main() {
  bar();
  return 0;
}
```

在安全区，`_Owned`指针指向的内存一定为堆上内存（如通过`safe_malloc`函数申请出的堆内存），`_Owned`指针不可能指向栈内存，在作用域结束前必须转移所有权或释放（如`safe_free`函数进行释放）。

这两个函数的函数原型如下：
```c
T *_Owned safe_malloc<T>(T t);
void safe_free(void *_Owned);
```
可以通过`safe_malloc`申请足够存放`T`类型的堆空间，并初始化为`t`，当`_Owned`指针生命周期结束前，必须通过`safe_free`进行释放，在使用时，需要将`T *_Owned`指针转换为`void *_Owned`指针类型再释放，对于**多级指针，需要从内到外进行释放**；对于结构体内部有`_Owned`指针成员的情况，需要**先将结构体内全部`_Owned`指针成员释放后，才能释放结构体的`_Owned`指针**。

**`_Owned` 与泛型类型参数**

在泛型函数中，`_Owned` 修饰泛型类型参数（如 `T _Owned` 或 `_Owned T`）时，`_Owned` 应用于完整类型 `T`。当 `T` 实例化为 `int*` 时，`T _Owned` 和 `_Owned T` 都产生 `int* _Owned`（_Owned 指针），而非 `_Owned int*`。泛型定义时允许对类型参数使用 `_Owned`，有效性在实例化时检查。

函数使用示例：
```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *_Owned p;
  int *_Owned q;
};

_Safe void foo(void) {
  //变量所有权初始化
  int *_Owned pi = safe_malloc(1);
  // 结构体所有权初始化
  struct S s = {.p = safe_malloc(2), .q = safe_malloc(3)};
  struct S *_Owned s1 = safe_malloc(s);
  // 多级指针所有权初始化
  int *_Owned p = safe_malloc(1);
  int *_Owned *_Owned pp = safe_malloc(p);
  // 变量所有权释放
  safe_free((void *_Owned)pi);
  // 结构体所有权释放(从内到外)
  safe_free((void *_Owned)s1->p);
  safe_free((void *_Owned)s1->q);
  safe_free((void *_Owned)s1);
  // 多级指针所有权释放(从内到外)
  safe_free((void *_Owned) * pp);
  safe_free((void *_Owned)pp);
}

int main() {
  foo();
  return 0;
}
```

#### 3.1.3. 语法及语义规则

为实现所有权特性，BiShengC 语言引入了`_Owned`关键字用于修饰指针类型的变量。`_Owned`关键字与`const`、`restrict`以及`volatile`均属于类型修饰符。

具体而言，所有权特性的语法及部分语义规则有以下几点：

1. `_Owned`关键字仅允许在 BiShengC 语言编译单元内使用；

2. `_Owned`关键字仅被允许用于修饰指针类型，不允许修饰非指针类型，修饰多级指针时，每级指针的类型修饰可以不一样，规则与`const`类似；

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe int main(void) {
  int *_Owned p = safe_malloc(2);
  int _Owned a = 2; // error: _Owned 关键字必须修饰指针

  double *_Owned q = safe_malloc(1.1);
  double _Owned b = 1.1; // error: _Owned 关键字必须修饰指针

  int *_Owned *_Owned pp = safe_malloc(p); // ok: 可以修饰多级指针
  safe_free((void *_Owned)pp); // error: 多级指针释放时，应当从内向外释放
  safe_free((void *_Owned) * pp); 

  double *d = (double *)malloc(sizeof(double));
  double **_Owned pd = safe_malloc(d);
  free(*pd); // 也可直接 free(d);
  safe_free((void *_Owned)pd);
  safe_free((void *_Owned)q);
  return 0;
}
```

3. 允许使用`_Owned`关键字修饰结构体指针及结构体的指针成员；

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int m;
  int n;
};

struct R {
  int *_Owned p;
  double *_Owned q;
};

_Safe void test(void) {
  struct S s = {.m = 1, .n = 2};
  struct S *_Owned sp = safe_malloc(s);
  struct S _Owned so = {.m = 1, .n = 2}; // error: _Owned 不允许修饰结构体变量
  
  struct R r = {.p = safe_malloc(1), .q = safe_malloc(2.5)};
  struct R *_Owned rp = safe_malloc(r);
  safe_free((void *_Owned)sp);
  // 先释放结构体内的 _Owned 指针成员
  safe_free((void *_Owned)rp->p);
  safe_free((void *_Owned)rp->q);
  // 再释放结构体 _Owned 指针
  safe_free((void *_Owned)rp);
}

int main() {
  test();
  return 0;
}
```

4. `_Owned`不允许修饰`union`类型和`union`的成员，且`union`的每个成员均不能拥有`_Owned`修饰的成员；

```c
struct S {
  int a;
  int b;
};

struct T {
  int *_Owned p;
  struct S s;
};

union A {
  int a;
  int *_Owned p; // error: 禁止 union 成员用 _Owned 修饰
  struct S s;
  struct S *_Owned sp; // error: 禁止 union 成员用 _Owned 修饰
  struct T t; // error: 禁止 union 成员结构体有 _Owned 成员
  struct T *_Owned tp; // error: 禁止 union 成员用 _Owned 修饰
  struct T *trp; // ok: 不跟踪裸指针指向的变量的 _Owned 成员
};
```

5. `_Owned`修饰的类型或拥有`_Owned`修饰的成员的类型不可以作为数组的成员、不可以作为 `_Owned _ArrayElem` 指针的指向类型或其成员。（`_ArrayElem` 指向类型限制详见 [3.3.1.1 节](#3311-指向类型限制)）

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct A {
  int *_Owned p;
};

_Safe void test(void) {
  int *_Owned arr_i[2] = {safe_malloc(1), safe_malloc(2)}; // error: 数组成员不能被 _Owned 修饰
  struct A arr_a[2] = {{safe_malloc(1)}, {safe_malloc(2)}}; // error: 数组成员不能被 _Owned 修饰
}
_Safe struct A *_Owned _ArrayElem test2(void); // error: _Owned _ArrayElem 指针指向类型不能含有 _Owned 成员
```

6. `_Owned`指针不支持下标运算、算术运算（指针偏移操作），但支持比较运算。`_Owned _ArrayElem`指针支持下标运算（详见 [3.3.1.2 节](#3312-_owned-_arrayelem-指针)）。

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe int main(void) {
  int *_Owned p = safe_malloc(2);
  int *_Owned q = safe_malloc(3);
  
  p += 1; // error: _Owned 指针不支持算术运算符
  
  p[3] = 3; // error: _Owned 指针不支持下标运算符

  if (p == q) { // ok: _Owned 指针支持比较运算符
  }

  _Unsafe {
    safe_free((void *_Owned)p);
    safe_free((void *_Owned)q);
  }
  return 0;
}
```

7. `_Owned`修饰的类型与非`_Owned`修饰的类型之间不允许隐式类型转换；

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe int main(void) {
  _Unsafe {
    int *b = (int *)malloc(sizeof(int));
    int *_Owned p = b; // error: _Owned 指针不允许隐式类型转换
  }
  int *_Owned q = safe_malloc(3);
  int *c = q; // error: _Owned 指针不允许隐式类型转换
  safe_free((void *_Owned)q);
  return 0;
}
```

8. 禁止 `_Owned` 指针类型与裸指针类型之间的显式强制类型转换（包括非安全区）。当用户需要将裸指针转换为 `_Owned` 指针时，必须使用内置函数 `__take_from_raw` 显式表达所有权转移；当用户需要将 `_Owned` 指针转换为裸指针时：若需转移所有权，必须使用 `__move_to_raw` 显式表达所有权转移；若不需转移所有权，必须先取借用再进行转换，例如 `(T *)&_Mut *p`。以上转换均需在非安全区进行。对 `_Owned` 指针与裸指针之间转换的限制只对最外层指针类型生效；形如 `T*_Owned*` 与 `T**` 之间的转换视为裸指针之间的转换，在安全区不允许、非安全区允许显式执行。
指向类型不一致时，只允许`void * _Owned`类型与`T * _Owned`类型之间的显式强制转换。此类转换在[下文](#3143-强制类型转换) 有额外说明。其他指向类型不一致的 `_Owned` 指针之间不允许转换。

    **显式所有权转移接口**（内置函数）：
    - `__move_to_raw(p)`：将 `_Owned` 指针 `p` 转为裸指针并转移所有权，返回的裸指针与入参具有相同的 Nullability。
    - `__take_from_raw(p)`：将裸指针 `p` 转为 `_Owned` 指针并取得其所指对象的所有权，返回的 `_Owned` 指针与入参具有相同的 Nullability。

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放
int main() {
  int *pi1 = (int *)malloc(sizeof(int));
  int *_Owned pi2 = (int *_Owned)pi1;  // error: 不允许裸指针转换为 _Owned 指针
  int *_Owned pi3 = __take_from_raw(pi1);  // ok: 显式将裸指针所有权转移至 _Owned

  double *_Owned pd1 = safe_malloc(1.0);
  int *_Owned pi4 = (int *_Owned)pd1;   // error: pi4 和 pd1 基本类型不一致
  double *pd2 = __move_to_raw(pd1); // ok: 显式将 _Owned 所有权转移至裸指针

  double *_Owned pd3 = safe_malloc(1.5);
  void *_Owned pv1 = (void *_Owned)pd3; // ok: 允许显式转换为 void *_Owned

  void *_Owned * ppv1 = &pv1;
  void ** ppv2 = (void **) ppv1; // ok: 允许显式从 T *_Owned * 转为 T **
  ppv1 = (void *_Owned *) ppv2; // ok: 允许显式从 T ** 转为 T *_Owned *
  safe_free((void *_Owned)pi3);
  safe_free(pv1);
  free((void*)pd2);
  return 0;
}
```

```c
// 不转移所有权时，先取借用再转换为裸指针:
#include "bishengc_safety.hbs"
void foo(int *);
int main() {
  int *_Owned p = safe_malloc(1);
  int *raw_ptr = (int *)&_Mut *p; // ok: 先取借用再转为裸指针，不转移所有权
  foo(raw_ptr); // 使用 raw_ptr
  safe_free((void *_Owned)p);
  return 0;
}
```

```c
// _Owned 与裸指针转换函数保留入参指针的 Nullability
#include "bishengc_safety.hbs"
int main() {
  int * p1 = malloc(sizeof(int));
  if (p1 != nullptr) {
    int * _Owned p2 = __take_from_raw(p1); // p1 判非空后 __take_from_raw(p1) 可以赋值给非空的 p2
    safe_free((void*_Owned)p2);
  }
  p1 = malloc(sizeof(int));
  int * _Owned p3 = __take_from_raw(p1); // p1 未判空，空指针检查启用时此处报错
  safe_free((void*_Owned)p3);
}
```

9.  `_Owned`允许修饰指向`_Trait`的指针，假设有一个具体类型`S`，它实现了`_Trait T`，则：

    - `S * _Owned`类型可以隐式转换为`_Trait T * _Owned`类型；
    - `_Trait T * _Owned`类型允许被显式转换为`void * _Owned`类型。
    - 不允许`_Trait T* _Owned`与`_Trait T*`之间的类型转换。

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Trait T{};

_Impl _Trait T for int;

void test() {
  int *_Owned pi = safe_malloc(1);
  _Trait T *_Owned pti = pi; // ok: 隐式转换为 _Trait T *_Owned
  void *_Owned pvi = (void *_Owned)pti; // ok: 显式转换为 void *_Owned
  safe_free(pvi);
}

int main() {
  test();
  return 0;
}
```

10. 通过函数指针调用函数的时候，规则与一般的函数调用一样，会在函数调用时检查形参类型与实参类型是否匹配及返回类型与返回值类型是否匹配；

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

int deref_add(int *a, int *b) { return *a + *b; }
typedef int (*FTP)(int *, int *);
typedef int (*FTOP)(int *_Owned, int *);

void foo() {
  FTP ftp = deref_add;
  int *_Owned pa = safe_malloc(1);
  int *_Owned pb = safe_malloc(2);
  
  ftp(pa, pb); // error: 类型不匹配
  FTOP ftop = deref_add; // error: 类型不匹配
  safe_free((void *_Owned)pa);
  safe_free((void *_Owned)pb);
}

int main() {
  foo();
  return 0;
}
```

11.  _Owned 指针可以使用逻辑运算符。

```c
void logical_not(int *_Owned _Nullable p) {
  if (!p) { // equivalent: p == nullptr
  }
}
void logical_and(int *_Owned _Nullable p, int *_Owned _Nullable q) {
  if (p && q) { // equivalent: (p != nullptr) && (q != nullptr)
  }
}
void logical_or(int *_Owned _Nullable p, int *_Owned _Nullable q) {
  if (p || q) { // equivalent: (p != nullptr) || (q != nullptr)
  }
}
```

12. 允许 _Owned 指针作为 if while do-while for 语句和三元表达式的条件，不允许作为 switch 的条件。

```c
void foo(int *_Owned _Nullable p) {
  if (p) { // equivalent: p != nullptr
  }
  while (p) { // equivalent: p!= nullptr
  }
  do {
  } while (p); // equivalent: p != nullptr

  for (;p;) { // equivalent: p != nullptr
  }
  swtich (p) { // error
  default:
    break;
  }
  int x = p ? 2 : 1; // equivalent: p != nullptr ? 2 : 1
}
```

13. 允许在变量初始化、变量赋值和函数传参中将 _Owned 指针隐式转换为 _Bool，不会消耗其指向内容的所有权。

```c
void foo(int *_Owned _Nullable p) {
  _Bool flag = p; // equivalent: _Bool flag = p != nullptr;
}
void bar(int *_Owned _Nullable p, _Bool flag) {
  flag = p; // equivalent: flag = p != nullptr;
}
void use(_Bool);
void baz(int *_Owned _Nullable p) {
  use(p); // equivalent: use(p != nullptr);
}
```

14. 可以在逗号表达式中使用 _Owned 指针，逗号表达式是否消耗所有权取决于使用逗号表达式的场合

```c
int *_Owned _Nullable bar(int *_Owned _Nullable x) { return x; }

int *_Owned _Nullable foo(int *_Owned _Nullable p, int a) {
  if (++a, p) { // if 条件中不消耗所有权
    int *_Owned _Nullable q = (++a, p); // 初始化消耗 p 的所有权转移到 q
    int *_Owned _Nullable r = nullptr;
    r = bar((++a, q)); // 传参消耗 q 的所有权转移到形参 x，再由返回值通过赋值转移到 r
    return r;
  }
  return p;
}
```

#### 3.1.4. 所有权状态转移规则

在对所有权特性的语法和部分语义有了解后，本节将对所有权的状态转移规则进行详细阐述。
为更好地理解所有权特性对内存安全带来的保障，首先需要明白程序执行时的堆栈内存模型。

总体而言，程序执行时内存可分为栈区和堆区两个部分，这两部分内存一起为程序在运行时提供内存空间。
栈区即为调用栈，保存的是程序执行所需要维护的所有信息。
每当一次函数调用发生时，就会创建一个对应的栈帧，函数调用的上下文、函数的入参以及函数体内的局部变量就存在这个栈帧中。
栈帧的基址一般由 rbp 寄存器指向，而栈顶由 rsp 寄存器指向，两个寄存器共同标识了一个函数的栈帧。
当一次函数调用结束时，相应的栈帧就会在函数返回前被销毁，相应的内存空间也就得到释放。
这个过程是通过调整 rbp 寄存器的值为调用者的栈帧基址、rsp 的值为调用者的栈顶地址完成的，这也是为什么栈区的变量不需要显式释放。
对于堆区，则存放的是那些在运行时动态分配内存的数据。
一个典型的例子是对于`int *p = malloc(sizeof(int))`这种操作，需要由操作系统在堆区找到一块适合大小的内存空间用于分配，然后将这块内存的地址存在 p 中，指针 p 是程序的一个栈上变量。
虽然堆区的内存分配更为灵活，但却缺乏组织，不正确的内存管理很容易导致堆区的内存泄漏。
例如，当一次函数调用完成后，其局部变量 p 被销毁，而其指向的堆内存未被显式地调用`free(p)`进行回收，则这块堆内存将永远无法被回收，产生了内存安全错误。

<div align="center">
    <img src=../BiShengCLanguageUserManualImages/memory-leak-example.png width=60% />
</div>

利用 BiShengC 语言提供的所有权特性，可以用`_Owned`关键字对那些需要管理的指针进行标识，这样就可以在程序编译时检查出潜在的错误，避免在运行时出现错误。
以下是 BiShengC 语言所有权的核心规则：

1. 在 BiShengC 语言中每一个值都被一个`_Owned`指针变量所拥有，该`_Owned`指针变量即为值的所有者；
2. 一个值同时只能被一个`_Owned`指针变量所拥有，即一个值只能拥有一个所有者；
3. 当`_Owned`指针变量离开作用域范围时，需要释放其拥有的值所在的堆内存。

基于以上核心规则，接下来结合详细的代码示例进行具体介绍。

##### 3.1.4.1. 转移所有权

**1. 一个拥有所有权的变量`s1`被赋值给另一个变量`s2`是一个移动语义，该操作后变量`s1`失去了对值的所有权，原先的变量`s1`无法再使用。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe void foo(void) {
  int *_Owned p = safe_malloc(10);
  int *_Owned q = p;
  int *_Owned r = p; // error: p 的所有权已移交给q了
  _Unsafe {
    safe_free((void *_Owned)q);
    safe_free((void *_Owned)r);
  }
}

int main() {
  foo();
  return 0;
}
```

在这个例子中，`p`拥有一块堆内存的所有权，这块内存大小为`sizeof(int)`，存储的值为10。
在声明`q`时，将`p`的所有权转移给了`q`，`p`不再拥有对这块堆内存的所有权。
则在声明`m`时，便不可再将`p`的所有权转移给`m`，编译器会在此处报错。
因此，利用所有权特性，可以保证一个值只能拥有一个所有者。
（那么如果没有这条规则会出现什么后果呢？三个指针会同时指向一块内存，在作用域结束时对这块内存释放三次，出现重复释放的错误）

**2. 一个拥有所有权的变量`s1`作为整体被赋值给另一个变量`s2`时，如果`s1`内部还有其他拥有所有权的指针，则会全部都转移给`s2`。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *p;
  int *_Owned q;
};

void test(void) {
  struct S s = {.p = (int *)malloc(sizeof(int)), .q = safe_malloc(1)};
  struct S *_Owned s1 = safe_malloc(s);
  struct S *_Owned s2 = s1;
  int *_Owned p = s1->q; // error: q 的所有权被一并交给 s2 了
  safe_free((void *_Owned)s2->q);
  safe_free((void *_Owned)s2);
  safe_free((void *_Owned)p);
}

int main() {
  test();
  return 0;
}
```

在这个例子中，将`s1`指向堆内存的所有权转移给了`s2`，但同时`s1`内部还有一个拥有所有权的指针`s1->q`，因此也会一并将他的所有权转移给`s2->q`，后续再使用`s1->q`时便会报错。

**3. 一个拥有所有权的变量`s1`作为整体被赋值给另一个变量`s2`时，如果`s1`内部还有其他拥有所有权的指针，则必须保证内部其他`_Owned`指针均拥有所有权，才能将`s1`赋值给`s2`。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *_Owned p;
  int *_Owned q;
};

_Safe void foo(void) {
  struct S s = {.p = safe_malloc(2), .q = safe_malloc(3)};
  struct S *_Owned s1 = safe_malloc(s);
  int *_Owned p = s1->p;
  struct S *_Owned s2 = s1; // error: s1 已无对全部成员的所有权
  safe_free((void *_Owned)p);
  safe_free((void *_Owned)s2->p);
  safe_free((void *_Owned)s2->q);
  safe_free((void *_Owned)s2);
}

int main() {
  foo();
  return 0;
}
```

在这个例子中，我们先将`s1->p`的所有权转移走，然后试图整体转移`s1`的所有权给`s2`，但此时`s1->p`已经不再持有对任何一块堆内存的所有权，因此这个操作是不合法的。

**4. 一个拥有所有权的变量`s1`在失去其所有权后，可以通过赋值的方式使其再次拥有指向某块堆内存的所有权，这样就可以再次使用`s1`。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe void foo(void) {
  int *_Owned p = safe_malloc(10);
  // 移走 p
  int *_Owned q = p;
  // 重新拿到新元素的所有权
  p = safe_malloc(4);
  // 仍然可以将所有权交给其他指针
  int *_Owned m = p;
  safe_free((void *_Owned)q);
  safe_free((void *_Owned)m);
}

int main() {
  foo();
  return 0;
}
```

在这个例子中，`p`的所有权转移给`q`后，再次调用了`safe_malloc`函数为其重新赋予了一块堆内存的所有权，因此后续仍可将`p`的所有权转移给`m`。

**5. 不允许将所有权转移给一个已经拥有所有权的变量。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe void foo(void) {
  int *_Owned p = safe_malloc(12);
  int *_Owned q = safe_malloc(67);
  q = p; // error: p 有拥有其他指针的所有权
  safe_free((void *_Owned)p);
  safe_free((void *_Owned)q);
}

int main() {
  foo();
  return 0;
}
```

在这个例子中，试图将`p`的所有权转移给`q`，但`q`此时已经拥有所有权，再试图转移的话会使`q`原先指向的堆内存泄漏，因此无法进行转移，会在编译时报错。

**6. 如果一个变量`s1`拥有所有权，而其内部的`_Owned`指针变量的所有权已被转移，如果想再次赋予内部变量所有权，需要保证内部变量的所有父`_Owned`指针变量均拥有所有权。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *_Owned p;
  int *_Owned q;
};

_Safe void foo(void) {
  struct S s = {.p = safe_malloc(2), .q = safe_malloc(3)};
  struct S *_Owned s1 = safe_malloc(s);
  struct S *_Owned s2 = s1;
  s1->p = safe_malloc(5); // error: s1 已不再拥有所有权，无法给内部成员赋予所有权
  safe_free((void *_Owned)s2->p);
  safe_free((void *_Owned)s2->q);
  safe_free((void *_Owned)s2);
}

int main() {
  foo();
  return 0;
}
```

在这个例子中，`s1`、`s1->p`以及`s1->q`的所有权均被转移给了`s2`，后续再试图赋予`s1->p`所有权时，其父`_Owned`变量指针`s1`尚未拥有所有权，因此此次操作是非法的，会在编译时报错。

##### 3.1.4.2. 作用域结束时的内存释放

**1. 对于所有的`_Owned`指针变量，会在其词法作用域结束时检查其是否依然拥有堆内存的所有权，如果依然拥有，则存在内存泄漏错误。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *_Owned p;
  int *_Owned q;
};

_Safe void foo(void) {
  int *_Owned p = safe_malloc(2);
  struct S s = {.p = safe_malloc(2), .q = safe_malloc(3)};
  struct S *_Owned s1 = safe_malloc(s);
  struct S *_Owned s2 = s1;
  // safe_free((void *_Owned)p);
  // safe_free((void *_Owned)s2->p);
  // safe_free((void *_Owned)s2->q);
  // safe_free((void *_Owned)s2);
} // error: 未释放 s2 内部成员所有权及 s2 所有权；未释放 p 所有权

int main() {
  foo();
  return 0;
}
```

在这个例子中，当作用域结束时，编译器会发现`p`、`s2`、`s2->p`以及`s2->q`依然拥有其指向的堆内存的所有权，即这些堆内存都没有被释放，因此会编译失败并报告内存泄漏。

##### 3.1.4.3. 强制类型转换

**1. 允许将`T * _Owned`类型的变量通过强制类型转换转为`void * _Owned`类型，但转换成功的条件为变量依然拥有所有权且其内部的`_Owned`指针变量均已不拥有所有权。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *_Owned p;
  int *_Owned q;
};

_Safe void foo(void) {
  struct S s = {.p = safe_malloc(2), .q = safe_malloc(3)};
  struct S *_Owned s1 = safe_malloc(s);
  int *_Owned p = s1->p;
  _Unsafe {
    safe_free((void *_Owned)s1); // error: s1 仍然拥有内部成员(q)所有权，无法转为 void *_Owned
    safe_free((void *_Owned)p);
  }
}

int main() {
  foo();
  return 0;
}
```

在这个例子中，试图将`s1`强制类型转换为`void * _Owned`类型，但`s1->q`依然拥有所有权，因此转换失败。

注意，在涉及 `T * _Owned` 到 `void *` 的转换时，转换的顺序不同会导致语义不同:
1. 先将 `_Owned` 指针的指向类型转为 `void`，再使用 `__move_to_raw` 将 `_Owned` 指针转换为裸指针 (即`T * _Owned` 转 `void * _Owned` 转 `void *`): `T * _Owned` 指针必须具有所有权，但其指向的 `T` 内部所有 `_Owned` 指针变量必须**均不拥有所有权**才能进行转换。
2. 先使用 `__move_to_raw` 将 `_Owned` 指针转换为裸指针，再改变指向类型 (即`T * _Owned` 转 `T *` 转 `void *`): `T * _Owned` 指针必须具有所有权、其指向的 `T` 内部所有 `_Owned` 指针变量仍**拥有所有权**才能进行转换。

用户在需要将 `T * _Owned` 转成裸指针进行释放时，应先将其成员释放，再采用第一种转换顺序，将 `T * _Owned` 转为 `void * _Owned` 再转为裸指针进行释放。编译器会在 `T * _Owned` 转为 `void * _Owned` 时检查 `T` 内的 `_Owned` 指针均已转移所有权，避免内存泄露。除了需要释放对象的情况，其余场景均应采用第二种转换顺序，将 `T * _Owned` 内 `_Owned` 指针的所有权保留下来，以免后续使用已释放的指针。

以下是示例代码：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *_Owned p;
  int *_Owned q;
};

void myfree(struct S *_Owned ptr) {
  // 释放场景：采用第一种顺序 (T * _Owned 转 void * _Owned 转 void *)
  safe_free((void *_Owned) ptr->p);
  safe_free((void *_Owned) ptr->q);

  // free((void *)__move_to_raw(ptr)); // error: ptr内部已经没有所有权， 不能使用 __move_to_raw
  free(__move_to_raw((void *_Owned)ptr)); // ok
}
void *mymove(struct S *_Owned ptr) {
  // 其余场景：采用第二种顺序 (T * _Owned 转 T * 转 void *)
  // return __move_to_raw((void *_Owned)ptr); // error: ptr内部仍有所有权，不能转为 void *_Owned
  return (void *)__move_to_raw(ptr); // ok
}
```

**2. 允许将`void * _Owned`类型的变量通过强制类型转换转为`T * _Owned`类型，转换成功的条件为变量依然拥有所有权，转换成功后，得到的`T *_Owned`类型内部的`_Owned`指针变量均不拥有所有权。**
以下是一段代码示例及说明：

```c
struct S {
  int *_Owned p;
  int *_Owned q;
};

_Safe void *_Owned memAlloc(unsigned long);
_Safe void memFree(void *_Owned);

_Safe void foo(void) {
  struct S *_Owned sp = _Unsafe((struct S *_Owned)memAlloc(sizeof(struct S)));
  int *_Owned p = sp->p; // error: sp->p 此时未拥有所有权
  sp->q = _Unsafe((int *_Owned)memAlloc(sizeof(int)));
  *(sp->q) = 2;
  _Unsafe {
    memFree((void *_Owned)sp->q);
    memFree((void *_Owned)sp);
  }
}

int main() {
  foo();
  return 0;
}
```

在这个例子中，试图直接转移`sp->p`的所有权给`p`，但此时`sp->p`还未拥有所有权，因此编译报错。

##### 3.1.4.4. 函数调用与返回

**1. 函数调用和返回时，如果函数的形参或函数的返回值为`_Owned`指针类型，则要求传入的实参以及返回值必须拥有堆内存的所有权。**
以下是一段代码示例及说明：

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int *_Owned p;
  int *_Owned q;
};

struct S *_Owned foo(struct S s) {
  struct S *_Owned ret = safe_malloc(s);
  return ret;
}

void bar(void) {
  struct S s = {.p = safe_malloc(2), .q = safe_malloc(3)};
  int *_Owned p = s.p;
  struct S *_Owned s1 = foo(s); // error: s 已不再拥有全部内部成员的所有权
  safe_free((void *_Owned)p);
  safe_free((void *_Owned)s1->p);
  safe_free((void *_Owned)s1->q);
  safe_free((void *_Owned)s1);
}

int main() {
  bar();
  return 0;
}
```

在这个例子中，传入`F`函数的结构体变量`s`内部有两个`_Owned`指针变量，而`s.p`已经被转移走，因此这次函数调用是非法的，会编译报错。

#### 3.1.5. 源源变换

BiShengC 语言的 clang 编译器支持源源变换功能，即将`.cbs`文件转换为等价的`.c`文件。
所有权特性仅引入了`_Owned`关键字表示所有权，在源源变换时只会去掉所有的`_Owned`关键字，然后生成相应的`.c`代码。
关于源源变换的详细细节，请参考手册的源源变换章节。

### 3.2. 借用

借用作为毕昇C内存安全特性的重要组成部分，是对所有权的一个补充。前面小节描述了所有权特性(ownership)，对某个资源拥有所有权的主体，有责任释放这个资源。这个小节，将介绍对资源的借用(borrow)。

#### 3.2.1. 特性简介

如果我们只有 ownership 类型，由于函数调用、赋值等操作都会转移所有权，那么代码能力会非常受限。在编程时，我们常常需要表达“对某个资源进行借用”的概念，区别于“拥有某个资源”。正如现实生活中，如果一个人拥有某样东西，你可以从他那里借来，当使用完毕后，也必须要物归原主。

##### 3.2.1.1. 借用的作用

假设我们有这样的一个需求：创建一个文件，并且调用一些操作函数对文件进行读写操作。如果没有借用的概念，调用文件操作函数会导致文件指针所有权的转移，为了使文件指针在函数调用之后仍然可以被使用，我们需要再将所有权返回给调用方：
```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放
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
  // 通过返回值，将所有权转移给调用方，避免所有权转移
  return p;
}

MyFile *_Owned other_operation(MyFile *_Owned p) {
  // some operation
  // 通过返回值，将所有权转移给调用方，避免所有权转移
  return p;
}

int main() {
  MyFile *_Owned p = create_file(0);
  char str[] = "insert str";
  // p的所有权先被移动到 insert_str 中，再通过返回值转移到调用方
  p = insert_str(p, str);
  p = other_operation(p);
  file_safe_free(p);
  return 0;
}
```
这种写法会造成文件指针所有权的频繁转移，在代码逻辑较为复杂的时候很容易出错，而且如果所有权被转移走了但是没有归还，后续将无法再使用该文件指针。有了借用，将对文件指针的借用作为参数传递给操作函数，函数返回之后文件指针仍可以用于后续其他操作，不再需要像上面那个例子一样，先通过函数参数传入所有权，然后再通过函数返回来传出所有权，代码更加简洁：
```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放
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
  // 无需返回所有权
}

void other_operation(MyFile *_Borrow p) {
  // some operation
  // 无需返回所有权
}

int main() {
  MyFile *_Owned p = create_file(0);
  char str[] = "insert str";
  // 所有权不会被移动
  insert_str(&_Mut * p, str);
  // 所有权不会被移动
  other_operation(&_Mut * p);
  file_safe_free(p);
  return 0;
}
```

示例中的`_Borrow`指针与`&_Mut`借用操作符将在下面的[3.2.1.2 借用的定义及借用操作符](#3212-借用的定义及借用操作符)中介绍。

##### 3.2.1.2. 借用的定义及借用操作符

毕昇 C 的**借用是一个指针类型，指向了被借用对象存储的内存地址**。为表达借用的概念：
1. 引入新关键字`_Borrow`，用`_Borrow`来修饰指针类型 T*，表示 T 的借用类型。`_ArrayElem`也可以与`_Borrow`一并修饰指针类型 T*, 表示对 T 数组成员的借用类型。
2. 引入借用操作符 `& _Mut` 和 `& _Const`，其中，`& _Mut e`表示获取对表达式 e 的**可变借用**，`& _Const e`表示获取对表达式`e`的**只读借用**。此处要求表达式`e`是 lvalue（左值）与标准 C 中的取地址操作符`&`类似, 借用操作符实际上获取的是表达式`e`的地址。

对于两个借用操作符，`&` 与 `_Mut`, `_Const` 之间可以有空格也可以没有。为了便于阅读，本手册中所有借用操作符都用没有空格的形式 `&_Mut` 和 `&_Const` 表示。

例如，我们可以创建指向局部变量`local`的可变借用`p1`和不可变借用`p2`，并使用它们：
```C
void use_immut(const int *_Borrow p) {}
void use_mut(int *_Borrow p) {}

void foo() {
  int local = 5;
  // p1是local的可变借用指针
  int *_Borrow p1 = &_Mut local;
  use_mut(p1);
  // p2是local的不可变借用指针
  const int *_Borrow p2 = &_Const local;
  use_immut(p2);
}

int main() {
  foo();
  return 0;
}
```
另外，表达式`e`如果是指针的解引用表达式(`*p`)，则`&_Mut *p`和`&_Const *p`分别可以看作对`p`指向的对象(也就是`*p`)取可变借用和不可变借用。`p`可以是裸指针、`_Owned`指针和其它借用指针。**这样的取借用操作不对`*p`进行求值**, 例如`p`为空指针时不发生空指针解引用、`p`为`_Owned`指针时不发生所有权转移。
```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

void foo() {
  int *x1 = malloc(sizeof(int));
  *x1 = 1;
  // p1借用了*x1
  int *_Borrow p1 = &_Mut * x1;
  int *_Owned x2 = safe_malloc(2);
  // p2借用了*x2
  int *_Borrow p2 = &_Mut * x2;
  int local = 3;
  int *_Borrow x3 = &_Mut local;
  // p3借用了*x3
  int *_Borrow p3 = &_Mut * x3;
  safe_free((void *_Owned)x2);
}

int main() {
  foo();
  return 0;
}
```

如果表达式 `e` 是数组下标表达式 `arr[i]`，则 `&_Mut arr[i]` 与 `&_Const arr[i]` 的结果类型为 `T *_Borrow _ArrayElem` 或 `const T *_Borrow _ArrayElem`，它们可以分别隐式转换为 `T *_Borrow` 或 `const T *_Borrow`。详细规则见 [3.3.1.3 节](#3313-_borrow-_arrayelem-指针)。

#### 3.2.2. 借用变量和被借用对象的生命周期

##### 3.2.2.1. 生命周期及其作用

我们可以对不同种类的对象取借用：`_Owned`指针、局部变量、全局变量、参数等，或是结构体的某个成员等。为了避免取借用得到的指针悬垂、导致程序使用本不该使用的数据，我们引入生命周期的概念来表示借用变量和被借用对象的有效作用域，并对两者的生命周期做检查。3.2.2的余下章节将详细介绍生命周期的具体规则。

以下 C 代码就是一个使用了悬垂指针的典型例子：

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
这段 C 代码中`p`指向了内部block中的`local` 变量，但是`local`会在block结束的时候被释放，因此回到外部block后，`p`会指向一个无效的地址，是一个悬垂指针，它指向了提前被释放的变量`local`。可以预料到，`*p = 1` 会导致该段程序运行时出现未定义行为（undefined behavior）。当代码逻辑较为复杂时，这类异常行为很难被发现。

接下来我们使用毕昇 C 的借用特性改写上面的 C 代码：
```C
int main() {
  int local1 = 1;
  // 借用指针变量 p 必须使用前被初始化，否则会报错
  int *_Borrow p = &_Mut local1;
  {
    int local2 = 2;
    // 对 p 进行再赋值之后，p 不再借用 local1，而是借用 local2
    p = &_Mut local2;
  } // local2 的生命周期到这里结束
  *p = 3; // error: local2 的生命周期不够长
  return 0;
}
```

为了避免悬垂指针的问题，毕昇 C 规定：任何一个对资源的借用，都不能比资源的所有者的生命周期长。也就是说：借用变量的生命周期，不能比被借用对象的生命周期长。在这个例子中，对`*p`的解引用要求`p`的生命周期覆盖`*p = 3`这一行，而被借用对象`local2`的生命周期在上一行作用域结束时就结束了。因此毕昇C通过检查借用变量和被借用对象的生命周期，在编译期就可以识别出这样的潜在安全风险。

##### 3.2.2.2. 借用变量和被借用对象

每个借用变量（也就是 _Borrow 指针变量）都会有一个或多个被借用对象，由以下规则决定：

1. 对于取借用操作(`&_Mut e`, `&_Const e`)得到的借用指针，除规则 2-5 所列的情形外，被借用对象即为`e`。
2. 对于`&_Mut *p`, `&_Const *p`（`p`为`_Borrow`指针），被借用对象即为`p`的被借用对象。
3. 对于`&_Mut *p`, `&_Const *p`（`p`为`_Owned`指针或裸指针），被借用对象为`p`所指向的对象。
4. 对于 `&_Mut arr[i]` / `&_Const arr[i]`（`arr` 为数组 lvalue），被借用对象为 `arr[]`（整个数组）。
5. 对于 `&_Mut p[i]` / `&_Const p[i]`（`p`为`_Owned _ArrayElem`指针，`_Borrow _ArrayElem`指针，或者裸指针），被借用对象为`p`所指向的对象所在的整个数组。
6. 如果在不同分支、循环语句中，借用指针有不同的被借用对象，则在这些分支、循环语句结束后的执行路径交汇处，借用指针的被借用对象是每条执行路径上最后的被借用对象集合的并集。
7. 对于借用指针类型的函数形参，被借用对象为调用点传递过来的被借用对象。
8. 对于借用指针类型的函数返回值，该函数的形参中应有一个或多个借用类型的参数，返回值的被借用对象是所有借用类型参数的被借用对象的并集。

```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct S {
  int a;
};

int *_Borrow bar(int *_Borrow, int *_Borrow);

int g = 5;
void foo(int a, int *_Owned b, int *c, struct S d) {
  // 被借用对象是普通局部变量
  int local = 5;
  int *_Borrow p1 = &_Mut local; // p1的被借用对象是local
  int *_Borrow p2 = &_Mut * p1;  // p2的被借用对象是local
  int *_Borrow p3 = p1;         // p3的被借用对象是local

  // 被借用对象是owned变量
  int *_Owned x1 = safe_malloc<int>(2);
  int *_Borrow p4 = &_Mut * x1; // p4的被借用对象是*x1

  // 被借用对象是裸指针变量
  int *x2 = malloc(sizeof(int));
  int *_Borrow p5 = &_Mut * x2; // p5的被借用对象是*x2

  // 被借用对象是结构体的某个字段
  struct S s = {.a = 5};
  int *_Borrow p6 = &_Mut s.a; // p6的被借用对象是s.a

  // 被借用对象是函数的返回值，与被调用函数的借用类型入参的 “被借用对象” 一样
  int local1 = 10, local2 = 20;
  // 被调用函数bar有两个借用类型入参，因此p7的被借用对象是local1和local2
  int *_Borrow p7 = bar(&_Mut local1, &_Mut local2);

  // 被借用对象是全局变量
  const int *_Borrow p8 = &_Const g; // p8的被借用对象是g

  // 被借用对象是函数入参
  int *_Borrow p9 = &_Mut a;    // p9的被借用对象是a
  int *_Borrow p10 = &_Mut * b; // p10的被借用对象是*b
  int *_Borrow p11 = &_Mut * c; // p11的被借用对象是*c
  int *_Borrow p12 = &_Mut d.a; // p12的被借用对象是d.a

  // 被借用对象是整个数组 arr[]
  int arr[8] = {0};
  int *_Borrow _ArrayElem p14 = &_Mut arr[2]; // p14 的被借用对象是 arr[]

  // 分支、循环语句后执行路径交汇处，被借用对象取并集
  {
    int local3 = 1;
    int local4 = 2;
    int *_Borrow p13;
    if (a > 0) {
      p13 = &_Mut local3; // p13的被借用对象是local3
    } else {
      p13 = &_Mut local4; // p13的被借用对象是local4
    }
    *p13 = 3; // p13的被借用对象是local3和local4
  }

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

##### 3.2.2.3. 表达式的生命周期

一个变量的生命周期从它的声明开始，到当前整个语句块结束，这个设计被称为 Lexical Lifetime，因为变量的生命周期是严格和词法中的作用域范围绑定的。对于不是借用变量的表达式（尤其是作为被借用对象时），其生命周期是 Lexical Lifetime。我们给出具体的定义：

| 表达式种类 | | 生命周期定义 |
| ---- | ---- | ---- |
| 全局变量 | | 全局变量的生命周期是整个程序，从程序开始到退出，一直存在 |
| 局部变量 | owned变量 | 从变量定义开始，到它被 move 走结束（_Owned struct类型如果没有被move，生命周期会在当前block结束的时候结束）
| | 非owned非borrow变量 | 从变量定义开始，到当前 block 结束 |
| 局部字面量 | "string literal" | 从使用处开始，到当前 block 结束的时候结束 |
| | (struct S) { ... } | 从使用处开始，到当前 block 结束的时候结束
| e->field | | *e 的生命周期 |
| e.field | | e 的生命周期 |
| e[index] 或 *e (e是数组) | | e 的生命周期 |
| e[index] 或 *e (e是指针) | e是`_Owned`指针 | e的生命周期。`_Owned`指针`e`所指的对象`*e`的生命周期与`e`相同 |
|  | e是`_Borrow`指针 | e 的被借用对象的生命周期 |
|  | e是裸指针 | 全局生命周期[1] |

[1] 毕昇C不会分析裸指针的实际指向对象、不会根据其实际指向对象来约束生命周期。例如：

```C
void f1() {
  int a = 1;
  int *p = &a;
  int *_Borrow p1 = (int *_Borrow)(int *)(&_Mut *p); // 不分析p1的被借用对象(*p)
  int *_Borrow p2 = &_Mut *&a; // 不分析p2的被借用对象(a)
}
```

对于借用变量，Lexical Lifetime 过于保守了，某些情况下借用变量的作用范围被过度拉长了，以至于某些实质上是安全的代码也被阻止了，这在一定程度上限制了程序员能编写出的代码。因此，毕昇 C 为借用变量引入 Non-Lexical Lifetime（简写为NLL），用更精细的手段计算借用变量真正起作用的范围，**借用变量的 NLL 范围为：从借用处开始，一直持续到最后一次使用的地方**。具体的，它是**从借用变量定义或被再赋值开始，到被再赋值之前最后一次被使用结束**。借用变量在被赋值后会开始一个新的生命周期。

其中，以下场景属于对借用变量 `p` 的使用：
1. 函数调用，如 `use(p)`
2. 函数返回 `return p`
3. 解引用 `*p`
4. 成员访问 `p->field`
5. 赋值 `q = p`、取借用 `&_Mut *p` / `&_Const *p`、类型转换
6. 若 `p` 的类型为 `T *_Borrow _ArrayElem`：下标 `p[i]`、指针算术 `p + n` / `p += n` / `p - n` / `p -= n`、自增自减 `++p` / `p++` / `--p` / `p--`、指针相减 `p - q`（`q` 为同类型的 `_Borrow _ArrayElem`）

举例来说：
```C
void use(int *_Borrow p) {}
void other_op() {}

// 本例中p的NLL是分段的，每段NLL都有一个被借用对象
void foo() {
  int local1 = 1, local2 = 2;  //#1
  int *_Borrow p = &_Mut local1; //#2，p的第一段NLL开始，被借用对象为local1
  other_op();                  //#3
  use(p);                      //#4，p的第一段NLL结束
  other_op();                  //#5
  p = &_Mut local2;             //#6，p的第二段NLL开始，被借用对象为local2，由于后面没有再对p的使用，p的NLL结束
  other_op();     //#7
}
// p的NLL是：[2,4]->local1, [6,6]->local2

int main() {
  foo();
  return 0;
}
```

##### 3.2.2.4. 借用的生命周期约束

对于借用，我们有这样的生命周期约束：**借用变量的生命周期，不能比被借用对象的生命周期长**。如果一个借用有多个被借用对象，取生命周期最短的那个进行比较。

举例来说：
```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

void use(int *_Borrow p) {}
int *_Borrow call(int *_Borrow p, int *_Borrow q) { return p; }

// 本例中，p的生命周期为[2,4]，被借用对象local的生命周期为[1,4]，满足生命周期约束
void test1() {
  int local = 5;              //#1
  int *_Borrow p = &_Mut local; //#2
  use(p);                     //#3
} //#4

// 本例中，p 的生命周期有两段：
// ok: 第一段为 [2,2]，被借用对象 local1 的生命周期为 [1,8]，满足生命周期约束
// error: 第二段为 [5,7]，被借用对象 local2 的生命周期为 [4,6]，不满足生命周期约束
void test2() {
  int local1 = 5;              //#1
  int *_Borrow p = &_Mut local1; //#2
  {                            //#3
    int local2 = 5;            //#4
    p = &_Mut local2;           //#5
  }                            //#6
  use(p);                      //#7
} //#8

// 本例中p的生命周期有两段：
// ok: 第一段为[2,2]，被借用对象local1的生命周期为[1, 8]，满足生命周期约束
// error: 第二段为[5,7]，被借用对象有两个，分别是local1和local2，其中local2的生命周期为[4, 6]，不满足生命周期约束
void test3() {
  int local1 = 5;                       //#1
  int *_Borrow p = &_Mut local1;          //#2
  {                                     //#3
    int local2 = 5;                     //#4
    p = call(&_Mut local1, &_Mut local2); //#5
  }                                     //#6
  use(p);                               //#7
} //#8

// 本例中，if分支对p进行了重新赋值，在#10
// error: use(p)时，p的被借用对象local2的生命周期已经结束，不满足生命周期约束
// ok: else 分支满足生命周期约束
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

//本例中，p的生命周期为[2,4]，被借用对象*x的生命周期为[1,3]，不满足生命周期约束，error
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

当从一个借用指针创建出一个新的借用指针，我们称该操作为 reborrow （再借用），该操作也是借用的一种，同样需要满足上述生命周期要求。

#### 3.2.3. 可变借用和不可变借用

毕昇 C 将借用指针的权限进行了分级，分为可变（mut）借用和不可变（immut）借用，我们可以通过操纵可变借用指针来读写被借用对象的内容，通过不可变借用指针，我们只能读取被借用对象的内容，但是不能修改它。例如：
```C
// 可变借用指针类型为 T *_Borrow
void use_mut(int *_Borrow p) {
  // 通过可变借用指针，可以修改被借用对象的值
  *p = 5;
  // 通过可变借用指针，可以读取被借用对象的值
  int a = *p;
}

// 不可变借用指针类型为 const T *_Borrow
void use_immut(const int *_Borrow p) {
  *p = 5; // error: 无法通过不可变借用指针，来修改被借用对象的值
  // 通过不可变借用指针，可以读取被借用对象的值
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

##### 3.2.3.1. `&_Mut e`要求 e 是可修改的

`&_Mut e`和`&_Const e`要求表达式 e 是 lvalue，即 e 是可以被取地址的，对于可变借用表达式`&_Mut e`，我们还要求 e 是可变的，具体规则按照下列情况递归：
| lvalue表达式 | 是否可修改 |
| ---- | ---- |
| ident | 变量 ident 没有被 const 修饰，且ident 不能是函数名 |
| "string literal" | 不允许，因为字符串常量保存在常量区，不能写。尝试对字符串字面量进行可变借用（如`&_Mut "hello"`或`&_Mut * "hello"`）会导致编译错误 |
| (struct S) { ... } | 允许 |
| e->field | 要求 e 是可变借用指针，或者是指向可修改类型（无 const 修饰）的 _Owned 指针，或者是指向可修改类型的裸指针，且field没有被const修饰，多级 field 的情况应该要求每一级的 field 都没有 const 修饰 |
| e.field | 要求 e 是可变的，且field没有被const修饰，多级 field 的情况应该要求每一级的 field 都没有 const 修饰 |
| e[index] 或 *e (e是数组) | 要求 e 是可变的 |
| e[index] 或 *e (e是指针) | 要求 e 是可变借用指针，或者是指向可修改类型的 _Owned 指针，或者是指向可修改类型的裸指针 |

##### 3.2.3.2. 可变借用同时只能存在一个

如果有两个或更多的指针同时访问同一数据，并且至少有一个指针被用来写入数据，可能会导致未定义行为。由于借用本质上也是指针，所以为了避免上述问题，毕昇 C 规定，**同一时刻，对于同一个对象，要么只能拥有一个可变借用, 要么任意多个不可变借用**。
```C
void write(int *_Borrow p) {}
void read(const int *_Borrow p) {}

void test1() {
  int local = 1;
  int *_Borrow p1 = &_Mut local;
  int *_Borrow p2 = &_Mut local; // error: 同一时刻最多只能有一个指向local的可变借用变量
  write(p1);
  write(p2);
}

void test2() {
  int local = 1;
  int *_Borrow p1 = &_Mut local;
  const int *_Borrow p2 = &_Const local; // error: 指向local的可变和不可变借用不能同时存在
  write(p1);
  read(p2);
}

void test3() {
  int local = 1;
  const int *_Borrow p1 = &_Const local;
  int *_Borrow p2 = &_Mut local; // error: 指向local的可变和不可变借用不能同时存在
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

如果同时存在对一个变量的可变借用和不可变借用，可能会出现通过可变借用修改被借用对象的内存状态，然后再使用不可变借用访问被修改的内存，从而导致未定义行为的情况。
例如：

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
  // q借用了a.p
  const int *_Borrow q = a.get_p();
  // a.p指向的内存被释放
  a.free_p(); // error: a 被可变借出了多次
  // *q的操作可能会导致未定义行为
  printf("%d\n", *q);
  return 0;
}
```

上述代码中，`a.free_p()`实际上使用了一个指向 a 的可变借用，该可变借用会使在它之前被定义的借用 q 失效，由于`printf("%d\n", *q)`使用了失效的 q，毕昇 C 编译器会报错，也就阻止了不安全行为的发生。

对数组取可变借用时（如 `&_Mut arr[i]`）， 借用的互斥范围包括整个数组 `arr[]`：在某一可变借用的 NLL 有效期内，不能同时对 `arr` 再取第二个可变借用，也不能通过冻结的数组名或借用读写 `arr` 的其它元素。

由于不可变借用不会导致被借用对象被修改，因此同一时刻可以拥有任意多个不可变借用，例如：
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

#### 3.2.4. 借用对被借用对象的影响

##### 3.2.4.1. 不可变借用对被借用对象的影响

对表达式 e 做不可变借用， 即`&_Const e`，表达式 e 进入 "只读" 状态。在这个不可变借用的生命周期结束之前，e 只能读不能修改，也不能被取可变借用。如果 e 是一个数组，则数组的所有元素都进入只读状态。

| 不可变借用表达式 | 被借用对象的状态 |
| ---- | ---- |
| &_Const ident | 变量 ident 进入 “只读” 状态 |
| &_Const "string literal" | 临时变量永远是 “只读” 状态 |
| &_Const (struct S) { ... } | 临时变量进入 “只读” 状态 |
| &_Const e->field | e->field 进入 “只读” 状态，也不允许整体修改 *e。但允许修改 e 指向的其它成员，或者对其它成员做可变借用 |
| &_Const e.field | e.field 进入 “只读” 状态，也不允许整体修改 e。但允许修改 e 的其它成员，或者对其它成员做可变借用 |
| &_Const e[index] 或 &_Const *e (e是数组) | e 进入 “只读” 状态 |
| &_Const e[index] 或 &_Const *e (e是指针) | e 指向的对象进入 “只读” 状态。如果 e 是 _Owned 指针类型，则 e 也进入只读状态。如果 e 是 _Borrow 指针类型（即 reborrow 的情况）或裸指针，则允许修改 e 的指向，且修改指向后 e 的可读写属性恢复到 reborrow 之前。 |

##### 3.2.4.2. 可变借用对被借用对象的影响

对表达式 e 做可变借用， 即`&_Mut e`，表达式 e 进入 "冻结" 状态。在这个可变借用的生命周期结束之前，e 不能读，不能修改（包含被move），也不能被借用。如果 e 是一个数组，则数组的所有元素都被冻结。
| 可变借用表达式 | 被借用对象的状态 |
| ---- | ---- |
| &_Mut ident | 变量 ident 被冻结 |
| &_Mut "string literal" | 编译错误（字符串字面量是不可变的，不能进行可变借用） |
| &_Mut (struct S) { ... } | 临时变量被冻结 |
| &_Mut e->field | e->field 被冻结，不允许读写 e->field，不允许整体修改 *e，但允许修改 e 指向的其它成员，或者对其它成员做可变借用 |
| &_Mut e.field | e.field 被冻结，不允许读写 e.field，不允许整体修改 e，但允许修改 e 的其它成员，或者对其它成员做可变借用 |
| &_Mut e[index] 或 &_Mut *e (e是数组) | e 被冻结 |
| &_Mut e[index] 或 &_Mut *e (e是指针) | e 指向的对象被冻结。如果 e 是 _Owned 指针类型，则也不允许读写 e；如果 e 是 _Borrow 指针类型（即 reborrow 的情况）或裸指针，则允许修改 e 的指向，且修改指向后可以对 *e 及其直接或间接成员进行读写、取借用。 |

#### 3.2.5. 函数原型中包含借用类型

1. 如果函数返回类型是借用类型，那么函数必须有借用类型的形参。

2. 如果函数参数中有一个借用类型的参数，函数返回是借用类型，那么我们直接认为这个返回值的借用，是来自于这个借用类型参数，返回的借用的 “被借用对象” 与这个借用类型参数的 “被借用对象” 一样，这个返回的借用也应该满足前面提到的那些借用规则。

3. 如果函数参数中有多个借用类型的参数，函数返回是借用类型，那么我们直接认为这个返回值的借用，其“被借用对象”是所有借用类型参数的 “被借用对象”的并集，这个返回的借用也应该满足前面提到的那些借用规则。

4. 在函数定义内，对于借用指针类型的形参，其借用对象为调用点传递过来的“被借用对象”，这些对象的生命周期覆盖整个函数。

```C
int *_Borrow f1(int *_Borrow p) { return p; }
int *_Borrow f2(int *_Borrow p1, int *_Borrow p2) { return p1; }

void test() {
  int local = 5;
  int *_Borrow p1 = f1(&_Mut local);
  /* 函数 f1 的参数创建了一个对 local 的可变借用，这个借用被传递给了返回值 p1，
     导致 p1 相当于是对 local 的一个可变借用, 所以返回值 p1 的被借用对象是
     local, 在 p1 的生命周期结束之前，local 会一直被冻结。*/

  int local1, local2;
  int *_Borrow p2 = f2(&_Mut local1, &_Mut local2);
  /* 函数 f2 的参数创建了一个对 local1 和 local2
     的可变借用，这两个借用被传递给了返回值 p2， 导致 p2 相当于是对 local1 和
     local2 的一个可变借用, 所以返回值 p2 的被借用对象是 local1 和 local2, 在 p2
     的生命周期结束之前，local1 和 local2 一直被冻结。*/
}

int main() {
  test();
  return 0;
}
```

#### 3.2.6. struct定义中包含借用类型

如果结构体包含借用类型的成员，那么毕昇C会为该结构体整体计算 NLL。访问结构体的成员时，如果该成员是借用类型，那么它的生命周期是整个结构体的生命周期。该结构体的被借用对象是所有借用类型成员的被借用对象的并集，该结构体的生命周期不能比任意一个被借用对象的生命周期长。

```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct R {
  int *_Borrow m1;
  int *_Borrow m2;
};

void test() {
  int local1, local2;
  struct R r = {.m1 = &_Mut local1, .m2 = &_Mut local2};
  // 在 r 的生命周期结束之前，local1 和 local2 一直被冻结。
  // 因为变量 r 在初始化的时候创建了一个对 local1 和 local2 的可变借用，
  // 导致 r 同时包含对 local1 的一个可变借用，也包含对 local2 的可变借用。
}

int main() {
  test();
  return 0;
}
```

#### 3.2.7. 借用变量的解引用操作

允许对借用指针变量解引用，与标准 C 的解引用操作一致：对借用指针变量`p`解引用的语法为 `*p`。

对于 `T * _Borrow` 或 `const T * _Borrow` 类型的借用变量 `e`，其解引用 `*e` 的类型为 `T`。

如果`p`是指向`T`类型的借用，`o`是`T`类型的 lvalue，对于`*p`表达式，有如下限制：
| | T是Copy语义 | T是Move语义 |
| ---- | ---- | ---- |
| p是immut借用 | *p = expr; 不允许 | *p = expr; 不允许 |
| | o = *p; 允许 | o = *p; 不允许|
| p是mut借用 | *p = expr; 允许 | *p = expr; 不允许 |
| | o = *p; 允许 | o = *p; 不允许|

上表中 move / copy 语义分别指： `T` 是 `_Owned` 修饰的类型（包括 `_Owned` 指针与 `_Owned struct`）和 `T` 是其它类型。

注：上表中的赋值操作的权限，可同样应用于函数的传参和返回场景。

#### 3.2.8. 借用变量的成员访问

允许借用指针变量访问成员变量或调用成员函数，与标准 C 的箭头运算符一致：访问指针变量`p`的成员变量`field`的语法为`p->field`，调用指针变量`p`的成员方法`method()`的语法为`p->method()`。

##### 3.2.8.1. 访问成员变量

通过借用访问成员变量，表达式的类型取决于成员变量本身的类型。`p->field` 表达式的类型与`field`成员定义的类型相同。
如果 `p->field` 的类型是`T` 且没有 const 修饰，`o`是`T`类型的 lvalue，对于 `p->field` 表达式，有如下限制：
| | T是Copy语义 | T是Move语义 |
| ---- | ---- | ---- |
| p是immut借用 | p->field = expr; 不允许 | p->field = expr; 不允许 |
| | o = p->field; 允许 | o = p->field; 不允许|
| p是mut借用 | p->field = expr; 允许 | p->field = expr; 不允许 |
| | o = p->field; 允许 | o = p->field; 不允许|

上表中 move / copy 语义分别指： `T` 是 `_Owned` 修饰的类型和 `T` 是其它类型。如果 `p->field` 有 const 修饰，则 `p->field = expr;` 也不允许。

注：上表中的赋值操作的权限，可同样应用于函数的传参和返回场景。

##### 3.2.8.2. 调用成员函数

通过借用调用成员函数，即 `p->method()` 的场景，实参和形参之间的规则如下：

|           | void method(const This * _Borrow this) | void method(This * _Borrow this) |     |
| --------- | ------------------------------------- | ------------------------------- | --- |
| p是immut借用 | 允许                                    | 不允许，immut借用不能创建出mut借用           |     |
| p是mut借用   | 允许，允许从mut借用创建immut借用                  | 允许                              |     |

举例来说：
```C
void int ::method1(const This *_Borrow this) {}
void int ::method2(This *_Borrow this) {}

void test() {
  int local = 5;
  const int *_Borrow p1 = &_Const local;
  int *_Borrow p2 = &_Mut local;
  p1->method1(); // ok: 形参类型和实参类型一致，都是不可变借用
  p1->method2(); // error: 形参是可变借用类型，实参是不可变借用类型，不可变借用不能创建出可变借用
  p2->method1(); // ok: 形参是不可变借用类型，实参是可变借用类型，允许从可变借用创建出不可变借用
  p2->method2(); // ok: 形参类型和实参类型一致，都是可变借用
}

int main() {
  test();
  return 0;
}
```

#### 3.2.9. 借用的类型转换

1. 对于任意类型 T，如果 T 实现了 _Trait TR，则允许指向类型 T 的借用向上转型为指向类型 TR 的借用；反过来，从类型 TR 的借用往类型 T 借用的转换，是不允许的。
```C
#include <stdio.h>

_Trait TR { void print(This * _Borrow this); };
void int ::print(int *this) { printf("%d\n", *this); }

_Impl _Trait TR for int;

void test() {
  int x = 10;
  int *_Borrow r = &_Mut x;
  _Trait TR *_Borrow p = r; // ok: 支持 int* 类型的借用向上转型为 _Trait TR* 类型的借用
  p->print();
  int *_Borrow px = (int *_Borrow)p; // error: 禁止 _Trait TR* 向下转型
}

int main() {
  test();
  return 0;
}
```

2. 允许指向 T 的借用隐式转换为指向 void 类型的借用（非安全区总是允许，安全区要求 `T` 满足 `is_trivial_data`），反过来从类型 void 的借用往类型 T 借用的转换只能在非安全区显式进行。其他指向类型不同的借用指针之间的类型转换均不允许。

```C
void test() {
  int x = 10;
  int *_Borrow r = &_Mut x;
  void *_Borrow p = r; // ok: 允许 T *_Borrow 隐式转换为 void* _Borrow
  int *_Borrow t1 = p; // error: 不允许隐式转为 T *_Borrow 类型
  int *_Borrow t2 = (int *_Borrow)p; // ok：允许强制转换为 T *_Borrow 类型
}

int main() {
  test();
  return 0;
}
```

3. 只允许在非安全区进行 `T * _Borrow` 和 `T *` 之间的转换。从 `T * _Borrow p` 转到 `T * q` 是对 `p` 的一次使用（影响 `p` 的 NLL 计算），后续对 `q` 的操作不影响 `p` 的 NLL 计算。从 `T * q` 转到 `T * _Borrow p` 等效于 `p = &_Mut *q` (当类型 `T` 带 `const` 修饰时等效于 `p = &_Const *q`)。
```C
int main() {
  int *_Borrow p = (int *_Borrow)NULL; // ok: 非安全区允许 T * _Borrow 和 T * 之间的转换
  int *q = p; // error: 类型转换必须是显式的，禁止隐式类型转换
  _Safe { int *_Borrow p = (int *_Borrow)NULL; } // error: 安全区禁止 T * _Borrow 和 T * 之间的转换
  return 0;
}
```

4. 不允许在 `T *_Owned` 和 `T *_Borrow` 指针之间使用 C 风格强制类型转换

```C
int *_Owned test(int *_Owned p) {
  int *_Borrow q = (int *_Borrow) p; // error: 应当使用 &_Mut *p 代替强制类型转换
  int *_Owned r = (int *_Owned) q; // error: 不能通过强制类型转换从 T *_Borrow 创建一个 T *_Owned 副本
  return r;
}
```

5. 可变借用 `T *_Borrow` 类型可隐式转换为不可变借用 `const T *_Borrow` 类型，由编译器自动插入 `&_Const *` 操作符。不允许在可变借用和只读借用之间进行强制类型转换

以下场景可发生可变借用到不可变借用的隐式转换：
  1. 变量的初始化与赋值
  2. 函数调用表达式传参
  3. 函数的返回语句

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
  const int *_Borrow b = (const int *_Borrow)p; // error: 不允许将可变借用强制转换为只读借用
  printf("%d\n", *b);  // 读取 b
  *p = 1;              // 修改 p (如果转换被允许,这会违反借用规则)
  printf("%d\n", *b);  // 再次读取 b

  const int *_Borrow c = &_Const local;
  int *_Borrow m = (int *_Borrow)c; // error: 不允许将只读借用强制转换为可变借用
  *m = 20;  // 如果转换被允许,这会违反const安全性

  return 0;
}
```

6. 允许在变量初始化、变量赋值、函数传参和返回、分支判断语句和三元表达式的条件中将 _Borrow 指针隐式转换为 _Bool 使用。不允许在 `switch` 语句的条件中将 _Borrow 指针作为 _Bool 使用。

```c
void f1(int *_Borrow _Nullable p) {
  _Bool flag = p; // ok, 等价于 _Bool flag = p != nullptr;
}
void f2(int *_Borrow _Nullable p, _Bool flag) {
  flag = p; // ok, 等价于 flag = p != nullptr;
}
void use(_Bool);
void f3(int *_Borrow _Nullable p) {
  use(p); // ok, 等价于 use(p != nullptr);
}
_Bool f4 (int *_Borrow _Nullable p) {
  return p; // ok, 等价于 return p != nullptr;
}
void f5(int *_Borrow _Nullable p) {
  if (p) { // ok, 等价于 p != nullptr
  }
  while (p) { // ok, 等价于 p != nullptr
  }
  do {
  } while (p); // ok, 等价于 p != nullptr

  for (;p;) { // ok, 等价于 p != nullptr
  }
  switch (p) { // error
  default:
    break;
  }
  int x = p ? 2 : 1; // ok, 等价于 p != nullptr ? 2 : 1;
}
```

#### 3.2.10. 借用的其它规则

除了上面的那些规则，对于借用，我们还有如下规则：
1. 在安全区内，对全局变量与静态变量只允许取只读借用，不允许取可变借用。这是为了保证安全性，因为对于全局变量与静态变量，我们无法在函数签名中跟踪哪个函数读取了它们，哪个函数修改了它们。

```C
int g;

_Safe void test() {
  const int * _Borrow p1 = &_Const g; // ok: 对全局变量取不可变借用
  int *_Borrow p2 = &_Mut g; // error: 不允许对全局变量取可变借用
}
```

2. 借用变量在使用前必须初始化。详细规则在[3.7. 初始化分析](#37-初始化分析)章节中介绍。
```C
void test() {
  int *_Borrow p; 
  use(p); // error: 必须初始化
}

int main() {
  test();
  return 0;
}
```

3. 不能声明全局变量与静态变量为借用类型。
```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

int g = 5;
int *_Borrow p = &_Mut g; // error: 借用变量不允许是全局变量
void test() { int *_Borrow p = &_Mut g; }

int main() {
  test();
  return 0;
}
```

4. 毕昇C不支持二级借用类型，包含(1)二级指针有借用指针类型嵌套（如`T *_Borrow *_Borrow`），以及(2)借用指针的指向类型是包含借用类型的结构体（如`T *_Borrow`指针，`T`包含`_Borrow`指针成员）。同理，不允许对包含借用类型的表达式再取借用。
```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

struct R {
  int *_Borrow p;
};

void test() {
  int local = 5;
  int *_Borrow p1 = &_Mut local;
  int *_Borrow *_Borrow p2 = &_Mut p1; // error: 不允许多级借用指针

  struct R r1 = {.p = &_Mut local};
  struct R *_Borrow r2 = &_Mut r1; // error: r1 中已经包含了借用
}

int main() {
  test();
  return 0;
}
```

5. 不允许对函数取借用，不允许 `_Borrow` 修饰函数指针类型。

```c
void foo(void) {}
void bar(void) {
  void (*_Borrow p)(void); // error: 不允许 `_Borrow` 修饰函数指针类型
  (void) &_Mut foo; // error: 不允许对函数取借用
  (void) &_Const foo; // error: 不允许对函数取借用
}
```

6. union 的成员不允许是借用类型。
```C
union U {
  int *_Borrow p; // error: 借用指针不允许作为 union 成员
};

int main() { return 0; }
```

7. 借用指针类型不能是泛型实参。

8.  允许对借用类型使用 `sizeof`、`alignof`操作符，并且借用类型的 `sizeof`、`alignof`结果与裸指针类型的结果一致。

9. 没有 `_ArrayElem` 修饰的借用指针不支持下标运算、算术运算。关于 `_Borrow _ArrayElem` 允许的操作（下标、算术、比较等），详见 [3.3.1.3 节](#3313-_borrow-_arrayelem-指针)。

10. 允许相同类型（可以忽略指向类型顶层的 `const` `volatile` `restrict` 修饰符）的借用指针之间，使用比较运算符 `==`、`!=`、`>`、`<`、`>=`、`<=`。

11.  允许对借用类型使用一元的`&`（取地址、获得裸指针）、`!`（判断指针是否为空）及二元的`&&`（逻辑与）、`||`（逻辑或）运算符。

12.  不允许对借用类型使用一元的`-`（取负数）、`~`（按位取反）、`&_Const`、`&_Mut`运算符，也不允许对借用类型使用二元的`&`（按位与）、`|`（按位或）、`<<`（左移）、`>>`（右移）运算符。

### 3.3. 数组与安全指针

数组和指针运算是系统编程语言中重要的语言特性，广泛使用于各类程序中。但是 C 语言中指针类型不区分指向数组元素的指针与指向单个对象的指针，因此 C 无法阻止对指向单个对象的指针进行指针运算。这会导致一些本可以在语言层面发现并阻止的越界行为漏到运行时。

毕昇C 引入了 `_ArrayElem` 关键字用于表达指向数组元素的安全指针。没有 `_ArrayElem` 修饰的安全指针不能进行指针运算与取下标 `[]`。有了 `_ArrayElem`，毕昇C 中所有指向数组的安全指针都可以在类型上与指向单个对象的指针区分开，阻止用户误用指针运算导致越界。

除此之外，毕昇C 将 C 的数组退化规则进行了修改，使数组在使用过程中可少写显式取借用与类型转换，改善其易用性。

#### 3.3.1. 指向数组元素的安全指针

毕昇C 引入了 `_ArrayElem` 关键字作为类型限定符，表明所标注的指针指向数组元素。
`_ArrayElem` 必须加在已有 `_Owned` 或 `_Borrow` 标注的指针上，不能加在裸指针上，也不能加在非指针类型上。
`_ArrayElem` 与 `_Owned`/`_Borrow` 的相对顺序无关，可以加在 `_Owned`/`_Borrow` 前面，也可以加在后面。本手册所有示例都使用将 `_ArrayElem` 后置的写法。

```c
int * _Owned _ArrayElem p1; // ok, _Owned _ArrayElem 指针是指向数组的带所有权的指针
int * _Borrow _ArrayElem p2; // ok, _Borrow _ArrayElem 指针是指向数组元素的借用指针
int * _ArrayElem p3; // error, _ArrayElem 不能加在裸指针上
```

##### 3.3.1.1. 指向类型限制

对于 `T * _Owned _ArrayElem` 与 `T* _Borrow _ArrayElem` 指针，类型 `T` 以及（对结构体来说）其所有递归展开后的成员不能是安全指针或 `_Owned struct`。

```c
int * _Borrow _ArrayElem p1 = ...; // ok, T = int
int * _Borrow * _Borrow _ArrayElem p2 = ...; // error, T = int * _Borrow
int * _Owned * _Borrow _ArrayElem p3 = ...; // error, T = int * _Owned
```

该规则可理解为：只有当 `T[]` 与 `T * _Borrow` 都是毕昇C支持的类型时，毕昇C才支持 `T* _Owned _ArrayElem` 与 `T* _Borrow _ArrayElem` 类型。由于毕昇C目前不支持在数组内跟踪元素的所有权、不支持二级借用，这些限制也作用于 `_ArrayElem`。

举例：`T` 不能包含 `_Owned` 指针：
```c
typedef struct {
  int * _Owned ptr;
} T;
T a[10]; // error
T * _Borrow b; // ok
T * _Owned _ArrayElem ptr; // error
T * _Borrow _ArrayElem ptr; // error
```

举例：`T` 不能包含 `_Borrow` 指针：
```c
typedef struct {
  int * _Borrow ptr;
} T;
T a[10]; // ok
T * _Borrow b; // error
T * _Owned _ArrayElem ptr; // error
T * _Borrow _ArrayElem ptr; // error
```

##### 3.3.1.2. `_Owned _ArrayElem` 指针

`_Owned _ArrayElem` 修饰的指针表示指向数组的带所有权的指针。除非明确说明或单独列出 `_Owned _ArrayElem` 的情况，否则 [3.1 节](#31-所有权) 中对 `_Owned` 指针的规则同样适用于 `_Owned _ArrayElem`。

**分配与释放**

`_Owned _ArrayElem` 指针的分配与释放通过 `safe_malloc_array`/`safe_calloc_array` 与 `safe_free_array` 实现：

```c
#include "bishengc_safety.hbs"
// _Safe T *_Owned _ArrayElem safe_malloc_array<T>(size_t size, T initializer);
// _Safe T *_Owned _ArrayElem safe_calloc_array<T>(size_t size);
// _Safe void safe_free_array(void *_Owned _ArrayElem);

_Safe int main(void) {
  int *_Owned _ArrayElem p1 = safe_malloc_array(10, 2); // 分配带有 10 个 int 的数组，每个元素初始化为 2
  int *_Owned _ArrayElem p2 = safe_calloc_array<int>(10); // 分配带有 10 个 int 的数组，每个元素初始化为 0
  safe_free_array((void *_Owned _ArrayElem)p1);
  safe_free_array((void *_Owned _ArrayElem)p2);
}
```

当 `safe_malloc_array`/`safe_calloc_array` 的 `size` 为 0 时，该调用返回一个非空但不能解引用的指针。该指针也需要释放，否则仍视为内存泄漏。

`safe_malloc_array`/`safe_calloc_array` 与 `safe_free_array` 的实现会通过 `malloc`/`calloc` 与 `free` 进行分配、释放，因此由 `safe_malloc_array`/`safe_calloc_array` 分配得到的 `_Owned _ArrayElem` 指针可以在转为裸指针后使用 `free` 释放，由 `malloc`/`calloc` 等分配函数分配得到的裸指针也可以在转为 `_Owned _ArrayElem` 指针后使用 `safe_free_array` 进行释放。

**允许的运算**

| 操作 | `T *_Owned` | `T *_Owned _ArrayElem` |
| ---- | ----------- | ---------------------- |
| 解引用 `*p`、成员访问 `p->field` | 允许 | 允许 |
| 取下标 `p[i]` | 不允许 | 允许 |
| 指针算术运算 `+` `-` `+=` `-=` `++` `--` | 不允许 | 不允许 |
| 比较运算 `==` `!=` `>=` `<=` `>` `<` | 允许 | 允许 |

`_Owned _ArrayElem` 指针可以取下标 `[]`，但是不能进行算术运算。这样可以保证 `safe_free_array` 入参指针是未偏移的，避免出现 invalid free。需要对指针进行算术运算、自增自减时可先取借用生成 `_Borrow _ArrayElem` 指针，再对其进行运算（该规则在 `_Borrow _ArrayElem` 指针部分讨论）。

```c
int * _Owned _ArrayElem p = safe_malloc_array(10, 42); // 分配 10 个 int 元素，初始值为 42
p = p + 10; // error: _Owned _ArrayElem 不能进行算术运算
safe_free_array((void * _Owned _ArrayElem)p); // 上一行报错可避免此处 invalid free
```

**类型转换**

1. `_Owned _ArrayElem` 指针不能转换为其他安全指针类型（包括 `_Owned` 指针与任何借用指针），其他安全指针类型也不能转换为 `_Owned _ArrayElem` 指针。进行 `_Owned _ArrayElem`指针与裸指针之间的转换时不能使用C的强制类型转换语法，需要在非安全区使用以下两个内置函数进行转换：
- `__take_array_from_raw(p)`：将裸指针 `p` 转为 `_Owned _ArrayElem` 指针，并取得其所指数组的所有权。
- `__move_array_to_raw(p)`：将 `_Owned _ArrayElem` 指针转为裸指针并转移所有权。

```c
#include <stdlib.h>

int main() {
  int *raw = (int *)malloc(4 * sizeof(int));
  int *_Owned _ArrayElem arr = __take_array_from_raw(raw); // ok
  int *_Owned plain = (int *_Owned)arr; // error: 不允许从 T *_Owned _ArrayElem 转为 T *_Owned
  int *raw2 = __move_array_to_raw(arr); // ok
  int *_Owned _ArrayElem arr2 = (int *_Owned _ArrayElem)raw2; // error: 应使用 __take_array_from_raw
  free(raw2);
  return 0;
}
```

2. 允许进行 `T * _Owned _ArrayElem` 与 `void * _Owned _ArrayElem` 之间的显式类型转换: `T * _Owned _ArrayElem` 可在安全区显式转为 `void * _Owned _ArrayElem`，但 `void * _Owned _ArrayElem` 只能在非安全区显式转为 `T * _Owned _ArrayElem`。其他指向类型不一致的转换（如 `T * _Owned _ArrayElem` 转 `U * _Owned _ArrayElem`）均不允许。

##### 3.3.1.3. `_Borrow _ArrayElem` 指针

`_Borrow _ArrayElem` 指针表示指向数组元素的借用指针。除非明确说明或单独列出 `_Borrow _ArrayElem` 的情况，否则 [3.2 节](#32-借用) 中对 `_Borrow` 指针的规则同样适用于 `_Borrow _ArrayElem`。

**创建 `_Borrow _ArrayElem` 指针**

除指针类型转换外，`_Borrow _ArrayElem` 指针主要由以下两种方式创建：
1. 对数组下标表达式取借用：`&_Mut a[i]`、`&_Const a[i]`。`a` 可以是任何支持下标运算的类型，包括数组、`_Owned _ArrayElem`/`_Borrow _ArrayElem`指针，非安全区还包括裸指针。
2. 在赋值、函数传参、返回值等场景中，元素类型为 `T` 的数组可退化为 `T *_Borrow _ArrayElem`（见 [3.3.2 数组退化规则拓展](#332-数组退化规则拓展)）。

```c
void foo(int *_Borrow _ArrayElem p);
void bar() {
  int arr[10];
  foo(&_Mut arr[0]); // ok, 显式对数组下标取借用
  foo(arr); // ok, 数组退化为 _Borrow _ArrayElem 指针
}
```

为同时支持 `&_Mut a[i]`、`&_Const a[i]` 表达“取单个元素的借用”（需要`_Borrow`指针）和“取数组中元素的借用”（需要`_Borrow _ArrayElem`指针），对数组下标表达式取借用时固定返回`_Borrow _ArrayElem`指针，且`_Borrow _ArrayElem`指针可隐式转为`_Borrow`指针。详细的类型转换规则见下方“类型转换”部分。

```c
int a[10];
int *_Borrow _ArrayElem p1 = &_Mut a[0]; // ok
int *_Borrow p2 = &_Mut a[0]; // ok，隐式转换到 _Borrow 指针后不允许下标 [] 或指针算术运算
```

**允许的运算**

| 操作 | `T *_Borrow` | `T *_Borrow _ArrayElem` |
| ---- | ------------ | ------------------------ |
| 解引用 `*p`、成员访问 `p->field` | 允许 | 允许 |
| 取下标 `p[i]` | 不允许 | 允许 |
| 指针算术运算 `+` `-` `+=` `-=` `++` `--` | 不允许 | 允许 |
| 比较运算 `==` `!=` `>=` `<=` `>` `<` | 允许 | 允许 |

```c
int foo(int *_Borrow _ArrayElem data, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i) {
    s += data[i]; // ok; 若为 int *_Borrow 则禁止取下标
  }
  return s;
}
```

其他额外规则：
1. `_Borrow _ArrayElem` 指针算术运算后的结果视为对同一数组 reborrow 的借用。

```c
int a[10];
int * _Borrow _ArrayElem p1 = &_Mut a[0];
int * _Borrow _ArrayElem p2 = p1 + 5; // 等效于 &_Mut p1[5]， p2 生命周期开始，p1 冻结
*p2 = 42;
// p2 生命周期结束，p1 解冻
p1[1] = 43; // ok
```

2. 当同一语句多次使用 `[]` 或 `*` 通过 `_Borrow _ArrayElem` 指针访问数组元素时，借用检查应将所有访问视为对原本的指针的使用，不认为产生临时的新借用。

```c
int * _Borrow _ArrayElem p = ...;
p[0] = p[1] + p[2]; // ok，借用检查应将其视为多次使用 p 本身，不报错
```

3. 同类型的 `_Borrow _ArrayElem` 指针之间支持减法运算。

```c
void foo(void) {
  int a[10] = {0};
  const int *_Borrow _ArrayElem p1 = &_Const a[0];
  const int *_Borrow _ArrayElem p2 = &_Const a[5];
  int n = p2 - p1; // ok, n = 5
}
```

4. 允许 `T *_Borrow _ArrayElem` 之间（忽略 `T` 上的 `const`, `volatile`, `restrict`）使用比较运算符 `==`、`!=`、`>`、`<`、`<=`、`>=`。

```c
int foo(void) {
  int a[5] = {1, 2, 3, 4, 5};
  int sum = 0;
  const int *_Borrow _ArrayElem p = &_Const a[0];
  const int *_Borrow _ArrayElem q = &_Const a[4];
  while (p <= q) { // ok
    sum += *p;
    p++;
  }
  return sum;
}
```

**类型转换**

1. `_Borrow _ArrayElem` 指针可以隐式转换为 `_Borrow` 指针，除此之外 `_Borrow _ArrayElem` 指针不能转换为其他安全指针类型。其他安全指针类型（包括 `_Borrow` 指针）不能转换为 `_Borrow _ArrayElem` 指针。
2. 可以在非安全区将 `_Borrow _ArrayElem` 指针显式转换为裸指针，或是从裸指针显式转换为 `_Borrow _ArrayElem` 指针。
3. 安全区支持 `T * _Borrow _ArrayElem` 隐式转为 `void * _Borrow _ArrayElem`，不支持指向类型不同的 `_Borrow _ArrayElem` 指针间的类型转换。非安全区支持任意指向类型不同的 `_Borrow _ArrayElem` 指针之间的显式类型转换，以及  `T * _Borrow _ArrayElem` 隐式转为 `void * _Borrow _ArrayElem`。

#### 3.3.2. 数组退化规则拓展

为了在兼容 C 语言的数组相关规则的基础上让用户便于通过借用指针访问数组元素，毕昇C 在C的基础上拓展数组退化规则，使数组能够在特定情况下隐式退化为借用指针。

注：毕昇C 语言中以下场景涉及数组但不属于数组退化范畴，此类规则与 C 语言一致：
1. 当数组作为函数形参时，函数形参的类型调整会固定将数组转化为裸指针。
    ```c
    void foo(int a[10]); // 一定调整为 void foo(int *a)
    ```
2. 当数组在以下场景使用时，语义是使用数组整体，不发生退化：(1)在 `sizeof()`, `typeof()` 中作为参数, (2) 整体取地址、取借用，(3)作为字符串字面量初始化字符数组。

##### 3.3.2.1. 数组退化为借用指针

如果在赋值、函数传参和函数返回场景使用元素类型为 `T` 的数组，且数组需被转化为以下借用类型之一，则数组会自动退化为该类型：

- `T* _Borrow _ArrayElem` 与 `const T* _Borrow _ArrayElem`
- `T* _Borrow` 与 `const T* _Borrow`
- `void* _Borrow` 与 `const void* _Borrow`（安全区要求 `T` 满足 `is_trivial_data`，否则报错）
- `void* _Borrow _ArrayElem` 与 `const void* _Borrow _ArrayElem`（安全区要求 `T` 满足 `is_trivial_data`，否则报错）

数组`a`退化为借用指针时视为对数组首元素取借用(`&_Mut a[0]`或`&_Const a[0]`)，适用于借用检查已有规则。目标类型不为以上之一时，数组会退化为裸指针 `T*`，详见 [3.3.2.2 节](#3322-数组退化为裸指针) 。

```c
void f1(int *_Borrow _ArrayElem p) {}
void f2(int *_Borrow p) {}
void f3(void *_Borrow _ArrayElem p) {}
void f4(void *_Borrow p) {}

void foo(void) {
  int arr[4] = {1, 2, 3, 4};
  f1(arr); // ok: 数组退化为 T *_Borrow _ArrayElem
  f2(arr); // ok: 数组退化为 T *_Borrow
  f3(arr); // ok: 数组退化为 void *_Borrow _ArrayElem
  f4(arr); // ok: 数组退化为 void *_Borrow
  int *_Borrow _ArrayElem p1 = arr;  // ok: 数组退化为 T *_Borrow _ArrayElem
  int *_Borrow p2 = arr;             // ok: 数组退化为 T *_Borrow
  void *_Borrow _ArrayElem p3 = arr; // ok: 数组退化为 void *_Borrow _ArrayElem
  void *_Borrow p4 = arr;            // ok: 数组退化为 void *_Borrow
}

typedef struct {
  int data[4];
} S;

int *_Borrow _ArrayElem f5(S *_Borrow s) {
  return s->data; // ok: 数组退化为 T *_Borrow _ArrayElem
}

int *_Borrow f6(S *_Borrow s) {
  return s->data; // ok: 数组退化为 T *_Borrow
}

void *_Borrow _ArrayElem f7(S *_Borrow s) {
  return s->data; // ok: 数组退化为 void *_Borrow _ArrayElem
}

void *_Borrow f8(S *_Borrow s) {
  return s->data; // ok: 数组退化为 void *_Borrow
}

typedef struct {
  int * ptr;
} U;

_Safe void f9(U *_Borrow _ArrayElem p) {}
_Safe void f10(U *_Borrow p) {}
_Safe void f11(void *_Borrow _ArrayElem p) {}
_Safe void f12(void *_Borrow p) {}
_Safe void bar(void) {
  U arr[4] = {0};
  f9(arr);  // ok
  f10(arr); // ok
  f11(arr); // error: T 不满足 is_trivial_data
  f12(arr); // error: T 不满足 is_trivial_data
  U *_Borrow _ArrayElem p1 = arr;    // ok: 数组退化为 T *_Borrow _ArrayElem
  U *_Borrow p2 = arr;               // ok: 数组退化为 T *_Borrow
  void *_Borrow _ArrayElem p3 = arr; // error: T 不满足 is_trivial_data
  void *_Borrow p4 = arr;            // error: T 不满足 is_trivial_data
}

typedef struct {
  U data[4];
} U2;

_Safe U *_Borrow _ArrayElem f13(U2 *_Borrow s) {
  return s->data; // ok: 数组退化为 T *_Borrow _ArrayElem
}

_Safe U *_Borrow f14(U2 *_Borrow s) {
  return s->data; // ok: 数组退化为 T *_Borrow
}

_Safe void *_Borrow _ArrayElem f15(U2 *_Borrow s) {
  return s->data; // error: T 不满足 is_trivial_data
}

_Safe void *_Borrow f16(U2 *_Borrow s) {
  return s->data; // error: T 不满足 is_trivial_data
}
```

##### 3.3.2.2. 数组退化为裸指针

当元素类型为 `T` 的数组需要退化为指针但是不满足 [3.3.2.1 节](#3321-数组退化为借用指针) 所描述的情形，则数组会退化为裸指针 `T*`。这条规则使得毕昇C 对数组的处理与 C 语言兼容，保证合法 C 语言代码的语义在毕昇C 中是相同的。

数组在以下场景中会先退化为裸指针，再执行操作：
1. 赋值、函数传参和函数返回场景，但目标类型不为 [3.3.2.1 节](#3321-数组退化为借用指针) 中列举的类型。

    ```c
    _Safe void f1(int * p);
    _Safe void foo(void) {
      int a[10];
      f1(a); // ok
      int * p = a; // ok
    }

    typedef struct {
      int data[4];
    } S;

    _Safe int * f2(S * _Borrow p) {
      return p->data; // ok
    }
    ```

2. 算术运算，数组先退化到裸指针再进行运算，运算结果为裸指针类型。只允许对指针有良好定义的运算（`+,-`）。

    ```c
    int array[10];
    int * p = array + 1;
    int dist = (array + 1) - array; // 1
    ```

3. 比较

    ```c
    int array1[10];
    int array2[10];

    bool eq = array1 == array2;
    bool ne = array1 != array2;
    bool eq2 = array1 == &array1[0];
    ```

4. 作为三目运算(`?:`)的值，以及逗号(`,`)

    ```c
    int array[10];
    int * p1 = (...)? array : nullptr; // 作为 ?: 中真分支/假分支的值
    int * p2 = (... , array); // 作为逗号连接的一个表达式
    ```

5. 作为分支、循环语句和三目运算的条件

    ```c
    int array[10];
    int value = array? 1 : 0;
    if (array) {...}
    if (!array) {...}
    if (array && ...) {...}
    if (array || ...) {...}
    ```

### 3.4. 非空指针

为了提高指针使用的安全性，BSC 引入了指针的可空性（Nullability）概念。

根据是否可以直接解引用使用，可以使用两种属性修饰任意指针类型（裸指针、_Owned 和 _Borrow 指针）。

1. 被关键字 `_Nullable` 修饰的是可空指针。
2. 被关键字 `_Nonnull` 修饰的是非空指针。

BSC 编译器会在编译时检查指针的可空性，避免出现解引用空指针、通过空指针访问成员等不安全行为。

BSC 不允许在安全区内使用宏 `NULL`，但是 C 风格编程不可避免地使用空指针来描述逻辑：

1. 一个指针的指向需要在运行时才能确定，那么我们可以将指针初始化为空指针，在后续再根据运行状态来修改指向
2. 对于可空指针，在使用前需要判断该指针是否为空指针

BSC 引入了 `nullptr` 关键字来替代 `NULL`。
用户可以定义 Nullable 指针，并将其初始化为 `nullptr`。
需要对 Nullable 指针判空才能解引用。

我们用一个简单的例子来学习如何定义指针的 nullabiliy 并使用它：

```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

// 获取当前运行状态的函数
_Safe int get_current_status(void) {
  _Unsafe { return rand() % 2; }
}

_Safe void read_data<T>(T *_Borrow p) {}

struct Data {
  int value;
};

// 如果 init 成功，返回具体的地址，否则返回 nullptr
_Safe struct Data *_Owned _Nullable init_data(void) {
  if (get_current_status() == 1) {
    struct Data data = {10};
    struct Data *_Owned p = safe_malloc<struct Data>(data);
    return p;
  }
  return nullptr;
}

_Safe int main(void) {
  // 使用 _Nullable 修饰指针类型：
  struct Data *_Owned _Nullable p = init_data();
  // init 后 p 可能为空指针，因此需要先判空再使用：
  if (p != nullptr) {
    // 如果没有对 p 做判空，编译器会报 error!
    read_data(&_Mut * p);
    safe_free((void *_Owned)p);
  }
  return 0;
}
```

#### 3.4.1. 编译器可以检查哪些指针的可空性

可以用 `_Nullable` 和 `_Nonnull` 修饰任意指针类型（裸指针、_Owned 和 _Borrow 指针），
但只有指针满足以下条件时，编译器可以对它的可空性进行跟踪，从而保护空指针解引用。

1. 是左值（存在一个内存地址）
2. 不被 `volatile` 修饰（指针自己不是 volatile）
3. 不是通过访问数组得到的指针

不满足以上要求的指针无法通过判空操作修改指针的可空性。如果不可跟踪的指针 `p1` 为 `_Nullable` 且用户需要对其解引用，请先创建一个可跟踪的指针变量 `p2` 并用 `p1` 对其初始化，再对 `p2` 进行判空、解引用。

例子：

```c
// 函数的返回值不是左值
_Safe int *_Borrow _Nullable identity(int *_Borrow _Nullable p) { return p; }

_Safe void test(int *_Borrow _Nullable p, int *_Borrow _Nullable volatile vp) {
  int local = 0;
  int *_Borrow _Nullable q = nullptr;
  int *_Borrow _Nullable arr[2] = {nullptr, &_Mut local};

  if ((q = p) != nullptr) {
    *q = 1; // ok：q 是左值指针，编译器可跟踪其可空性
  }

  if ((0, q) != nullptr) {
    *q = 2; // ok：逗号表达式最终提取到 q，仍可跟踪
  }

  if (identity(p) != nullptr) { // 不是左值，无法更新其可空性
    *identity(p) = 3; // error
  }
  int *_Borrow _Nullable t1 = identity(p); // 应创建可跟踪的临时变量去接
  if (t1 != nullptr) {
    *t1 = 3; // ok
  }

  if (vp != nullptr) { // volatile 指针，无法更新其可空性
    *vp = 4; // error
  }
  int *_Borrow _Nullable t2 = vp; // 应创建可跟踪的临时变量去接
  if (t2 != nullptr) {
    *t2 = 4; // ok
  }

  if (arr[1] != nullptr) { // 指针表达式中有下标运算 '[]'，无法更新指针的可空性
    *arr[1] = 5; // error
  }
  int *_Borrow _Nullable t3 = arr[1];  // 应创建可跟踪的临时变量去接
  if (t3 != nullptr) {
    *t3 = 5; // ok
  }
}
```

#### 3.4.2. 指针类型的 Nullability

指针类型的可空性标记和指针的类型以及指针的修饰符有关：

1. 裸指针是 Nullable 的， `_Owned` 和 `_Borrow` 是 Nonnull 的
2. 可以显式地使用 `_Nonnull` 和 `_Nullable` 修饰符覆盖上述规则

```C
// Nullable 指针：
int *_Nullable p1 = nullptr;
int *_Borrow _Nullable p2 = nullptr;
int *_Owned _Nullable p3 = nullptr;
int *p4 = nullptr;
int (*p5)(int) = nullptr;
// Nonnull 指针：
int *_Nonnull p5 = &a;
int *_Borrow _Nonnull p6 = &_Mut a;
int *_Owned _Nonnull p7 = safe_malloc<int>(5);
int *_Borrow p8 = &_Mut a;
int *_Owned p9 = safe_malloc<int>(5);
```

对于可空性标记为 Nonnull 的指针，它的可空性状态（Nullability）一定是 Nonnull 的。
如果在控制流语句中对其做了判空，那么在空的分支中，其被认为是空指针。

```C
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe int test(void) {
  int * _Owned a = safe_malloc<int>(1); // _Owned 指针默认为 _Nonnull
  if (a != nullptr) {
    safe_free((void * _Owned)a);
    return 1;
  }
  return 0; // ok 不会有 error: memory leak of value: `a`
}

int main() {
  test();
  return 0;
}
```

对于可空性标记为 Nullable 的指针，它的可空性状态（Nullability）可能会发生变化：

1. 如果用非空表达式对其进行了赋值，那么在这条赋值语句之后可空性状态变为 Nonnull
2. 如果在控制流语句中对其做了判空，那么在非空的分支中可空性状态变为 Nonnull

```C
// 返回值类型为 Nullable
_Safe T *_Borrow _Nullable foo<T>(T *_Borrow p) { return p; }
// 返回值类型为 Nonnull
_Safe T *_Borrow bar<T>(T *_Borrow p) { return p; }

_Safe void test(void) {
  int *_Borrow _Nullable p1 = nullptr; // p1 初始化后 Nullability 为 Nullable
  *p1 = 10;                           // error

  int local = 10;
  p1 = &_Mut local;      // p1 被再赋值后 Nullability 变为 Nonnull
  *p1 = 20;             // ok

  p1 = foo(&_Mut local); // p1 被再赋值后 Nullability 变为 Nullable
  *p1 = 20;             // error
  
  p1 = bar(&_Mut local); // p1 被再赋值后 Nullability 变为 Nonnull
  *p1 = 20;             // ok

  int *_Borrow _Nullable p2 = foo(&_Mut local); // Nullable
  if (p2 != nullptr) // if 分支中 p2 的 Nullability 为 Nonnull
    *p2 = 10;        // ok
  else               // else 分支中 p2 的 Nullability 为 Nullable
    *p2 = 20;        // error
}

int main() {
  test();
  return 0;
}
```

#### 3.4.3. 什么是判空语句

在条件语句、循环语句的条件和三元表达式的条件表达式中，只有部分行为可以被视作“判断指针是否为空指针”：

1. 直接使用指针 `e`

- `if (p)`
- `while (s.p)`
- `p ? do_with(p) : do_without()`

2. 逻辑运算符 `!e` `e && f` `e || f`

- `if (!p)` `if (p && q)` `if (p || q)`
- `while (!s.p)`  `while (s.p && s.q)` `while (s.p || s.q)`

3. 显式和空指针做比较 `e == nullptr` `e != nullptr` 

- 比较运算符具有对称性，因此 `nullptr == e` 和 `nullptr != e` 也支持

4. 可以出现赋值运算符 `e = ...`

- `if (p = q)` 只能判断 `p` 的可空性
- 赋值运算符可结合，但 `if (p = q = r)` 只能判断 `p` 的可空性

5. 可以作为逗号表达式的一部分，即 `(..., e)`

- `if (x, p != nullptr)` 可以判断 `p` 的可空性
- `if (p != nullptr, q != nullptr)` 只能判断  `q` 的可空性

6. 上述模式可以嵌套括号

其中 `e` 需要满足 3.4.1 中“可被跟踪”的要求。

对这些“判空语句”，编译器会在后续控制流中更新 `e` 的可空性状态：

- 在 `if (e)` / `while (e)` 的真分支（或循环体）里，`e` 视作 Nonnull
- 在 `if (!e)` 的假分支里，`e` 视作 Nonnull
- 在 `if (e != nullptr)` 的真分支里，`e` 视作 Nonnull
- 在 `if (e == nullptr)` 的假分支里，`e` 视作 Nonnull

```c

_Safe void test(int *_Borrow _Nullable p, int *_Borrow _Nullable q) {
  // 1) 直接指针条件
  if (p) {
    *p = 1; // ok: 真分支里 p 视作 nonnull
  }

  // 2) 逻辑运算符
  if (!p) {
    *p = 1; // error: 真分支里 p 视作 nullable
  }
  if (p && q) {
    *p = 1; // ok
    *q = 1; // ok
  }
  if (!p || !q) {
    // snip
  } else {
    *p = 1; // ok
    *q = 1; // ok
  }

  // 3) 与 nullptr 比较
  if (p != nullptr) {
    *p = 1; // ok
  }
  if (nullptr == p) {
    *p = 1; // error
  }

  // 4) 赋值/逗号包裹
  if ((q = p) != nullptr) {
    *q = 1; // ok
  }
  if ((0, q) != nullptr) {
    *q = 1; // ok
  }
}
```

#### 3.4.4. 指针的赋值、传参和返回

1. 不允许用可空表达式给可空性标记为 Nonnull 的指针赋值：
```C
// 返回值类型为 Nullable
_Safe T *_Borrow _Nullable foo<T>(T *_Borrow p) { return p; }
// 返回值类型为 Nonnull
_Safe T *_Borrow bar<T>(T *_Borrow p) { return p; }

_Safe void test(void) {
  int *_Borrow p1 = nullptr; // error: 安全区内禁止用 nullable 值给 nonnull 指针赋值

  int local = 10;
  int *_Borrow p2 = foo(&_Mut local); // error: 安全区内禁止用 nullable 值给 nonnull 指针赋值
  int *_Borrow p3 = bar(&_Mut local); // ok

  int *_Borrow _Nullable p4 = foo(&_Mut local);
  int *_Borrow p5 = p4; // error: 安全区内禁止用 nullable 值给 nonnull 指针赋值
}

int main() {
  test();
  return 0;
}
```

2. 函数调用时，如果形参类型具有 Nonnull 可空性标记，那么不能传递可空表达式作为实参：
```C
// 返回值类型为 Nullable
_Safe T *_Borrow _Nullable foo<T>(T *_Borrow p) { return p; }

// 接收 Nonnull 类型的指针作为参数
_Safe void bar(int *_Borrow p) {}

_Safe void test(void) {
  int local = 10;
  int *_Borrow _Nullable p = foo(&_Mut local);
  bar(p); // error: 要求 nonnull 指针，但实参是 nullable
}

int main() {
  test();
  return 0;
}
```

3. 函数返回值类型如果具有 Nonnull 可空性标记，不能返回可空表达式的结果：
```C
_Safe int *_Borrow return_nonnull(int *_Borrow p) {
  int *_Borrow _Nullable q = nullptr;
  return q; // error: 要求返回指针是 nonnull
}
int main() {
  int i = 1;
  int *_Borrow pi = return_nonnull(&_Mut i);
  return 0;
}
```

#### 3.4.5. 指针的强制类型转换

在开启编译选项`-nullability-check=all`后，在非安全区中不允许将 Nullable 指针强制类型转换为 Nonnull 类型：
```C
void foo() {
  int *p1 = nullptr;
  int *p2 = (int *_Nonnull)p1; // error: cannot cast nullable pointer to nonnull type
  int *_Owned p3 = (int *_Owned)p1; // error: cannot cast nullable pointer to nonnull type
  int *_Borrow p4 = (int *_Borrow)p1; // error: cannot cast nullable pointer to nonnull type
}

int main() {
  foo();
}
```

对 Nullable 指针进行判空，那么在非空的分支中，可以强制类型转换为 Nonnull 类型：
```C
void foo() {
  int *p1 = nullptr;
  if (p1 != nullptr) {
    int *p2 = (int *_Nonnull)p1;
    int *_Owned p3 = (int *_Owned)p1;
    int *_Borrow p4 = (int *_Borrow)p1;
    safe_free((void *_Owned)p3);
  }
}

int main() {
  foo();
}
```

#### 3.4.6. 指针的解引用、成员访问

可空性标记为 Nullable 的指针的可空性状态（Nullability）会随赋值和控制流发生变化，
BSC 编译器会跟踪这些变换，保证指针解引用、成员访问等操作的安全性。
```C
// 返回值类型为 Nullable
_Safe T *_Borrow _Nullable foo<T>(T *_Borrow p) { return p; }

struct Data {
  int value;
};

_Safe void test(void) {
  struct Data data = {.value = 10};
  struct Data *_Borrow _Nullable p = foo(&_Mut data);
  if (p != nullptr) {
    // 经过判空后， nullable 会转换为 nonnull
    p->value = 10;
  }
}

int main() {
  test();
  return 0;
}
```

#### 3.4.7. 结构体成员是 Nullable 指针

初始化有 Nullable 指针成员的结构体变量时：
1. 如果是通过初始化列表进行初始化，BSC 编译器会根据初始化表达式来初始化 Nullable 指针成员的 Nullability
2. 对于其它初始化方式，直接认为 Nullable 指针成员的 Nullability 为 Nullable 的，这可能会导致本身没有问题的代码无法通过编译，此时需要对 Nullable 指针成员作再赋值，或使用判空语句，即可改变 Nullability。
```C
// 返回值类型为 Nonnull
_Safe T *_Borrow bar<T>(T *_Borrow p) { return p; }

struct Data {
  int *_Borrow _Nullable value;
};

_Safe struct Data init_data(int *_Borrow p) {
  struct Data d = {.value = p};
  return d;
}

_Safe void test(void) {
  int local = 10;
  // 使用初始化列表做初始化： 推断为 nonnull
  struct Data data1 = {.value = bar(&_Mut local)};
  *data1.value = 10;

  // 使用函数返回值做初始化： 无法推断 nullability 默认为 nullable
  struct Data data2 = init_data(&_Mut local);
  *data2.value = 10; // error: 直接使用 nullable 指针

  // 使用变量赋值做初始化： 无法推断 nullability 默认为 nullable
  struct Data data3 = data1;
  *data3.value = 10; // error: 直接使用 nullable 指针

  // 对 Nullable 指针成员作再赋值，可以改变 Nullability：
  data2.value = bar(&_Mut local);
  *data2.value = 10;

  // 通过指针判空语句，也可以改变 Nullability：
  if (data3.value != nullptr)
    *data2.value = 10;
}

int main() {
  test();
  return 0;
}
```
#### 3.4.8. 非空指针检查的范围和控制选项

非空指针检查是一项强大的功能，能帮助开发者在编译期识别出潜在的危险行为。同时，这会带来一定的编译性能开销和编码行为限制。**默认情况下，对非空指针的检查仅在安全区生效**，安全区的定义详见[内存安全-安全区](#安全区)章节。

在非安全区，我们将是否开启非空指针检查的选择权交给开发者，即通过编译选项`-nullability-check=value`来控制该项检查的作用域。其中`value`是一个枚举值，有2种选项`safeonly`, `all`：

1. `value`的默认值为`safeonly`，控制着检查只在安全区生效；无该编译选项时，等同`-nullability-check=safeonly`。
2. `value`值为`all`时，将整体使能非空指针检查，安全区与非安全区均开启检查。

提供一个示例说明：

```C
_Safe void test(void) {
  int *_Borrow _Nullable p = nullptr;
  _Unsafe {
    *p = 10; // error1: nullable pointer cannot be dereferenced
  }
  *p = 5; // error2: nullable pointer cannot be dereferenced
}

int main() {
  test();
  return 0;
}
```
对于上面这个示例，当编译选项`-nullability-check`不存在或者`-nullability-check=safeonly`时，只有在`_Safe`区的`error2`会被报告；当`-nullability-check=all`时，非安全区的`error1`和安全区`error2`均会被报告。

### 3.5. _Owned struct 类型

`_Owned struct` 是一种自定义类型，与 `struct` 不同，主要体现在非拷贝语义上，一律是 `move` 语义整体跟踪。这意味着原变量中的资源所有权会转移到新变量或参数中。本节依次介绍如何定义 `_Owned struct` 类型，如何创建 `_Owned struct` 实例。

#### 3.5.1. 定义 `_Owned struct` 类型
`_Owned struct` 类型的定义由关键字 `_Owned struct` 和自定义的名字组成，紧跟着是定义在一对花括号中 `_Owned struct` 定义体，在定义体中可以定义成员变量、析构函数、成员函数和访问修饰符。允许定义泛型 `_Owned struct`。

**限制：** `_Owned struct` 必须在翻译单元的声明层级定义（与 `_Trait` 定义、普通函数定义等同级），不允许在函数体或块作用域内定义（例如在任何函数、`main`、条件/循环等块内直接写 `_Owned struct S { ... };`）。这与标准 C 允许在函数内定义 `struct` 不同。违反时编译器报错：`_Owned struct cannot be defined in function scope; move the definition to file scope`。若在函数里需要该类型，应将 `_Owned struct` 定义放在函数外部，再在函数内使用。

示例：
```c
#include <stdio.h>

_Owned struct Person {
_Public:
  char name[50];
  int age;
  char *getName(Person *this) { return this->name; }
  int getAge(Person *this) { return this->age; }
  ~Person(Person this) {
    printf("Name = %s\n", this.getName());
    printf("Age = %d\n", this.getAge());
  }
};

int main() {
  Person p = {.name = "Tom", .age = 18};
  _Owned struct S { }; // error: _Owned struct cannot be defined in function scope; move the definition to file scope
  return 0;
}
```
上例定义了名为 `Person` 的 `_Owned struct` 类型，它有两个成员变量 `name` 和 `age`、成员函数`getName` 和 `getAge`、以及析构函数 `~Person`。

注：`_Owned struct` 与 `C struct`类似，允许结构体嵌套。

##### 3.5.1.1. 成员变量

`_Owned struct` 的成员变量分为实例成员变量和静态成员变量(由`static` 修饰)，和 `struct` 一样，成员变量定义的时候不允许写初始化。

```c
#include <stdio.h>

_Owned struct Person{
_Public:
    char name[50];
    static int age;
};

int main() {
  Person::age = 18;
  printf("%d\n", Person::age);
  return 0;
}
```

##### 3.5.1.2. 析构函数

在一个对象生命周期结束时，会自动调用析构函数。析构函数与 `_Owned struct` 类型同名，前面带有 `~`, 但是不允许写泛型参数。当用户未定义析构函数时，编译器会自动提供默认的析构函数。
```c
#include <stdio.h>

_Owned struct S {
  ~S(S this) { printf("S destructor\n"); }
};

int main() {
  S s = {};
  return 0;
}
```
当`_Owned struct`内有owned 修饰的指针的时候，该指针需要在析构函数内手动释放
```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放
#include <stdio.h>

_Owned struct Person {
_Public:
  int *_Owned age;
  ~Person(Person this) {
    printf("%d\n", *this.age);
    safe_free((void *_Owned)this.age);
  }
};

int main() {
  Person p = {.age = safe_malloc(18)};
  return 0;
}
```


析构函数的使用有以下限制：

+ 一个 `_Owned struct` 最多只能有一个析构函数。
+ 不允许作为扩展定义，不允许写返回类型，参数只有一个 `_Owned struct` 类型的 `this`, 不允许用户主动调用。
+ 析构函数内部需要将 `this` 的所有 `_Owned` 成员释放，规则同 `ownership` 的检查规则。需要注意的是如果 `this` 的成员是 `_Owned struct`类型，那么成员的释放是由编译器自动添加的。
+ 局部变量的析构函数, 如果在本函数内没有被 `move` 走，析构的顺序为先定义的变量后析构。
+ 临时变量是不支持析构的，会报内存泄漏。
+ 自定义类型内部的成员带有析构函数的时候，析构函数调用顺序：
  + 先调用外层类型的析构函数，再调用成员变量的析构函数
  + 不保证各个成员的析构函数的调用顺序
+ `_Owned struct` 及其内部的成员变量，在作用域结束时，必须处于以下两种状态之一
  + `_Owned` 状态：`_Owned struct` 及其内部所有`_Owned`修饰的成员（1.`_Owned struct` 也属于`_Owned` 修饰的成员 2.递归包含）均未被移动
  + `moved` 状态：`_Owned struct` 作为一个整体已被显式移动
+ `_Owned struct` 及其内部的成员变量，在作用域结束时，如若处于如下状态，则报错。 报错信息模板为 partially moved `_Owned struct`:`%0` at scope end, `%1` moved"（`%0` 为`_Owned struct` 变量名，`%1` 为被移动的成员变量名）
  1. 有成员变量被转移所有权的状态： `_Owned struct`（递归包含嵌套成员）至少有一个`_Owned` 修饰的成员被移动。结构体本身未被整体移动。
+ `_Owned struct`内 `_Owned` 修饰的指针需要在析构函数内被手动释放。如未被手动释放，则报错。 报错模板信息为 destructor for`%0` incorrect, %1 of _Owned type and needs to be handled manually（ `%0` 为 `_Owned struct` 变量名）

##### 3.5.1.3. 成员函数

`_Owned struct` 的成员函数分为实例成员函数和静态成员函数(不允许 `static` 修饰)。实例成员函数需要显式参数 `this`, 假设当前 `_Owned struct` 类型是 `C`, `this` 的类型可以是 `C*`、`const C*`、`C* _Borrow`、`const C * _Borrow`、`C * _Owned`。静态成员函数是没有 `this`。成员函数的访问与 `struct` 扩展成员函数访问一样(详见成员函数章节)。

注：当前不支持 `_Owned struct` 内部定义泛型函数。

```c
#include <stdio.h>

_Owned struct Person {
_Public:
  char name[50];
  int age;
  char *getName(Person *this) { return this->name; }
  char *getTypeName() { return "Preson"; }
};

int main() {
  Person p = {.name = "Tom", .age = 18};
  printf("TypeName: %s\nInstance Name: %s\n", Person::getTypeName(),
         p.getName());
  return 0;
}
```

上例是 `_Owned struct` 定义体内部实例成员函数 `getName` 和静态成员函数 `getTypeName`。

`_Owned struct` 定义体内部可以只有函数声明，把函数体外置。等同于 `_Owned struct` 定义体内部的成员函数。
示例：

```c
#include <stdio.h>

_Owned struct Person {
_Public:
  char name[50];
  int age;
  char *getName(Person *this);
  char *getTypeName();
};
char *Person::getName(Person *this) { return this->name; }
char *Person::getTypeName() { return "Preson"; }

int main() {
  Person p = {.name = "Tom", .age = 18};
  printf("TypeName: %s\nInstance Name: %s\n", Person::getTypeName(),
         p.getName());
  return 0;
}
```
`_Owned struct` 与 `struct` 类似，允许扩展成员函数（详见成员函数章节）。

##### 3.5.1.4. 访问控制控制权限

`_Owned struct` 定义体内，允许成员指定可见性分为 `_Public` 和 `_Private`, 默认为 `_Private`。只有 `_Owned struct` 定义体内部的成员函数有权访问 `_Private` 和 `_Public`成员，在`_Owned struct` 外部（包括扩展成员函数）, 只能访问 `_Public` 成员。示例：

```c
_Owned struct A{
    _Private:
    int a;
    _Public:
    int b;
};
int A::f(A* this) {
    this->a; // error
    this->b; // ok
    return 0;
}
```

#### 3.5.2. 创建 `_Owned struct`实例

`_Owned struct` 允许使用 `struct initializer` 语法创建实例，也允许单独对每个成员变量初始化（如果成员变量是 `_Public`, 此时与 `struct` 一样单独跟踪每个成员的初始化状态，但是需要在安全区状态下保证在发生 `move`、传参、析构和返回等场景下该变量一定已经完整初始化, 非安全区不做保证。同时为了方便 **`_Owned struct` 类型在声明、定义时不携带 `_Owned struct` 关键字**。

### 3.6. 安全区

#### 3.6.1. 概述

c 语言有很多规则过于灵活，不方便编译器做静态检查。因此我们引入一个新语法，使得在一定范围内的毕昇 c 代码必须遵循更严格的约束，保证在这个范围内的代码肯定不会出现“内存安全”问题。

允许用`_Safe`和`_Unsafe`关键字修饰函数、语句、括号表达式。

- `_Unsafe`表示这段代码在非安全区，这部分代码遵循标准 c 的规定，同时这部分代码的安全性由用户保证。


- `_Safe`表示这段代码在安全区，这部分代码必须遵循更严格的约束，同时编译器可以保证内存安全。


- 没有 `_Safe`或`_Unsafe`关键字修饰的全局函数默认是非安全的。

```c
#include <stdlib.h>

typedef struct File {
} FileID;

// 无关键字修饰，表示按默认非安全区
FileID *_Owned create(void) {
  FileID *p = malloc(sizeof(FileID));
  return (FileID * _Owned) p;
}
FileID *_Owned foo(FileID *_Owned p) { return p; }

// 使用 _Safe 修饰函数，表示该函数为安全函数，函数内为安全区
_Safe void safely_free(FileID *_Owned p) {
  // 使用 _Unsafe 修饰代码块，表示这段代码在非安全区。这段代码是在安全区内的非安全区，也属于非安全区
  _Unsafe { free((FileID *)p); }
}

int main(void) {
  FileID *_Owned p1 = create();
  FileID *_Owned p2 = foo(p1);
  // 使用 _Safe 修饰语句，表示这段代码在安全区
  _Safe safely_free(p2);
  return 0;
}
```

#### 3.6.2. 语法与作用域规则

1. 允许使用`_Safe/_Unsafe`修饰函数声明、函数签名、函数定义、函数指针、语句、括号表达式。

```c
// 修饰函数签名
_Safe int foo(int, int);
// 修饰函数声明
_Safe int bar(int a, int b);
// 修饰函数定义
_Safe int add(int a, int b) { return a + b; }
// 修饰函数指针
_Safe int (*p)(int, int);

_Safe int main(void) {
  // 修饰代码块
  _Safe {
    int a = 1;
  }
  _Unsafe { int b = 1; }
  // 修饰语句
  _Safe int c = 1;
  _Unsafe c++;
  // 修饰括号表达式
  char d = _Unsafe((char)c);
  return 0;
}
```

2. 不能使用`_Safe/_Unsafe`修饰全局变量、函数外类型声明、`typedef`声明（允许修饰函数指针）。

```c
_Safe int g_a; // error: 不能修饰全局变量

_Safe struct b { int a; }; // error: 不能修饰函数外类型声明

_Safe typedef int mm; // error: 不能修饰 typedef

int main() { return 0; }
```

3. 安全区内允许再包含`_Unsafe`修饰的语句、函数指针、括号表达式，非安全区内也允许再包含`_Safe`修饰的语句、函数指针、括号表达式。

```c
int add(int a, int b) { return a + b; }
_Safe int max(int a, int b) { return a > b ? a : b; }
int main() {
  _Safe {
    int a = 0;
    _Unsafe a++;
    _Unsafe {
      a = add(1, 3);
      _Safe a = max(3, 5);
    }
  }
}
```

#### 3.6.3. `_Safe` 函数的声明约束

1. `_Safe`修饰的函数，参数类型和返回类型允许是裸指针类型、成员中包含裸指针的`struct`类型、数组类型以及`union`类型。

```c
_Safe int *foo(int a); // ok: 返回值为裸指针类型

_Safe int bar(int *a); // ok: 参数类型为裸指针类型

typedef struct F {
  int *a;
} SF;

_Safe SF wrapped_foo(int a); // ok: 返回值为成员中包含裸指针类型的 struct 类型

_Safe int wrapped_bar(SF b); // ok: 参数类型为成员中包含裸指针类型的 struct 类型
```

2. `_Safe`修饰的函数，函数参数列表不可以省略。

```c
_Safe void test(); // error

_Safe void test(void); // ok
```

3. `_Safe`修饰的函数，函数参数列表不可以包含变长参数，除非该函数使用了`__attribute__((format(...)))`属性。

```c
_Safe int foo(int a, ...); // error
__attribute__((format(printf, 1, 2))) _Safe int bar(const char *fmt, ...); // ok
```

注意：即使允许声明带format属性的变长参数函数，在函数体内仍然不能使用`va_start`、`va_arg`、`va_end`等，这些操作在安全区域内是被禁止的。

4. 如果`_Trait`中的函数被声明为`_Safe`，那么要求实现`_Trait`的类型的对应成员函数也必须是`_Safe`修饰的函数。若`_Trait`中的函数未声明为`_Safe`，也允许实现`_Trait`中的类型的成员函数为`_Safe`，但编译器会给出**warning**。

```c
_Trait G {
  _Safe int *_Owned foo(This * _Owned this);
  int *_Owned bar(This * _Owned this);
};

_Safe int *_Owned int ::foo(int *_Owned this) { return this; } // ok: _Trait 实现函数必须为 _Safe
// 非 _Safe 函数的实现可以为 _Safe
_Safe int *_Owned int ::bar(int *_Owned this) { return this; }
_Impl _Trait G for int;

int main() { return 0; }
```


5. `_Safe`修饰泛型函数时，会对泛型每个实例化版本也做`_Safe`检查。

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

_Safe T identity<T>(T a) { return a; }

void foo() {
  int a = 1;
  int b = identity<int>(a);
  int *_Owned c = (int *_Owned)safe_malloc(1);
  int *_Owned d = identity<int * _Owned>(c); // ok
  int *e = identity<void *>((void *)0); // ok
  safe_free((void *_Owned)d);
}

int main() {
  foo();
  return 0;
}
```

6. 成员函数也可以被`_Safe/_Unsafe`修饰，其规则和全局函数一样。

```c
struct MyStruct<T> {
  T res;
};
_Safe T struct MyStruct<T>::foo(T a) {
  return a;
}

int main() { return 0; }
```

#### 3.6.4. 安全区内的操作限制

##### 3.6.4.1. 内存安全基本规则

1. 安全区内被调用的函数或函数指针必须是`_Safe`的函数签名，不允许调用非安全函数或函数指针。有关同名函数重声明的调用解析，详见 [3.6.5 节](#365-函数的重声明与混合模式)。

```c
_Safe void safe_foo(void) {}
_Unsafe void unsafe_foo(void) {}
_Safe void (*safe_func_ptr)(void);
_Unsafe void (*unsafe_func_ptr)(void);
int main() {
  _Safe {
    safe_foo();
    unsafe_foo(); // error: 安全区内不允许调用非安全函数
    safe_func_ptr();
    unsafe_func_ptr(); // error: 安全区内不允许调用非安全函数指针
  }
  _Unsafe {
    safe_foo();
    unsafe_foo(); // ok
    safe_func_ptr();
    unsafe_func_ptr(); // ok
  }
}
```

2. 安全区内不允许使用取地址符`&`（允许对函数取地址），只允许`&_Const`，`&_Mut`取借用。

    安全区内不允许解引用裸指针，但可以解引用`_Owned`指针、`_Borrow`指针和非空函数指针。安全区中允许对数组类型取下标、解引用，解读为直接对数组进行操作、不会将其视为对退化后的裸指针解引用。

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

typedef _Safe int (*unary_f)(int);
_Safe int inc(int a) { return a + 1; }

void test(unary_f _Nonnull fn) {
  _Safe {
    int a = 10;
    
    int *b = &a; // error: 安全区不允许取地址符号
    int c = *b; // error: 安全区不允许解引用裸指针

    int *_Owned d = safe_malloc(2);
    int e = *d; // ok: 允许解引用owned指针
    safe_free((void *_Owned)d);

    int *_Borrow f = &_Mut a;
    int g = *f; // ok: 允许解引用borrow指针

    int h = (*fn)(1);  // ok: 允许解引用函数指针
  }
}

_Safe int test2(unary_f f) {
  if (f != nullptr) {
    return (*f)(1);  // ok: 判空后确认为非空
  }
  return 0;
}
```

##### 3.6.4.2. 类型安全限制

1. 安全区内不允许指向类型不同的指针类型之间转换，但有以下例外：
    1. 允许指向其他类型的owned指针显式转换为指向void类型的owned指针，该转换需要符合 [3.1.4.3 owned指针强制类型转换](#3143-强制类型转换) 的规则。
    2. 允许指向其他类型的borrow指针隐式转换为指向void类型的borrow指针，但原类型必须满足 is_trivial_data 的条件（不含指针和 _Owned struct），否则不允许转换。

    安全区内不允许指针和非指针类型之间的转换。关于数组退化到指针的完整规则，详见 [3.3.2 数组退化规则拓展](#332-数组退化规则拓展)。除数组退化规则外，安全区内不允许`_Owned/_Borrow/raw`指针之间的转换；另外，允许 `T *_Borrow _ArrayElem` 转换为 `T *_Borrow`。

```c
void test() {
  int *pa;
  double *pb;
  _Safe {
    pb = pa; // error：不允许指向类型不同的指针类型之间转换
    pa = pb; // error：不允许指向类型不同的指针类型之间转换
    pb = (double *)pa; // error：不允许指向类型不同的指针类型之间转换
  }
  int i;
  _Safe {
    pa = i; // error：不允许指针和非指针类型之间的转换
    i = pa; // error：不允许指针和非指针类型之间的转换
  }
  int *_Owned pd = (int *_Owned)pa;
  _Safe {
    pa = pd; // error：不允许 _Owned/raw 指针之间的转换
    pd = pa; // error：不允许 _Owned/raw 指针之间的转换

    void *_Owned pe = (void *_Owned)pd; // ok: 允许显式转换为void类型的owned指针
  }
  struct S {int *ptr;};
  struct S s = {.ptr = nullptr};
  int a = 0;
  _Safe {
    int *_Borrow p1 = &_Mut a;
    void *_Borrow p2 = p1; // ok: int 满足 is_trivial_data 约束，允许 int *_Borrow 隐式转换到 void *_Borrow
    struct S *_Borrow p3 = &_Mut s;
    void *_Borrow p4 = (void *_Borrow)p3; // error: struct S 不满足 is_trivial_data，不允许 struct S *_Borrow 转换到 void *_Borrow
  }
}

int main() {
  test();
  return 0;
}
```

2. 安全区内允许定义和声明`union`类型变量，允许指向union的裸指针和安全指针，但不允许对`union`成员读写和取借用。

```c
#include "bishengc_safety.hbs" // BiShengC 语言提供的头文件，用于安全地进行内存分配及释放

union U {
  int age;
  char name[16];
};

_Safe void foo(union U *p) {       // ok: 安全区允许指向union的裸指针
  int py = p->age;                 // error: 安全区不允许对 union 成员读写
  p->age = 20;                     // error: 安全区不允许对 union 成员读写
  int *_Borrow pz = &_Mut p->age;  // error: 安全区不允许对 union 成员取借用
  union U u = {10};                // ok: 安全区允许声明和初始化 union 类型变量
  int y = u.age;                   // error: 安全区不允许对 union 成员读写
  u.age = 20;                      // error: 安全区不允许对 union 成员读写
  int *_Borrow z = &_Mut u.age;    // error: 安全区不允许对 union 成员取借用
  _Unsafe {
    int y2 = u.age;                // ok: 非安全区允许对 union 成员读写
    u.age = 30;                    // ok: 非安全区允许对 union 成员读写
    int *_Borrow z2 = &_Mut u.age; // ok: 非安全区允许对 union 成员取借用
  }
  union U *_Owned p1 = safe_malloc(u);  // ok, 允许指向union的_Owned指针
  int y3 = p1->age;                // error: 安全区不允许对 union 成员读写
  p1->age = 40;                    // error: 安全区不允许对 union 成员读写
  int *_Borrow z3 = &_Mut p1->age; // error: 安全区不允许对 union 成员取借用
  safe_free((void * _Owned)p1);
  union U *_Borrow p2 = &_Mut u;   // ok, 允许指向union的_Borrow指针
  int y4 = p2->age;                // error: 安全区不允许对 union 成员读写
  p2->age = 50;                    // error: 安全区不允许对 union 成员读写
  int *_Borrow z4 = &_Mut p2->age; // error: 安全区不允许对 union 成员取借用
}
```

##### 3.6.4.3. 数值类型转换规则

安全区内不允许**隐式执行**表达范围从大向小的类型转换（比如从`long`转换为`int`，从`int`转换为`_Bool`，从`int`转换为`enum`），不允许**隐式执行**表达精度从高向低的类型转换（比如从`double`转换为`float`）。安全区内不允许将一个算术类型转换为一个枚举类型，除非是从一个枚举类型显式转换为一个表达范围更大或相等的枚举类型。安全区内不允许从浮点类型转换到整数类型。对于编译期能确定值的常量发生类型转换，或是编译期能确定取值范围的整数类型表达式转换为其他整数类型，如果目标类型可以描述这个值，那么在安全区内该类型转换可以隐式执行，否则此类转换必须显式执行且需符合上述规则。

排除枚举类型时，安全区允许执行的数值类型转换如下图所示：（若同方向支持隐式转换则一定支持显式转换；只写需显式转换则说明不允许隐式转换。表示允许转换的边有一头连着子图时，等效于连接该子图的所有结点，比如 `_Bool` 有条边指向整数，表示允许 `_Bool` 以相同规则转换为任一整数类型。）

``` mermaid
graph TB
  bool["_Bool"]
  subgraph int["整数"]
    direction LR
    subgraph signedint["有符号整数"]
      direction TB
      signedint_s["小类型"]
      signedint_l["大类型"]
      signedint_s -- 正向隐式，反向显式 --> signedint_l
    end
    subgraph unsignedint["无符号整数"]
      direction TB
      unsignedint_s["小类型"]
      unsignedint_l["大类型"]
      unsignedint_s -- 正向隐式，反向显式 --> unsignedint_l
    end
    signedint <-- 需显式互转 --> unsignedint
  end
  bool -- 正向隐式，反向显式 --> int
  subgraph fp["浮点数"]
  direction TB
    fp_s["小类型"]
    fp_l["大类型"]
    fp_s -- 正向隐式，反向显式 --> fp_l
  end
  int -- 正向显式，反向不允许 --> fp
```

###### 内置整数类型与 _Bool

1. 当将一个算术类型转换为 `_Bool` 时，当原类型的值为零，则转换后值为 0；否则，转换后值为 1 （与C一致）。
```c
int b = 0;
_Bool c = (_Bool)b; // ok: int -> _Bool; c = 0
c = (_Bool)1;       // ok: int -> _Bool; c = 1
c = (_Bool)2;       // ok: int -> _Bool; c = 1
```

2. 将一个有符号整数类型或一个表达范围更大的无符号整数类型转换为一个**无符号**整数类型时，如果原类型的值的大小在目标类型的范围内，则转换后值的大小不变；否则，转换后值的大小为"不断对原值加或减(1+目标类型的最大值)直到结果落在目标类型的范围内的值" （与C一致）。如果目标无符号整数类型的表达范围是从 0 到 $2^N-1$ 且原类型的值为 $X$，那么转换后值的大小为 $X$ 不断加或减 $2^N$ 直到结果 $Y$ 落入 0 到 $2^N-1$ 之间，转换后的值即为 $Y$。上述表示等效于将大的整数类型截断为小的无符号整数类型，或是将小的有符号整数类型通过符号位拓展转为无符号整数类型。
```c
unsigned long a = 2;
unsigned b = (unsigned) a; // ok: unsigned long -> unsigned int
int c = -1;
unsigned d = (unsigned) c; // ok: int -> unsigned int; d = 2^32 - 1
unsigned long e = (unsigned long) c; // ok: int -> unsigned long; e = 2^64 - 1
```

3. 将一个表达范围更大的整数类型转换为表达范围更小的**有符号**整数类型（不包括枚举类型）时，如果原类型的值的大小在目标类型的范围内，则转换后值的大小不变；否则，转换后值的大小由实现定义。在当前毕昇C编译器的实现中，原值超过目标类型的表示范围时，大的整数类型会截断为小的有符号整数类型。
```c
long a = 2;
int b = (int) a; // ok: long -> int
unsigned c = 4294967295; // c = 2^32 - 1
int d = (int) c; // ok: unsigned int -> int; d = -1
```

###### 枚举类型

1. 安全区只允许从一个枚举类型显式转换为另一个枚举类型（有限定条件）；安全区内不允许其他任何类型在转换为枚举类型。使用显式转换将枚举类型 `E1` 转换为另一个枚举类型 `E2` 时，当且仅当 `E2` 中包含 `E1` 所有枚举值时允许，转换后的值不变，否则编译报错。安全区内不能隐式执行枚举到枚举转换。

2. 从枚举类型隐式转换为整数类型时，遵循与整数到整数转换相同的安全区规则（见内置整数类型规则2-3）：允许转换到该枚举的底层整数类型；对于带显式底层类型的枚举，转换到其他整数类型时，仅当目标类型与底层类型符号相同且表达范围不更小，或编译期能证明取值范围可由目标类型精确表示时，才允许隐式转换。不允许隐式执行同宽异符号的枚举到整数转换。这些转换都可以显式执行，规则与整数到整数显式转换一致。

在非安全区则与 C 一致，不对枚举类型转换做额外检查。

```c
_Safe void foo(void) {
  enum E {ZERO, ONE, TWO}; // represented by int
  enum F {SUN, MON, TUES}; // represented by int
  enum F day = SUN;
  enum E num = (enum E)day; // ok: enum F -> enum E
  num = (enum E)SUN;        // ok: enum F -> enum E
  int a = num; // ok: enum E -> int
  a = ZERO;    // ok: enum E -> int
  num = 0; // error: cannot cast int to enum E in safe zone
  enum G {G1, G2};
  enum G g1 = (enum G) num; // error: cannot cast enum E to enum G in safe zone
  g1 = (enum G) ZERO; // error: cannot cast enum E to enum G in safe zone
}
_Safe void bar(void) {
  enum EU : unsigned int { EU_BIG = 0xFFFFFFFFu };
  enum EU e = EU_BIG;
  unsigned int u = e;     // ok: enum EU -> unsigned int (同符号，同底层类型)
  unsigned long long w = e; // ok: 同符号加宽
  long long s = e;        // ok: unsigned -> 更宽的有符号，保值
  int n = e;              // error: 同宽异符号，隐式转换会改变数值，不允许
  int m = (int)e;         // ok: 显式转换允许，与 (int)u 一致
}
// 不带显式底层类型的枚举与 C 保持兼容：不施加额外的符号检查。
_Safe void baz(void) {
  enum BU { BU_BIG = 0xFFFFFFFF }; // 无显式底层类型, 编译器选择同宽无符号底层类型
  enum BU e = BU_BIG;
  int n = e;              // ok: 非固定底层类型枚举保留 C 的宽松转换行为
}
```

###### 浮点类型

1. 在安全区使用显式转换将一个浮点类型转换为精度更低的另一个浮点类型时，如果原值能在目标类型中精确表示，则转换后的值不变。如果原值在目标类型的表示范围内但是无法精确表示，则转换后的结果是最近的向上或向下近似的值，取决于具体实现。如果原值在目标类型的表示范围外，则具体值取决于具体实现。当前毕昇C编译器的实现应符合 IEEE 754 规范。安全区内不能隐式执行这样的类型转换。在非安全区则行为与 C 一致，语义相同且支持隐式执行这样的类型转换。
```c
_Safe {
  double a = 1.0;
  float b = 0.0f;
  b = (float)a; // ok: double -> float
  b = a; // error: cannot implicitly cast double to float
  a = b; // ok
}
```
2. 在安全区使用显式转换将一个整数类型转换为浮点类型时，如果原值能在目标类型中精确表示，则转换后的值不变。如果原值在目标类型的表示范围内但无法精确表示，则转换后的结果是最近的向上或向下近似的值，取决于具体实现。当前毕昇C编译器的实现应符合 IEEE 754 规范、使用 IEEE 754 中的默认舍入规则。安全区内不能隐式执行这样的类型转换。安全区内不允许从浮点类型转换为整数类型。在非安全区则行为与 C 一致，语义相同且支持隐式执行这样的类型转换。
```c
_Safe {
  int a = -1;
  double b = a; // error: cannot implicitly cast int to double in safe zone
  b = (double) a; // ok: int -> double
  a = (int) b; // error: cannot cast double to int in safe zone
}
```

###### 其他规则

1. 比较运算符（`==`, `!=`, `>=`, `<=`, `>`, `<`）、逻辑运算符（`&&`, `||`, `!`）的运算结果为 `int` 类型的 0 或 1，这些值允许在安全区隐式转换到其他整数类型（因为任何基本整数类型都可以表示 0 和 1）。
```c
_Safe {
  int a = 0, b = 1;
  _Bool c = (a == 0) || (b == 0); // ok: int (0/1) -> _Bool
  char x = a < 3 || a >= 6; // ok: int (0/1) -> char
}
```

2. 对于编译期能确定值的常量发生类型转换，若目标类型可精确表示该值，则允许隐式转换。如果编译期能确定取值范围整数类型表达式转换为另一整数类型，如果原值的取值范围小于等于目标类型的表示范围，则也允许隐式转换。否则需显式转换并符合上述规则。
毕昇C保证二元位运算 `&` `|` `^` 可以在编译期确定取值范围。其他运算的支持由实现决定。
```c
_Safe {
  int a = 10l; // ok: long -> int
  unsigned long b = 2*2; // ok: int -> unsigned long

  // int -2147483648~2147483647
  int c = 2147483648; // error: 2147483648 out of range for int
  int d = (int)2147483648; // ok: long -> int; d = -2147483648
  int e = 2147483647; // ok

  // unsigned int 0~4294967295
  unsigned int f = 0;
  f = 4294967296; // error: 4294967296 out of range for unsigned int
  f = (unsigned int) 4294967296; // ok: long -> unsigned int; f = 0

  unsigned long g = (unsigned long)-1; // ok: int -> unsigned long; g = 2^64-1
  int h = (int) 1.2; // error: cannot cast double to int in safe zone
  float k = 1.0; // ok: double -> float
  float l = 1; // ok: int -> float
}
```
```c
_Safe {
  unsigned char flag = 0;
  flag = flag | 0x10;          // ok
  flag = (flag | 0x01) & 0xFF; // ok
  flag = flag | 0x100; // error: [0,255] | 0x100 超过了 unsigned char 的表示范围
  unsigned short flag2 = 0;
  flag2 = flag | 0x100; // ok
  flag2 = flag2 | 0x10000; // error: [0, 65535] | 0x10000 超过了 unsigned short 的表示范围
  unsigned int flag3 = 0; // 编译器不会跟踪 flag3 的精确范围，只使用保守的类型信息的值域（整个 unsigned int 的范围）
  flag2 = flag3 | 0x10; // error: flag3 | 0x10 超过了 unsigned short 的表示范围
  flag2 = (unsigned int)flag2 | 0x100; // error: flag2 转为 unsigned int 后取值范围扩大到 unsigned int 的范围，超出 unsigned short 的表示范围
}
```

3.  `if/while/do-while/for` 语句的条件对表达式类型的要求与C一致，任何算术类型和指针类型都可以作为条件。在条件中使用任何值不会视为额外发生类型转换。
```c
int a = 2;
_Safe {
  if (a) { // ok: if 条件可以接受 int，不认为先转成了 _Bool
    a += 1;
  } else {
    a -= 1;
  }
}
```

##### 3.6.4.4. 其他语法规则

1.  安全区内`switch`语句中的`case/default`只能存在于`switch`后面的第一层代码块中，且第一层代码块不允许有变量定义。

```c
_Safe void foo(int a) {
  switch (a) {
    int b = 10; // error: 第一层代码块不允许有变量定义
    case 0: {
        int c = 1;
        break;
    }
    {
        case 1 : { break; } // error: case 只能存在于 switch 后面的第一层代码块中
    }
    {
        default: { break; } // error: default 只能存在于 switch 后面的第一层代码块中
    }
  }
}

int main() {
  int a = 1;
  foo(a);
  return 0;
}
```

2. 安全区内不允许内嵌汇编语句。

```c
void test() {
  _Safe {
    int ret = 0;
    int src = 1;
    asm("move %0, %1\n\t" : "=r"(ret) : "r"(src)); // error: 安全区不允许内嵌汇编
  }
}
int main() { return 0; }
```

3. 安全区内，前缀/后缀自增（`++`）和自减（`--`）表达式的结果类型为 `void`，即仅允许使用其副作用（自增或自减1），不得使用 `++`/`--` 表达式的值。在返回类型为 `void` 的安全函数中，不得将 `++`/`--` 的结果直接作为 return 语句的返回值。

```c
safe void take_int(int x);
safe void foo(void) {
  int a = 0;
  a++; // ok: 安全区允许自增自减（仅副作用，表达式结果为 void）
  a--;
  int x = a++; // error: 自增自减的结果为 void 类型，不能使用其值初始化 int 变量
  int arr[5] = {1, 2, 3, 4, 5};
  arr[a++] = 0; // error: 自增自减的结果为 void 类型，不能使用其值做为 arr[] 下标
  take_int(a++); // error: 自增自减的结果为 void 类型，不能使用其值做为 take_int() 入参
  unsafe {take_int(a++);} // ok: 非安全区不受影响
  for (int i = 0; i < 10; i++) {} // ok: for 循环的迭代部分可以使用 ++/--
  int y = 0;
  y = (a++, a); // ok: a++ 先求值、完成自增，再对逗号右侧的 a 求值
  return a++; // error: 不得将 ++/-- 的结果直接作为返回值
  return (void)a++; // ok
}
```

4. 安全区内，使用字符串字面量初始化字符数组时，不允许字符数组的长度小于字符串字面量的长度（包含结尾的`\0`）。

```c
_Safe void foo(void) {
  char arr1[3] = "abc"; // error: 字符串长度为4(包含结尾的`\0`)，超出字符数组长度
  char arr2[4] = "abc"; // ok
}

```

#### 3.6.5. 函数的重声明与混合模式

同一函数标识符可以有多个声明,支持`_Safe`和`_Unsafe`混合声明。

##### 3.6.5.1. 函数声明检查

1. 兼容性要求

   **相同修饰符**: 多个`_Safe`声明之间、或多个`_Unsafe`声明之间,函数类型必须兼容

   **混合模式**: `_Safe`和`_Unsafe`声明可以共存,但必须满足混合模式兼容性(见7.3)
   
   **泛型函数除外**: 泛型函数不支持混合模式,同一实例化不能同时有`_Safe`和`_Unsafe`声明
   
   **成员函数**: 与普通函数规则相同

2. 函数类型兼容性

   **两个函数类型兼容的条件**:
   - 返回类型兼容
   - 参数数量相同,省略号(`...`)使用一致
   - 对应参数类型兼容(兼容性检查时移除除`_Owned`、`_Borrow`、`_ArrayElem`、`_Nonnull`、`_Nullable`外的所有限定符)

  **指针兼容性**:

   - 指针类型需要相同限定符(`_Owned`、`_Borrow`、`_ArrayElem`、`_Nonnull`、`_Nullable`)且目标类型兼容

   - `_Owned`和`_Borrow`指针（以及带`_ArrayElem`的版本）之间两两互不兼容。

   - `_Owned`/`_Borrow`指针（以及带`_ArrayElem`的版本）与裸指针不兼容(混合模式除外,见7.3)

3. 混合模式兼容性(`_Unsafe`与`_Safe`共存)

   **返回类型兼容**:

   - `_Safe`声明与`_Unsafe`声明中的返回值类型在去掉毕昇C引入的类型限定符(`_Owned`,`_Borrow`,`_ArrayElem`等)、**保留**其他C的限定符(`const`,`volatile`等)之后是兼容的。

   - `_Safe`声明只能为返回类型**添加**`_Owned`，`_Borrow`，`_Owned _ArrayElem`，或`_Borrow _ArrayElem`限定符，不能**移除**已有的限定符。`_Owned _ArrayElem`与`_Borrow _ArrayElem`应当视作整体进行添加。

   **参数类型兼容**:

   - 参数数量、省略号使用必须一致

   - `_Safe`声明与`_Unsafe`声明中的参数类型在去掉毕昇C引入的类型限定符(`_Owned`,`_Borrow`,`_ArrayElem`等)以及**去除**其他C的限定符(`const`,`volatile`等)之后是兼容的。

   - `_Safe`声明只能为参数**添加**`_Owned`，`_Borrow`，`_Owned _ArrayElem`，或`_Borrow _ArrayElem`限定符，不能**移除**已有的限定符。`_Owned _ArrayElem`与`_Borrow _ArrayElem`应当视作整体进行添加。

   **示例**:
   ```c
   // ok: _Safe声明添加_Owned限定符
   _Unsafe int* f1(int* p);
   _Safe int* _Owned f1(int* _Owned p);

   // ok: _Safe声明添加_Borrow限定符
   _Unsafe int* f2(int* p);
   _Safe int* _Borrow f2(int* _Borrow p);

   // ok: _Safe声明添加_Owned _ArrayElem限定符
   _Unsafe int* f3(int* p);
   _Safe int* _Owned _ArrayElem f3(int* _Owned _ArrayElem p);

   // ok: _Safe声明添加_Borrow _ArrayElem限定符
   _Unsafe int* f4(int* p);
   _Safe int* _Borrow _ArrayElem f4(int* _Borrow _ArrayElem p);

   // error: _Safe声明移除了_Unsafe声明中的_Owned限定符
   int* _Owned f5(int* _Owned p);
   _Safe int* f5(int* p);

   // error: _Safe声明移除了_Unsafe声明中的_Borrow限定符
   int* _Borrow f6(int* _Borrow p);
   _Safe int* f6(int* p);

   // error: _Safe声明移除了_Unsafe声明中的_Owned _ArrayElem限定符
   int* _Owned _ArrayElem f7(int* _Owned _ArrayElem p);
   _Safe int* f7(int* p);

   // error: _Safe声明移除了_Unsafe声明中的_Borrow _ArrayElem限定符
   int* _Borrow _ArrayElem f8(int* _Borrow _ArrayElem p);
   _Safe int* f8(int* p);

   // error: _Unsafe声明中没有_ArrayElem限定符时_Safe声明不能加_ArrayElem
   int* _Borrow  f9(int* _Borrow p);
   _Safe int* _Borrow _ArrayElem f9(int* _Borrow _ArrayElem p);
   int* _Owned f10(int* _Owned p);
   _Safe int* _Owned _ArrayElem f10(int* _Owned _ArrayElem p);

   // error: owned与borrow不兼容
   _Unsafe int* _Owned f11(int* _Borrow p);
   _Safe int* _Borrow f11(int* _Owned p);

   // ok: 相同修饰符,类型完全相同
   _Safe int* _Owned f12(int* _Owned p);
   _Safe int* _Owned f12(int* _Owned q);
   ```

4. 函数定义

   混合模式函数只能定义**一次**

   定义可基于`_Unsafe`版本或`_Safe`版本,但必须与所有声明兼容

   ```c
   _Unsafe int* foo(int* p);
   _Safe int* _Owned foo(int* _Owned p);
   
   // 定义(选择其一)
   _Safe int* _Owned foo(int* _Owned p) { return p; }
   // 或
   _Unsafe int* foo(int* p) { return p; }
   ```

##### 3.6.5.2. 函数调用解析

1. 安全上下文调用

   安全上下文(`_Safe`块)内只能调用`_Safe`函数。如果函数只有`_Unsafe`声明,编译错误。
   
   ```c
   _Unsafe void foo(void);
   
   int main() {
     _Safe {
       foo();  // error: 安全区内不允许调用非安全函数
     }
   }
   ```

2. 非安全上下文调用

   非安全上下文(`_Unsafe`块或默认)内执行重载解析:

   - 优先匹配`_Safe`声明
   - `_Safe`不匹配时使用`_Unsafe`声明
   
   ```c
   _Unsafe int* foo(int* p);
   _Safe int* _Owned foo(int* _Owned p);
   
   int *_Owned bar() {
     int* raw_p = nullptr;
     int* _Owned owned_p = nullptr;
   
     // 根据参数类型选择对应版本
     foo(raw_p);      // 调用unsafe版本
     return foo(owned_p);    // 调用safe版本
   }
   ```

##### 3.6.5.3. 函数指针赋值

函数指针赋值规则:

**给`_Safe`修饰的函数指针赋值时**:
- 如果被用于赋值的函数标识符没有`_Safe`版本的声明,则报错,即不允许`_Unsafe`版本的声明赋值给`_Safe`修饰的函数指针
- 如果被用于赋值的函数标识符有`_Safe`版本的声明,则要求`_Safe`版本的声明类型与函数指针类型是兼容的

**给`_Unsafe`修饰的函数指针赋值时**:
- 只要被用于赋值的函数标识符的`_Unsafe`版本声明类型与函数指针类型是兼容的,或`_Safe`版本声明类型与函数指针类型是`_Unsafe-_Safe`兼容的,即允许赋值
- 都不满足则编译报错

**示例**:
```c
_Safe void safe_foo(void);
_Unsafe void unsafe_foo(void);

// 混合模式声明
_Unsafe int* bar(int* p);
_Safe int* _Owned bar(int* _Owned p);

int main() {
  _Safe void (*safe_ptr)(void) = nullptr;
  _Unsafe void (*unsafe_ptr)(void) = nullptr;

  safe_ptr = safe_foo;      // ok: safe函数赋值给safe指针
  safe_ptr = unsafe_foo;    // error: 没有safe版本的声明

  unsafe_ptr = unsafe_foo;  // ok: unsafe函数赋值给unsafe指针
  unsafe_ptr = safe_foo;    // ok: safe版本与unsafe-safe兼容

  // 混合模式函数指针赋值
  _Safe int* _Owned (*safe_bar_ptr)(int* _Owned) = nullptr;
  _Unsafe int* (*unsafe_bar_ptr)(int*) = nullptr;

  safe_bar_ptr = bar;   // ok: 使用safe版本
  unsafe_bar_ptr = bar; // ok: 使用unsafe版本或safe版本(_Unsafe-safe兼容)
}
```

函数参数中声明为数组类型的形参与对应的指针类型形参等价，函数指针赋值时两者可以互换：

```c
_Safe void test10(int arr[3]) {}
_Safe void test11(int *arr) {}

_Safe void (*p10)(int arr[3]) = nullptr;
_Safe void (*p11)(int *arr) = nullptr;

_Safe int main(void) {
    p10 = test11;  // ok: int arr[3] 与 int *arr 等价
    p11 = test10;  // ok: int *arr 与 int arr[3] 等价
    return 0;
}
```

### 3.7. 初始化分析

#### 3.7.1. 概述

在 C 语言中，使用未初始化的变量是一种常见的未定义行为。编译器可能不会对此产生警告，但未初始化的值会导致不可预测的程序行为、安全漏洞或崩溃。

毕昇 C 在安全区（`_Safe`）中引入了**初始化分析**（Initialization Analysis），在编译期通过数据流分析确保每个变量在使用前都已被确定初始化。该分析支持结构体字段级别的精确追踪。

初始化分析默认在 `_Safe` 区域内生效，也可通过 `-uninit-check=all` 选项扩展到所有代码区域，或通过 `-uninit-check=none` 禁用。

#### 3.7.2. 代码示例

```c
_Safe void example(int cond) {
    int x;
    if (cond) {
        x = 42;
    }
    int y = x; // error: use of possibly uninitialized value: `x`
}

_Safe void example_ok(int cond) {
    int x;
    if (cond) {
        x = 1;
    } else {
        x = 2;
    }
    int y = x; // ok: x 在所有路径上都已初始化
}
```

#### 3.7.3. 语法及语义规则

1. 在安全区中，所有局部变量（标量、指针、结构体等）在使用前必须被确定初始化。未初始化或仅在部分路径上初始化的变量会导致编译错误。

```c
_Safe void rule1(void) {
    int x;
    int y = x; // error: use of uninitialized value: `x`
}

_Safe void rule1_ok(void) {
    int x = 0;
    int y = x; // ok
}
```

2. 对结构体类型，分析精确追踪每个字段的初始化状态。部分字段未初始化时，整体使用该结构体会报错。当所有字段都被初始化后，编译器会自动将整个结构体提升为已初始化状态。

```c
struct Pair { int a; int b; };

_Safe void rule2_partial(void) {
    struct Pair p;
    p.a = 1;
    // p.b 未初始化
    struct Pair q = p; // error: use of uninitialized value: `p`
}

_Safe void rule2_full(void) {
    struct Pair p;
    p.a = 1;
    p.b = 2;
    struct Pair q = p; // ok: p 的所有字段都已初始化，自动提升为整体已初始化
}
```

没有可初始化存储的结构体被视为已初始化。这包括：没有任何字段的结构体，以及递归地只包含此类结构体字段的结构体。此类结构体作为某个结构体的成员时不会阻止该结构体的整体提升，直接读取此类成员也总是合法的。无名位域（如 `unsigned : 3;`）仍占用存储空间，含有无名位域的结构体不属于此类——直接整体读取仍会报告未初始化。

```c
struct Empty { };
struct Wrap { struct Empty inner; };      // 递归地只含空结构体字段
struct Outer { struct Empty e; struct Wrap w; int n; };

_Safe void rule2_empty_field(void) {
    struct Outer o;
    o.n = 1;
    struct Empty x = o.e;   // ok: o.e 无可初始化存储，直接读取合法
    struct Outer c = o;     // ok: e、w 均无需初始化，o 自动提升为整体已初始化
}

struct Bits { unsigned : 3; };

_Safe void rule2_bitfield(void) {
    struct Bits b;
    struct Bits c = b;      // error: use of uninitialized value: `b`
}
```

3. 控制流分支中的初始化必须覆盖所有路径。如果变量仅在部分分支中被初始化，分析会报告"可能未初始化"。

```c
_Safe void rule3(int cond) {
    int x;
    if (cond) {
        x = 1;
    }
    // else 分支未初始化 x
    int y = x; // error: use of possibly uninitialized value: `x`
}

_Safe void rule3_ok(int cond) {
    int x;
    if (cond) {
        x = 1;
    } else {
        x = 2;
    }
    int y = x; // ok
}
```

4. 取地址操作（`&_Mut`、`&_Const`、`&`）被视为对变量的使用。对未初始化的变量取地址会报错。例外：作为 `ensure_init` 参数或 `__assume_initialized` 参数的取地址表达式不受此限制（详见 3.7.4 和 3.7.5）。

```c
_Safe void rule4(void) {
    int x;
    int *_Borrow p = &_Mut x; // error: use of uninitialized value: `x`
}

_Safe void rule4_ok(void) {
    int x = 0;
    int *_Borrow p = &_Mut x; // ok
}
```

5. 有返回值的函数必须在所有路径上初始化返回值。

```c
_Safe int rule5(int cond) {
    if (cond) {
        return 1;
    }
    // 其他路径未 return
} // error: return value of `rule5` may not be initialized on all paths
```

6. 数组元素的逐个赋值**不会**将数组标记为已初始化。数组必须通过初始化列表或在 `_Unsafe` 区域中使用 `__assume_initialized` 来初始化（详见 3.7.5）。此规则同样适用于结构体中的数组字段——对数组元素的逐个写入不会将该字段标记为已初始化。

```c
_Safe void rule6_error(void) {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    int x = arr[0]; // error: use of uninitialized value: `arr`
}

_Safe void rule6_ok_init_list(void) {
    int arr[3] = {1, 2, 3};
    int x = arr[0]; // ok: 通过初始化列表初始化
}

_Safe void rule6_ok_assume(void) {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    _Unsafe { __assume_initialized(&arr); }
    int x = arr[0]; // ok: 通过 __assume_initialized 标记
}

// 结构体中的数组字段同理
typedef struct { int a[2]; int b; } ArrStruct;

_Safe void rule6_struct_error(void) {
    ArrStruct s;
    s.a[0] = 1;
    s.a[1] = 2;
    s.b = 3;
    ArrStruct t = s; // error: use of uninitialized value: `s.a`
}

_Safe void rule6_struct_ok(void) {
    ArrStruct s = {{1, 2}, 3};
    ArrStruct t = s; // ok: 通过初始化列表初始化
}
```

7. 联合体（union）写入任一成员会将整个联合体标记为已初始化（符合 C 语言联合体语义：写入任一变体覆盖全部字节）。对于联合体中包含结构体变体的情况，写入结构体的某个字段也会将整个联合体标记为已初始化，跨变体读取也是允许的。

注：由于安全区内不允许直接访问联合体字段，以下示例使用非安全函数配合 `-uninit-check=all` 来展示联合体的初始化行为。

```c
// 编译选项: -uninit-check=all
union U { int a; float f; };

void rule7(void) {
    union U u;
    u.a = 42;
    float f = u.f; // ok: 联合体已通过 u.a 初始化
}

// 联合体包含结构体变体
struct S { int x; int y; };
union US { int a; struct S s; };

void rule7_struct(void) {
    union US u;
    u.s.x = 1;      // 写入结构体变体的字段 → 整个联合体标记为已初始化
    int v = u.a;     // ok: 跨变体读取，联合体已初始化
}
```

> **已知限制**：写入一个变体会将整个联合体标记为已初始化，因此跨变体读取结构体字段时，即使对应的字节实际上未被写入，编译器也不会报错。例如：
>
> ```c
> struct S2 { int b; int c; };
> union U2 { int a; struct S2 s; };
>
> void example(void) {
>     union U2 u;
>     u.a = 1;
>     int v = u.s.c; // 编译通过，但 u.s.c 的字节实际上可能未被有意义地写入
> }
> ```

8. 嵌套结构体支持任意深度的字段级追踪。当所有叶子字段都被初始化后，父字段和整个结构体会自动提升为已初始化状态。

```c
struct Inner { int x; int y; };
struct Outer { struct Inner inner; int z; };

_Safe void rule8(void) {
    struct Outer o;
    o.inner.x = 1;
    o.inner.y = 2;  // inner 的所有字段已初始化 → inner 自动提升
    o.z = 3;        // 所有字段已初始化 → o 自动提升
    struct Outer p = o; // ok
}
```

9. 全局变量和静态变量被视为隐式已初始化（由 C 语言规范保证零初始化），不需要显式初始化。

```c
static int global_count;

_Safe void rule9(void) {
    int x = global_count; // ok: 全局/静态变量隐式已初始化
}
```

#### 3.7.4. `__attribute__((ensure_init))`

`__attribute__((ensure_init))` 是一个参数属性，用于标注指针参数，建立初始化契约：

- **调用者**：调用后，被指向的变量标记为已初始化
- **被调用者**：编译器验证函数确实在所有返回路径上初始化了 `*param`

```c
void init_int(int *__attribute__((ensure_init)) out);

_Safe void caller(void) {
    int x;                    // 未初始化
    _Unsafe { init_int(&x); } // 调用后 x 被标记为已初始化
    int y = x;                // ok
}

// 编译器验证被调用者的契约
void good_init(int *__attribute__((ensure_init)) out) {
    *out = 42; // ok: 在返回前初始化了 *out
}

void bad_init(int *__attribute__((ensure_init)) out) {
} // error: __attribute__((ensure_init)) parameter 'out' not initialized at return
```

字段级别的部分初始化也受支持：

```c
struct Pair { int a; int b; };
void init_field(int *__attribute__((ensure_init)) p);

_Safe void field_level(void) {
    struct Pair p;
    _Unsafe {
        init_field(&p.a);  // 仅标记 p.a 为已初始化
        init_field(&p.b);  // 标记 p.b 为已初始化 → p 整体自动提升为已初始化
    }
    struct Pair q = p; // ok
}
```

当 `ensure_init` 指针所指向的类型没有可初始化存储（空结构体或递归地只含空结构体字段的结构体，见 3.6.3）时，该契约被自动视为已履行：被调用端即使不写入 `*param` 也不会在返回处报错。

```c
struct Empty { };
void init_empty(struct Empty *__attribute__((ensure_init)) out) {
} // ok: *out 无可初始化存储，契约自动满足
```

```c
_Safe void init_safe(int *_Borrow __attribute__((ensure_init)) out);

_Safe void caller_safe(void) {
    int x;
    init_safe(&_Mut x);  // ok: 在安全区中使用 &_Mut
    int y = x;            // ok
}
```

`ensure_init` 参数在 `*param` 初始化完成前不允许被重新赋值或复制到其他变量（别名）：

```c
void bad_reassign(int *__attribute__((ensure_init)) out) {
    int local;
    out = &local; // error: __attribute__((ensure_init)) parameter 'out' cannot be reassigned or aliased before '*out' is initialized
} // error: __attribute__((ensure_init)) parameter 'out' not initialized at return

void bad_alias(int *__attribute__((ensure_init)) out) {
    int *p = out; // error: __attribute__((ensure_init)) parameter 'out' cannot be reassigned or aliased before '*out' is initialized
    *out = 42;
}
```

初始化完成后可以自由重新赋值或别名：

```c
void ok_reassign(int *__attribute__((ensure_init)) out) {
    *out = 42;         // 契约已履行
    int local = 10;
    out = &local;      // ok: *out 已初始化，指针可以自由使用
}

void ok_alias(int *__attribute__((ensure_init)) out) {
    *out = 42;         // 契约已履行
    int *p = out;      // ok: *out 已初始化
}
```

`ensure_init` 参数可以委托给另一个 `ensure_init` 函数，编译器会追踪委托链：

```c
void init_val(int *__attribute__((ensure_init)) out);

void init_delegated(int *__attribute__((ensure_init)) out) {
    init_val(out); // ok: 委托给另一个 ensure_init 函数
}
```

**重声明规则**：safe/safe unsafe/unsafe compatible 的重声明（`_Safe`/`_Safe`、`_Unsafe`/`_Unsafe` 或 default/`_Unsafe`）必须保持 `ensure_init` 一致——要么都有，要么都没有。safe unsafe compatible 的重声明（`_Safe` 与非安全）是独立的重载，`ensure_init` 差异是允许的。

```c
// safe/safe unsafe/unsafe compatible ：必须一致
_Safe void foo(int *__attribute__((ensure_init)) _Borrow out);
_Safe void foo(int *__attribute__((ensure_init)) _Borrow out) { // ok: 一致
    *out = 1;
}

// safe/safe unsafe/unsafe compatible ：不一致 → 错误
// _Safe void bar(int *__attribute__((ensure_init)) _Borrow out);
// _Safe void bar(int *_Borrow out) { ... } // error: incompatible declarations

// 不safe/safe unsafe/unsafe compatible ：allow differences
void init_value(int *__attribute__((ensure_init)) out);
_Safe void init_value(int *_Borrow out) { // ok: 不safe/safe unsafe/unsafe compatible 
    // ...
}
```

**函数指针兼容性**：`ensure_init` 是函数类型的一部分。在 safe zone 中，不能把不具有 `ensure_init` 的函数转换为需要 `ensure_init` 的函数指针，赋值和显式强制转换都不行。在 unsafe zone 中，强制转换由用户自行负责，不受此约束：

```c
typedef _Safe void (*InitFn)(int *__attribute__((ensure_init)) _Borrow out);

_Safe void has_attr(int *__attribute__((ensure_init)) _Borrow out) { *out = 1; }
_Safe void no_attr(int *_Borrow out) { *out = 1; }

_Safe void test(void) {
    InitFn fn = has_attr; // ok: 签名匹配
    // InitFn fn2 = no_attr; // error: 目标需要 ensure_init 但源没有
    // InitFn fn3 = (InitFn)no_attr; // error: safe zone 中显式强制转换也不能绕过该约束
    _Unsafe {
        InitFn fn4 = (InitFn)no_attr; // ok: unsafe zone 中由用户自行负责，允许强制转换
    }
}
```

通过函数指针的间接调用同样支持 `ensure_init` 效果验证：

```c
_Safe void indirect_call(InitFn fn) {
    int x;
    fn(&_Mut x); // ok: ensure_init 通过函数指针类型识别
    int y = x;   // ok: x 已通过 ensure_init 标记为已初始化
}
```

#### 3.7.5. `__attribute__((ensure_init_if_ret(arg)))`

**概要**：`ensure_init_if_ret` 用于表达「函数内有条件地对入参做初始化」——把「是否初始化」与「函数返回值」绑定，从而减少不必要的声明时初始化。

**动机**：目前毕昇 C 要么在声明变量的函数内初始化，要么把变量传给带 `ensure_init` 的形参由被调用函数初始化（后者要求该形参指向的内存在**所有**返回路径上都被初始化）。但有一类在 C 中广泛存在的接口——返回成功值（如 `0`）时才回填输出参数、返回失败值时不碰它——无法用 `ensure_init` 表达：

```c
// 返回 0 时才初始化 *info；因失败路径不写，标不了 ensure_init
_Safe unsigned int Init(Info *_Borrow info);
```

现状下只能在声明处先用初始化列表做一次冗余初始化，或在调用前用 `__assume_initialized`——后者是不安全调用，会漏掉对 `Init` 内部是否真正初始化 `*info` 的检查，破坏安全性。`ensure_init_if_ret(arg)` 填补这一空缺：返回值等于 `arg` 时才要求被修饰形参指向的内存被初始化，既不破坏安全区安全性，又避免性能敏感场景下的冗余初始化：

```c
_Safe unsigned int Init(Info *_Borrow __attribute__((ensure_init_if_ret(0))) info);
_Safe unsigned int GetSize(const Info *_Borrow info);

_Safe unsigned int example(void) {
    Info info;                                 // 无需声明时初始化
    unsigned int ret = Init(&_Mut info);
    if (ret != 0) return ret;
    unsigned int size = GetSize(&_Const info); // ok：info 已初始化
    return 0;
}
```

##### 语法

`__attribute__((ensure_init_if_ret(arg)))` 接收一个参数 `arg`，表示「函数返回值为 `arg` 时，才要求被修饰的形参指向的内存被初始化」。

```c
int foo1(int *__attribute__((ensure_init_if_ret(2))));                            // ok
_Safe int foo2(int *_Borrow __attribute__((ensure_init_if_ret(0))));              // ok
_Safe int foo3(__attribute__((ensure_init_if_ret(1))) int *_Borrow, int, float);  // ok
```

##### 语义检查

**1. 只能修饰函数形参。**

```c
typedef _Safe int (*FP)(int *_Borrow __attribute__((ensure_init_if_ret(0))), int *_Borrow); // ok
_Safe int foo(int *_Borrow p, __attribute__((ensure_init_if_ret(1))) int *_Borrow q);       // ok

__attribute__((ensure_init_if_ret(1))) int g_a;        // error：不能修饰全局变量
_Safe void bar(int *_Borrow p, int *_Borrow q) {
    int *_Borrow __attribute__((ensure_init_if_ret(0))) m; // error：不能修饰局部变量
}
```

**2. 被修饰形参的类型必须是「指向 non-const 类型的裸指针或可变借用」。**

```c
_Safe int foo(int *_Borrow __attribute__((ensure_init_if_ret(1))), int,
              int *_Borrow __attribute__((ensure_init_if_ret(1))));               // ok
typedef _Safe int (*FP)(int *_Borrow __attribute__((ensure_init_if_ret(1))),
                        int __attribute__((ensure_init_if_ret(0))), int);          // error：第二个形参不是指针/借用
int bar(int *, int, const int *_Borrow __attribute__((ensure_init_if_ret(0))));   // error：第三个形参指向 const
```

**3. 函数返回类型必须是整数类型或 `_Bool`。**

```c
_Safe int   test1(int *_Borrow __attribute__((ensure_init_if_ret(1))), int);   // ok
_Safe _Bool test2(int *_Borrow __attribute__((ensure_init_if_ret(1))), int);   // ok
_Safe float test3(float *_Borrow __attribute__((ensure_init_if_ret(1))), int); // error：返回类型不符合
```

**4. `arg` 必须是整数字面量（整数类型，可为负）。**

```c
_Safe int   test1(int *_Borrow __attribute__((ensure_init_if_ret(1))), int);    // ok
_Safe _Bool test2(int *_Borrow __attribute__((ensure_init_if_ret(-1))), int);   // ok
_Safe int   test4(int *_Borrow __attribute__((ensure_init_if_ret(-1.0f))), int);// error：arg 不是整数字面量
_Safe int   test5(int *_Borrow __attribute__((ensure_init_if_ret('s'))), int);  // error：arg 不是整数字面量
```

**5. 函数指针赋值兼容。** 不允许把「无 `ensure_init_if_ret`」的函数指针类型赋给「有」的；反向（强契约赋给弱契约）允许。两端都有时，对应形参的 `arg` 必须相同。

```c
typedef _Safe int (*FP1)(int *_Borrow __attribute__((ensure_init_if_ret(0))));
typedef _Safe int (*FP2)(int *_Borrow);
_Safe int foo(int *_Borrow);
_Safe int bar(int *_Borrow __attribute__((ensure_init_if_ret(0))));

FP1 g1 = foo; // error
FP1 g2 = bar; // ok
FP2 g3 = foo; // ok
FP2 g4 = bar; // ok
```

**6. 重声明一致性。** 同安全级别（`_Safe`/`_Safe`、`_Unsafe`/`_Unsafe`）的多份声明之间，`ensure_init_if_ret` 的有无及 `arg` 必须一致；若 `_Unsafe` 声明带它，则配对的 `_Safe` 声明对应形参也必须带且 `arg` 相同。

```c
int baz(void *, int, int);
_Safe int baz(void *_Borrow __attribute__((ensure_init_if_ret(0))), int, int);  // ok

_Safe int goo(int *_Borrow __attribute__((ensure_init_if_ret(0))), int);
_Safe int goo(int *_Borrow __attribute__((ensure_init_if_ret(2))), int);        // error：两份 _Safe 声明 arg 不同
```

**7. 与 `ensure_init` 互斥。**

```c
_Safe int t(int *_Borrow __attribute__((ensure_init_if_ret(1))) __attribute__((ensure_init)), int); // error
```

##### 调用端

允许对未初始化变量取地址 / 可变借用，**前提是所得表达式直接作为带 `ensure_init_if_ret` 的实参传入**。调用返回后，只有在「返回值 == `arg`」的分支上才认为该变量已初始化，其他分支仍视为未初始化。

```c
_Safe int bar(int *_Borrow __attribute__((ensure_init_if_ret(0))));

_Safe void foo(void) {
    int a;
    (void)bar(&_Mut a); // ok
    int b = a;          // error：没有对返回值做判断，a 可能未初始化
}
_Safe void baz(void) {
    int a;
    int *_Borrow p = &_Mut a; // error：借用未直接传给 bar，且 a 未初始化
    (void)bar(p);
    int b = a;                // error：a 未初始化
}
_Safe void goo(void) {
    int a;
    int res = bar(&_Mut a);
    if (res != 0) return;
    int b = a;                // ok：a 已初始化
}
```

返回值判断支持四种形式：`e == value`、`e != value`、`value == e`、`value != e`（`value` 为整数 / `_Bool` 字面量）。其中 `e` 可以是：

- 带 `ensure_init_if_ret` 入参的函数调用 `foo(...)`；
- 与该调用关联的变量 `a`——「关联」指 `a` 在判断时的值是由「直接把 `foo(...)` 的返回值赋给 `a`」得到，且从赋值到判断之间 `a` 未被取地址、取可变借用或重新赋值。

```c
_Safe int bar(int *_Borrow __attribute__((ensure_init_if_ret(0))));
_Safe void goo(void) {
    int a;
    int res = bar(&_Mut a);
    if (res != 0) ;           // ok
    if (0 != res) ;           // ok
    if (res == 0) ;           // ok
    if (0 == res) ;           // ok
    if (bar(&_Mut a) != 0) ;  // ok（直接用调用）
    if (0 == bar(&_Mut a)) ;  // ok
}
```

##### 被调用端

对带 `ensure_init_if_ret(arg)` 的形参 `p` 做初始化检查：初始认为 `*p` 未初始化；若在「返回值为 `arg`」的分支上 `*p` 未完成初始化则报错（结构体要求所有字段都被初始化）。

```c
typedef struct { int a; int b; } S;

_Safe unsigned int ok(int a, S *_Borrow __attribute__((ensure_init_if_ret(0))) p) {
    if (a == 2) return 1;       // ok：返回 1 的路径不要求初始化
    p->a = 1; p->b = 2;
    return 0;                   // ok：返回 0 的路径已初始化
}
_Safe unsigned int bad(int a, S *_Borrow __attribute__((ensure_init_if_ret(0))) p) {
    if (a == 2) { p->a = 0; p->b = 0; return 1; } // ok
    p->a = 1;
    return 0;                   // error：(*p).b 未初始化
}
```

**改变 `p` 的指向**是允许的，但在「返回 `arg`」的分支上，改指之前必须先初始化 `*p`。

```c
_Safe int foo(int a, int *_Borrow __attribute__((ensure_init_if_ret(0))) p) {
    if (a == 1) return 1;
    *p = 2;
    int b = 3;
    p = &_Mut b;                // ok：*p 已初始化，可改变 p 的指向
    return 0;
}
```

**把 `p` 赋给其他变量**也允许，但赋值前必须先初始化 `*p`。

```c
_Safe int foo(int *_Borrow __attribute__((ensure_init_if_ret(0))) p) {
    int *_Borrow q = p;         // error：*p 未初始化
    *p = 2;
    return 0;
}
_Safe int bar(int a, int *_Borrow __attribute__((ensure_init_if_ret(0))) p) {
    *p = 2;
    int *_Borrow q = p;         // ok：*p 已初始化
    if (a == 1) return 1;
    return 0;
}
```

##### 已知限制

初始化检查是流敏感的定值分析，不做循环 trip-count 推理，也不追踪路径相关性，因此对某些写法会保守报错：

- 即便循环上界是常量（`for (i = 0; i < 10; i++)`），循环体仍被视作可能执行 0 次，循环内的初始化不算数；用初始化前置或 `do-while` 规避。
- 两个由同一变量控制的 `if` 被视为相互独立，可能误报；把相关检查并入同一 `if` 规避。
- 用 `&&`、`||` 组合多个条件时，组合条件中的子比较暂不被识别为对返回值的判断。
- 把返回值复制到的局部变量若被重新赋值、取地址或取可变借用，原先的关联会被丢弃。


#### 3.7.6. `__assume_initialized`

`__assume_initialized(&x)` 是一个内建函数，用于在某个程序点将变量标记为已初始化。与 `ensure_init` 不同，它**不做契约验证**——由用户保证变量确实已初始化。

每次调用只能标记**一个**变量。如需标记多个变量，须分别调用。

使用 `__assume_initialized` 函数时，参数应使用 `&`。

`__assume_initialized` 只能在 `_Unsafe` 区域中使用，因为它绕过了编译器的初始化验证。

该内建函数是路径敏感的：仅在执行到该调用的 CFG 路径上生效。

```c
_Safe void example_builtin(void) {
    int x;
    _Unsafe { __assume_initialized(&x); } // 从此处起 x 被视为已初始化
    int y = x; // ok
}

// 路径敏感：仅在执行到的路径上生效
_Safe void path_sensitive(int cond) {
    int x;
    if (cond) {
        _Unsafe { __assume_initialized(&x); }
    }
    int y = x; // error: use of possibly uninitialized value: `x`
}

// 对结构体使用：所有字段都被标记为已初始化
_Safe void struct_example(void) {
    struct Pair s;
    _Unsafe { __assume_initialized(&s); }
    int a = s.a; // ok
    int b = s.b; // ok
}
```

注意：数组类型由于存在隐式数组退化（decay），需要使用 `&` 而非直接传递数组名：

```c
_Safe void array_example(void) {
    int arr[3];
    _Unsafe { __assume_initialized(&arr); } // ok: 使用 &
    // __assume_initialized(arr);           // error: 需要 & 表达式
}
```

支持的参数形式（取地址表达式的 lvalue 必须是下列之一的组合）：

| 形式 | 说明 |
|------|------|
| `&x` | 局部变量；若 `x` 是 `ensure_init` 指针参数，同时将 `*x` 标记为已初始化 |
| `&x.f.g...` | 局部变量的结构体字段（任意嵌套深度，纯字段访问） |
| `&*p` | `ensure_init` 指针参数所指向的整个对象 |
| `&p->f.g...` | `ensure_init` 指针所指向结构体的字段（任意嵌套深度，纯字段访问） |

对于 `&p->f...` 形式，当所指向结构体的所有字段都被标记为已初始化后，`*p` 会被自动提升为已初始化（满足 `ensure_init` 契约）。

```c
struct Pair { int a; int b; };

void assume_through_ptr(struct Pair *__attribute__((ensure_init)) out) {
    out->a = 1;
    _Unsafe { __assume_initialized(&out->b); } // 所有字段已覆盖 → *out 已初始化
}
```

**限制：参数路径中不能包含数组下标（`[i]`）。** 初始化分析将数组视为整体单元（不追踪单个元素的状态），因此必须对整个数组断言，而不能断言某个元素。

```c
struct WithArr { int arr[3]; };
struct Outer { struct WithArr s[2]; };

_Safe void array_subscript_example(void) {
    struct WithArr w;
    _Unsafe { __assume_initialized(&w.arr); }    // ok: 断言整个数组字段
    // _Unsafe { __assume_initialized(&w.arr[0]); } // error: 参数不能包含数组下标

    struct Outer o;
    _Unsafe { __assume_initialized(&o); }        // ok: 断言整个结构体
    // _Unsafe { __assume_initialized(&o.s[0].arr); }    // error: 路径中间含下标
    // _Unsafe { __assume_initialized(&o.s[0].arr[0]); } // error: 末端含下标
}
```

#### 3.7.7. 检查模式

通过编译选项 `-uninit-check=<mode>` 控制初始化分析的范围：

| 模式 | 行为 |
|------|------|
| `none` | 禁用初始化分析 |
| `safeonly`（默认） | 仅在 `_Safe` 区域内检查，或函数具有 `ensure_init` 参数时检查 |
| `all` | 在所有代码区域内检查（包括非安全区） |

注：只要启用了初始化分析（即模式不为 `none`）, 则`ensure_init` 契约验证生效（包括非安全区）。

## 4. 并行并发

### 4.1. 无栈协程

#### 4.1.1. 无栈协程简介

无栈协程与有栈协程调用栈由程序员显示分配的不同，协程的调用栈由运行时系统隐式管理的，协程本身不持有自己的调用栈，在切换协程时不需要保存和恢复整个调用栈，只保存协程执行状态。它是通过 `_Async/_Await` 关键字进行定义和调用。`_Async` 用来修饰异步函数，`_Await` 实现异步函数调用。

毕昇 C 的无栈协程目标是支持异步、高并发场景，例如 Web 服务器实现高并发网络通信、异步处理请求、 Web 应用程序的数据库连接池等。

#### 4.1.2. Future/PollResult标准库定义

##### 4.1.2.1. Future/PollResult定义

**Future** 用于描述某个计算（或任务）尚未结束，用一个对象来代理这个未知的结果，这个计算（或任务）可以暂停和恢复。

Future 的执行是通过调用 poll 函数实现的。

**PollResult** 即 poll 函数返回值，用于描述计算（或任务）执行状态以及结果。

```c
struct PollResult<T> {
    _Bool isPending; //isCompleted
    T res;
};

T struct PollResult<T>::is_pending(struct PollResult<T>* this) { ... }
struct PollResult<T> struct PollResult<T>::pending() { ... }
_Bool struct PollResult<T>::is_completed(struct PollResult<T> *this, T *out) { ... }
struct PollResult<T> struct PollResult<T>::completed(T result) { ... }

_Trait Future<T> {
    struct PollResult<T> poll(This* this);
    void free(This* this);
};
```

##### 4.1.2.2. Future/PollResult使用

协程的实现使用到 Future/PollResult 定义，其导入方式有系统默认导入和用户显式导入两种。

用户显式导入时需要引用头文件 `future.hbs`。文件安装在系统 /usr/include 默认搜索路径下（或者安装在指定目录）在编译时使用 -I 编译选项指定路径。

```c
#include  "future.hbs"
```

#### 4.1.3. 语法规则

1. 无栈协程只允许在毕昇 C 编译单元使用

2. 如果函数中使用了 `_Await` 关键字，那么这个函数必须用 `_Async` 修饰。`_Async` 函数内可以有 0,1...n 个 `_Await` 表达式

```c
_Async int TimeOut(int t) {
    return t;
}

_Async int getData1() {
    int t = _Await TimeOut(1000);
    return t;
}

_Async int getData2() {
    _Await getData1();
    _Await TimeOut(1000);
    _Await TimeOut(2000);
    return 0;
}
```

3. `_Async` 函数声明和实现可以分开

```c
#include "string.h"

_Async int ReadBuffer(char *str);

_Async int GetBufferSize() {
    char* Content;
    char ContentCopy[12] = "hello,word!";
    Content = ContentCopy;
    int size = _Await ReadBuffer(Content);
    return size;
}

_Async int ReadBuffer(char *str) {
    char *cstr = "hello,word!";
    if (strcmp(str, cstr) == 0)
      return sizeof(str);
    else return 0;
}
```

4. `_Async` 函数支持递归调用

```c
_Async int f(int n) {
    if (n == 0 || n == 1)
        return 1;
    int tmp = _Await f(n-1);
    return n*tmp;
}
```

5. `_Async` 关键字可以修饰成员函数

```c
_Async void int::g(int* this);

_Async int int::f() {
    int i = 1;
    _Await int::g(&i);
    _Await i.g();
    return 0;
}

_Async void int::g(int* this) {
    _Trait Future<int>* a = read(1);
    _Await a;
}
```

6. `_Async` 函数中可以出现多个不同或相同 `_Await` 表达式

```c
_Async void client1() {
    // client1 send message...
}

_Async void client2() {
    // client2 send message...
}

_Async int Server(int start) {
    // server receive message
    _Await client1();
    if (start < 20)
        _Await client2();
    return start;
}
```

7. `_Async` 函数不支持变量数组，即数组中含有变量

```c
_Async int f() {
    int *VarArray1[n]; // expected-error {{_Async function does not support VariableArrayType}}
    int VarArray2[3][2][n]; // expected-error {{_Async function does not support VariableArrayType}}
    int *VarArrayPtr[n][2][n][5]; // expected-error {{_Async function does not support VariableArrayType}}
    int Array[3]; // support
    int MultiArray[2][3][4][5]; // support
    return 0;
}
```

8. `_Await` 表达式不能出现在 if/while/for/do-while 等判断条件中

```c
_Async int read(int n) {
    // read data...
    return n;
}

_Async int getData() {
    int res = 0;
    if (_Await read(1)) { // expected-error {{_Await expression is not allowed to appear in condition statement of if statement}}
        res = _Await read(1);
    }

    if (res == 2) { // support
        res = _Await read(1);
    }
    return res;
}
```

9. `_Await` 表达式不能和“有副作用”的表达式（例如函数）并存

```c
_Async int read(int n) {
    // read data...
    return n;
}

int test(int a, int b) {
    return 42;
}

_Async int f() {
    test(_Await read(2), _Await read(2)); // expected-error {{_Await expression is not allowed to appear in function parameters}}
    test(t(), _Await read(2)); // expected-error {{_Await expression is not allowed to appear in function parameters}}
    test(3, _Await read(2)); // support
    return 0;
}
```

10. `_Await` 表达式不能出现在复合表达式中，例如：表达式中含有 +、-、*、/、%、&、|、>>、<< 等

```c
_Async int read(int n) {
    // read data...
    return n;
}

_Async int f() {
    int x = _Await read(2) + 3; // expected-error {{_Await expression is not allowed to appear in binary operator expression}}
    int y = _Await read(2); // support
    return 0;
}
```

11. `_Await` 表达式支持 `_Await` 多层嵌套调用

```c
_Async int test0(int n) {
    // read data...
    return n;
}

_Async int test1(int n) {
    // ...
    return n;
}

_Async int test2(int n) {
    // ...
    return n;
}

_Async int test3(int n1, int n2) {
    // ...
    return 0;
}

_Async int f() {
    int start = 0;
    int result1 = _Await test1(_Await test1(start));
    int result2 = _Await test1(_Await test2(start));
    int result3 = _Await test3(2, _Await test1(_Await test2(start)));
    return result1 + result2 + result3;
}
```

12. `_Await` 表达式可以出现在 return 语句中

```c
_Async int read(int n) {
    // read data...
    return n;
}

_Async int f() {
    return _Await read(2);
}
```

#### 4.1.4. 代码样例

```c
# include "future.hbs"
const int MAX = 3;

_Async int read(int a) {
    return 0;
}

_Async int f() {
    int *nptr = NULL;
    int  var[] = {10, 100, 200};
    int  i, *ptr;

    ptr = &var[MAX-1];
    for ( i = MAX; i > 0; i--)
    {
    ptr--;
    }
    int result = _Await read(1);
    result += *ptr;
    return result;
}

_Async void g(int start) {
    int result = start;
    for (int i = 0; i< start; i++) {
        int a = _Await f();
    }
}

int main() {
    _Trait Future<int>* this1 = f();
    this1->poll();
    this1->free();
    // 当 _Async 函数的返回类型是 void 时，我们需要用 struct Void 类型（会自动创建）来对 _Trait Future 实例化
    _Trait Future<struct Void>* this2 = g(5);
    this2->poll();
    this2->free();
    return 0;
}
```

## 5. 工具链

### 5.1. 源源变换

#### 5.1.1. 概述

对于一个毕昇 C 源代码文件，通常情况下只需要使用毕昇 C 编译器直接将其编译为对应的二进制代码并运行。
毕昇 C 编译器也提供了将编写的毕昇 C 源代码转换为等价的标准 C 代码的能力，该能力由编译选项`-rewrite-bsc`提供。
在编译时加上该编译选项后，编译会生成包含等价 C 代码的文件，该文件可再使用 gcc、clang 等编译器编译为目标代码。
一般而言，源源变换适用于如下场景：

1. 希望查看毕昇 C 代码对应的等价的 C 代码；
2. 基于现有的使用 C 语言开发的项目，使用毕昇 C 进行增量开发，但又希望依然使用原先的编译工具链进行编译；
3. 运行程序的目标平台不是毕昇 C 编译器所支持的标准后端时，也需要先将毕昇 C 代码转换为 C 代码，再使用支持目标平台的编译器编译 C 代码。

#### 5.1.2. 使用方式

毕昇 C 编译器的`-rewrite-bsc`编译选项的使用方式和 clang 原生的`-rewrite-objc`编译选项的用法相似。在编译想要被转换的毕昇 C 源码时加上`-rewrite-bsc`即可使用源源变换模式进行编译。

例如：如果你想要对`test.cbs`进行源源变换，那么在编译的时候使用如下命令即可在当前目录下得到一个名为`test.c`的 C 文件，文件的内容即为变换后的 C 源码。

```shell
clang -rewrite-bsc test.cbs
```

如果要变换的文件需要引用某些头文件，那么在源源变换时同样需要使用`-I`选项指定头文件搜索路径。例如如果`test.cbs`依赖于毕昇 C 标准库（后面的章节会具体说明），那么相应的编译命令为：

```shell
clang -rewrite-bsc test.cbs -I/path/to/libcbs/include
```

对于一个名为`xx.cbs`的毕昇 C 文件，源源变换生成的 C 文件的默认文件名为`xx.c`。
源源变换也支持使用`-o`指定输出的 C 文件名，如下示例就将源源变换生成的目标文件命名为`a.c`：

```shell
clang -rewrite-bsc test.cbs -o a.c
```

毕昇 C 编译器也支持同时对多个文件进行变换，不过多个文件同时变换时不支持通过`-o`指定输出的目录或名称，示例如下：

```shell
clang -rewrite-bsc boo.cbs foo.cbs
```

对于源源变换，还有以下几点需要说明和注意：

1. 使用源源变换模式进行编译时，编译器同样会像普通编译模式下那样先对要变换的文件进行词法、语法、语义的检查，对文件中的错误会编译报错，并且不会进行源源变换生成 C 文件。只有能正确通过编译的文件才会在源源变换模式下生成对应的 C 文件。
2. 对于非 cbs 后缀文件使用 -rewrite-bsc 选项，该选项会被忽略（除非添加`-x bsc`将文件按照毕昇 C 进行处理），并报如下 warning 。
    ```shell
    warning: ignoring '-rewrite-bsc' option because rewriting input type 'c' is not supported [-Woption-ignored]
    ```
3. 只需要对毕昇 C 源文件（cbs 文件）进行源源变换，不需要对毕昇 C 头文件（hbs 文件）进行变换。源源变换生成的 C 文件在后续编译时不再依赖任何 hbs 文件，原先 cbs 文件中依赖的 hbs 文件中的内容已经包含在了生成的 C 文件中。因此，可以直接编译生成的 C 文件并生成二进制代码。
4. 源源变换严格区分标准 C 头文件和 hbs 文件，但为方便用户使用，并不要求所有的 hbs 文件后缀名都为 .hbs。具体的区分规则为：
   - 以 .hbs 为后缀的是 hbs 文件
   - 以 .h 为后缀的头文件，且文件的第一行为`#pragma bsc`，则该文件也被视为 hbs 文件
   - 以 .h 为后缀的头文件，且该文件直接或间接包含了 hbs 文件，则该文件也被视为 hbs 文件

#### 5.1.3. 目标文件结构

对于源源变换，还需要对生成的 C 文件的结构进行说明。C 文件中的内容按照如下顺序排列：

1. 头文件包含。这里包含了所有需要引用的标准 C 的头文件，包括直接被 cbs 文件引用的头文件以及被 hbs 文件间接引用的头文件。由于 hbs 文件不再被目标文件依赖，因此目标代码中不会再包含对毕昇 C 头文件的引用。
2. 宏定义。这里包含了所有的 cbs 文件中定义的宏以及 cbs 文件引用的 hbs 文件中定义的宏。
3. 类型别名和枚举定义。这里包含了所有的类型别名定义以及枚举类型定义。如果一个类型别名是对 _Trait 的类型别名，那么它不会出现在目标文件中。如果一个类型别名是对匿名类型的类型别名，那么在目标文件中会为匿名类型加上 typedef 的类型别名作为类型名。如：
    ```c
    // 毕昇 C 文件中：
    typedef struct {
        char *buf;
        int len;
    } MSG;
    
    // 生成的 C 文件中：
    struct _TD_MSG;
    typedef struct _TD_MSG MSG;
    
    struct _TD_MSG {
        char *buf;
        int len;
    };
    ```
4. 类型定义。这里包含了所有 cbs 文件中以及 cbs 文件引用的 hbs 文件中的类型定义。由于毕昇 C 中有泛型类型，因此需要考虑到目标文件中类型定义的顺序问题。毕昇 C 编译器的源源变换采取的是一种类型拓扑排序的方案，保证在定义一个类型时，已经在前面包含了所有它依赖的类型的完整定义。如下用例展示了一个使用毕昇 C标准库构造出的`LinkedList<Vec<int>>`类型以及对应的目标文件中的类型定义顺序：
    ```c
    // 毕昇 C 文件中：
    #include "list.hbs"
    #include "vec.hbs"
    int main() {
        Vec<int> v1 = Vec<int>::new();
        Vec<int> v2 = Vec<int>::new();
        Vec<int> v3 = Vec<int>::new();
        for (int i = 0; i < 10; i = i + 1) {
            v1.push(i);
        }
        for (int i = 0; i < 10; i = i + 1) {
            v2.push(i);
        }
        for (int i = 0; i < 10; i = i + 1) {
            v3.push(i);
        }
        LinkedList<Vec<int>> l = LinkedList<Vec<int>>::new();
        l.push_back(v1);
        l.push_back(v2);
        l.push_back(v3);
        Vec<int> ele = l.pop_back();
        return 0;
    }
    
    // 编译命令：clang -rewrite-bsc main.cbs -I/path/to/libcbs/include
    // clang -rewrite-bsc bishengc_safety.cbs -o bishengc_safety.c
    // clang main.c bishengc_safety.c
    // 生成的 C 文件中的类型定义（省略其他代码）：
    struct RawVec_int {
        int *ptr;
        size_t cap;
    };
    struct LinkedList_struct_Vec_int {
        struct _BSC_ListNode_struct_Vec_int *head;
        struct _BSC_ListNode_struct_Vec_int *tail;
        size_t len;
    };
    struct Vec_int {
        struct RawVec_int buf;
        size_t len;
    };
    struct _BSC_ListNode_struct_Vec_int {
        struct _BSC_ListNode_struct_Vec_int *next;
        struct _BSC_ListNode_struct_Vec_int *prev;
        struct Vec_int element;
    };
    ```
5. 泛型函数、泛型结构体的扩展成员函数的声明。
6. 非泛型函数的定义。
7. 泛型函数、泛型结构体扩展的成员函数的定义。

### 5.2. 调试

支持毕昇c的基础调试功能；源源变换到标准c后，依旧支持对源文件的调试能力，方便程序调优与问题定位。

#### 5.2.1. 直接调试毕昇c源文件

这里的调试主要指gdb调试，能够设置断点，打印基础变量的值。标准c原有的类型，支持正确打印变量的值，毕昇c新增类型，如 _Owned struct，泛型等，调试时也能正确打印。

下面是一个例子：
```c
_Owned struct Person<T>{
_Public:
    char name[50];
    T age;
    ~Person(This this) {
    }
};

char* Person<T>::getName(This *this) {
    return this->name;
}

int Person<T>::getAge(This *this) {
    T a = this->age;
    return a;
}

int main() {
    Person<int> per = {"davi", 21};
    char* name = per.getName();
    int age = per.getAge();
    return 0;
}
```
完整的调试过程如下：

- 生成带调试信息的可执行文件：
  ```shell
  clang -g debug_demo.cbs -o debug_demo_cbs
  ```
  **注意：** clang默认生成gdwarf-5格式的调试文件，安装的gdb版本需不低于10.1，否则存在兼容性问题。另外，也可以加上`-gdwarf-4`编译选项，生成兼容性更好的gdwarf-4格式的调试文件：
  ```shell
  clang -g -gdwarf-4 debug_demo.cbs -o debug_demo_cbs
  ```

- 运行`gdb debug_demo_cbs`，进入gdb调试界面。

- 下面是一些常用的调试命令：

  输入`b main`，设置断点，输入`run`，运行程序，程序运行到断点处，输入`info locals`，查看局部变量的值，输入`p var`，打印变量`var`的值，输入`s`或`n`，步进下一行。


在上面这个调试用例中，记录了一些调试过程信息，如下：
  ```shell
  (gdb) b main
  Breakpoint 1 at 0x1162: file debug_demo.cbs, line 19.
  (gdb) r
  Starting program: /path/to/debug_demo 

  Breakpoint 1, main () at debug_demo.cbs:19
  19	    Person<int> per = {"davi", 21};
  (gdb) n
  20	    char* name = per.getName();
  (gdb) p per
  $1 = {name = "davi", '\000' <repeats 45 times>, age = 21}
  (gdb) s
  _ZN_ZTS6PersonIiEgetName (this=0x7fffffffdce0) at debug_demo.cbs:10
  10	        return this->name;
  (gdb) s
  main () at debug_demo.cbs:21
  21	    int age = per.getAge();
  (gdb) s
  _ZN_ZTS6PersonIiEgetAge (this=0x7fffffffdce0) at debug_demo.cbs:14
  14	        T a = this->age;
  (gdb) s
  15	        return a;
  (gdb) p a
  $2 = 21
  (gdb) s
  _ZN_ZTS6PersonIiE~Person (this=...) at debug_demo.cbs:5
  5	    ~Person(This this) {

  ...

  ```
  在调试中，`_Owned struct`类型的`per` 以及泛型`a` 的值被正确打印出来了。

  **注意：** 函数名称显示为诸如`_ZN_ZTS6PersonIiEgetName`，这是经过编译器名称修饰(`Mangle`)之后的，与原始名称存在差异。

#### 5.2.2. 源源变换后调试毕昇c源文件

将毕昇c源源变换为标准c后，可以直接调试生成的c文件。由于标准c与毕昇c间的关联信息缺失，调试中无法建立其与原始毕昇c之间的映射关系，调试友好性欠佳。在此背景下，我们建立了源源变换后c文件与原cbs文件之间的行号映射关系，从而支持源源变换后对毕昇c源文件的调试能力。

**注意：** 直接的调试对象仍是变换后的c文件，但调试过程中显示的是变换前的cbs文件，本质是建立了一定程度的行号映射关系。

##### 5.2.2.1. 单文件调试

仍以上一小节的cbs文件为例，尝试帮助开发者更好地理解这个调试特性。

完整的调试过程如下：
- 源源变换生成带行号映射关系的标准c文件：
  ```shell
  clang -rewrite-bsc -line debug_demo.cbs -o debug_demo.c
  ```

- 查看生成的c文件：
  ```c
  struct Person_int {
      char name[50];
      int age;
  };
  
  static char *struct_Person_int_getName( struct Person_int *this);
  
  static int struct_Person_int_getAge( struct Person_int *this);
  
  static void struct_Person_int_D( struct Person_int this);
  
  #line 18 "debug_demo.cbs"
  int main(void) {
      struct Person_int per = {"davi", 21};
      _Bool per_is_moved = 0;
      char *name = struct_Person_int_getName(&per);
      int age = struct_Person_int_getAge(&per);
      if (!per_is_moved)
          struct_Person_int_D(per);
      return 0;
  }
  
  #line 9 "debug_demo.cbs"
  static char *struct_Person_int_getName( struct Person_int *this) {
      return this->name;
  }
  
  #line 13 "debug_demo.cbs"
  static int struct_Person_int_getAge( struct Person_int *this) {
      int a = this->age;
      return a;
   }
  
  #line 5 "debug_demo.cbs"
  static void struct_Person_int_D( struct Person_int this) {
      _Bool this_is_moved = 0;
  }
  
  ```
  观察生成的c文件，可以发现，每段函数代码块前都插入了其在原始cbs文件中对应部分的开始行号，格式为
  ```c
  #line raw_number "debug_demo.cbs"
  ```
  其中 `raw_number`为起始行号，`"debug_demo.cbs"`为相对路径下原始cbs文件名。

- 生成带调试信息的可执行文件：
  ```shell
  clang -g debug_demo.c -o debug_demo_c
  ```

- 运行`gdb debug_demo_c`，进入gdb调试界面。

  此时，`gdb` 交互界面显示的是原始cbs文件中的内容和行号，而不是变换后的c文件中的行号。借助`gdb`的常用调试命令，可以在毕昇c原始代码中跳转，并打印一些变量值。
  
  **注意：** 源源变换时若采用相对路径，在调试时需要控制在同一相对路径下；若采用绝对路径，则无此问题。

- 记录了一些调试过程信息，如下：
  ```shell
  (gdb) b main
  Breakpoint 1 at 0x1152: file /path/to/debug_demo.cbs, line 19.
  (gdb) r
  Starting program: /path/to/debug_demo_c
  
  Breakpoint 1, main () at /path/to/debug_demo.cbs:19
  19	    Person<int> per = {"davi", 21};
  (gdb) s
  20	    char* name = per.getName();
  (gdb) s
  21	    int age = per.getAge();
  (gdb) s
  struct_Person_int_getName (this=0x7fffffffdcf0) at /path/to/debug_demo.cbs:10
  10	        return this->name;
  (gdb) s
  main () at /path/to/debug_demo.cbs:22
  22	    return 0;
  (gdb) s
  struct_Person_int_getAge (this=0x7fffffffdcf0) at /path/to/debug_demo.cbs:14
  14	        T a = this->age;
  (gdb) s
  15	        return a;
  (gdb) info locals
  a = 21
  (gdb) s
  main () at /path/to/debug_demo.cbs:23
  23	}
  (gdb) s
  24
  (gdb) info locals
  per = {name = "davi", '\000' <repeats 45 times>, age = 21}
  per_is_moved = false
  name = 0x7fffffffdcf0 "davi"
  age = 21
  (gdb) s
  struct_Person_int_D (this=...) at /path/to/debug_demo.cbs:6
  6	    }
  
  ...
  ```

##### 5.2.2.2. 多文件调试

当存在多文件引用时，调试时也能在多文件间正确跳转。例如，存在一个包含基本数学运算函数的源文件`basic_math.cbs`及其头文件`basic_math.hbs`，在`cacl_demo.cbs`中调用`basic_math`中的函数。
- 源文件

  `basic_math.cbs`
  ```c
  #include "basic_math.hbs"
  
  int struct MyStruct::add(int a, int b){
      int res = a + b;
      return res;
  }
  
  int struct MyStruct::subtract(int a, int b) {
      int res = a - b;
      return res;
  }
  
  int struct MyStruct::multiply(int a, int b) {
      int res = a * b;
      return res;
  }
  
  int struct MyStruct::divide(int a, int b) {
      if (b != 0) {
          return a/b;
      } else {
          return 0;
      }
  }
  ```

  `basic_math.hbs`
  ```c
  #ifndef REWRITE_H
  #define REWRITE_H
  
  struct MyStruct {
      int a;
      int b;
  };
  
  int struct MyStruct::add(int a, int b);
  int struct MyStruct::subtract(int a, int b);
  int struct MyStruct::multiply(int a, int b);
  int struct MyStruct::divide(int a, int b);
  
  T min<T>(T a, T b) {
      return a > b ? b : a;
  }
  
  T max<T>(T a, T b) {
      return a > b ? a : b;
  }
  
  #endif
  ```

  `cacl_demo.cbs`
  ```c
  #include "basic_math.hbs"
  
  int main() {
      struct MyStruct s = {4, 2};
      int r1 = struct MyStruct::add(s.a, s.b);
      int r2 = struct MyStruct::subtract(s.a, s.b);
      int r3 = struct MyStruct::multiply(s.a, s.b);
      int r4 = struct MyStruct::divide(s.a, s.b);
      int c1 = max<int>(s.a, s.b);
      int c2 = min<int>(s.a, s.b);
      return 0;
  }
  ```
- 源源变换
  ```shell
  clang -rewrite-bsc -line basic_math.cbs basic_math.hbs calc_demo.cbs
  ```
  变换后，生成了`basic_math.c`、`basic_math.h`与`calc_demo.c`，分别如下：
  
  `basic_math.c`
  ```c
  #include "basic_math.h"
  
  #line 3 "basic_math.cbs"
  int struct_MyStruct_add(int a, int b) {
      int res = a + b;
      return res;
  }
  
  #line 8 "basic_math.cbs"
  int struct_MyStruct_subtract(int a, int b) {
      int res = a - b;
      return res;
  }
  
  #line 13 "basic_math.cbs"
  int struct_MyStruct_multiply(int a, int b) {
      int res = a * b;
      return res;
  }
  
  #line 18 "basic_math.cbs"
  int struct_MyStruct_divide(int a, int b) {
      if (b != 0) {
          return a / b;
      } else {
          return 0;
      }   
  }
  ```
  `basic_math.h`
  ```c
  #ifndef REWRITE_H
  #define REWRITE_H
  
  struct MyStruct {
      int a;
      int b;
  };
  
  #line 9 "basic_math.hbs"
  int struct_MyStruct_add(int a, int b);
  
  #line 10 "basic_math.hbs"
  int struct_MyStruct_subtract(int a, int b);
  
  #line 11 "basic_math.hbs"
  int struct_MyStruct_multiply(int a, int b);
  
  #line 12 "basic_math.hbs"
  int struct_MyStruct_divide(int a, int b);
  #endif
  ```

  `calc_demo.c`
  ```c
  #include "basic_math.h"
  
  static int min_int(int a, int b); 
  
  static int max_int(int a, int b); 
  
  #line 3 "calc_demo.cbs"
  int main(void) {
      struct MyStruct s = {4, 2}; 
      int r1 = struct_MyStruct_add(s.a, s.b);
      int r2 = struct_MyStruct_subtract(s.a, s.b);
      int r3 = struct_MyStruct_multiply(s.a, s.b);
      int r4 = struct_MyStruct_divide(s.a, s.b);
      int c1 = max_int(s.a, s.b);
      int c2 = min_int(s.a, s.b);
      return 0;
  }
  
  #line 14 "./basic_math.hbs"
  static int min_int(int a, int b) {
      return a > b ? b : a;
  }
  
  #line 18 "./basic_math.hbs"
  static int max_int(int a, int b) {
      return a > b ? a : b;
  }
  ```
- 生成调试信息
  ```shell
  clang -g calc_demo.c basic_math.c -o calc_demo_c
  ```
- gdb调试
  ```shell
  gdb calc_demo_c
  ```
  调试中，可以正常跳转到原始`cbs`和`hbs`文件，部分调试过程示例如下：
  ```shell
  Breakpoint 1, main () at /path/go/calc_demo.cbs:4
  4	  struct MyStruct s = {4, 2};
  (gdb) n
  5	  int r1 = struct MyStruct::add(s.a, s.b);
  (gdb) s
  struct_MyStruct_add (a=4, b=2) at /path/go/basic_math.cbs:4
  4	  int res = a + b;
  (gdb) s
  5	  return res;
  (gdb) p res
  $1 = 6
  (gdb) c
  Continuing.
  
  Breakpoint 2, max_int (a=4, b=2) at ./basic_math.hbs:19
  19	  return a > b ? a : b;
  
  ...
  ```
  该调试特性支持的功能与问题，汇总如下：
- 调试的对象是变换后的标准c，不改其运行逻辑、变量信息等。
- 调试中显示的代码位置指向原始cbs文件，支持多源文件的调试跳转。
- 当源源变换前后代码行数存在差异、无法逐行映射时，例如 _Owned struct析构函数、 _Trait 等会生成新代码的特性，显示的调试位置可能不准确，需要开发者注意。


### 5.3. IDE插件

## 6. 标准库

### 6.1. 安全API

#### 6.1.1. `safe_malloc`

`safe_malloc`是 BiShengC 语言提供的一个安全的内存分配函数。
该函数接收一个泛型类型`T`的变量，表示要分配的内存的大小以及分配后对内存的初始化。
该函数的返回值为`T * _Owned`类型，即指向分配好的堆内存的`_Owned`指针。
一些具体的使用例子如下。

在 C 语言中，如果我们需要申请一段堆内存，我们可以使用`malloc`函数进行分配，然后给该内存赋值，如：

```c
void example() {
    int *p = (int *)malloc(sizeof(int));
    *p = 2;
}
```

然而，这样分配的内存不会在`p`的作用域结束时检查是否调用了`free`进行释放，会造成内存泄漏。
此外，如果再使用一个指针也指向该内存，并在作用域结束时释放，则会出现重复释放的问题，如：

```c
void example() {
    int *p = (int *)malloc(sizeof(int));
    *p = 2;
    int *q = p;
    free(p);
    free(q); // error: double free!
}
```

使用 BiShengC 语言即可解决这种问题，相应的代码如下：

```c
_Safe void example(void) {
    int * _Owned p = safe_malloc(2);
    int * _Owned q = p;
    _Unsafe {
        safe_free((void * _Owned)q);
    }
}
```

在使用 BiShengC 语言改写后的代码中，如果我们在函数退出前什么都不做，则会出现编译错误`"memory leak of value: `q`"`，避免了内存泄漏问题的发生；
如果我们在函数退出前同时调用`safe_free((void * _Owned)p)`和`safe_free((void * _Owned)q)`，则会出现编译错误`"use of moved value: `p`"`，避免了重复释放问题的发生。

那么对于更为复杂的结构体类型，该如何正确使用`safe_malloc`进行内存分配呢？
对于结构体类型，需要首先在栈上构造出相应的变量，然后传给`safe_malloc`在堆上完成相应内存的分配，以下代码为具体示例：

```c
struct S {
    int * _Owned p;
    int * _Owned q;
};

_Safe void example(void) {
    struct S s = { .p = safe_malloc(1), .q = safe_malloc(2) };
    struct S * _Owned sp = safe_malloc(s);
    ...
}
```

#### 6.1.2. `safe_calloc`

`safe_calloc` 是 BiShengC 语言提供的一个安全的内存分配函数，其行为与 C 标准库的 `calloc` 类似。
该函数接收一个泛型类型参数 `T`，在堆上分配一块足以存放一个 `T` 类型对象的内存，并将该内存**零初始化**。
该函数的返回值为 `T * _Owned` 类型，即指向分配好的堆内存的 `_Owned` 指针。
当 `_Owned` 指针生命周期结束前，必须通过 `safe_free` 进行释放。

与 `safe_malloc` 不同，`safe_calloc` 不接受用于初始化的实参，所有字段（包括结构体成员与指针成员）均按零值初始化。
对于指针类型的成员，零初始化意味着其值为空指针。对不可被零初始化的类型使用 `safe_calloc` 将导致编译报错。

```c
#include "bishengc_safety.hbs"

_Safe void example(void) {
    int * _Owned p = safe_calloc<int>();
    // *p == 0

    struct S {
        int * _Owned _Nullable a;
        int * _Owned _Nullable b;
    };
    struct S * _Owned sp = safe_calloc<struct S>();
    // sp->a 与 sp->b 均为空指针

    ...
}
```

#### 6.1.3. `safe_free`

`safe_free`是 BiShengC 语言提供的一个安全的内存释放函数。
该函数接收一个`void * _Owned`类型的指针，表示要释放的内存的地址。
该函数的返回值为`void`类型。
因此，在调用`safe_free`进行释放前需要将`_Owned`指针显式地强制转换为`void * _Owned`类型，具体的转换规则可参考 [3.1.4.3](#3143-强制类型转换) 节。
一些具体的使用例子如下。

```c
struct S {
    int * _Owned p;
    int * _Owned q;
};

_Safe void example(void) {
    int * _Owned pa = safe_malloc(199);
    struct S s = { .p = safe_malloc(1), .q = safe_malloc(2) };
    struct S * _Owned sp = safe_malloc(s);
    _Unsafe {
        safe_free((void * _Owned)pa);
        safe_free((void * _Owned)sp->p);
        safe_free((void * _Owned)sp->q);
        safe_free((void * _Owned)sp); // 必须先释放 sp->p 和 sp->q，才能释放 sp
    }
}
```

#### 6.1.4. `safe_malloc_array`

`safe_malloc_array` 是 BiShengC 语言提供的用于分配数组的安全内存分配函数。
该函数接收一个泛型类型参数 `T`、数组长度 `size` 以及用于初始化每个元素的值 `initializer`，在堆上分配一块足以存放 `size` 个 `T` 类型对象的内存，并将每个元素初始化为 `initializer`。
该函数的返回值为 `T * _Owned _ArrayElem` 类型，即指向分配好的堆数组的 `_Owned _ArrayElem` 指针。
关于 `_Owned _ArrayElem` 指针的语义与使用限制，可参考 [3.3.1.2](#3312-_owned-_arrayelem-指针) 节。

```c
#include "bishengc_safety.hbs"

_Safe void example(void) {
    int * _Owned _ArrayElem arr = safe_malloc_array(10, 2);
    // arr[0] 至 arr[9] 的值均为 2

    arr[3] = 42;
    safe_free_array((void * _Owned _ArrayElem)arr);
}
```

当 `size` 为 0 时，该调用返回一个非空但不能解引用的指针。该指针也需要通过 `safe_free_array` 释放，否则仍视为内存泄漏。

`safe_malloc_array` 的实现通过 `malloc` 进行分配，因此由 `safe_malloc_array` 分配得到的 `_Owned _ArrayElem` 指针可以在转为裸指针后使用 `free` 释放；由 `malloc` 等分配函数分配得到的裸指针也可以在转为 `_Owned _ArrayElem` 指针后使用 `safe_free_array` 进行释放。

#### 6.1.5. `safe_free_array`

`safe_free_array` 是 BiShengC 语言提供的用于释放数组的安全内存释放函数。
该函数接收一个 `void * _Owned _ArrayElem` 类型的指针，表示要释放的堆数组。
该函数的返回值为 `void` 类型。
因此，在调用 `safe_free_array` 进行释放前需要将 `T * _Owned _ArrayElem` 指针显式地转换为 `void * _Owned _ArrayElem` 类型。

```c
#include "bishengc_safety.hbs"

_Safe void example(void) {
    int * _Owned _ArrayElem arr = safe_malloc_array(4, 7);
    safe_free_array((void * _Owned _ArrayElem)arr);
}
```

由于 `_Owned _ArrayElem` 指针不允许进行指针算术运算，在调用 `safe_free_array` 前必须保证传入的是未偏移的数组首地址，否则可能导致 invalid free。若需要对数组进行偏移访问，应先生成 `_Borrow _ArrayElem` 借用指针后再进行运算（参见 [3.3.1.2](#3312-_owned-_arrayelem-指针) 节）。

#### 6.1.6. `safe_calloc_array`

`safe_calloc_array` 是 BiShengC 语言提供的用于分配数组的安全内存分配函数，其行为与 C 标准库的 `calloc` 类似。
该函数接收一个泛型类型参数 `T` 以及数组长度 `size`，在堆上分配一块足以存放 `size` 个 `T` 类型对象的内存，并将所有元素**零初始化**。
该函数的返回值为 `T * _Owned _ArrayElem` 类型。
当 `_Owned _ArrayElem` 指针生命周期结束前，必须通过 `safe_free_array` 进行释放。

与 `safe_malloc_array` 不同，`safe_calloc_array` 不接受用于初始化每个元素的实参，所有元素均按零值初始化。
由于缺少初始化实参，调用 `safe_calloc_array` 时需要显式指定模板类型参数，例如 `safe_calloc_array<int>(10)`。

```c
#include "bishengc_safety.hbs"

_Safe void example(void) {
    int * _Owned _ArrayElem arr = safe_calloc_array<int>(10);
    // arr[0] 至 arr[9] 的值均为 0

    arr[5] = 100;
    safe_free_array((void * _Owned _ArrayElem)arr);

    int * _Owned _ArrayElem empty = safe_calloc_array<int>(0);
    safe_free_array((void * _Owned _ArrayElem)empty);
}
```

当 `size` 为 0 时，该调用返回一个非空但不能解引用的指针，与 `safe_malloc_array` 的行为一致，同样需要通过 `safe_free_array` 释放。

`safe_calloc_array` 的实现通过 `calloc` 进行分配，因此由 `safe_calloc_array` 分配得到的 `_Owned _ArrayElem` 指针可以在转为裸指针后使用 `free` 释放；由 `calloc` 等分配函数分配得到的裸指针也可以在转为 `_Owned _ArrayElem` 指针后使用 `safe_free_array` 进行释放。

#### 6.1.7. `safe_swap`

`safe_swap`是 BiShengC 语言提供的一个安全交换两个变量的值的函数。
该函数是一个泛型函数,接收两个类型为`T* _Borrow`类型的参数,即需要交换的变量的值的借用。
该函数的返回值为`void`类型,该 API 的主要作用为在交换两个变量的值时,同时能交换两个变量所拥有的所有权.
一个具体的使用例子如下。

```c
_Owned struct S {
_Public:
    int* _Owned p;
    int* _Owned q;
    ~S(S this) {
        safe_free((void* _Owned)this.p);
        safe_free((void* _Owned)this.q);
    }
};

_Safe void example(void) {
    S s1 = { .p = safe_malloc(1), .q = safe_malloc(2) };
    S s2 = { .p = safe_malloc(3), .q = safe_malloc(4) };
    safe_swap(&_Mut s1, &_Mut s2); // 交换后,s1.p为3,s1.q为4
}
```

#### 6.1.8. `forget`

`forget` 主要用于获取变量的所有权并且“忘记”它，该函数是一个泛型函数，接收一个类型为泛型类型`T`的变量，表示要“忘记”的值：
1. 如果该变量是 _Owned 指针，那么该指针指向的内存不会被释放；
2. 如果该变量是 _Owned struct 类型，那么不会调用其析构函数。

在一些特殊的场景，用户希望取得变量的所有权而不通过该变量来释放管理的底层资源（如堆内存或文件句柄，这些资源可能已经通过裸指针操作被转移或释放），例如：

```c
#include "bishengc_safety.hbs"
#include <string.h>
_Owned struct Resource {
_Public:
    char *_Owned s;
    ~Resource(This this) {
        safe_free((void *_Owned)this.s);
    }
};

void get_resource(char* val) {
    Resource r = { .s = safe_malloc<char>(100) };
    memcpy(val, (const void *)&r, sizeof(Resource)); // Resource中的资源被转移
    forget<Resource>(r); //此时 forget 函数会获取 r 的所有权，但是并不会调用 Resource 的析构函数来释放堆内存
}

int main() {
    char val[sizeof(Resource)];
    get_resource(val);
    return 0;
}
```

### 6.2. 安全数据结构

#### 6.2.1. `Vec`

`Vec`是 BiShengC 语言提供的安全动态数组类型,数组元素的数据存储在堆上，它是一个泛型数据结构，接受一个类型参数 T 表示其内部存储的数据的类型，其使用示例如下：

```c
#include "vec.hbs"

_Safe void example(void) {
    Vec<int> vec = Vec<int>::new(); // 分配一个空的动态数组
    vec.push(1); // 向数组中插入元素
    vec.push(2); // 向数组中插入元素

    size_t len = vec.length();
    int elem = *vec.get(0); // elem is 1

    vec.set(0, 7); // 将索引为0处的值置为7
    elem = *vec.get(0); // elem is 7

    vec.shrink_to_fit();
    // vec作用域结束时自动调用析构函数释放堆内存空间,无需手动释放
}
```

索引：
`Vec`目前不支持直接使用索引进行访问，但其提供了`set`和`get`等方法,可以传入想要访问的元素的索引，通过函数调用的方式进行访问。

容量及重新分配：
`Vec`的容量是为将来添加到`Vec`中的任何元素预先分配的内存空间大小，不要将其与`Vec`的长度向混淆，`Vec`的长度表示的是当前`Vec`内实际存储的元素的数量。
如果`Vec`的长度超过其容量，则容量会自动增长，且其元素也需要重新分配(这由Vec内部实现)。

注意：在使用`Vec<T>`时，需要保证 T 是 copy 语义的类型或 _Owned struct 类型，否则会编译期报错，因为对于其它类型，编译器无法知道应该如何清理内存。

`Vec`提供的对外接口及相应的使用用例如下：

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe size_t Vec<T>::capacity(const Vec<T>* _Borrow this)`|返回数组的容量|size_t cap = vec.capacity();|
|`_Safe void Vec<T>::clear(Vec<T>* _Borrow this)`|清空数组的所有元素|vec.clear();|
|`_Safe const T* _Borrow Vec<T>::get(const Vec<T>* _Borrow this, size_t index)`|获取数组中下标为index的元素的不可变借用(**要做边界检查**)|const int* _Borrow elem = vec.get(2);|
|`_Safe T* _Borrow Vec<T>::get_mut(Vec<T>* _Borrow this, size_t index)`|获取数组中下标为index的元素的可变借用(**要做边界检查**)|int* _Borrow elem = vec.get_mut(2);|
|`_Safe _Bool Vec<T>::is_empty(const Vec<T>* _Borrow this)`|判断数组是否为空|_Bool flag = vec.is_empty();|
|`_Safe size_t Vec<T>::length(const Vec<T>* _Borrow this)`|返回数组的长度|size_t len = vec.length();|
|`_Safe Vec<T> Vec<T>::new(void)`|创建一个空的数组|Vec<int> vec = Vec<int>::new();|
|`_Safe T Vec<T>::pop(Vec<T>* _Borrow this)`|弹出数组中最后一个元素(**要做边界检查**)|int last = vec.pop();|
|`_Safe void Vec<T>::push(Vec<T>* _Borrow this, T value)`|向数组的尾部插入一个值为value的元素|vec.push(2);|
|`_Safe T Vec<T>::remove(Vec<T>* _Borrow this, size_t index)`|从数组中移除下标为index的元素(**要做边界检查**)|int m = vec.remove(3);|
|`_Safe void Vec<T>::set(Vec<T>* _Borrow this, size_t index, T value)`|将数组中下标为index的元素置为value(**要做边界检查**)|vec.set(3, 5);|
|`_Safe void Vec<T>::shrink_to_fit(Vec<T>* _Borrow this)`|调整数组占用的内存空间,将容量缩减到数组的长度|vec.shrink_to_fit();|
|`_Safe Vec<T> Vec<T>::with_capacity(size_t cap)`|创建一个容量为cap的空数组|Vec<int> vec = Vec<int>::with_capacity(120);|

注：`Vec`提供的安全 API 内部实现了严格的边界访问检查逻辑，确保在使用这些 API 时不会出现越界访问的问题。当发生越界访问时，程序打印当前的函数调用栈，并终止执行。

#### 6.2.2. `String`

`String`是 BiShengC 语言提供的 C 风格的安全字符串类型，用于安全地管理分配在堆上的字符串。它拥有字符串内容的所有权，字符串的内容存储在堆分配的缓冲区中。其使用示例如下：

```c
String hello = String::from("Hello, world!");

hello.push('w');
hello.set(0, 'k');

String world = String::from("hello bishengc");
hello.equals(&_Const world);

String new_s = world.slice(1, 4);
```

内部表示：
`String`内部由三个部分组成：指向某些字节的指针、长度和容量。该指针指向`String`用于存储其数据的内部缓冲区，长度是当前存储在缓冲区中的字节数，容量是当前缓冲区的大小（以字节为单位）。因此，长度会始终小于或等于容量。存储字节的缓冲区始终分配在堆上。

`String`提供的对外接口及相应的使用用例如下：

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe char* _Borrow String::as_mut_str(String* _Borrow this)`|返回对字符串的可变借用|char* _Borrow str = s.as_mut_str();|
|`_Safe const char* _Borrow String::as_str(const String* _Borrow this)`|返回对字符串的不可变借用|const char* _Borrow str = s.as_str();|
|`_Safe char String::at(const String* _Borrow this, size_t index)`|返回字符串的下标为index处的值（**要做边界检查**）|char c = s.at(2);|
|`_Safe size_t String::capacity(const String* _Borrow this)`|返回字符串的容量|size_t cap = s.capacity();|
|`_Safe _Bool String::equals(const String* _Borrow this, const String* _Borrow other);`|比较两个字符串是否相等|_Bool flag = str1.equals(str2);|
|`_Safe size_t String::find(const String* _Borrow this, char c)`|查找字符串中是否有字符c，返回第一次找到时的下标，如果没有找到，则返回bsc_string_no_pos|size_t pos = s.find('A');|
|`_Unsafe String String::from(const char* str)`|根据字符串字面量创建字符串|String s = String::from("hello");|
|`_Safe const T* _Borrow String::get(const String* _Borrow this, size_t index)`|获取字符串中下标为index的元素的不可变借用(**要做边界检查**)|const char* _Borrow elem = s.get(2);|
|`_Safe T* _Borrow String::get_mut(String* _Borrow this, size_t index)`|获取字符串中下标为index的元素的可变借用(**要做边界检查**)|char* _Borrow elem = s.get_mut(2);|
|`_Safe _Bool String::is_empty(const String* _Borrow this)`|判断字符串是否为空|_Bool flag = s.is_empty();|
|`_Safe size_t String::length(const String* _Borrow this)`|返回字符串的长度|size_t len = s.length();|
|`_Safe String String::new(void)`|创建一个空的字符串|String s = String::new();|
|`_Safe void String::push(String* _Borrow this, char value)`|向字符串的尾部插入一个值为value的元素|s.push('h');|
|`_Safe void String::set(String* _Borrow this, size_t index, char value)`|将字符串中下标为index的元素置为value(**要做边界检查**)|s.set(3, '5');|
|`_Safe void String::shrink_to_fit(String* _Borrow this)`|调整字符串占用的内存空间,将容量缩减到字符串的长度|s.shrink_to_fit();|
|`_Safe String String::slice(const String* _Borrow this, size_t start, size_t length);`|字符串切片，如果 start 大于字符串的长度，则会触发越界；对于 length，如果start + length 大于字符串的长度，则字符串切片只会取到字符串的结尾处|String new_string = s.slice(0, 5);|
|`_Safe String String::with_capacity(size_t cap)`|创建一个容量为cap的空字符串|String s = String::with_capacity(20);|

注：`bsc_string_no_pos`实际上是一个很大的 size_t 类型的值，即 SIZE_MAX。

#### 6.2.3. `Slice`

`Slice`是 BiShengC 语言提供的不可变切片类型，用于对一段连续的且类型相同的内存进行读操作。其使用示例如下：

```c
#include "slice.hbs"

int main() {
  int arr[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  Slice<int> slice = Slice<int>::new(&_Const *arr, 5);
  const int *_Borrow _Nonnull first = slice.get(2);
  if (*first == 3) {
    printf("The third element of the slice is 3\n");
  }
  return 0;
}
```

索引：
`Slice`目前不支持直接使用索引进行访问，但其提供了`get`等方法,可以传入想要访问的元素的索引，通过函数调用的方式进行访问。在访问`Slice`的元素时，会做**边界检查**。

`Slice`提供的对外接口及相应的使用用例如下：

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Unsafe Slice<T> Slice<T>::new(const T *_Borrow _Nonnull ptr, size_t len)`|根据指针和长度创建切片|Slice<int> slice = Slice<int>::new(&_Const *arr, 5);|
|`_Safe Slice<T> Slice<T>::sub(This this, size_t start, size_t end)`|从现有的切片中创建子切片|Slice<int> sub = slice.sub(0, 5);|
|`_Safe const T *_Borrow _Nonnull Slice<T>::get(This this, size_t index)`|返回切片中下标为index的元素的不可变借用|const int *_Borrow _Nonnull first = slice.get(0);|
|`_Safe size_t Slice<T>::length(This this)`|返回切片长度|size_t len = slice.length();|
|`_Safe _Bool Slice<T>::is_empty(This this)`|判断切片是否为空|_Bool is_empty = slice.is_empty();|
|`_Unsafe const T *_Nonnull Slice<T>::as_ptr(This this)`|返回切片中保存的裸指针|const T *_Nonnull ptr = slice.as_ptr();|

#### 6.2.4. `SliceMut`

`SliceMut`是 BiShengC 语言提供的可变切片类型，用于对一段连续的且类型相同的内存进行读写操作。其使用示例如下：

```c
#include "slice.hbs"

int main() {
  int arr[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  SliceMut<int> slice = SliceMut<int>::new(&_Const *arr, 5);
  const int *_Borrow _Nonnull first = slice.get(2);
  *first = 5;
  if (*first == 5) {
    printf("The third element of the slice is 5\n");
  }
  return 0;
}
```

索引：
`SliceMut`目前不支持直接使用索引进行访问，但其提供了`set`和`get`等方法,可以传入想要访问的元素的索引，通过函数调用的方式进行访问。在访问`SliceMut`的元素时，会做**边界检查**。

`SliceMut`提供的对外接口及相应的使用用例如下：

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Unsafe SliceMut<T> SliceMut<T>::new(T *_Borrow _Nonnull ptr, size_t len)`|根据指针和长度创建可变切片|SliceMut<int> slice = SliceMut<int>::new(&_Mut *arr, 5);|
|`_Safe Slice<T> SliceMut<T>::sub(This this, size_t start, size_t end)`|从现有的切片中创建不可变子切片|Slice<int> sub = slice.sub(0, 4);|
|`_Safe SliceMut<T> SliceMut<T>::sub_mut(This this, size_t start, size_t end)`|从现有的切片中创建可变子切片|SliceMut<int> sub_mut = slice.sub_mut(0, 4);|
|`_Safe const T *_Borrow _Nonnull SliceMut<T>::get(This this, size_t index)`|返回切片中下标为index的元素的不可变借用|const int *_Borrow _Nonnull first = slice.get(0);|
|`_Safe T *_Borrow _Nonnull SliceMut<T>::get_mut(This this, size_t index)`|返回切片中下标为index的元素的可变借用|int *_Borrow _Nonnull first = slice.get_mut(0);|
|`_Safe size_t SliceMut<T>::length(This this)`|返回切片长度|size_t len = slice.length();|
|`_Safe _Bool SliceMut<T>::is_empty(This this)`|判断切片是否为空|_Bool is_empty = slice.is_empty();|
|`_Unsafe const T *_Nonnull SliceMut<T>::as_ptr(This this)`|返回切片中保存的裸指针|const T *_Nonnull ptr = slice.as_ptr();|
|`_Unsafe T *_Nonnull SliceMut<T>::as_mut_ptr(This this)`|返回切片中保存的裸指针|T *_Nonnull ptr = slice.as_mut_ptr();|

#### 6.2.5. LinkedList

##### 6.2.5.1. 概述

LinkedList是由双向链表来实现的，支持前后两种移动方向。

注意：在使用`LinkedList<T>`时，需要保证 T 是 copy 语义的类型或 _Owned struct 类型，否则会编译期报错，因为对于其它类型，编译器无法知道应该如何清理内存。

##### 6.2.5.2. 头文件

```c
#include "list.hbs"
```

##### 6.2.5.3. API

| 对外接口                                             | 接口功能                                                     | 代码示例                                                     |
| ---------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------ |
| LinkedList<T> LinkedList<T>::new()                   | 创建一个链表对象。                                           | #include "list.hbs"<br />// 新增int类型链表对象<br />LinkedList<int> list = LinkedList<int>::new(); |
| void LinkedList<T>::push_back(T el)                  | 向链表尾端插入一个数据。                                     | list.push_back(5); // 向链表尾插入数据5                      |
| void LinkedList<T>::push_front(T el)                 | 向链表表头插入一个数据。                                     | list.push_front(6); // 向链表头插入数据6                     |
| size_t LinkedList<T>::length()                       | 返回链表中已有数据个数。                                     | assert(2 == list.length());                                  |
| _Bool LinkedList<T>::is_empty()                      | 判断表是否为空表。                                           | list.push_back(4); <br />assert(0 == list.is_empty())        |
| T LinkedList<T>::pop_back()                          | 返回表尾数据，并将该数据从链表中删除。表不可以为空表。       | assert(5 == list.pop_back());                                |
| T LinkedList<T>::pop_front()                         | 返回表头数据，并将该数据从链表中删除。表不可以为空表。       | assert(6 == list.pop_front());                               |
| void LinkedList<T>::remove(T el)                     | 删除链表中所有指定数据。                                     | list.push_back(4);<br />list.remove(4); // 删除链表中所有值为4的元素 |
| void LinkedList<T>::clear()                          | 删除链表中所有数据。                                         | list.clear();                                                |
| T LinkedList<T>::front()                             | 返回表头数据，表不可以为空表。                               | list.push_front(3);<br />assert(3 == list.front())           |
| T LinkedList<T>::back()                              | 返回表尾数据，表不可以为空表。                               | list.push_back(3); <br />assert(3 == list.back())            |
| _Bool LinkedList<T>::contains(T el)                  | 检查表是否包含指定数据。                                     | list.push_back(6); <br />assert(1 == list.contains(6))       |
| LinkedList<T> LinkedList<T>::split_off(size_t index) | 在索引处将链表拆分为两个链表，<br />索引值不能大于链表中数据个数，<br />新链表包含索引值以及往后的数据。<br />若索引值为0，则旧链表将为空表，<br />若索引值等于表数据个数，则新链表将为空。 | LinkedList<int> l = LinkedList<int>::new();<br />l.push_back(5);<br />l.push_back(6);<br />l.push_front(4);<br />l.push_front(3);<br />LinkedList<int> j = l.split_off(2);<br />// 链表被拆为l和j两个表<br />assert(2 == l.length());      // 3 4<br />assert(2 == j.length());      // 5 6 |

#### 6.2.6. `Option`

`Option`是 BiShengC 语言提供的用于处理可选值的一个安全数据结构，用来表示一个值可能存在或不存在这两种情况。
它是一个泛型数据结构，具有一个泛型参数`T`，用于表示其存储的可选值的类型。使用`Option`可以安全地处理一些函数的返回值，避免使用空指针等数据结构来表示一个值不存在的情况。
`Option`有两种状态，即`Some`和`None`，`Some`状态中会包含所需要的类型的值，而`None`状态中没有存储相应的值。
它的一个使用示例如下：

```c
#include "option.hbs" // Option类型声明在该头文件中，使用时需包含该头文件

Option<size_t> string_find(String str) {
    size_t pos = str.find('k');
    if (pos == bsc_string_no_pos) {
        return Option<size_t>::None(); // 如果没有找到k，则返回None
    } else {
        return Option<size_t>::Some(pos); // 如果找到了k，则返回Some，且Some中包含相应的值pos
    }
}

void use_option() {
    size_t pos = -1;
    String s = String::from("hello");
    Option<size_t> res = string_find(s);
    if (res.is_some()) { // 判断res是Some还是None
        pos = option_unwrap(res); // 对于Some，则调用option_unwrap获取其中的值
    } else {
        return; // 对于None，则不做处理
    }
}
```

这个例子中首先定义了一个函数`string_find`，对`String`的`find`方法进行了封装，用于处理找到字符k和没有找到字符k的情况。接下来在使用处，可以使用`is_some`方法处理返回的`Option<size_t>`类型的值，从而获取里面的数据或者不做处理。这种写法可以安全地处理一些函数的返回值，提高程序的安全性。

`Option`是一个 _Owned struct类型的数据结构，因此其自带析构函数，可以安全地释放其所占用的内存空间，无需使用者进行手动内存释放。

注意：在使用`Option<T>`时，需要保证 T 是 copy 语义的类型或 _Owned struct 类型，否则会编译期报错，因为对于其它类型，编译器无法知道应该如何清理内存。

`Option`提供的对外接口及相应的使用用例如下：

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe _Bool Option<T>::is_none(const Option<T>* _Borrow this)`|判断`Option`是否为`None`，若是则返回1，否则返回0|_Bool flag = option.is_none();|
|`_Safe _Bool Option<T>::is_some(const Option<T>* _Borrow this)`|判断`Option`是否为`Some`，若是则返回1，否则返回0|_Bool falg = option.is_some();|
|`_Safe Option<T> Option<T>::Some(T t)`|创建一个状态为`Some`的`Option`值，其中包含的`T`类型的结果的值为`t`|Option<size_t> option = Option<size_t>::Some(3);|
|`_Safe Option<T> Option<T>::None(void)`|创建一个状态为`None`的`Option`值，其中不包含`T`的类型的结果值|Option<size_t> option = Option<size_t>::None();|
|`_Safe T option_unwrap<T>(Option<T> option)`|展开option，从中取出存储的T类型的值，对于`Some`可以成功取出`T`类型的值，对于`None`则会导致运行时终止|size_t index = option_unwrap(option);|

注：`option_unwrap`函数并非`Option`类型的成员方法，且其再展开`None`时会运行时终止，因此建议在调用该函数前使用`is_some`或`is_none`对`Option`的状态先进行判断。

#### 6.2.7. `HashMap`

`HashMap`是 BiShengC 语言提供的安全哈希表类型，哈希表的键和值都存储在堆上，它是一个泛型数据结构，具有三个泛型参数`K`、`V`和`S`。
其中`K`代表的是键的类型，`V`代表的是值的类型，而`S`代表的是该`HashMap`所使用的哈希函数。

我们可以用各种类型来作为键的类型，包含除float和double以外的各种基本类型，以及自定义的数据类型。
在将一个类型作为键的类型时，该类型必须实现了`hash`和`equals`这两个方法。BiShengC 已经为基本类型提供了这两个方法，无需使用者再次手动实现。

注意：在使用`HashMap<K, V>`时，需要保证 K 和 V 是 copy 语义的类型或 _Owned struct 类型，否则会编译期报错，因为对于其它类型，编译器无法知道应该如何清理内存。

对于哈希函数，BiShengC 提供了一个`SipHasher13`作为默认的哈希函数。该类型有两个方法用于创建其实例：

|对外接口|接口功能|代码示例|
|---|---|---|
|`SipHasher13 SipHasher13::new()`|创建默认的`SipHasher13`|SipHasher13 sh = SipHasher13::new();|
|`SipHasher13 SipHasher13::new_with_keys(uint64_t key0, uint64_t key1)`|使用`key0`和`key1`创建`SipHasher13`，BiShengC 还提供了`RandomState`类型，用于创建随机的`key0`和`key1`值|RandomState rs = RandomState::new();<br/>SipHasher13 sh = SipHasher13::new_with_keys(rs.k0, rs.k1);|

`HashMap`的一个使用实例如下：

```c
#include "hash_map.hbs" // HashMap类型声明在该头文件中，使用时需包含该头文件

// 使用HashMap来统计一个数组中各个值出现的次数
void hash_map_example(Vec<int> vec) {
    HashMap<int, int, SipHasher13> map = HashMap<int, int, SipHasher13>::with_hasher(SipHasher13::new()); // 创建HashMap

    for (size_t i = 0; i < vec.length(); i++) {
        const int* _Borrow cur = vec.get(i);
        if (map.contains_key(cur)) { // 判断map中是否已经有cur
            int* _Borrow v = map.get_mut(cur); // 获取键对应的值的可变借用
            *v = *v + 1; // 更新相应的值
        } else {
            int k = *cur;
            map.insert(k, 1); // 向map中插入键值对
        }
    }

    _Bool empty_flag = map.is_empty(); // 判断map是否为空
    int x = 3;
    _Bool key_flag = map.contains_key(&_Const x); // 判断map是否有键3
    Option<int> removed = map.remove(&_Const x);
    if (removed.is_some()) {
        int v = option_unwrap(removed);
    }
    map.clear(); // 清空map的所有元素
}
```

`HashMap`提供的对外接口及相应的使用用例如下：

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe size_t HashMap<K, V, S>::capacity(const HashMap<K, V, S>* _Borrow this)`|返回哈希表的容量|size_t cap = map.capacity();|
|`_Safe void HashMap<K, V, S>::clear(HashMap<K, V, S>* _Borrow this)`|清空哈希表的所有键值对，但表的容量保持不变|map.clear();|
|`_Safe _Bool HashMap<K, V, S>::contains_key(const HashMap<K, V, S>* _Borrow this, const K* _Borrow k)`|判断哈希表是否存在值为\*k的键|int x = 1;<br/>_Bool flag = map.contains_key(&_Const x);|
|`_Safe Option<V* _Borrow> HashMap<K, V, S>::get_mut(HashMap<K, V, S>* _Borrow this, const K* _Borrow k)`|获取值为\*k的键对应的值的可变借用，若表中不存在键\*k，则返回None|int x = 1;<br/>int* _Borrow v = map.get_mut(&_Const x);|
|`_Safe void HashMap<K, V, S>::reserve(HashMap<K, V, S>* _Borrow this, size_t additional)`|调整哈希表的容量，使其调整后在不扩容的情况下能至少再存入additional个元素（一般来说，该方法无需手动调用，在调用insert时会被隐式调用一次）|map.reserve(1);|
|`_Safe Option<V> HashMap<K, V, S>::insert(HashMap<K, V, S>* _Borrow this, K k, V v)`|向哈希表中插入键为k、值为v的键值对|map.insert(1, 2);|
|`_Safe _Bool HashMap<K, V, S>::is_empty(const HashMap<K, V, S>* _Borrow this)`|判断哈希表是否为空|_Bool empty map.is_empty();|
|`_Safe size_t HashMap<K, V, S>::length(const HashMap<K, V, S>* _Borrow this)`|返回哈希表中存储的键值对的数量|size_t len = map.length();|
|`_Safe Option<V> HashMap<K, V, S>::remove(HashMap<K, V, S>* _Borrow this, const K* _Borrow k)`|从哈希表中移除键为\*k的键值对，并返回对应的值，如果哈希表不存在键\*k，则返回None|int x = 3;<br/>Option<int> removed = map.remove(&_Const x);<br/>if (removed.is_some()) { int y = option_unwrap(removed); }|
|`_Safe HashMap<K, V, S> HashMap<K, V, S>::with_capacity_and_hasher(size_t cap, S hash_builder)`|创建初始容量为cap、哈希函数为hash_builder的哈希表|HashMap<int, int, SipHasher13> map = HashMap<int, int, SipHasher13>::new_with_cap_and_hasher(3, SipHasher13::new());|
|`_Safe HashMap<K, V, S> HashMap<K, V, S>::with_hasher(S hash_builder)`|创建初始容量为0，哈希函数为hash_builder的哈希表|HashMap<int, int, SipHasher13> map = HashMap<int, int, SipHasher13>::new_with_hasher(SipHasher13::new());|

### 6.3. 智能指针

#### 6.3.1. 概述

BiShengC 的所有权机制要求一个值只能有一个所有者，然而，我们考虑以下场景：
1. 在图数据结构中，多个边可能指向相同的节点，这个节点为所有指向它的边所共有，该节点直到没有边指向它时，才能被释放清理
2. 当我们希望在堆上分配一些内存供程序的多个部分读取，而且无法在编译时确定程序的哪一部分会最后结束使用它的时候，这部分内存直到最后一个部分使用结束时，才能被释放清理

为解决此类问题，需要允许一个值在同一时刻拥有多个所有者，因此 BSC **在标准库中提供 `Rc<T>` 类型来实现所有权的共享，它是一种智能指针，指向被共享的数据**。为提高 Rc 的安全性，BSC 同时提供了 `Weak<T>` 来避免循环引用造成的内存泄漏问题，也提供了 `Cell<T>` 和 `RefCell<T>` 与 Rc 组合使用来实现 Rc 指向数据的可变性。

#### 6.3.2. 使用 `Rc<T>` 共享数据

Rc 是引用计数（reference counting）的缩写，通过记录数据被引用的次数，来确定数据是否正在被使用，引用的数量就相当于所有者的数量。**如果引用计数变为 0，就意味着被引用数据可以被清理，此时 Rc 会自动调用数据的析构函数，避免内存泄漏。**

注意：在使用 `Rc<T>` 时，需要保证 T 是 copy 语义的类型或 _Owned struct 类型，否则会编译期报错，因为对于其它类型，编译器无法知道应该如何清理内存。

`Rc<T>` 的使用方法是：
1. 调用 new 方法来创建 Rc，该方法会分配堆内存，并将所要引用的数据存储在堆上
2. 调用 clone 方法可以从已有 Rc 得到另一个 Rc，该方法不执行深拷贝，仅仅创建另一个指向被引用数据的指针
3. 调用 deref 方法可以获取 Rc 所指向数据的不可变借用

我们通过以下的例子来学习 Rc 的使用：
假如一个班上有若干学生，他们共享一份成绩单，这份成绩记录文件就是需要被共享的数据资源，现在我们读取该文件来获取 1 号学生和 2 号学生的成绩：
```C
#include "rc.hbs"

_Owned struct ScoreRecord {  //学生成绩记录文件
    FILE *fp;
    ~ScoreRecord(This this) {
        fclose(this.fp);
    }
};

_Owned struct Student {
    unsigned student_id;    //学生id
    Rc<ScoreRecord> scores; //假设学生成绩记录文件为所有学生所共有
};

// 打开文件
ScoreRecord ScoreRecord::open(String filename);
// 读取文件第 id 行内容
String ScoreRecord::read(const This *_Borrow this, unsigned id);

// 获取 id 号学生的成绩
String Student::get_score(const This *_Borrow this) {
    const ScoreRecord *_Borrow scores = this->scores.deref(); 
    return scores->read(this->id);
}

int main() {
    ScoreRecord scores = ScoreRecord::open(String::from("scores.txt"));

    // student1 和 student2 共同拥有成绩文件的所有权
    Rc<ScoreRecord> rc1 = Rc<ScoreRecord>::new(scores);
    Rc<ScoreRecord> rc2 = rc1.clone()；
    Student student1 = { .id = 1, .scores = rc1 };
    Student student2 = { .id = 2, .scores = rc2 };
    // 此时共享文件的引用计数为 2，因为它有 2 个所有者

    print("学生1的成绩为：" + student1.get_score());
    print("学生2的成绩为：" + student2.get_score());

    return 0;
}   // 共享文件被使用完毕，引用计数变为 0，此时 Rc 会自动调用 ～ScoreRecord 来关闭文件
```

#### 6.3.3. 引用计数

让我们通过以下案例来观察 `Rc<T>` 在创建、克隆和走出作用域时引用计数的变化。
```C
int main() {
    Rc<int> rc1 = Rc<int>::new(5);
    printf("ref count after creating rc1 = %d\n", rc1.strong_ref_count());
    Rc<int> rc2 = rc1.clone();
    printf("ref count after creating rc2 = %d\n", rc1.strong_ref_count());
    {
        Rc<int> rc3 = rc1.clone();
        printf("ref count after creating rc3 = %d\n", rc1.strong_ref_count());
    }
    printf("ref count after rc3 goes out of scope = %d\n", rc1.strong_ref_count());
    return 0;
}
```
这段代码会打印出：
```
ref count after creating rc1 = 1
ref count after creating rc2 = 2
ref count after creating rc3 = 3
ref count after rc3 goes out of scope = 2
```
我们能够看到 rc1 的初始引用计数为 1，随着每次调用 clone，计数会增加 1。当 rc3 离开作用域时，计数减 1。不必像调用 clone 增加引用计数那样调用一个函数来减少计数，因为 `Rc<T>` 析构函数的实现可以保证当一个 Rc 离开作用域时自动减少引用计数。

从这个例子我们所不能看到的是，在 ref_count 函数的结尾处，随着 rc1、rc2 离开作用域，计数会降为 0，其指向的数据也将被清理，这一清理操作也是通过 `Rc<T>` 的析构函数来自动实现的。

#### 6.3.4. `Cell<T>`/`RefCell<T>` 与内部可变性

通过 deref 方法可以获取 Rc 所指向数据的不可变借用，这允许在程序的多个部分之间只读地共享数据。如果 `Rc<T>` 也允许多个可变借用，会违反 BSC 的借用规则：不能同时存在对同一数据的多个可变借用。但是在实际应用场景，修改数据是不可避免的。在这一部分，我们将讨论内部可变性模式和 Cell/RefCell 类型，它们**可以与 Rc 结合使用来修改 Rc 所指向的数据**。

BSC 编译器会在编译时对借用规则进行严格的检查，它可能会拒绝一个正确的程序，这往往会让用户痛苦不堪。因此在标准库中提供了 `Cell<T>` 和 `RefCell<T>` 来实现内部可变性：在拥有不可变借用的同时修改目标数据！对于正常的代码实现来说，这个是不可能做到的（要么一个可变借用，要么多个不可变借用）。

##### 6.3.4.1. `Cell<T>`

`Cell<T>` 用于获取它内部包裹的值的拷贝或直接修改内部值，适用于 Copy 语义的类型，例如 int、float 等基本数据类型或 struct 类型。
```C
void cell_example() {
    Cell<int> c = Cell<int>::new(5);
    printf("Value in cell is: %d\n", c.get()); // get 方法可以获取 cell 内部包裹的值的拷贝
    c.set(10);   // set 方法可以直接修改 cell 内部包裹的值
    printf("Value in cell is: %d\n", c.get());
}
```

##### 6.3.4.2. `RefCell<T>`

`RefCell<T>` 用于获取它内部包裹的值的可变或不可变借用，它实际上**并没有解决可变借用和不可变借用可以共存的问题，只是将报错从编译期推迟到运行时 abort**。

通过使用 `RefCell<T>` 的 borrow_mut 和 borrow_immut 方法，来尝试在运行时获取借用，如果借用成功，返回 `RefMut<T>` 或 `RefImmut<T>` 类型的值，否则会导致运行时 abort。规则是：
1. borrow_mut 方法用于获取 `RefMut<T>`，如果当前作用域内存在**任何其它借用**，则借用失败
2. borrow_immut 方法用于获取 `RefImmut<T>`，如果当前作用域存在**任何其它可变借用**，则借用失败

`RefMut<T>` 和 `RefImmut<T>` 是 `RefCell<T>` 的辅助数据结构，它们的 deref 方法可以分别获取 RefCell 内部值的可变和不可变借用。
```C
void refcell_example() {
    RefCell<int> c = RefCell<int>::new(5);

    // 通过 RefCell 的 borrow_mut 方法获得 RefMut：
    RefMut<int> ref_mut_c1 = c.borrow_mut();

    // 通过 RefMut 的 deref 方法获得 RefCell 内部值的可变借用：
    int* _Borrow mut_p1 = ref_mut_c.deref();

    // 通过可变借用修改内部值：
    *mut_p1 = 10;  

    // 再次尝试获取可变借用
    RefMut<int> ref_mut_c2 = c.borrow_mut(); //运行时abort，因为同时存在两个可变借用
}
```

#### 6.3.5. Rc 和 RefCell 相结合来实现共享数据的修改

上面的读取学生成绩的案例我们只对数据进行了读操作，有了 RefCell，我们可以实现对共享数据的修改，以下例子实现了对成绩数据的读取和更新：
```C
#include "rc.hbs"
#include "cell.hbs"

_Owned struct ScoreRecord {  //学生成绩记录文件
    FILE *fp;
    ~ScoreRecord(This this) {
        fclose(this.fp);
    }
};

_Owned struct Student {
    unsigned student_id;
    Rc<RefCell<ScoreRecord>> scores; //将需要被修改的共享数据用 RefCell 进行包装
};

// 打开文件
ScoreRecord ScoreRecord::open(String filename);
// 读取文件第 id 行内容
String ScoreRecord::read(const This *_Borrow this, unsigned id);
// 更新文件第 id 行内容
void ScoreRecord::update(This *_Borrow this, unsigned id, String new);

// 获取 id 号学生的成绩
String Student::get_score(const This *_Borrow this) {
    // RefCell 的 borrow_immut 方法获取不可变借用
    RefImmut<ScoreRecord> ref_scores = this->scores.deref()->borrow_immut(); 
    const ScoreRecord *_Borrow scores = ref_scores.deref();
    return scores->read(this->id);
}

// 更新 id 号学生的成绩
String Student::update_score(const This *_Borrow this, String new_score) {
    // RefCell 的 borrow_mut 方法获取可变借用
    RefMut<ScoreRecord> ref_scores = this->scores.deref()->borrow_mut(); 
    ScoreRecord *_Borrow scores = ref_scores.deref();
    return scores->update(this->id, new_score);
}

int main() {
    ScoreRecord scores = ScoreRecord::open(String::from("scores.txt"));

    Rc<RefCell<ScoreRecord>> rc1 = Rc<RefCell<ScoreRecord>>::new(RefCell<ScoreRecord>::new(scores));
    Rc<RefCell<ScoreRecord>> rc2 = rc1.clone()；
    Student student1 = { .id = 1, .scores = rc1 };
    Student student2 = { .id = 2, .scores = rc2 };

    student1.update_score(String::from("100"));
    student2.update_score(String::from("99"));

    print("学生1的成绩为：" + student1.get_score());
    print("学生2的成绩为：" + student2.get_score());

    return 0;
}
```

#### 6.3.6. `Weak<T>` 解决循环引用问题

尽管 BSC 有着完善的内存安全保障机制，但是不代表不会产生内存泄漏。一个典型的例子就是同时使用 `Rc<T>` 和  `RefCell<T>` 创建循环引用，由于引用计数无法被归零，因此 `Rc<T>` 指向的数据也就不会被释放清理。

##### 6.3.6.1. 制造循环引用

```C
#include "rc.hbs"
#include "cell.hbs"

_Owned struct B;

_Owned struct A {
_Public:
    int value;
    RefCell<Rc<B>> c;
    ~A(A this) {
        printf("A is destructed, value = %d\n", this.value);
    }
};

_Owned struct B {
_Public:
    int value;
    RefCell<Rc<A>> d;
    ~B(B this) {
        printf("B is destructed, value = %d\n", this.value);
    }
};

int main() {
    A a = { .value = 5 };
    Rc<A> ap = Rc<A>::new(a);      //ap 拥有 a

    B b = { .value = 10 };
    RefMut<Rc<A>> rm_b = b.d.borrow_mut();
    *(rm_b.deref()) = ap.clone();  //通过 ap 的克隆，此处 b.d 拥有了 a
    Rc<B> bp = Rc<B>::new(b);      //bp 拥有 b

    RefMut<Rc<B>> rm_a = ap.deref()->c.borrow_mut();
    *(rm_a.deref()) = bp.clone();  //通过 bp 的克隆，此处 a.c 拥有了 b

    printf("ref count of a : %d\n", ap.strong_ref_count());
    printf("ref count of b : %d\n", bp.strong_ref_count());

    return 0;
}
```
这段代码会打印出：
```
ref count of a : 2
ref count of b : 2
```
在函数结束前，a 和 b 的引用计数都为 2，其中 a 为 ap 和 b.d 所共有，b 为 bp 和 a.c 所共有。

在函数结束处：
1. 对 a 来说，ap 走出作用域后 a 的引用计数由 2 变为 1，只有当 b.d 析构会使 a 的引用计数减为 0 从而触发 a 的析构
2. 对 b 来说，bp 走出作用域后 b 的引用计数由 2 变为 1，只有当 a.c 析构会使 b 的引用计数减为 0 从而触发 b 的析构

a 的析构需要 b 先被析构，而 b 的析构需要 a 先被析构，结果就是 a 和 b 都没有被析构，内存泄漏了！

##### 6.3.6.2. 使用 `Weak<T>` 解决循环引用

为解决这一问题，BSC 引入 `Weak<T>` 类型，与 Rc 持有所有权不同，Weak 不持有所有权，它仅仅保存一份指向数据的弱引用，**建立弱引用不会增加引用计数，它也无法阻止所引用的数据被清理掉**。

`Weak<T>` 的使用方法是：
1. 调用 new 方法，通过一个已有的 `Rc<T>` 创建 `Weak<T>`
2. 无法通过 `Weak<T>` 直接访问被引用数据，如果想要访问数据，需要将 Weak 升级为 Rc，这通过 Weak 的 upgrade 方法实现，该方法返回一个类型为 `Option<Rc<T>>` 的值，如果被引用数据已经被清理释放，则 Option 的值是 None，也就是说，Weak 本身不对被引用数据的存在性做任何担保。

##### 6.3.6.3. 循环引用场景1

我们以上一小节中 a 和 b 循环引用导致内存泄漏的例子来学习`Weak<T>`的使用：
```C
#include "rc.hbs"
#include "cell.hbs"

_Owned struct B;

_Owned struct A {
_Public:
    int value;
    RefCell<Rc<B>> c;     //A 通过 Rc 引用 B
    ~A(A this) {
        printf("A is destructed, value = %d\n", this.value);
    }
};

_Owned struct B {
_Public:
    int value;
    RefCell<Weak<A>> d;  //B 通过 Weak 引用 A
    ~B(B this) {
        printf("B is destructed, value = %d\n", this.value);
    }
};

int main() {
    A a = { .value = 5 };
    Rc<A> ap = Rc<A>::new(a);           //ap 拥有 a

    B b = { .value = 10 };
    RefMut<Weak<A>> rm_b = b.d.borrow_mut();
    *(rm_b.deref()) = Weak<A>::new(ap); //通过 ap 创建弱引用，此处 b.d 弱引用 a
    Rc<B> bp = Rc<B>::new(b);           //bp 拥有 b

    RefMut<Rc<B>> rm_a = ap.deref()->c.borrow_mut();
    *(rm_a.deref()) = bp.clone();       //通过 bp 的克隆，此处 a.c 拥有了 b

    printf("ref count of a : %d\n", ap.strong_ref_count());
    printf("ref count of b : %d\n", bp.strong_ref_count());

    return 0;
}
```
这段代码会打印出：
```
ref count of a : 1
ref count of b : 2
B is destructed, value = 10
A is destructed, value = 5
```
通过打印结果，我们看到 a 和 b 都被析构了，分析这段代码：
1. 在函数结束前，a 为 ap 所拥有，引用计数为 1，b 为 bp 和 a.c 所共有，引用计数为 2；
2. 在函数结束处，ap 走出作用域后 a 的引用计数由 1 变为 0，从而触发 a 的析构，因为 a.c 拥有 b，随着 a 的析构，b 的引用计数由 2 减为 1；随后 bp 走出作用域后 b 的引用计数由 1 变为 0，从而触发 b 的析构。

##### 6.3.6.4. 循环引用场景2

Weak 常常被应用于父子循环引用关系中，做法是：让父节点通过 Rc 来引用子节点，然后让子节点通过 Weak 来引用父节点。接下来我们构造一个简单的 tree，它有 1 个 root 节点和 2 个 leaf 节点：
```C
_Owned struct Node {
_Public:
    int value;
    RefCell<Weak<Node>> parent;      //子结点通过 Weak 引用父节点
    RefCell<Vec<Rc<Node>>> children; //父结点通过 Rc 引用子节点
	
    ~Node(Node this) {
        printf("node %d is destructed\n", this.value);
    }
};

void tree() {
    // 构造叶子节点
    Node temp_leaf1 = { .value = 5, .children = RefCell<Vec<Rc<Node>>>::new(Vec<Rc<Node>>::new()) };
    Node temp_leaf2 = { .value = 6, .children = RefCell<Vec<Rc<Node>>>::new(Vec<Rc<Node>>::new()) };
    Rc<Node> leaf1 = Rc<Node>::new(temp_leaf1);
    Rc<Node> leaf2 = Rc<Node>::new(temp_leaf2);

    printf("ref count of leaf1 after init : %d\n", leaf1.strong_ref_count());
    printf("ref count of leaf2 after init : %d\n", leaf2.strong_ref_count());

    // 构造父节点，并让父结点通过 Rc 引用子节点
    Node temp_root = { .value = 10, .children = RefCell<Vec<Rc<Node>>>::new(Vec<Rc<Node>>::new()) };
    RefMut<Vec<Rc<Node>>> rm_root_children = temp_root.children.borrow_mut();
    rm_root_children.deref()->push(leaf1.clone());
    rm_root_children.deref()->push(leaf2.clone());
    Rc<Node> root = Rc<Node>::new(temp_root);

    printf("ref count of root after init : %d\n", root.strong_ref_count());
    printf("ref count of leaf1 after root points to it : %d\n", leaf1.strong_ref_count());
    printf("ref count of leaf2 after root points to it : %d\n", leaf2.strong_ref_count());

    // 子结点通过 Weak 引用父节点
    RefMut<Weak<Node>> rm_leaf1_parent = leaf1.deref()->parent.borrow_mut();
    *(rm_leaf1_parent.deref()) = Weak<Node>::new(&_Const root);
    RefMut<Weak<Node>> rm_leaf2_parent = leaf2.deref()->parent.borrow_mut();
    *(rm_leaf2_parent.deref()) = Weak<Node>::new(&_Const root);

    printf("ref count of root after leaf1 and leaf2 weakly points to it : %d\n", root.strong_ref_count());
}
```
这段代码会打印出：
```
ref count of leaf1 after init : 1
ref count of leaf2 after init : 1
ref count of root after init : 1
ref count of leaf1 after root points to it : 2
ref count of leaf2 after root points to it : 2
ref count of root after leaf1 and leaf2 weakly points to it : 1
node 5 is destructed
node 6 is destructed
node 10 is destructed
```
通过打印结果可以看到所有的节点数据都被清理释放。

#### 6.3.7. 相关 API

##### 6.3.7.1. `Rc<T>` 对外接口

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe Rc<T> Rc<T>::new(T data)`|构造一个 Rc，开辟堆内存存储 data|`Rc<int> rc = Rc<int>::new(5);`|
|`_Safe Rc<T> Rc<T>::clone(const This * _Borrow this)`|通过一个已有 Rc，克隆出另一个 Rc，会使引用计数加 1|`Rc<int> rc2 = rc.clone();`|
|`_Safe unsigned Rc<T>::strong_ref_count(const This * _Borrow this)`|获取强引用计数|`unsigned count = rc.strong_ref_count();`|
|`_Safe unsigned Rc<T>::weak_ref_count(const This * _Borrow this)`|获取弱引用计数|`unsigned count = rc.weak_ref_count();`|
|`_Safe const T * _Borrow Rc<T>::deref(const This * _Borrow this)`|获取 Rc 指向数据的不可变借用|`const int * _Borrow data = rc.deref();`|

##### 6.3.7.2. `Weak<T>` 对外接口

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe Weak<T> Weak<T>::new(const Rc<T> * _Borrow rc)`|通过一个已有 Rc，构造出一个 Weak|`Weak<int> w = Weak<int>::new(&_Const rc);`|
|`_Safe Weak<T> Weak<T>::clone(const This * _Borrow this)`|通过一个已有 Weak，克隆出另一个 Weak|`Weak<int> w2 = w.clone();`|
|`_Safe unsigned Weak<T>::strong_ref_count(const This * _Borrow this)`|获取强引用计数|`unsigned count = w.strong_ref_count();`|
|`_Safe unsigned Weak<T>::weak_ref_count(const This * _Borrow this)`|获取弱引用计数|`unsigned count = w.weak_ref_count();`|
|`_Safe Option<Rc<T>> Weak<T>::upgrade(const This * _Borrow this)`|将 Weak 转化为对应的 Rc，转化失败时返回 None|`Option<Rc<T>> rc = w.upgrade();`|

##### 6.3.7.3. `Cell<T>` 对外接口

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe Cell<T> Cell<T>::new(T value)`|用所给 value 构造 Cell|`Cell<int> c = Cell<int>::new(5);`|
|`_Safe T Cell<T>::get(const This * _Borrow this)`|获得 Cell 内部值的拷贝|`int a = c.get();`|
|`_Safe void Cell<T>::set(const This * _Borrow this, T val)`|将 Cell 内部值替换为 val|`c.set(10);`|

##### 6.3.7.4. `RefCell<T>` 对外接口

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe RefCell<T> RefCell<T>::new(T value)`|用所给 value 构造 RefCell|`RefCell<int> c = RefCell<int>::new(5);`|
|`_Safe RefImmut<T> RefCell<T>::borrow_immut(const This * _Borrow this)`|获得 RefCell 内部值的RefImmut<T>，失败会导致 abort|`RefImmut<int> ref = c.borrow_immut();`|
|`_Safe Option<RefImmut<T>> RefCell<T>::try_borrow_immut(const This * _Borrow this)`|获得 RefCell 内部值的 RefImmut<T>，失败会返回 None|`Option<RefImmut<int>> ref = c.try_borrow_immut();`|
|`_Safe RefMut<T> RefCell<T>::borrow_mut(const This * _Borrow this)`|获得 RefCell 内部值的 RefMut<T>，失败会导致 abort|`RefMut<int> ref = c.borrow_mut();`|
|`_Safe Option<RefMut<T>> RefCell<T>::try_borrow_mut(const This * _Borrow this)`|获得 RefCell 内部值的 RefMut<T>，失败会返回 None|`Option<RefMut<int>> ref = c.try_borrow_mut();`|

##### 6.3.7.5. `RefImmut<T>/RefMut<T>` 对外接口

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe const T * _Borrow RefImmut<T>::deref(const This * _Borrow this)`|获取 RefCell 内部值的不可变借用|`const int * _Borrow p = immut_ref.deref();`|
|`_Safe T * _Borrow RefMut<T>::deref(This * _Borrow this)`|获取 RefCell 内部值的可变借用|`int * _Borrow p = mut_ref.deref();`|

### 6.4. 协程调度器

#### 6.4.1. Scheduler

##### 6.4.1.1. 概述

`Scheduler` 是 BiShengC 语言提供的负责协调和管理协程的数据结构，它使用了协程机制来支持高并发的异步编程，主要作用是将可执行的任务分配给可用线程来执行，以最大程度地利用计算机资源。其使用示例如下：

```c
#include <unistd.h>
#include "scheduler.hbs" // 引入头文件

atomic_int g_student_num = 10;

struct Student {
    char * name;
    int age;
    int score;
};

struct Student nameList[] =
{
    {"Mary", 12, 90},
    {"John", 13, 95},
    {"Michael", 12, 93},
    {"Emily", 11, 92},
    {"James", 12, 86},
    {"Emma", 12, 95},
    {"William", 11, 90},
    {"Jessica", 13, 80},
    {"Lily", 12, 92},
    {"Sarah", 12, 88}
};

_Async void readName(int i) {
    // 模拟 IO 操作，读取学生名单
    printf("name: %s, age: %d, score: %d\n", nameList[i].name, nameList[i].age, nameList[i].score);

    atomic_fetch_sub(&g_student_num, 1);
    // 任务完成，销毁调度器
    if (atomic_load(&g_student_num) == 0) {
        struct Scheduler::destroy();
    }
}

int main() {
    // 初始化，创建 4 个线程
    struct Scheduler::init(4);
    for (int i = 0; i < g_student_num; i++) {
        // 将任务放入执行队列中
        struct Scheduler::spawn(readName(i));
    }
    // 执行调度器
    struct Scheduler::run();
    return 0;
}
```

输出结果如下：

```
name: Mary, age: 12, score: 90
name: Michael, age: 12, score: 93
name: Emily, age: 11, score: 92
name: James, age: 12, score: 86
name: Emma, age: 12, score: 95
name: William, age: 11, score: 90
name: Jessica, age: 13, score: 80
name: Lily, age: 12, score: 92
name: Sarah, age: 12, score: 88
name: John, age: 13, score: 95
```

调度器是一个用于管理和调度任务的工具，`init`方法是在创建调度器对象时调用的，主要用于初始化调度器的一些属性。`spawn` 方法用于创建一个异步任务，并将任务添加到任务队列中**等待执行**。`run` 方法是调度器的核心方法，用于**执行异步任务**。需要注意的一点是，只有当我们调用 `run` 方法时，调度器才会真正的执行任务。`destroy` 方法是在我们不再需要使用调度器时调用的，主要用于清理调度器的一些资源和释放占用的系统资源。

另外，从这个输出结果来看，输出顺序并不是按照数组本身的顺序，并且多次执行输出顺序也可能不同，所以这比较适用于不需要严格要求执行顺序的任务。

##### 6.4.1.2. 头文件

```c
#include "scheduler.hbs"
```

##### 6.4.1.3. API

| 对外接口                                                     | 接口功能                                                     | 代码示例                                                     |
| ------------------------------------------------------------ | ------------------------------------------------------------ | ------------------------------------------------------------ |
| `void struct Scheduler::init(unsigned int threadCount)`      | 用于初始化调度器，不可多次重复初始化，参数为用户需要创建的线程数 | struct Scheduler::init(4);                                   |
| `struct Task * struct Scheduler::spawn(_Trait Future<struct Void> * future)` | 创建异步任务，并将任务放入执行队列中等待执行（目前仅接受返回类型是void的任务）。 | _Async void  taskFunc(int i) { <br/>    while(i < 10000) { <br/>        i = (i * 2) + 3 ; <br/>    }<br/>    printf("result: %d\n", i);<br/>}<br>struct Scheduler::spawn(taskFunc(0)); |
| `void struct Scheduler::run()`                               | 执行通过 spawn 函数创建的异步任务                            | struct Scheduler::run();                                     |
| `void struct Scheduler::destroy()`                           | 销毁调度器，释放资源                                         | struct Scheduler::destroy();                                 |

具体数据结构介绍如下：

`struct Scheduler`：表示调度器，包括任务队列和线程池。通过 `struct Scheduler::init`方法创建并初始，但由于一个进程只能有一个 Scheduler，所以不能重复初始化。当然，也不允许未初始化就直接使用，在调用 `struct Scheduler::spawn` 或  `struct Scheduler::run` 方法前，必须调 `struct Scheduler::init`。如果不再需要调度器了，可以通过 `struct Scheduler::destroy` 方法进行销毁（如果不销毁，进程无法终止）。

`strcut Task`: 表示异步任务，包括任务的状态和执行上下文。通过 `struct Scheduler::spawn` 方法进行创建，创建完成后会将其放入执行队列，但不会立马执行。只有调用了 `struct Scheduler::run` 方法才会真正的执行队列中的任务。

另外，我们注意到 `struct Scheduler::spawn` 方法的入参是 `_Trait Future<struct Void> *` 类型（`_Trait Future` 的定义可参考无栈协程章节），所以我们除了可以传入返回类型是 `void` 的 _Async 函数调用，也可以传显式返回 `_Trait Futrue<struct Void> *`  类型的函数调用，具体使用如下：

```c
#include "scheduler.hbs"

atomic_int g_task_num = 10;

struct _Futurework {
    int a;
    int __future_state;
};

// 必须实现 poll 和 free 两个函数
struct PollResult<struct Void> struct _Futurework::poll(struct _Futurework *this) {
    switch (this->__future_state) {
      case 0:
        goto __L0;
    }
  __L0:
    ;
    printf("The %d-th calculation begins\n", this->a);
    while (this->a < 100000000)
        {
            this->a++;
        }

    atomic_fetch_sub(&g_task_num, 1);
    if (atomic_load(&g_task_num) == 0) {
        struct Scheduler::destroy();
    }

    this->__future_state = -1;
    struct Void __RES_RETURN = (struct Void){};
    return struct PollResult<struct Void>::completed(__RES_RETURN);
}

void struct _Futurework::free(struct _Futurework *this) {
    if (this != 0) {
        free((void *)this);
        this = (struct _Futurework *)(void *)0;
    }
}

_Impl _Trait Future<struct Void> for struct _Futurework;

_Trait Future<struct Void>* work(int a)     {
  struct _Futurework* ptr = malloc(sizeof(struct _Futurework));
  ptr->a = a;
  ptr->__future_state = 0;
  return ptr;
}

int main() {
    struct Scheduler::init(4);
    for (int i = 0; i < 10; i++) {
        struct Scheduler::spawn(work(i));
    }
    struct Scheduler::run();
    return 0;
}
```

### 6.5. 网络库

#### 6.5.1. Ipv4Addr

`Ipv4Addr`是 BiShengC 语言提供的表示 IPv4 地址的数据结构，其使用示例如下：

```c
void test() {
    struct Ipv4Addr ip = struct Ipv4Addr::new(192, 168, 0, 1);
    uint32_t bits = ip.to_bits();
    assert(bits == 0xc0a80001);
}
```

`Ipv4Addr`提供的对外接口及相应的使用用例如下：

|对外接口|接口功能|代码示例|
|---|---|---|
|`_Safe struct Ipv4Addr struct Ipv4Addr::new(uint8_t a, uint8_t b, uint8_t c, uint8_t d)`|创建 Ipv4Addr 实例|struct Ipv4Addr ip = struct Ipv4Addr::new(192, 168, 0, 1);|
|`_Safe uint32_t struct Ipv4Addr::to_bits(This this)`|将 Ipv4Addr 实例转换成 uint32_t 类型|uint32_t bits = ip.to_bits();|

## 附录

### 附录A. 关键字

下面的列表包含了毕昇C语言新增的关键字及其功能描述。

- `_Trait`: 用于定义一个`_Trait`
- `_Impl`: 用于为一个类型实现一个或多个`_Trait`
- `This`: 用于在成员函数中表示当前类型
- `this`: 用于在实例成员函数中表示当前实例或指向当前实例的指针
- `constexpr`: 用于定义编译时常量表达式或在编译时求值的函数
- `_Safe`: 用于声明安全区
- `_Unsafe`: 用于声明非安全区
- `_Owned`: 用于修饰拥有所有权的指针类型或`_Owned struct`
- `_Borrow`: 用于修饰借用指针类型
- `_ArrayElem`: 用于修饰指向数组的带所有权的指针或指向数组元素的借用指针
- `_Const`: 用于获取不可变借用
- `_Mut`: 用于获取可变借用
- `_Private`: 用于修饰`_Owned struct`的私有成员
- `_Public`: 用于修饰`_Owned struct`的公共成员
- `_Nonnull`: 用于修饰非空指针类型
- `_Nullable`: 用于修饰可空指针类型
- `nullptr`: 用于表示空指针的字面量
- `_Async`: 用于修饰异步函数
- `_Await`: 用于异步函数调用

### 附录B. 语法

本章对毕昇 C 语言的语法使用 EBNF（Extended Backus-Naur Form，扩展巴科斯范式）进行规范化描述。由于毕昇 C 语言扩展自 C 语言，本节不再列出 C 语言已有的语法的 EBNF 表示，**仅给出毕昇 C 语言扩展的语法的 EBNF 表示**，关于 C 语言已有语法的 EBNF 表示可查阅[ C11 标准规范](https://www.iso-9899.info/n1570.html#A.)。本章的组织结构与 C11 标准规范的附录 A 保持一致，用户可将两者对比阅读，能够清晰直观地了解到毕昇 C 扩展的 EBNF 表示的内容。

在本章中使用的语法符号中，采用与 C11 标准规范中类似的表示方式：

- 使用前下划线（_）加小写字母表示语法类别（即非终结符），前下划线加大写字母或没有前下划线则表示字面量和字符集合（即终结符）；
- 在代码块的第一行，非终结符后接冒号（:）表示非终结符的具体定义（即产生式规则）；
- 除了以“one of”开头外，其他的可选定义都列在不同的行中，可选的非终结符使用后缀“_opt”表示；
- 对于向 C11 已有的语法类别中新增语法，使用“....”表示省略已有的语法规则。


#### B.1. Lexical grammar

##### B.1.1. Lexical elements

无新增或修改。

##### B.1.2. Keywords

`_keyword`的产生式规则有如下变化：

1. 新增 19 个关键字，为`_keyword`新增 19 条产生式：

```text
_keyword : one of
    ....         _Impl         This
    _Async       nullptr       this
    _Await       _Owned        _Trait
    _Borrow      _Private      _Unsafe
    constexpr    _Public       _Nonnull
    _Safe        _Nullable     _Const
    _Mut         _ArrayElem
```

##### B.1.3. Identifiers

无新增或修改。

##### B.1.4. Universal character names

无新增或修改。

##### B.1.5. Constants

无新增或修改。

##### B.1.6. String literals

无新增或修改。

##### B.1.7. Punctuators

`_punctuator`的产生式规则有如下变化：

1. 新增 5 个运算符和分隔符，为`_punctuator`新增 5 条产生式。

```text
_punctuator : one of
    ....          ::
    &_Const       &_Mut
    &_Const *     &_Mut *
```

##### B.1.8. Header names

无新增或修改。

##### B.1.9. Preprocessing numbers

无新增或修改。

##### B.1.10. Pointer literals

新增`_pointer-literal`，表示空指针字面量：

```text
_pointer-literal :
    nullptr
```


#### B.2. Phrase structure grammar

##### B.2.1. Expressions

`_primary-expression`的产生式规则有如下变化：

1. 支持泛型特性，修改了`_primary-expression`的第 1 条产生式；
2. `this`属于`_primary-expression`，为`_primary-expression`新增 1 条产生式。

```text
_primary-expression :
    ....
    _identifier _template-declaration_opt
    this
```

`_postfix-expression`的产生式规则有如下变化：

1. 支持泛型函数，修改了`_postfix-expression`的第 4 和第 5 条产生式；
2. 允许调用静态成员函数和实例成员函数，为`_postfix-expression`新增 2 条产生式。

```text
_postfix-expression :
    ....
    _postfix-expression . _identifier _template-declaration_opt
    _postfix-expression -> _identifier _template-declaration_opt
    _nested-name-specifier
    _postfix-expression _identifier _template-declaration_opt
```

`_unary-expression`的产生式规则有如下变化：

1. 允许使用`_Await`关键字修饰表达式，为`_unary-expression`新增 1 条产生式。

```text
_unary-expression :
    ....
    _Await _unary-expression
```

`_unary-operator`的产生式规则有如下变化：

1. 新增 4 种一元运算符，为`_unary-operator`新增 4 条产生式。

```text
_unary-operator : one of
    ....         &_Const *     &_Mut
    &_Const       &_Mut *
```

##### B.2.2. Declarations

新增`_template-parameter`，表示泛型形参：

```text
_template-parameter :
    _identifier
    _parameter-declaration
```

新增`_template-parameter-list`，表示泛型形参列表：

```text
_template-parameter-list :
    _template-parameter
    _template-parameter-list , _template-parameter
```

新增`_template-argument`，表示泛型实参：

```text
_template-argument :
    _constant-expression
    _type-name
```

新增`_template-argument-list`，表示泛型实参列表：

```text
_template-argument-list :
    _template-argument
    _template-argument-list , _template-argument
```

新增`_template-declaration`，表示泛型形参声明或泛型实参声明：

```text
_template-declaration :
    < _template-parameter-list >
    < _template-argument-list >
```

新增`_constexpr-specifier`，表示常量表达式限定符：

```text
_constexpr-specifier :
    constexpr
```

`_declaration-specifiers`的产生式规则有如下变化：

1. `_constexpr-specifier`可修饰声明，为`_declaration-specifiers`新增 1 条产生式。

```text
_declaration-specifiers :
    ....
    _constexpr-specifier _declaration-specifiers_opt
```

`_initializer`的产生式规则有如下变化：

1. 支持通过`typedef`和等号定义类型别名的新语法，为`_initializer`新增 1 条产生式。

```text
_initializer :
    ....
    _type-name
```

`_type-specifier`的产生式规则有如下变化：

1. 新增 2 种类型限定符，为`_type-specifier`新增 2 条产生式。

```text
_type-specifier :
    ....
    This
    _trait-name
```

`_struct-or-union-specifier`的产生式规则有如下变化：

1. 允许定义泛型结构体和联合体，修改了`_struct-or-union-specifier`的第 1 和第 2 条产生式。

```text
_struct-or-union-specifier :
    _struct-or-union _identifier_opt _template-declaration_opt { _struct-declaration-list }
    _struct-or-union _identifier _template-declaration_opt
```

`_type-qualifier`的产生式规则有如下变化：

1. 新增 5 种类型限定符，为`_type-qualifier`新增 5 条产生式。

```text
_type-qualifier :
    ....
    _Borrow
    _Owned
    _ArrayElem
    _Nonnull
    _Nullable
```

新增`_safe-specifier`，表示安全限定符：

```text
_safe-specifier :
    _Safe
    _Unsafe
```

新增`_access-specifier`，表示访问限定符：

```text
_access-specifier :
    _Public
    _Private
```

`_function-specifier`的产生式规则有如下变化：

1. 新增 3 种函数限定符，为`_function-specifier`新增 2 条产生式。

```text
_function-specifier :
    ....
    _Async
    _safe-specifier
```

新增`_nested-name-specifier`，表示嵌套限定符：

```text
_nested-name-specifier :
    _type-name ::
```

`_direct-declarator`的产生式规则有如下变化：

1. 支持泛型函数定义，修改了`_direct-declarator`的第 1 条产生式；
2. 支持为类型声明和定义扩展成员函数，为`_direct-declarator`新增 2 条产生式。

```text
_direct-declarator :
    ....
    _identifier _template-declaration_opt
    _nested-name-specifier
    _direct-declarator _identifier _template-declaration_opt ( _parameter-type-list )
```

`_typedef-name`的产生式规则有如下变化：

1. 支持对泛型类型定义别名，修改了`_typedef-name`的第 1 条产生式。

```text
_typedef-name :
    _identifier _template-declaration_opt
```

##### B.2.3. Statements

`_compound-statement`的产生式规则有如下变化：

1. 安全限定符可修饰代码块，修改了`_compound-statement`的第 1 条产生式。

```text
_compound-statement :
    _safe-specifier_opt { _block-item-list_opt }
```

`_selection-statement`的产生式规则有如下变化：

1. 常量表达式限定符可修饰`if`语句的条件表达式，修改了`_selection-statement`的第 1 和 第 2 条产生式。

```text
_selection-statement :
    if _constexpr-specifier_opt ( _expression ) _statement
    if _constexpr-specifier_opt ( _expression ) _statement else _statement
```

##### B.2.4. External definitions

新增`_function-declaration`，表示函数声明：

```text
_function-declaration :
    _declaration-specifiers _pointer_opt _identifier ( _parameter-type-list ) ;
```

新增`_function-declaration-list`，表示函数声明列表：

```text
_function-declaration-list :
    _function-declaration
    _function-declaration-list _function-declaration
```

新增`_trait-name`的定义，表示`_Trait`的名称：

```text
_trait-name :
    _Trait _identifier _template-declaration_opt
```

新增`_trait-definition`，表示`_Trait`的定义：

```text
_trait-definition :
    _trait-name { _function-declaration-list } ;
```

新增`_impl-declaration`，表示实现`_Trait`：

```text
_impl-declaration :
    _Impl _trait-name for _type-name ;
```

新增`_dtor-definition`，表示析构函数定义：

```text
_dtor-definition :
    ~ _identifier ( _declaration ) _compound-statement
```

新增`_member-declaration`，表示`_Owned struct`成员种类：

```text
_member-declaration :
    _declaration
    _dtor-definition
    _function-definition
```

新增`_member-specification`，表示`_Owned struct`成员变量或成员函数：

```text
_member-specification :
    _member-declaration _member-specification_opt
    _access-specifier : _member-specification_opt
```

新增`_owned-struct-declaration`，表示`_Owned struct`类型声明：

```text
_owned-struct-declaration :
    _Owned struct _identifier _template-declaration_opt { _member-specification_opt } ;
```

`_external-declaration`的产生式规则有如下变化：

1. `_Trait`定义、实现`_Trait`和`_Owned struct`类型声明都属于外部声明，为`_external-declaration`新增 3 条产生式。

```text
_external-declaration :
    ....
    _trait-definition
    _impl-declaration
    _owned-struct-declaration
```


#### B.3. Preprocessing directives

无新增或修改。
