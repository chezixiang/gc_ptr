#ifndef GC_PTR_HPP
#define GC_PTR_HPP

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
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
#include <thread>
#endif

using GcDeleterFn = void (*)(uintptr_t);

struct GcNode {
    void* address = nullptr;
    std::size_t size = 0;
    bool marked = false;
    bool under_construction = true;
    GcDeleterFn invoke_deleter = nullptr;
    uintptr_t deleter_context = 0;
    GcDeleterFn destroy_deleter_ctx = nullptr;
};

class GcPtrBase;

struct GcContext {
    std::map<void*, GcNode> gc_objects;
    std::map<const void*, GcPtrBase*, std::less<const void*>> all_ptrs;
    std::vector<std::pair<const char*, const char*>> ranges;
    bool ranges_dirty = true;

#ifdef GPTR_THREAD
    std::recursive_mutex mutex;
#endif

    std::size_t destruct_count = 0;
    std::size_t destruct_threshold = 40;
    std::chrono::seconds gc_interval{120};
    std::chrono::steady_clock::time_point last_gc_time{};
    std::atomic<bool> time_initialized{false};
    std::atomic<bool> auto_gc_enabled{true};

    #ifdef GPTR_THREAD
    std::atomic<bool> gc_in_progress{false};
    std::atomic<bool> gc_pending{false};
#else
    bool gc_in_progress = false;
    bool gc_pending = false;
#endif
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
private:
    void* object_ptr = nullptr;
    bool is_root = false;

protected:
    void* get_object_ptr() const {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return object_ptr;
    }
    void set_object_ptr(void* new_ptr) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        object_ptr = new_ptr;
    }

    struct defer_init_t {};
    GcPtrBase(defer_init_t) {}

    void do_init_base() {
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            is_root = !is_within_any_gc_object_unsafe(this);
        }
        register_this();
    }

    bool get_is_root() const {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return is_root;
    }

    void mark_as_root() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        is_root = true;
    }

    void unmark_as_root() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        is_root = false;
    }

    GcPtrBase() {
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            is_root = !is_within_any_gc_object_unsafe(this);
        }
        register_this();
        check_auto_gc();
    }

    GcPtrBase(const GcPtrBase& other) {
        (void)other;
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            is_root = !is_within_any_gc_object_unsafe(this);
        }
        register_this();
        check_auto_gc();
    }

    GcPtrBase(GcPtrBase&& other) noexcept {
        (void)other;
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            is_root = !is_within_any_gc_object_unsafe(this);
        }
        register_this();
        check_auto_gc();
    }

    GcPtrBase& operator=(const GcPtrBase&) = delete;
    GcPtrBase& operator=(GcPtrBase&&) = delete;

public:
    virtual ~GcPtrBase() {
        auto_collect_on_destruct();
        unregister_this();
    }

    static std::map<void*, GcNode>& gc_objects() {
        return gc_get_context().gc_objects;
    }

    static std::map<const void*, GcPtrBase*, std::less<const void*>>& all_ptrs() {
        return gc_get_context().all_ptrs;
    }

    static std::vector<std::pair<const char*, const char*>>& gc_ranges() {
        return gc_get_context().ranges;
    }

    static bool& gc_ranges_dirty() {
        return gc_get_context().ranges_dirty;
    }

#ifdef GPTR_THREAD
    static std::recursive_mutex& gc_mutex() {
        return ::gc_mutex();
    }
