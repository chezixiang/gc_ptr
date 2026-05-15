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

在多线程模式（定义 `GPTR_THREAD` 宏）下，`get()` 或 `operator->` 返回的指针仅在**不触发 GC** 的期间有效。避免在 GC 回收点（如分配新对象的函数调用或显式 `gc()` 调用）之间持有此类裸指针。

### 多态对象

如果 `GcPtr<Base>` 实际指向 Derived 对象，注册时必须传递正确的实际大小（`sizeof(Derived)`），否则 GC 不会扫描 Derived 部分。使用接受 `real_size` 参数的重载版本：

```cpp
GcPtr<Base> ptr(derived_raw_ptr,
                [](Base* p){ delete static_cast<Derived*>(p); },
                sizeof(Derived));
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

#### 静态成员函数

- `static void set_gc_interval(std::chrono::seconds t)` - 设置定时GC间隔
- `static void set_destruct_threshold(std::size_t n)` - 设置析构计数阈值

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

## 实现细节

- 根对象检测：栈上的 GcPtr 自动识别为 GC 根
- 对象注册表：使用 `std::map` 和排序区间表管理所有 GC 对象
- 引用扫描：在 GC 对象内存区域内扫描内部 GcPtr 成员
- 标记-清除：标准的标记清除算法，支持循环引用回收
- 线程安全：定义 `GPTR_THREAD` 宏后启用
  - 递归互斥锁保护内部数据结构（`gc_objects`、`all_ptrs`、`gc_ranges`）
  - 读写锁（`std::shared_mutex`）保护对象访问：`operator->`/`operator*`/`get()` 持共享锁，`collect()` 持独占锁，确保 GC 期间不会出现 use‑after‑free

## 注意事项

- `reset()` 检查是否存在其他 `GcPtr` 指向同一对象：若仅有当前指针持有该对象，则立即销毁（保持与 `std::shared_ptr::reset()` 兼容）；若存在其他共享引用，则仅将当前指针置空，由 GC 后续回收该对象。
- `release()` 将对象移出 GC 管理，调用者负责手动 `delete`。释放后的对象不可再交给其他 `GcPtr` 管理。
- 多线程模式下，`collect()` 采用读写锁实现 stop‑the‑world 语义：GC 回收期间所有 `operator->` / `operator*` / `get()` 访问将阻塞，确保不会出现 use‑after‑free。持有 `operator->` 返回的裸指针跨越 GC 周期是不安全的。
- 自定义 `deleter` 不应抛出异常；若必须抛出，对象在 deleter 调用前已从 GC 注册表中移除，保证 GC 状态一致性。
- 根检测依赖编译器和平台的对象布局，对于非标准内存布局可能存在局限。

## 构建和测试

项目使用 Makefile 构建和测试。需要 g++ 和 GTest。

```bash
make        # 构建测试
make test   # 构建并运行测试
make clean  # 清理
```

## 许可证

MIT License