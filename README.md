# gc_ptr

一个用 C++ 实现的垃圾回收智能指针库，向标准库智能指针看齐。

## 功能特性

- 自动垃圾回收：使用标记-清除算法自动回收不可达对象
- 循环引用处理：自动检测并回收循环引用
- 自动触发：可配置定时回收或析构计数阈值回收，支持手动开关
- 线程安全：多线程模式下提供基本线程安全保证
- 自定义删除器：支持 lambda 和函数对象作为 deleter
- 与 `std::shared_ptr` 兼容的接口：`reset()`、`reset_with_deleter()`、`release()`、`swap()`
- **容器支持**：可存储在任意标准容器中（`std::vector`、`std::set`、`std::map`、`std::unordered_set` 等）
- **标准库兼容**：支持 `operator<`、`owner_before`、`std::hash`、`std::owner_less`、`std::atomic`
- **原子操作**：`std::atomic<GcPtr<T>>` 支持 load/store/exchange/compare_exchange_weak/compare_exchange_strong

## 重要安全规则

本 GC 是一个精确的、保守风格的回收器，仅扫描每个托管对象的连续内存区域（从对象地址开始的 real_size 字节）。它通过内部 GcPtr 成员的地址落在该内存范围内来发现它们。

### 容器支持

**GcPtr 可以安全地存储在任何标准容器中**，无论容器本身如何分配：

```cpp
// 支持：栈上容器
std::vector<GcPtr<int>> vec;
std::set<GcPtr<int>> s;
std::map<GcPtr<int>, std::string> m;

// 支持：堆上容器（裸 new）
auto* heap_vec = new std::vector<GcPtr<int>>();

// 支持：GC 管理的容器
struct ContainerHolder {
    std::vector<GcPtr<int>> items;
};
auto holder = make_gc<ContainerHolder>();
```

### 多线程模式注意事项

在多线程模式（定义 `GPTR_THREAD` 宏）下，多个线程可以并发访问不同的 `GcPtr` 实例，但**并发修改同一个 `GcPtr` 实例必须外部同步**，遵循与 `std::shared_ptr` 相同的约定。

`operator->`/`operator*`/`get()` 短暂获取 GC 锁后返回裸指针，随后释放锁。如果另一个线程同时修改或销毁同一个 `GcPtr`，返回的指针可能悬空。使用 `std::atomic<GcPtr<T>>` 在多线程间安全共享。

### 多态对象

如果 `GcPtr<Base>` 实际指向 Derived 对象，注册时必须传递正确的实际大小（`sizeof(Derived)`），否则 GC 不会扫描 Derived 部分。使用接受 `real_size` 参数的重载版本：

```cpp
GcPtr<Base> ptr(derived_raw_ptr,
                [](Base* p){ delete static_cast<Derived*>(p); },
                sizeof(Derived));
```

或使用工厂函数：

```cpp
auto ptr = make_gc_with_deleter<Base>(raw,
                [](Base* p){ delete static_cast<Derived*>(p); },
                sizeof(Derived));
```

### 显式根声明

提供 `mark_as_root()`/`unmark_as_root()` 以显式声明根状态，覆盖自动地址范围推断：

```cpp
auto ptr = make_gc<MyObj>();
ptr.mark_as_root();   // 显式声明为根
ptr.unmark_as_root(); // 取消根声明
```

## 使用方法

### 单一定义规则

gc_ptr 采用 STB 风格的单头文件模式，需要在一个 `.cpp` 文件中定义 `GC_PTR_IMPLEMENTATION` 宏来提供全局状态的唯一定义：

```cpp
// 在项目的某个 .cpp 文件中（仅需一个）
#define GC_PTR_IMPLEMENTATION
#include "gc_ptr.hpp"

// 其他所有 .cpp 文件中直接包含即可
#include "gc_ptr.hpp"
```

如果忘记定义 `GC_PTR_IMPLEMENTATION`，链接时会报未定义符号 `gc_active_context` 错误。

### 基本用法

```cpp
#include "gc_ptr.hpp"

// 使用工厂函数创建对象
auto obj = make_gc<MyClass>(arg1, arg2);

// 使用类似指针的方式访问成员
obj->method();
(*obj).method();

// 显式触发垃圾回收
obj.gc();

// 配置自动垃圾回收
GcPtr<MyClass>::set_gc_interval(std::chrono::seconds(60));
GcPtr<MyClass>::set_destruct_threshold(100);
```

