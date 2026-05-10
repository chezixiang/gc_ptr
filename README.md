# gc_ptr

一个用 C++ 实现的垃圾回收智能指针库。

## 功能特性

- 自动垃圾回收：使用标记-清除算法自动回收不可达对象
- 标记-清除算法：自动检测并回收循环引用
- 自动垃圾回收触发：可配置定时回收或析构计数阈值回收
- 异常安全：提供异常安全保证
- 简单易用的接口

## 使用方法

### 基本用法

```cpp
#include "gc_ptr.hpp"

// 创建对象
GcPtr<MyClass> obj(arg1, arg2);

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

gc_ptr 自动处理循环引用，无需手动 break 引用链：

```cpp
GcPtr<Node> a;
GcPtr<Node> b;
a->next = b;
b->prev = a;
// 无需手动处理，垃圾回收会自动清理
```

## API 文档

### GcPtr&lt;T&gt;

模板类，提供垃圾回收智能指针功能。

#### 构造函数

- `GcPtr()` - 默认构造函数，创建空指针
- `GcPtr(Args&&... args)` - 构造并初始化对象
- `GcPtr(const T* p)` - 从原始指针构造
- `GcPtr(const GcPtr& other)` - 拷贝构造
- `GcPtr(GcPtr&& other)` - 移动构造

#### 成员函数

- `T* reset()` - 释放当前对象所有权
- `T* reset(T* new_ptr)` - 重置为新对象
- `void swap(GcPtr& other)` - 交换内容
- `T& operator*()` / `const T& operator*() const` - 解引用
- `T* operator->()` / `const T* operator->() const` - 成员访问
- `T* get()` / `const T* get() const` - 获取原始指针
- `explicit operator bool() const` - 检查是否为空
- `void gc()` - 显式触发垃圾回收
- `void release()` - 释放并删除当前对象

#### 静态成员函数

- `static void set_gc_interval(std::chrono::seconds t)` - 设置定时GC间隔
- `static void set_destruct_threshold(std::size_t n)` - 设置析构计数阈值

### GcPtrBase

所有 GcPtr 的基类，提供全局垃圾回收控制。

#### 静态公共成员函数

- `static void collect()` - 执行一次垃圾回收

## 实现细节

- 根对象检测：自动检测栈上的 GcPtr 作为根
- 对象注册表：使用 std::map 管理所有 GC 对象
- 引用扫描：扫描每个 GC 对象内存区域寻找内部指针
- 标记-清除：标准的标记清除算法

## 许可证

MIT License
