# gc_ptr

一个用 C++ 实现的垃圾回收智能指针库，向标准库智能指针看齐。

## 功能特性

- 自动垃圾回收：使用标记-清除算法自动回收不可达对象
- 循环引用处理：自动检测并回收循环引用
- 自动触发：可配置定时回收或析构计数阈值回收
- 线程安全：多线程模式下提供基本线程安全保证
- 与 `std::shared_ptr` 兼容的接口：`reset()`、`release()`、`swap()`
- **容器支持**：可存储在任意标准容器中（`std::vector`、`std::set`、`std::map`、`std::unordered_set` 等）
- **标准库兼容**：支持 `operator<`、`owner_before`、`std::hash`、`std::owner_less`

## 重要安全规则

本 GC 是一个精确的、保守风格的回收器，仅扫描每个托管对象的连续内存区域（从对象地址开始的 real_size 字节）。它通过内部 GcPtr 成员的地址落在该内存范围内来发现它们。

### 容器支持

**GcPtr 可以安全地存储在任何标准容器中**，无论容器本身如何分配：

```cpp
// ✅ 支持：栈上容器
std::vector<GcPtr<int>> vec;
std::set<GcPtr<int>> s;
std::map<GcPtr<int>, std::string> m;

// ✅ 支持：堆上容器（裸 new）
auto* heap_vec = new std::vector<GcPtr<int>>();

// ✅ 支持：GC 管理的容器
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

### 显式根声明

提供 `mark_as_root()`/`unmark_as_root()` 以显式声明根状态，覆盖自动地址范围推断：

```cpp
auto ptr = make_gc<MyObj>();
ptr.mark_as_root();   // 显式声明为根
ptr.unmark_as_root(); // 取消根声明
```

## 使用方法

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
- `GcPtr(T* p)` - 从原始指针构造（接管所有权）
- `GcPtr(const GcPtr& other)` - 拷贝构造
- `GcPtr(GcPtr&& other)` - 移动构造

#### 工厂函数

- `template <typename T, typename... Args> GcPtr<T> make_gc(Args&&... args)` - 推荐的创建方式

#### 成员函数

- `void reset()` - 立即销毁当前对象并置空
- `void reset(T* new_ptr)` - 销毁当前对象并接管新指针的所有权
- `T* release()` - 释放所有权，返回原始指针。对象不再由 GC 管理，调用者需手动 `delete`
- `void swap(GcPtr& other)` - 交换内容
- `T& operator*()` / `const T& operator*() const` - 解引用
- `T* operator->()` / `const T* operator->() const` - 成员访问
- `T* get()` / `const T* get() const` - 获取原始指针
- `explicit operator bool() const` - 检查是否为空
- `void gc()` - 显式触发垃圾回收
- `bool owner_before(const GcPtr& other) const` - 所有权比较（用于 `std::owner_less`）
- `void mark_as_root()` / `void unmark_as_root()` - 显式声明/取消根状态

#### 静态成员函数

- `static void set_gc_interval(std::chrono::seconds t)` - 设置定时GC间隔
- `static void set_destruct_threshold(std::size_t n)` - 设置析构计数阈值
- `static void emergency_cleanup()` - 强制触发垃圾回收，用于程序退出前清理资源

#### 友元运算符

- `bool operator==(const GcPtr& a, const GcPtr& b)` - 相等比较
- `bool operator!=(const GcPtr& a, const GcPtr& b)` - 不等比较
- `bool operator<(const GcPtr& a, const GcPtr& b)` - 小于比较（支持有序容器）

### GcPtrBase

所有 GcPtr 的基类，提供全局垃圾回收控制。

#### 静态公共成员函数

- `static void collect()` - 执行一次垃圾回收

### 标准库特化

- `std::hash<GcPtr<T>>` - 哈希支持（用于 `std::unordered_set`、`std::unordered_map`）
- `std::owner_less<GcPtr<T>>` - 所有权比较器（用于异构查找）
- `std::atomic<GcPtr<T>>` - 原子支持（线程安全地共享 GcPtr）

## 实现细节

- 根对象检测：栈上的 GcPtr 自动识别为 GC 根，或通过 `mark_as_root()` 显式声明
- 对象注册表：使用 `std::map` 和排序区间表管理所有 GC 对象
- 引用扫描：在 GC 对象内存区域内扫描内部 GcPtr 成员
- 标记-清除：标准的标记清除算法，支持循环引用回收
- 垃圾析构安全：调用删除器之前，先将垃圾对象内部指向其他垃圾的 `GcPtr` 置空，防止悬空访问
- 线程安全：定义 `GPTR_THREAD` 宏后启用
  - 递归互斥锁保护内部数据结构（`gc_objects`、`all_ptrs`、`gc_ranges`）
  - `std::atomic<GcPtr<T>>` 使用全局互斥锁提供原子操作

## 注意事项

- **非确定性析构**：与 `std::shared_ptr` 不同，`~GcPtr()` 不会立即销毁对象。对象销毁被延迟到下一次 GC 循环。如果需要确定性资源释放，可调用 `gc()`/`collect()` 显式触发回收，或使用 `reset()` 在最后引用消失时立即销毁。
- `reset()` 检查是否存在其他 `GcPtr` 指向同一对象：若仅有当前指针持有该对象，则立即销毁；若存在其他共享引用，则仅将当前指针置空，由 GC 后续回收。
- `release()` 将对象移出 GC 管理，调用者负责手动 `delete`。释放后的对象不可再交给其他 `GcPtr` 管理。
- 自定义 `deleter` 不应抛出异常；若必须抛出，库会确保堆分配的删除器对象被正确清理后再重新抛出异常，避免内存泄漏。
- 根检测依赖编译器和平台的对象布局，对于非标准内存布局可能存在局限，此时可使用 `mark_as_root()` 显式声明。
- **构造期间保护**：正在构造的对象（`under_construction` 状态）即使从根集合不可达也不会被回收，避免构造函数中 `this` 指针被提前销毁的灾难。
- 跨模块限制：每个动态库（DLL/.so）拥有自己的 GC 堆，跨模块共享 GC 对象时需注意。

## 系统要求

- **C++ 标准**：C++17 或更高版本
- **编译器**：支持 C++17 的 g++ 7+/clang++ 5+（推荐 mingw-w64 15.2.0 + C++26 以获得最佳兼容性）
- **测试框架**：Google Test（GTest）C++17 版本
- **操作系统**：跨平台（Linux、macOS、Windows）

### 多线程支持

多线程模式需要定义 `GPTR_THREAD` 宏，并在链接时添加 `-pthread` 标志。

## 构建和测试

项目使用 Makefile 构建和测试。

```bash
make        # 构建测试
make test   # 构建并运行测试
make clean  # 清理
```

## 许可证

MIT License