### 自定义删除器

```cpp
// 方式一：使用 make_gc_with_deleter（推荐）
auto ptr = make_gc_with_deleter<MyType>(raw_ptr,
    [](MyType* p) { custom_free(p); });

// 方式二：使用构造函数
GcPtr<MyType> ptr(raw_ptr, [](MyType* p) { custom_free(p); });

// 方式三：使用 reset_with_deleter
GcPtr<MyType> ptr;
ptr.reset_with_deleter(raw_ptr, [](MyType* p) { custom_free(p); }, sizeof(MyType));
```

### 开关自动 GC

```cpp
// 禁用自动 GC
GcPtr<int>::disable_auto_gc();

// 启用自动 GC
GcPtr<int>::enable_auto_gc();

// 检查状态
if (GcPtr<int>::is_auto_gc_enabled()) { /* ... */ }
```

### 循环引用支持

gc_ptr 自动处理循环引用，无需手动断开引用链：

```cpp
auto a = make_gc<Node>();
auto b = make_gc<Node>();
a->next = b;
b->prev = a;
// 无需手动处理，垃圾回收会自动清理
```

### 容器使用示例

```cpp
// 栈上容器
std::vector<GcPtr<int>> vec;
vec.push_back(make_gc<int>(1));
vec.push_back(make_gc<int>(2));

// 有序容器（需要 operator<）
std::set<GcPtr<int>> s;
s.insert(make_gc<int>(42));

// 关联容器
std::map<GcPtr<int>, std::string> m;
m[make_gc<int>(1)] = "one";

// 哈希容器（需要 std::hash 特化）
std::unordered_set<GcPtr<int>> us;
us.insert(make_gc<int>(42));

// 裸 new 分配的容器（不在 GC 管理下）
auto* heap_vec = new std::vector<GcPtr<int>>();
heap_vec->push_back(make_gc<int>(100));
delete heap_vec;  // 容器销毁时，内部 GcPtr 也会正确清理
```

## API 文档

### GcPtr\<T>

模板类，提供垃圾回收智能指针功能。

#### 构造函数

- `GcPtr()` - 默认构造函数，创建空指针
- `GcPtr(std::in_place_t, Args&&... args)` - 原地构造并初始化对象
- `GcPtr(T* p)` - 从原始指针构造（接管所有权，使用默认 deleter 删除）
- `GcPtr(T* p, Deleter deleter)` - 从原始指针和自定义删除器构造
- `GcPtr(T* p, Deleter deleter, std::size_t real_size)` - 多态对象，指定实际大小
- `GcPtr(const GcPtr& other)` - 拷贝构造
- `GcPtr(GcPtr&& other)` - 移动构造

#### 工厂函数

- `make_gc<T>(Args&&... args)` - 推荐的创建方式
- `make_gc_with_deleter<T>(T* ptr, Deleter deleter)` - 带自定义删除器
- `make_gc_with_deleter<T>(T* ptr, Deleter deleter, std::size_t real_size)` - 带自定义删除器和多态大小

#### 成员函数

- `void reset()` - 立即销毁当前对象并置空（若当前指针是唯一持有者）
- `void reset(T* new_ptr)` - 销毁当前对象并接管新指针的所有权
- `void reset_with_deleter(T* new_ptr, Deleter deleter, std::size_t real_size)` - 接管新指针并指定自定义删除器和大小
- `T* release()` - 释放所有权，返回原始指针。对象不再由 GC 管理，调用者需手动 `delete`
- `void swap(GcPtr& other)` - 交换内容
- `T& operator*()` / `const T& operator*() const` - 解引用
- `T* operator->()` / `const T* operator->() const` - 成员访问
- `T* get()` / `const T* get() const` - 获取原始指针
- `explicit operator bool() const` - 检查是否为空
- `void gc()` - 显式触发垃圾回收
- `bool owner_before(const GcPtr& other) const` - 所有权比较（用于 `std::owner_less`）
- `bool owner_before(const GcPtr<U>& other) const` - 异构所有权比较
- `void mark_as_root()` / `void unmark_as_root()` - 显式声明/取消根状态

