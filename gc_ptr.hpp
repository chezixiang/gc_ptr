#ifndef GC_PTR_HPP
#define GC_PTR_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef GPTR_THREAD
#include <mutex>
#endif

struct ControlBlock {
    std::atomic<int> ref_count;
    void* object;
    std::size_t object_size;

    void (*invoke_deleter)(void* obj, void* ctx);
    void* deleter_ctx;
    void (*destroy_ctx)(void*);

    ControlBlock(void* obj, std::size_t sz,
                 void (*del)(void*, void*), void* dctx, void (*dd)(void*))
        : ref_count(1), object(obj), object_size(sz),
          invoke_deleter(del), deleter_ctx(dctx), destroy_ctx(dd) {}
};

struct GcContext {
    std::map<void*, ControlBlock*, std::less<void*>> gc_objects;
    std::map<const void*, ControlBlock*, std::less<const void*>> all_ptrs;
    std::vector<std::pair<const char*, const char*>> ranges;
    bool ranges_dirty = true;

#ifdef GPTR_THREAD
    std::recursive_mutex mutex;
#endif

    std::atomic<bool> gc_in_progress{false};
};

#ifndef GC_PTR_API
#  if defined(_WIN32) || defined(_WIN64)
#    ifdef GC_PTR_BUILD_DLL
#      define GC_PTR_API __declspec(dllexport)
#    elif defined(GC_PTR_USE_DLL)
#      define GC_PTR_API __declspec(dllimport)
#    else
#      define GC_PTR_API
#    endif
#  else
#    define GC_PTR_API
#  endif
#endif

#ifdef GC_PTR_IMPLEMENTATION
GC_PTR_API std::atomic<GcContext*> gc_active_context{nullptr};
#else
extern GC_PTR_API std::atomic<GcContext*> gc_active_context;
#endif

inline GcContext& gc_default_context() {
    static GcContext ctx;
    return ctx;
}

inline GcContext& gc_get_context() {
    GcContext* ctx = gc_active_context.load(std::memory_order_acquire);
    return ctx ? *ctx : gc_default_context();
}

inline void gc_set_context(GcContext* ctx) {
    gc_active_context.store(ctx, std::memory_order_release);
}

inline void gc_reset_context() {
    gc_active_context.store(nullptr, std::memory_order_release);
}

#ifdef GPTR_THREAD
inline std::recursive_mutex& gc_mutex() {
    return gc_get_context().mutex;
}
#endif

class GcPtrBase {
public:
    GcPtrBase() = delete;

    static void register_gc_object(void* obj, std::size_t, ControlBlock* cb) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto& ctx = gc_get_context();
        ctx.gc_objects.emplace(obj, cb);
        ctx.ranges_dirty = true;
    }

    static void unregister_gc_object(void* obj) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto& ctx = gc_get_context();
        ctx.gc_objects.erase(obj);
        ctx.ranges_dirty = true;
    }

    static void register_gcptr(const void* gcptr_addr, ControlBlock* cb) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        gc_get_context().all_ptrs.emplace(gcptr_addr, cb);
    }

    static void unregister_gcptr(const void* gcptr_addr) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        gc_get_context().all_ptrs.erase(gcptr_addr);
    }

    [[nodiscard]] static int count_references(void* obj) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto& ctx = gc_get_context();
        auto it = ctx.gc_objects.find(obj);
        if (it == ctx.gc_objects.end())
            return 0;
        return it->second->ref_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static bool is_within_any_gc_object(const void* addr) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto& ctx = gc_get_context();
        if (ctx.ranges_dirty)
            rebuild_ranges(ctx);

        auto target = static_cast<const char*>(addr);
        auto it = std::lower_bound(
            ctx.ranges.begin(), ctx.ranges.end(), target,
            [](const auto& range, const char* val) {
                return std::less<const char*>{}(range.second, val);
            });
        if (it == ctx.ranges.end())
            return false;
        return !std::less<const char*>{}(target, it->first) &&
               std::less<const char*>{}(target, it->second);
    }

    [[nodiscard]] static std::size_t gc_object_count() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return gc_get_context().gc_objects.size();
    }

    static void collect() {
        auto& ctx = gc_get_context();
        bool expected = false;
        if (!ctx.gc_in_progress.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_acquire))
            return;