#endif

    template <typename Deleter>
    static void register_gc_object(void* p, std::size_t sz, Deleter&& d,
                                   bool under_construction = true) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        using DecayD = std::decay_t<Deleter>;
        GcNode node;
        node.address = p;
        node.size = sz;
        node.under_construction = under_construction;

        auto* heap_d = new DecayD(std::forward<Deleter>(d));
        node.invoke_deleter = [](uintptr_t ctx) {
            (*reinterpret_cast<DecayD*>(ctx))();
        };
        node.deleter_context = reinterpret_cast<uintptr_t>(heap_d);
        node.destroy_deleter_ctx = [](uintptr_t ctx) {
            delete reinterpret_cast<DecayD*>(ctx);
        };

        try {
            gc_objects().emplace(p, node);
            gc_ranges_dirty() = true;
        } catch (...) {
            delete heap_d;
            throw;
        }
    }

    static void unregister_gc_object(void* p) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto it = gc_objects().find(p);
        if (it != gc_objects().end()) {
            if (it->second.destroy_deleter_ctx &&
                it->second.deleter_context) {
                it->second.destroy_deleter_ctx(it->second.deleter_context);
            }
            gc_objects().erase(it);
            gc_ranges_dirty() = true;
        }
    }

    [[nodiscard]] static int count_references(void* obj) {
        int count = 0;
        for (const auto& [_, base] : all_ptrs()) {
            if (base->get_object_ptr() == obj)
                ++count;
        }
        return count;
    }

    static void rebuild_ranges() {
        gc_ranges().clear();
        for (const auto& [addr, node] : gc_objects()) {
            auto begin = static_cast<const char*>(addr);
            gc_ranges().emplace_back(begin, begin + node.size);
        }
        std::sort(gc_ranges().begin(), gc_ranges().end(),
                  [](const auto& a, const auto& b) {
                      return std::less<const char*>{}(a.first, b.first);
                  });
        gc_ranges_dirty() = false;
    }

    static bool is_within_any_gc_object_unsafe(const void* addr) {
        if (gc_ranges_dirty())
            rebuild_ranges();

        auto target = static_cast<const char*>(addr);
        auto it = std::lower_bound(
            gc_ranges().begin(), gc_ranges().end(), target,
            [](const auto& range, const char* val) {
                return std::less<const char*>{}(range.second, val);
            });
        if (it == gc_ranges().end())
            return false;
        return !std::less<const char*>{}(target, it->first) &&
               std::less<const char*>{}(target, it->second);
    }

    [[nodiscard]] static bool is_within_any_gc_object(const void* addr) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return is_within_any_gc_object_unsafe(addr);
    }

    static void set_context(GcContext* ctx) { gc_set_context(ctx); }
    static void reset_context() { gc_reset_context(); }
    [[nodiscard]] static GcContext& get_context() { return gc_get_context(); }

    static void check_auto_gc() {
        auto& ctx = gc_get_context();
        if (!ctx.auto_gc_enabled.load(std::memory_order_relaxed))
            return;

        bool should_collect = false;
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            if (!ctx.time_initialized.load(std::memory_order_relaxed)) {
                ctx.last_gc_time = std::chrono::steady_clock::now();
                ctx.time_initialized.store(true, std::memory_order_relaxed);
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - ctx.last_gc_time >= ctx.gc_interval) {
                should_collect = true;
                ctx.last_gc_time = now;
            }
        }
        if (should_collect) {
            collect();
        }
    }

    static void auto_collect_on_destruct() {
        auto& ctx = gc_get_context();
        if (!ctx.auto_gc_enabled.load(std::memory_order_relaxed))
            return;

        bool should_collect = false;
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            ++ctx.destruct_count;
            if (ctx.destruct_count >= ctx.destruct_threshold) {
                should_collect = true;
                ctx.destruct_count = 0;
            }
        }
        if (should_collect) {
            collect();
        }
        check_auto_gc();
    }

    template <typename Func>
    static void* allocate_with_retry(Func alloc_func) {
        void* p = nullptr;
        try {
            p = alloc_func();
        } catch (const std::bad_alloc&) {
#ifdef GPTR_THREAD
            auto& ctx = gc_get_context();
            if (ctx.gc_in_progress.load(std::memory_order_acquire)) {
                ctx.gc_pending.store(true, std::memory_order_release);
                while (ctx.gc_in_progress.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                try {
                    p = alloc_func();
                } catch (const std::bad_alloc&) {
                    collect();
                    p = alloc_func();
                }
                return p;
            }
#else
            auto& ctx = gc_get_context();
            if (ctx.gc_in_progress) {
                ctx.gc_pending = true;
                try {
                    p = alloc_func();
                    return p;
                } catch (const std::bad_alloc&) {
                    throw;
                }
            }
#endif
            collect();
            p = alloc_func();
        }
        return p;
    }

    static void run_pending_gc() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        bool pending = ctx.gc_pending.exchange(false, std::memory_order_acq_rel);
        if (pending && !ctx.gc_in_progress.load(std::memory_order_acquire)) {
#else
        bool pending = ctx.gc_pending;
        ctx.gc_pending = false;
        if (pending && !ctx.gc_in_progress) {
#endif
            collect();
        }
    }

    static void reset_all_marks() {
        for (auto& [_, node] : gc_objects())
            node.marked = false;
    }

    static std::vector<void*> collect_root_objects() {
        std::vector<void*> queue;
        for (const auto& [_, base] : all_ptrs()) {
            void* obj = base->get_object_ptr();
            if (base->get_is_root() && obj) {
                auto it = gc_objects().find(obj);
                if (it != gc_objects().end() && !it->second.marked) {
                    it->second.marked = true;
                    queue.push_back(obj);
                }
            }
        }
        return queue;
    }

    static void mark_reachable_objects(std::vector<void*>& queue) {
        while (!queue.empty()) {
            void* cur = queue.back();
            queue.pop_back();

            auto it = gc_objects().find(cur);
            if (it == gc_objects().end())
                continue;

            auto begin_addr = static_cast<const char*>(cur);
            auto end_addr = begin_addr + it->second.size;

            auto ptr_it = all_ptrs().lower_bound(begin_addr);
            for (; ptr_it != all_ptrs().end(); ++ptr_it) {
                auto key_addr = static_cast<const char*>(ptr_it->first);
                if (!std::less<const char*>{}(key_addr, end_addr))
                    break;

                GcPtrBase* inner = ptr_it->second;
                void* inner_obj = inner->get_object_ptr();
                if (!inner_obj)
                    continue;

                auto inner_it = gc_objects().find(inner_obj);
                if (inner_it != gc_objects().end() &&
                    !inner_it->second.marked) {
                    inner_it->second.marked = true;
                    queue.push_back(inner_obj);
                }
            }
        }
    }

public:
    static void collect() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        if (ctx.gc_in_progress.exchange(true, std::memory_order_acq_rel))
            return;
#else
        if (ctx.gc_in_progress)
            return;
        ctx.gc_in_progress = true;
#endif
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif

        try {
            collect_core();
        } catch (...) {
#ifdef GPTR_THREAD
            ctx.gc_in_progress.store(false, std::memory_order_release);
#else
            ctx.gc_in_progress = false;
#endif
            throw;
        }

#ifdef GPTR_THREAD
        ctx.gc_in_progress.store(false, std::memory_order_release);
#else
        ctx.gc_in_progress = false;
#endif
        run_pending_gc();
    }

    static void emergency_cleanup() {
        collect();
    }

    static void set_gc_interval(std::chrono::seconds interval) {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        ctx.gc_interval = interval;
        ctx.time_initialized.store(false, std::memory_order_relaxed);
    }

    static void set_destruct_threshold(std::size_t threshold) {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        ctx.destruct_threshold = threshold;
    }

    static void disable_auto_gc() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        ctx.auto_gc_enabled.store(false, std::memory_order_relaxed);
    }

    static void enable_auto_gc() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        ctx.auto_gc_enabled.store(true, std::memory_order_relaxed);
    }

    [[nodiscard]] static bool is_auto_gc_enabled() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return ctx.auto_gc_enabled.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static std::size_t gc_object_count() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return gc_objects().size();
    }