#### 静态成员函数

- `static void set_gc_interval(std::chrono::seconds t)` - 设置定时 GC 间隔
- `static void set_destruct_threshold(std::size_t n)` - 设置析构计数阈值
- `static void disable_auto_gc()` - 禁用自动 GC
- `static void enable_auto_gc()` - 启用自动 GC
- `[[nodiscard]] static bool is_auto_gc_enabled()` - 检查自动 GC 是否启用
- `[[nodiscard]] static std::size_t gc_object_count()` - 获取当前 GC 管理的对象数量
- `static void emergency_cleanup()` - 强制触发垃圾回收，用于程序退出前清理资源
- `static void set_context(GcContext* ctx)` - 设置跨模块共享的 GC 上下文
- `static void reset_context()` - 重置为默认独立 GC 上下文

#### 友元运算符

- `bool operator==(const GcPtr& a, const GcPtr& b)` - 相等比较
- `bool operator!=(const GcPtr& a, const GcPtr& b)` - 不等比较
- `bool operator<(const GcPtr& a, const GcPtr& b)` - 小于比较（支持有序容器）

### GcPtrBase

所有 GcPtr 的基类，提供全局垃圾回收控制。

#### 静态公共成员函数

- `static void collect()` - 执行一次垃圾回收
- `static void emergency_cleanup()` - 强制触发垃圾回收
- `static void set_gc_interval(std::chrono::seconds interval)` - 设置定时 GC 间隔
- `static void set_destruct_threshold(std::size_t threshold)` - 设置析构计数阈值
- `static void disable_auto_gc()` - 禁用自动 GC
- `static void enable_auto_gc()` - 启用自动 GC
- `[[nodiscard]] static bool is_auto_gc_enabled()` - 检查自动 GC 是否启用
- `[[nodiscard]] static std::size_t gc_object_count()` - 获取 GC 管理的对象数量
- `static void set_context(GcContext* ctx)` - 设置跨模块共享的 GC 上下文
- `static void reset_context()` - 重置为默认独立 GC 上下文
- `[[nodiscard]] static GcContext& get_context()` - 获取当前 GC 上下文
- `[[nodiscard]] static int count_references(void* obj)` - 统计指向某 GC 对象的 GcPtr 数量
- `[[nodiscard]] static bool is_within_any_gc_object(const void* addr)` - 检查地址是否位于某个 GC 对象内存区域内
- `static void register_gc_object(void* p, std::size_t sz, Deleter&& d, bool under_construction = true)` - 手动注册 GC 对象
- `static void unregister_gc_object(void* p)` - 手动注销 GC 对象（不触发删除器）
- `static void rebuild_ranges()` - 重建 GC 对象的内存区间缓存

#### 静态数据访问器（内部状态）

- `static std::map<void*, GcNode>& gc_objects()` - 访问 GC 对象注册表
- `static std::map<const void*, GcPtrBase*, ...>& all_ptrs()` - 访问所有 GcPtr 实例的映射
- `static std::vector<std::pair<...>>& gc_ranges()` - 访问内存区间缓存
- `static bool& gc_ranges_dirty()` - 访问区间缓存脏标志

### GcContext

GC 上下文结构体，封装所有 GC 状态。

#### 公共成员

- `std::map<void*, GcNode> gc_objects` - GC 对象注册表
- `std::map<const void*, GcPtrBase*, ...> all_ptrs` - 所有 GcPtr 实例的映射
- `std::vector<std::pair<...>> ranges` - 内存区间缓存
- `bool ranges_dirty` - 区间缓存是否脏
- `std::size_t destruct_count` - 析构计数器
- `std::size_t destruct_threshold` - 析构阈值（默认 40）
- `std::chrono::seconds gc_interval` - 定时 GC 间隔（默认 120 秒）
- `bool auto_gc_enabled` - 自动 GC 开关
- `std::atomic<bool> gc_in_progress` - GC 是否正在进行中
- `std::atomic<bool> gc_pending` - 是否有待处理的 GC 请求

### GcNode

GC 对象节点结构体。

#### 公共成员