#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif

        try {
            collect_core(ctx);
        } catch (...) {
            ctx.gc_in_progress.store(false, std::memory_order_release);
            throw;
        }

        ctx.gc_in_progress.store(false, std::memory_order_release);
    }

    static void set_context(GcContext* ctx) { gc_set_context(ctx); }
    static void reset_context() { gc_reset_context(); }

#ifdef GC_PTR_EXPOSE_INTERNALS
    [[nodiscard]] static bool is_gc_in_progress() {
        return gc_get_context().gc_in_progress.load(std::memory_order_acquire);
    }
#endif

private:
    static void rebuild_ranges(GcContext& ctx) {
        ctx.ranges.clear();
        for (const auto& [addr, cb] : ctx.gc_objects) {
            auto begin = static_cast<const char*>(addr);
            ctx.ranges.emplace_back(begin, begin + cb->object_size);
        }
        std::sort(ctx.ranges.begin(), ctx.ranges.end(),
                  [](const auto& a, const auto& b) {
                      return std::less<const char*>{}(a.first, b.first);
                  });
        ctx.ranges_dirty = false;
    }

    static bool is_gcptr_root(const void* gcptr_addr, GcContext& ctx) {
        return !is_within_any_gc_object_unsafe(gcptr_addr, ctx);
    }

    static bool is_within_any_gc_object_unsafe(const void* addr, GcContext& ctx) {
        if (ctx.ranges_dirty)
            rebuild_ranges(ctx);

        auto target = static_cast<const char*>(addr);
        auto it = std::lower_bound(
            ctx.ranges.begin(), ctx.ranges.end(), target,
            [](const auto& range, const char* val) {
                return std::less<const char*>{}(range.second, val);
            });
        if (it == ctx.ranges.end())
            return false;
        return !std::less<const char*>{}(target, it->first) &&
               std::less<const char*>{}(target, it->second);
    }

    static void collect_core(GcContext& ctx) {
        std::set<void*> marked;

        for (const auto& [gcptr_addr, cb] : ctx.all_ptrs) {
            if (!cb || !cb->object)
                continue;
            if (is_gcptr_root(gcptr_addr, ctx)) {
                mark_reachable(cb->object, ctx, marked);
            }
        }

        std::vector<ControlBlock*> garbage;
        for (auto it = ctx.gc_objects.begin(); it != ctx.gc_objects.end();) {
            if (marked.count(it->first) == 0) {
                garbage.push_back(it->second);
                it = ctx.gc_objects.erase(it);
                ctx.ranges_dirty = true;
            } else {
                ++it;
            }
        }

        for (auto* cb : garbage) {
            int expected = 1;
            if (!cb->ref_count.compare_exchange_strong(expected, 0,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                continue;
            if (cb->invoke_deleter)
                cb->invoke_deleter(cb->object, cb->deleter_ctx);
            if (cb->destroy_ctx)
                cb->destroy_ctx(cb->deleter_ctx);
            delete cb;
        }

        std::vector<const void*> orphaned_ptrs;
        for (const auto& [gcptr_addr, cb] : ctx.all_ptrs) {
            if (cb && ctx.gc_objects.find(cb->object) == ctx.gc_objects.end()) {
                orphaned_ptrs.push_back(gcptr_addr);
            }
        }
        for (auto* addr : orphaned_ptrs) {
            ctx.all_ptrs.erase(addr);
        }
    }

    static void mark_reachable(void* obj, GcContext& ctx, std::set<void*>& marked) {
        if (!obj || marked.count(obj))
            return;
        marked.insert(obj);

        auto it = ctx.gc_objects.find(obj);
        if (it == ctx.gc_objects.end())
            return;

        auto begin_addr = static_cast<const char*>(obj);
        auto end_addr = begin_addr + it->second->object_size;

        auto ptr_it = ctx.all_ptrs.lower_bound(begin_addr);
        for (; ptr_it != ctx.all_ptrs.end(); ++ptr_it) {
            auto key_addr = static_cast<const char*>(ptr_it->first);
            if (!std::less<const char*>{}(key_addr, end_addr))
                break;

            ControlBlock* inner_cb = ptr_it->second;
            if (inner_cb && inner_cb->object) {
                mark_reachable(inner_cb->object, ctx, marked);
            }
        }
    }
};

template <typename T>
class GcPtr {
public:
    GcPtr() : ptr_(nullptr), cb_(nullptr) {
        GcPtrBase::register_gcptr(this, nullptr);
    }

    template <typename... Args>
    explicit GcPtr(std::in_place_t, Args&&... args) {
        void* raw = operator new(sizeof(T));
        T* obj = static_cast<T*>(raw);
        try {
            new (obj) T(std::forward<Args>(args)...);
        } catch (...) {
            operator delete(obj);
            GcPtrBase::register_gcptr(this, nullptr);
            throw;
        }

        ControlBlock* cb = nullptr;
        try {
            cb = new ControlBlock(
                obj, sizeof(T),
                [](void* obj, void*) { delete static_cast<T*>(obj); },
                obj, nullptr);
        } catch (...) {
            obj->~T();
            operator delete(obj);
            GcPtrBase::register_gcptr(this, nullptr);
            throw;
        }

        cb_ = cb;
        ptr_ = obj;
        GcPtrBase::register_gc_object(obj, sizeof(T), cb);
        GcPtrBase::register_gcptr(this, cb);
    }

    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter, std::size_t real_size) {
        if (!p) {
            ptr_ = nullptr;
            cb_ = nullptr;
            GcPtrBase::register_gcptr(this, nullptr);
            return;
        }

#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto& ctx = gc_get_context();
        if (ctx.gc_objects.find(p) != ctx.gc_objects.end()) {
            GcPtrBase::register_gcptr(this, nullptr);
            throw std::runtime_error("Pointer already registered with GC");
        }

        using DecayD = std::decay_t<Deleter>;
        DecayD* heap_d = nullptr;
        ControlBlock* cb = nullptr;

        try {
            heap_d = new DecayD(std::move(deleter));
        } catch (...) {
            GcPtrBase::register_gcptr(this, nullptr);
            throw;
        }

        try {
            cb = new ControlBlock(
                p, real_size,
                [](void* obj, void* ctx) { (*static_cast<DecayD*>(ctx))(static_cast<T*>(obj)); },
                heap_d,
                [](void* ctx) { delete static_cast<DecayD*>(ctx); });
        } catch (...) {
            delete heap_d;
            GcPtrBase::register_gcptr(this, nullptr);
            throw;
        }

        cb_ = cb;
        ptr_ = p;
        GcPtrBase::register_gc_object(p, real_size, cb);
        GcPtrBase::register_gcptr(this, cb);
    }

    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter)
        : GcPtr(p, std::move(deleter), sizeof(T)) {}

    explicit GcPtr(T* p)
        : GcPtr(p, [](T* ptr) { delete ptr; }, sizeof(T)) {}

    GcPtr(const GcPtr& other) : ptr_(other.ptr_), cb_(other.cb_) {
        incref();
        GcPtrBase::register_gcptr(this, cb_);
    }

    GcPtr(GcPtr&& other) noexcept
        : ptr_(other.ptr_), cb_(other.cb_) {
        other.ptr_ = nullptr;
        other.cb_ = nullptr;
        GcPtrBase::register_gcptr(this, cb_);
        GcPtrBase::unregister_gcptr(&other);
        GcPtrBase::register_gcptr(&other, nullptr);
    }

    GcPtr& operator=(const GcPtr& other) {
        if (this == &other)
            return *this;
        decref();
        ptr_ = other.ptr_;
        cb_ = other.cb_;
        incref();
        GcPtrBase::unregister_gcptr(this);
        GcPtrBase::register_gcptr(this, cb_);
        return *this;
    }

    GcPtr& operator=(GcPtr&& other) noexcept {
        if (this == &other)
            return *this;
        decref();
        ptr_ = other.ptr_;
        cb_ = other.cb_;
        other.ptr_ = nullptr;
        other.cb_ = nullptr;
        GcPtrBase::unregister_gcptr(&other);
        GcPtrBase::register_gcptr(&other, nullptr);
        GcPtrBase::unregister_gcptr(this);
        GcPtrBase::register_gcptr(this, cb_);
        return *this;
    }

    ~GcPtr() {
        decref();
        GcPtrBase::unregister_gcptr(this);
    }

    void reset() {
        decref();
        ptr_ = nullptr;
        cb_ = nullptr;
        GcPtrBase::unregister_gcptr(this);
        GcPtrBase::register_gcptr(this, nullptr);
    }

    void reset(T* new_ptr) {
        reset_with_deleter(new_ptr, [](T* p) { delete p; }, sizeof(T));
    }

    template <typename Deleter>
    void reset_with_deleter(T* new_ptr, Deleter deleter, std::size_t real_size) {
        if (ptr_ == new_ptr && new_ptr != nullptr)
            return;

        if (new_ptr) {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            auto& ctx = gc_get_context();
            if (ctx.gc_objects.find(new_ptr) != ctx.gc_objects.end()) {
                throw std::runtime_error("Pointer already registered with GC");
            }
        }

        decref();

        if (new_ptr) {
            using DecayD = std::decay_t<Deleter>;
            auto* heap_d = new DecayD(std::move(deleter));
            auto* cb = new ControlBlock(
                new_ptr, real_size,
                [](void* obj, void* ctx) { (*static_cast<DecayD*>(ctx))(static_cast<T*>(obj)); },
                heap_d,
                [](void* ctx) { delete static_cast<DecayD*>(ctx); });
            cb_ = cb;
            ptr_ = new_ptr;
            GcPtrBase::register_gc_object(new_ptr, real_size, cb);
        } else {
            cb_ = nullptr;
            ptr_ = nullptr;
        }

        GcPtrBase::unregister_gcptr(this);
        GcPtrBase::register_gcptr(this, cb_);
    }

    [[nodiscard]] T* release() {
        T* old = ptr_;
        if (old) {
            GcPtrBase::unregister_gc_object(old);
            if (cb_) {
                if (cb_->destroy_ctx)
                    cb_->destroy_ctx(cb_->deleter_ctx);
                delete cb_;
            }
            ptr_ = nullptr;
            cb_ = nullptr;
            GcPtrBase::unregister_gcptr(this);
            GcPtrBase::register_gcptr(this, nullptr);
        }
        return old;
    }

    void swap(GcPtr& other) noexcept {
        std::swap(ptr_, other.ptr_);
        std::swap(cb_, other.cb_);
        GcPtrBase::unregister_gcptr(this);
        GcPtrBase::unregister_gcptr(&other);
        GcPtrBase::register_gcptr(this, cb_);
        GcPtrBase::register_gcptr(&other, other.cb_);
    }

    T& operator*() {
        assert(ptr_ && "dereferencing null GcPtr");
        return *ptr_;
    }
    const T& operator*() const {
        assert(ptr_ && "dereferencing null GcPtr");
        return *ptr_;
    }

    T* operator->() {
        assert(ptr_ && "dereferencing null GcPtr");
        return ptr_;
    }
    const T* operator->() const {
        assert(ptr_ && "dereferencing null GcPtr");
        return ptr_;
    }

    [[nodiscard]] T* get() { return ptr_; }
    [[nodiscard]] const T* get() const { return ptr_; }

    [[nodiscard]] explicit operator bool() const {
        return ptr_ != nullptr;
    }

    static void set_context(GcContext* ctx) { GcPtrBase::set_context(ctx); }
    static void reset_context() { GcPtrBase::reset_context(); }

    friend bool operator==(const GcPtr& a, const GcPtr& b) {
        return a.ptr_ == b.ptr_;
    }
    friend bool operator!=(const GcPtr& a, const GcPtr& b) {
        return a.ptr_ != b.ptr_;
    }
    friend bool operator<(const GcPtr& a, const GcPtr& b) noexcept {
        return std::less<const T*>{}(a.ptr_, b.ptr_);
    }

    bool owner_before(const GcPtr& other) const noexcept {
        return std::less<const void*>{}(ptr_, other.ptr_);
    }

    template <typename U>
    bool owner_before(const GcPtr<U>& other) const noexcept {
        return std::less<const void*>{}(ptr_, other.get());
    }