#ifdef GC_PTR_EXPOSE_INTERNALS
    [[nodiscard]] static bool is_gc_in_progress() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        return ctx.gc_in_progress.load(std::memory_order_acquire);
#else
        return ctx.gc_in_progress;
#endif
    }

    [[nodiscard]] static std::size_t get_destruct_count() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return ctx.destruct_count;
    }

    static void reset_destruct_count() {
        auto& ctx = gc_get_context();
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        ctx.destruct_count = 0;
    }
#endif

private:
    static void collect_core() {
        reset_all_marks();

        std::vector<void*> queue;
        for (auto& [addr, node] : gc_objects()) {
            if (node.under_construction && !node.marked) {
                node.marked = true;
                queue.push_back(addr);
            }
        }

        std::vector<void*> roots = collect_root_objects();
        queue.insert(queue.end(), roots.begin(), roots.end());

        mark_reachable_objects(queue);

        struct GarbageInfo {
            void* addr;
            std::size_t size;
        };

        struct PendingDeleter {
            GcDeleterFn invoke;
            uintptr_t context;
            GcDeleterFn destroy;
        };
        std::vector<PendingDeleter> pending_deleters;
        std::vector<GarbageInfo> garbage_objects;
        std::set<void*> garbage_addrs;

        for (auto it = gc_objects().begin(); it != gc_objects().end();) {
            if (!it->second.marked && !it->second.under_construction) {
                garbage_addrs.insert(it->first);
                garbage_objects.push_back({it->first, it->second.size});
                pending_deleters.push_back({it->second.invoke_deleter,
                                            it->second.deleter_context,
                                            it->second.destroy_deleter_ctx});
                it = gc_objects().erase(it);
                gc_ranges_dirty() = true;
            } else {
                ++it;
            }
        }

        for (const auto& [key, base] : all_ptrs()) {
            auto k = static_cast<const char*>(key);
            for (const auto& gi : garbage_objects) {
                auto begin = static_cast<const char*>(gi.addr);
                auto end = begin + gi.size;
                if (k >= begin && k < end) {
                    void* inner = base->get_object_ptr();
                    if (inner && garbage_addrs.count(inner)) {
                        base->set_object_ptr(nullptr);
                    }
                    break;
                }
            }
        }

        std::exception_ptr first_exception;
        for (auto& pd : pending_deleters) {
            if (!pd.invoke && !pd.destroy) continue;
            try {
                if (pd.invoke) pd.invoke(pd.context);
            } catch (...) {
                if (!first_exception)
                    first_exception = std::current_exception();
            }
            if (pd.destroy) {
                try {
                    pd.destroy(pd.context);
                } catch (...) {
                    if (!first_exception)
                        first_exception = std::current_exception();
                }
            }
        }
        if (first_exception)
            std::rethrow_exception(first_exception);
    }

    void register_this() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        all_ptrs().emplace(this, this);
    }
    void unregister_this() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        all_ptrs().erase(this);
    }
};