- `void* address` - 对象地址
- `std::size_t size` - 对象大小
- `bool marked` - 标记位
- `bool under_construction` - 构造中标志（构造期间不被回收）
- `GcDeleterFn invoke_deleter` - 调用删除器的函数指针
- `uintptr_t deleter_context` - 删除器上下文指针
- `GcDeleterFn destroy_deleter_ctx` - 销毁删除器上下文的函数指针

### 标准库特化

- `std::hash<GcPtr<T>>` - 哈希支持（用于 `std::unordered_set`、`std::unordered_map`）
- `std::owner_less<GcPtr<T>>` - 所有权比较器（用于异构查找）
- `std::atomic<GcPtr<T>>` - 原子支持（线程安全地共享 GcPtr）
  - `load()` / `store()` / `exchange()` / `compare_exchange_weak()` / `compare_exchange_strong()`
  - `is_lock_free()` 返回 `false`（基于互斥锁实现）
- `std::swap(GcPtr<T>&, GcPtr<T>&)` - 非成员 swap

## 实现细节

- 全局状态通过 `GC_PTR_IMPLEMENTATION` 宏机制保证唯一定义，避免跨 TU 重复符号，也为跨 DLL 共享奠定基础
- GC 上下文：所有 GC 状态封装在 `GcContext` 结构体中，通过 `gc_set_context()` / `set_context()` 可在模块间共享同一 GC 堆
- 根对象检测：栈上的 GcPtr 自动识别为 GC 根，或通过 `mark_as_root()` 显式声明
- 对象注册表：使用 `std::map` 和排序区间表管理所有 GC 对象
- 引用扫描：在 GC 对象内存区域内扫描内部 GcPtr 成员
- 标记-清除：标准的标记清除算法，支持循环引用回收
- 垃圾析构安全：调用删除器之前，先将垃圾对象内部指向其他垃圾的 `GcPtr` 置空，防止悬空访问
- 线程安全：定义 `GPTR_THREAD` 宏后启用
  - 递归互斥锁保护内部数据结构（`gc_objects`、`all_ptrs`、`gc_ranges`）
  - `std::atomic<GcPtr<T>>` 使用全局互斥锁提供原子操作
- 异常安全：`collect()` 中的 deleter 异常会被收集，所有 deleter 执行完毕后重新抛出第一个异常；`gc_in_progress` 标志在异常时会被正确重置

## 注意事项

- **非确定性析构**：与 `std::shared_ptr` 不同，`~GcPtr()` 不会立即销毁对象。对象销毁被延迟到下一次 GC 循环。如果需要确定性资源释放，可调用 `gc()`/`collect()` 显式触发回收，或使用 `reset()` 在最后引用消失时立即销毁。
- `reset()` 检查是否存在其他 `GcPtr` 指向同一对象：若仅有当前指针持有该对象，则立即销毁；若存在其他共享引用，则仅将当前指针置空，由 GC 后续回收。
- `release()` 将对象移出 GC 管理，调用者负责手动 `delete`。释放后的对象不可再交给其他 `GcPtr` 管理。
- 自定义 `deleter` 不应抛出异常；若必须抛出，库会确保堆分配的删除器对象被正确清理后再重新抛出异常，避免内存泄漏。
- 根检测依赖编译器和平台的对象布局，对于非标准内存布局可能存在局限，此时可使用 `mark_as_root()` 显式声明。
- **构造期间保护**：正在构造的对象（`under_construction` 状态）即使从根集合不可达也不会被回收，避免构造函数中 `this` 指针被提前销毁的灾难。
- **拷贝/移动构造线程安全**：`GcPtr` 的拷贝和移动构造函数在单一锁内完成基类注册和指针赋值，消除构造期间的 GC 竞态窗口。
- **重复注册保护**：尝试将已注册的指针再次注册到 GC 会抛出 `std::runtime_error`。`reset(T*)` 和 `reset_with_deleter()` 同样会检查重复注册。
- **自赋值保护**：`reset()` 和 `reset_with_deleter()` 对同一指针的自赋值操作会被安全忽略。
- **重入 GC 保护**：`collect()` 内部使用 `gc_in_progress` 标志防止递归调用；在 deleter 中调用 `collect()` 会安全返回而不重复执行。

### 全局/静态存储期 GcPtr