private:
    T* ptr_;
    ControlBlock* cb_;

    void incref() {
        if (cb_)
            cb_->ref_count.fetch_add(1, std::memory_order_relaxed);
    }

    void decref() {
        if (!cb_)
            return;
        if (cb_->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (cb_->invoke_deleter)
                cb_->invoke_deleter(cb_->object, cb_->deleter_ctx);
            if (cb_->destroy_ctx)
                cb_->destroy_ctx(cb_->deleter_ctx);
            GcPtrBase::unregister_gc_object(cb_->object);
            delete cb_;
            cb_ = nullptr;
            ptr_ = nullptr;
        }
    }
};

template <typename T, typename... Args>
[[nodiscard]] GcPtr<T> make_gc(Args&&... args) {
    return GcPtr<T>(std::in_place, std::forward<Args>(args)...);
}

template <typename T, typename Deleter>
[[nodiscard]] GcPtr<T> make_gc_with_deleter(T* ptr, Deleter deleter) {
    return GcPtr<T>(ptr, std::move(deleter), sizeof(T));
}

template <typename T, typename Deleter>
GcPtr<T> make_gc_with_deleter(T* ptr, Deleter deleter, std::size_t real_size) {
    return GcPtr<T>(ptr, std::move(deleter), real_size);
}