template <typename T>
class GcPtr : private GcPtrBase {
public:
    GcPtr() = default;

    template <typename... Args>
    explicit GcPtr(std::in_place_t, Args&&... args) {
        void* raw =
            allocate_with_retry([] { return operator new(sizeof(T)); });
        T* ptr = static_cast<T*>(raw);

        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            try {
                gc_objects().emplace(ptr, GcNode{ptr, sizeof(T), false, true,
                                                 [](uintptr_t ctx) {
                                                     delete reinterpret_cast<T*>(ctx);
                                                 },
                                                 reinterpret_cast<uintptr_t>(ptr), nullptr});
                gc_ranges_dirty() = true;
            } catch (...) {
                operator delete(ptr);
                throw;
            }
        }

        try {
            new (ptr) T(std::forward<Args>(args)...);
        } catch (...) {
            {
#ifdef GPTR_THREAD
                std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
                gc_objects().erase(ptr);
                gc_ranges_dirty() = true;
            }
            operator delete(ptr);
            throw;
        }

        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            set_object_ptr(ptr);
            auto it = gc_objects().find(ptr);
            if (it != gc_objects().end()) {
                it->second.under_construction = false;
            }
        }
    }

    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter, std::size_t real_size) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        if (p) {
            if (gc_objects().find(p) != gc_objects().end()) {
                throw std::runtime_error("Pointer already registered with GC");
            }
            register_gc_object(p, real_size,
                               [p, d = std::move(deleter)]() mutable { d(p); },
                               false);
        }
        set_object_ptr(p);
    }

    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter)
        : GcPtr(p, deleter, sizeof(T)) {}

    explicit GcPtr(T* p)
        : GcPtr(p, [](T* ptr) { delete ptr; }, sizeof(T)) {}

    GcPtr(const GcPtr& other) : GcPtrBase(defer_init_t{}) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        do_init_base();
        set_object_ptr(other.get_object_ptr());
        check_auto_gc();
    }

    GcPtr(GcPtr&& other) noexcept : GcPtrBase(defer_init_t{}) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        do_init_base();
        void* ptr = other.get_object_ptr();
        set_object_ptr(ptr);
        other.set_object_ptr(nullptr);
        check_auto_gc();
    }

    GcPtr& operator=(const GcPtr& other) {
        if (this != &other) {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            void* old_ptr = get_object_ptr();
            void* new_ptr = other.get_object_ptr();
            set_object_ptr(new_ptr);
            destroy_object_if_last(old_ptr);
            run_pending_gc();
        }
        return *this;
    }

    GcPtr& operator=(GcPtr&& other) noexcept {
        if (this != &other) {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            void* old_ptr = get_object_ptr();
            void* new_ptr = other.get_object_ptr();
            set_object_ptr(new_ptr);
            other.set_object_ptr(nullptr);
            destroy_object_if_last(old_ptr);
            run_pending_gc();
        }
        return *this;
    }

    ~GcPtr() = default;

    void reset() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        void* old_ptr = get_object_ptr();
        set_object_ptr(nullptr);
        destroy_object_if_last(old_ptr);
    }

    void reset(T* new_ptr) {
        reset_with_deleter(new_ptr, [](T* p) { delete p; }, sizeof(T));
    }

    template <typename Deleter>
    void reset_with_deleter(T* new_ptr, Deleter deleter,
                            std::size_t real_size) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        void* old_ptr = get_object_ptr();

        if (static_cast<T*>(old_ptr) == new_ptr && new_ptr != nullptr)
            return;

        if (new_ptr) {
            if (gc_objects().find(new_ptr) != gc_objects().end()) {
                throw std::runtime_error(
                    "Pointer already registered with GC");
            }
            register_gc_object(
                new_ptr, real_size,
                [new_ptr, d = std::move(deleter)]() mutable { d(new_ptr); },
                false);
        }

        set_object_ptr(new_ptr);
        destroy_object_if_last(old_ptr);
    }

    [[nodiscard]] T* release() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        T* old_ptr = static_cast<T*>(get_object_ptr());
        if (old_ptr) {
            set_object_ptr(nullptr);
            unregister_gc_object(old_ptr);
        }
        return old_ptr;
    }

    void mark_as_root() { GcPtrBase::mark_as_root(); }
    void unmark_as_root() { GcPtrBase::unmark_as_root(); }

    void swap(GcPtr& other) noexcept {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        void* tmp = get_object_ptr();
        set_object_ptr(other.get_object_ptr());
        other.set_object_ptr(tmp);
    }

    T& operator*() {
        void* ptr = get_object_ptr();
        assert(ptr && "dereferencing null GcPtr");
        return *static_cast<T*>(ptr);
    }
    const T& operator*() const {
        void* ptr = get_object_ptr();
        assert(ptr && "dereferencing null GcPtr");
        return *static_cast<const T*>(ptr);
    }

    T* operator->() {
        void* ptr = get_object_ptr();
        assert(ptr && "dereferencing null GcPtr");
        return static_cast<T*>(ptr);
    }
    const T* operator->() const {
        void* ptr = get_object_ptr();
        assert(ptr && "dereferencing null GcPtr");
        return static_cast<const T*>(ptr);
    }

    [[nodiscard]] T* get() { return static_cast<T*>(get_object_ptr()); }
    [[nodiscard]] const T* get() const {
        return static_cast<const T*>(get_object_ptr());
    }

    [[nodiscard]] explicit operator bool() const {
        return get_object_ptr() != nullptr;
    }

    void gc() { GcPtrBase::collect(); }

    static void emergency_cleanup() { GcPtrBase::emergency_cleanup(); }

    static void set_gc_interval(std::chrono::seconds t) {
        GcPtrBase::set_gc_interval(t);
    }
    static void set_destruct_threshold(std::size_t n) {
        GcPtrBase::set_destruct_threshold(n);
    }
    static void disable_auto_gc() { GcPtrBase::disable_auto_gc(); }
    static void enable_auto_gc() { GcPtrBase::enable_auto_gc(); }
    [[nodiscard]] static bool is_auto_gc_enabled() { return GcPtrBase::is_auto_gc_enabled(); }
    [[nodiscard]] static std::size_t gc_object_count() {
        return GcPtrBase::gc_object_count();
    }
    static void set_context(GcContext* ctx) { GcPtrBase::set_context(ctx); }
    static void reset_context() { GcPtrBase::reset_context(); }

    friend bool operator==(const GcPtr& a, const GcPtr& b) {
        return a.get_object_ptr() == b.get_object_ptr();
    }
    friend bool operator!=(const GcPtr& a, const GcPtr& b) {
        return a.get_object_ptr() != b.get_object_ptr();
    }
    friend bool operator<(const GcPtr& a, const GcPtr& b) noexcept {
        return std::less<const T*>{}(a.get(), b.get());
    }

    bool owner_before(const GcPtr& other) const noexcept {
        return std::less<const void*>{}(get(), other.get());
    }

    template <typename U>
    bool owner_before(const GcPtr<U>& other) const noexcept {
        return std::less<const void*>{}(get(), other.get());
    }

    template <typename U>
    friend class std::atomic;

    template <typename U>
    friend class GcPtr;