gc_ptr 的默认 GC 上下文（`gc_default_context()`）采用函数局部静态变量策略，确保在所有 `GcPtr` 析构期间始终有效。这意味着全局或命名空间作用域的 `GcPtr` 可以在程序退出时安全析构，不会因析构顺序不确定导致未定义行为。

### 测试专用 API

以下 API 仅在定义 `GC_PTR_EXPOSE_INTERNALS` 宏后可用，仅供内部测试使用：

- `is_gc_in_progress()` - 检查 GC 是否正在进行中
- `get_destruct_count()` - 获取当前析构计数
- `reset_destruct_count()` - 重置析构计数

生产代码不应依赖这些接口。

### 跨平台兼容性

`std` 命名空间特化（`std::hash`、`std::owner_less`、`std::atomic`）已在以下环境通过 CI 验证：

- GCC 13+ / libstdc++（mingw-w64 ucrt）
- Clang 19+ / libc++（mingw-w64 clang64）
- MSVC 2022 / Microsoft STL

多编译器项目可直接使用，无需额外配置。

### 线程安全与 ThreadSanitizer

拷贝/移动构造的竞态窗口已修复（见注意事项）。对于 GC 库这类 low-level 并发组件，建议在 CI 中启用 ThreadSanitizer（`-fsanitize=thread`）运行所有并发测试，以确保无数据竞争。

### 跨动态库（DLL/SO）共享

gc_ptr 通过 `GcContext` 支持跨模块共享同一个 GC 堆。默认情况下，每个可执行模块有独立的 GC 堆；若需要多个 DLL 共享同一个堆，按以下步骤操作：

**主模块（可执行文件或主 DLL）**：

```cpp
#define GC_PTR_IMPLEMENTATION
#include "gc_ptr.hpp"

void init_gc() {
    // 主模块创建 context，其他模块通过 set_context() 指向它
}

void plugin_init(GcContext* shared_ctx) {
    GcPtr<int>::set_context(shared_ctx);
}
```

**插件 DLL**：

```cpp
// 注意：不在插件 DLL 中定义 GC_PTR_IMPLEMENTATION
#include "gc_ptr.hpp"

void plugin_init(GcContext* shared_ctx) {
    GcPtr<int>::set_context(shared_ctx);
}
```

**如果需要从 DLL 导出 `gc_active_context` 符号**，在编译 DLL 时定义 `GC_PTR_BUILD_DLL`，在使用方定义 `GC_PTR_USE_DLL`。

主模块通过插件 API 将自身 `GcContext` 指针传递给各 DLL，DLL 调用 `set_context()` 绑定后，所有模块共享同一个 GC 堆——对象可以在模块间自由引用，GC 标记-清除会统一扫描。

## 系统要求

- **C++ 标准**：C++17 或更高版本
- **编译器**：GCC 7+ / Clang 5+ / MSVC 2022+（推荐 mingw-w64 15.2.0 + C++26 以获得最佳兼容性）
- **测试框架**：Google Test（GTest）
- **操作系统**：跨平台（Linux、macOS、Windows）

### 多线程支持

多线程模式需要定义 `GPTR_THREAD` 宏，并在链接时添加 `-pthread` 标志。

## 构建和测试

项目使用 Makefile 构建和测试。默认构建启用 `GPTR_THREAD` 宏（线程安全模式）。

### GCC / Clang (MSYS2 / Linux / macOS)

```bash
make        # 构建测试（线程安全模式）
make test   # 构建并运行测试
make clean  # 清理
```

### MSVC (Visual Studio 2022+)

```cmd
:: 从源码编译 gtest（一次性）
git clone --depth 1 --branch v1.14.0 https://github.com/google/googletest.git

:: 编译并运行测试
cl /std:c++17 /W4 /DGPTR_THREAD /EHsc /I. /Igoogletest\googletest\include /Itests ^
  tests\gc_ptr_test.cpp ^
  googletest\googletest\src\gtest-all.cc ^
  googletest\googletest\src\gtest_main.cc ^
  /Fe:gc_ptr_test.exe
gc_ptr_test.exe
```

如果需要非线程安全模式，移除 `-DGPTR_THREAD`（GCC/Clang）或 `/DGPTR_THREAD`（MSVC）标志。

## 许可证

MIT License