template <typename T>
void swap(GcPtr<T>& a, GcPtr<T>& b) noexcept {
    a.swap(b);
}

namespace std {

template <typename T>
void swap(GcPtr<T>& lhs, GcPtr<T>& rhs) noexcept {
    lhs.swap(rhs);
}

template <typename T>
struct atomic<GcPtr<T>> {
    using value_type = GcPtr<T>;

    atomic() noexcept = default;
    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;

    atomic(value_type desired) noexcept {
        store(std::move(desired));
    }

    ~atomic() = default;

    value_type load(std::memory_order order = std::memory_order_seq_cst) const {
        (void)order;
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return value_type(ptr);
    }

    operator value_type() const noexcept {
        return load();
    }

    void store(value_type desired,
               std::memory_order order = std::memory_order_seq_cst) noexcept {
        (void)order;
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        ptr = std::move(desired);
    }

    value_type operator=(value_type desired) noexcept {
        store(std::move(desired));
        return desired;
    }

    value_type exchange(value_type desired,
                        std::memory_order order =
                            std::memory_order_seq_cst) noexcept {
        (void)order;
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        value_type old = std::move(ptr);
        ptr = std::move(desired);
        return old;
    }

    bool compare_exchange_weak(
        value_type& expected, value_type desired,
        std::memory_order success = std::memory_order_seq_cst,
        std::memory_order failure = std::memory_order_seq_cst) noexcept {
        (void)success;
        (void)failure;
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        if (ptr == expected) {
            ptr = std::move(desired);
            return true;
        }
        expected = value_type(ptr);
        return false;
    }

    bool compare_exchange_strong(
        value_type& expected, value_type desired,
        std::memory_order success = std::memory_order_seq_cst,
        std::memory_order failure = std::memory_order_seq_cst) noexcept {
        return compare_exchange_weak(expected, std::move(desired), success,
                                     failure);
    }

    bool is_lock_free() const noexcept { return false; }

private:
    value_type ptr;
};

template <typename T>
struct hash<GcPtr<T>> {
    size_t operator()(const GcPtr<T>& p) const noexcept {
        return hash<const void*>{}(p.get());
    }
};

template <typename T>
struct owner_less<GcPtr<T>> {
    bool operator()(const GcPtr<T>& a, const GcPtr<T>& b) const noexcept {
        return a.owner_before(b);
    }
};

} // namespace std

#endif // GC_PTR_HPP