private:
    static void destroy_object_if_last(void* old_ptr) {
        if (!old_ptr)
            return;
        if (count_references(old_ptr) > 0)
            return;

        auto it = gc_objects().find(old_ptr);
        if (it == gc_objects().end())
            return;

        auto invoke = it->second.invoke_deleter;
        auto ctx = it->second.deleter_context;
        auto destroy = it->second.destroy_deleter_ctx;

        if (!invoke && !destroy) {
            gc_objects().erase(it);
            gc_ranges_dirty() = true;
            return;
        }

        bool invoke_threw = false;
        try {
            if (invoke) invoke(ctx);
        } catch (...) {
            invoke_threw = true;
        }

        if (destroy) {
            try {
                destroy(ctx);
            } catch (...) {
            }
        }

        gc_objects().erase(old_ptr);
        gc_ranges_dirty() = true;

        if (invoke_threw) throw;
    }
};

template <typename T, typename... Args>
[[nodiscard]] GcPtr<T> make_gc(Args&&... args) {
    return GcPtr<T>(std::in_place, std::forward<Args>(args)...);
}

template <typename T, typename Deleter>
[[nodiscard]] GcPtr<T> make_gc_with_deleter(T* ptr, Deleter deleter) {
    return GcPtr<T>(ptr, deleter, sizeof(T));
}

template <typename T, typename Deleter>
GcPtr<T> make_gc_with_deleter(T* ptr, Deleter deleter,
                              std::size_t real_size) {
    return GcPtr<T>(ptr, deleter, real_size);
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
        return ptr;
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
        ptr.set_object_ptr(desired.get_object_ptr());
        desired.set_object_ptr(nullptr);
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
        value_type old;
        old.set_object_ptr(ptr.get_object_ptr());
        ptr.set_object_ptr(desired.get_object_ptr());
        desired.set_object_ptr(nullptr);
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
            ptr.set_object_ptr(desired.get_object_ptr());
            desired.set_object_ptr(nullptr);
            return true;
        }
        expected.set_object_ptr(ptr.get_object_ptr());
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