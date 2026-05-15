#ifndef GC_PTR_HPP
#define GC_PTR_HPP

// ============================================================================
// Thread Safety Model (aligned with std::shared_ptr rules)
// ============================================================================
//
// Multiple threads can concurrently:
//   - Read the same GcPtr instance (copy, compare, get, operator*, operator->)
//   - Access DIFFERENT GcPtr instances that point to the same object
//   - Access different GcPtr instances
//
// Synchronization is REQUIRED when:
//   - Modifying the same GcPtr instance from multiple threads
//     (reset, assignment, swap, release)
//
// CRITICAL DIFFERENCE from std::shared_ptr:
//   std::shared_ptr uses reference counting; object destruction is strictly
//   determined by the last owner, and no other shared_ptr can access a
//   destroyed object. This GC, however, uses reachability analysis. A raw
//   pointer obtained via get() or operator-> is NOT protected from concurrent
//   reset of OTHER GcPtr instances that may cause the object to become
//   unreachable and be collected. The returned pointer is valid only as long
//   as you do not trigger a GC (via allocation, explicit gc(), or auto-GC).
//
//   Pattern: do NOT hold a raw pointer (from get()) across any operation
//   that might trigger GC. Use the GcPtr's own operators instead.
// ============================================================================

// GC scanning limitation:
// This GC is a precise, conservative-style collector that scans only the
// contiguous memory region of each managed object (real_size bytes starting
// from the object's address). It discovers GcPtr members by their addresses
// falling within that memory range.
//
// IMPORTANT SAFETY RULES:
// 1. GcPtr can be safely stored in ANY container (std::vector, std::set,
//    std::map, std::unordered_set, etc.) regardless of how the container
//    itself is allocated (stack, GC-managed, or raw new). Like std::shared_ptr,
//    a live GcPtr always protects its pointee from collection.
// 2. The raw pointer returned by get() or operator-> is valid only as long
//    as no GC is triggered. See the Thread Safety Model documentation above.
// 3. In multithreaded mode (-DGPTR_THREAD), synchronize writes to the same
//    GcPtr instance as you would with std::shared_ptr.

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef GPTR_THREAD
#include <atomic>
#include <mutex>
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
    void* get_object_ptr_unsafe() const { return object_ptr; }
    void set_object_ptr_unsafe(void* new_ptr) { object_ptr = new_ptr; }

    bool get_is_root_unsafe() const { return is_root; }

    GcPtrBase() {
        is_root = !is_within_any_gc_object(this);
        register_this();
        check_auto_gc();
    }

    GcPtrBase(const GcPtrBase& other) {
        (void)other;
        is_root = !is_within_any_gc_object(this);
        register_this();
        check_auto_gc();
    }

    GcPtrBase(GcPtrBase&& other) noexcept {
        (void)other;
        is_root = !is_within_any_gc_object(this);
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

    using deleter_invoke_fn = void (*)(uintptr_t);

    struct GcNode {
        void* address;
        std::size_t size;
        bool marked = false;
        bool under_construction = true;
        deleter_invoke_fn invoke_deleter = nullptr;
        uintptr_t deleter_context = 0;
        deleter_invoke_fn destroy_deleter_ctx = nullptr;
    };

    static std::map<void*, GcNode>& gc_objects() {
        static std::map<void*, GcNode> objects;
        return objects;
    }

    static std::map<uintptr_t, GcPtrBase*>& all_ptrs() {
        static std::map<uintptr_t, GcPtrBase*> pointers;
        return pointers;
    }

    static std::vector<std::pair<uintptr_t, uintptr_t>>& gc_ranges() {
        static std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
        return ranges;
    }

    static bool& gc_ranges_dirty() {
        static bool dirty = true;
        return dirty;
    }

#ifdef GPTR_THREAD
    static std::recursive_mutex& gc_mutex() {
        static std::recursive_mutex mtx;
        return mtx;
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

        gc_objects().emplace(p, node);
        gc_ranges_dirty() = true;
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

    static int count_references(void* obj) {
        int count = 0;
        for (const auto& [_, base] : all_ptrs()) {
            if (base->get_object_ptr_unsafe() == obj)
                ++count;
        }
        return count;
    }

    static void rebuild_ranges() {
        gc_ranges().clear();
        for (const auto& [addr, node] : gc_objects()) {
            gc_ranges().emplace_back(
                reinterpret_cast<uintptr_t>(addr),
                reinterpret_cast<uintptr_t>(addr) + node.size);
        }
        std::sort(gc_ranges().begin(), gc_ranges().end());
        gc_ranges_dirty() = false;
    }

    static bool is_within_any_gc_object(const void* addr) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        if (gc_ranges_dirty())
            rebuild_ranges();

        auto target = reinterpret_cast<uintptr_t>(addr);
        auto it = std::lower_bound(
            gc_ranges().begin(), gc_ranges().end(), target,
            [](const std::pair<uintptr_t, uintptr_t>& range, uintptr_t val) {
                return range.second <= val;
            });
        if (it == gc_ranges().end())
            return false;
        return target >= it->first && target < it->second;
    }

    static std::size_t destruct_count;
    static std::size_t destruct_threshold;
    static std::chrono::seconds gc_interval;
    static std::chrono::steady_clock::time_point last_gc_time;
    static bool time_initialized;
    static bool auto_gc_enabled;

#ifdef GPTR_THREAD
    static std::atomic<bool> gc_in_progress;
    static std::atomic<bool> gc_pending;
#else
    static bool gc_in_progress;
    static bool gc_pending;
#endif

    static void check_auto_gc() {
        if (!auto_gc_enabled)
            return;

        bool should_collect = false;
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            if (!time_initialized) {
                last_gc_time = std::chrono::steady_clock::now();
                time_initialized = true;
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_gc_time >= gc_interval) {
                should_collect = true;
                last_gc_time = now;
            }
        }
        if (should_collect) {
            collect();
        }
    }

    static void auto_collect_on_destruct() {
        if (!auto_gc_enabled)
            return;

        bool should_collect = false;
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            ++destruct_count;
            if (destruct_count >= destruct_threshold) {
                should_collect = true;
                destruct_count = 0;
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
            if (gc_in_progress) {
#ifdef GPTR_THREAD
                gc_pending.store(true, std::memory_order_release);
#else
                gc_pending = true;
#endif
                throw;
            }
            collect();
            p = alloc_func();
        }
        return p;
    }

    static void run_pending_gc() {
        bool pending = false;
#ifdef GPTR_THREAD
        pending = gc_pending.exchange(false, std::memory_order_acq_rel);
#else
        pending = gc_pending;
        gc_pending = false;
#endif
        if (pending && !gc_in_progress) {
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
            void* obj = base->get_object_ptr_unsafe();
            if (base->get_is_root_unsafe() && obj) {
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

            auto begin_addr = reinterpret_cast<uintptr_t>(cur);
            auto end_addr = begin_addr + it->second.size;

            auto ptr_it = all_ptrs().lower_bound(begin_addr);
            for (; ptr_it != all_ptrs().end(); ++ptr_it) {
                if (ptr_it->first >= end_addr)
                    break;

                GcPtrBase* inner = ptr_it->second;
                void* inner_obj = inner->get_object_ptr_unsafe();
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
#ifdef GPTR_THREAD
        if (gc_in_progress.exchange(true, std::memory_order_acq_rel))
            return;
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());

        try {
            collect_core();
        } catch (...) {
            gc_in_progress.store(false, std::memory_order_release);
            throw;
        }

        gc_in_progress.store(false, std::memory_order_release);
        run_pending_gc();
#else
        if (gc_in_progress)
            return;
        gc_in_progress = true;

        try {
            collect_core();
        } catch (...) {
            gc_in_progress = false;
            throw;
        }

        gc_in_progress = false;
        run_pending_gc();
#endif
    }

    static void set_gc_interval(std::chrono::seconds interval) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        gc_interval = interval;
        time_initialized = false;
    }

    static void set_destruct_threshold(std::size_t threshold) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        destruct_threshold = threshold;
    }

    static void disable_auto_gc() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto_gc_enabled = false;
    }

    static void enable_auto_gc() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        auto_gc_enabled = true;
    }

    static bool is_auto_gc_enabled() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return auto_gc_enabled;
    }

    static std::size_t gc_object_count() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        return gc_objects().size();
    }

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

        struct PendingDeleter {
            deleter_invoke_fn invoke;
            uintptr_t context;
            deleter_invoke_fn destroy;
        };
        std::vector<PendingDeleter> pending_deleters;

        for (auto it = gc_objects().begin(); it != gc_objects().end();) {
            if (!it->second.marked && !it->second.under_construction) {
                pending_deleters.push_back({it->second.invoke_deleter,
                                            it->second.deleter_context,
                                            it->second.destroy_deleter_ctx});
                it = gc_objects().erase(it);
                gc_ranges_dirty() = true;
            } else {
                ++it;
            }
        }

        for (auto& pd : pending_deleters) {
            if (pd.invoke) {
                pd.invoke(pd.context);
            }
            if (pd.destroy) {
                pd.destroy(pd.context);
            }
        }
    }

    void register_this() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        all_ptrs().emplace(reinterpret_cast<uintptr_t>(this), this);
    }
    void unregister_this() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        all_ptrs().erase(reinterpret_cast<uintptr_t>(this));
    }
};

inline std::size_t GcPtrBase::destruct_count = 0;
inline std::size_t GcPtrBase::destruct_threshold = 40;
inline std::chrono::seconds GcPtrBase::gc_interval(120);
inline std::chrono::steady_clock::time_point GcPtrBase::last_gc_time{};
inline bool GcPtrBase::time_initialized = false;
inline bool GcPtrBase::auto_gc_enabled = true;

#ifdef GPTR_THREAD
inline std::atomic<bool> GcPtrBase::gc_in_progress{false};
inline std::atomic<bool> GcPtrBase::gc_pending{false};
#else
inline bool GcPtrBase::gc_in_progress = false;
inline bool GcPtrBase::gc_pending = false;
#endif

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
            gc_objects().emplace(ptr, GcNode{ptr, sizeof(T), false, true,
                                             [](uintptr_t ctx) {
                                                 delete reinterpret_cast<T*>(ctx);
                                             },
                                             reinterpret_cast<uintptr_t>(ptr), nullptr});
            gc_ranges_dirty() = true;
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

        set_object_ptr_unsafe(ptr);
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            auto it = gc_objects().find(ptr);
            if (it != gc_objects().end()) {
                it->second.under_construction = false;
            }
        }
    }

    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter, std::size_t real_size) {
        if (p) {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            if (gc_objects().find(p) != gc_objects().end()) {
                throw std::runtime_error("Pointer already registered with GC");
            }
            register_gc_object(p, real_size,
                               [p, d = std::move(deleter)]() mutable { d(p); },
                               false);
        }
        set_object_ptr_unsafe(p);
    }

    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter)
        : GcPtr(p, deleter, sizeof(T)) {}

    explicit GcPtr(T* p)
        : GcPtr(p, [](T* ptr) { delete ptr; }, sizeof(T)) {}

    GcPtr(const GcPtr& other) : GcPtrBase(other) {
        set_object_ptr(other.get_object_ptr());
    }

    GcPtr(GcPtr&& other) noexcept : GcPtrBase(std::move(other)) {
        void* ptr = other.get_object_ptr();
        set_object_ptr(ptr);
        other.set_object_ptr(nullptr);
    }

    GcPtr& operator=(const GcPtr& other) {
        if (this != &other) {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            void* old_ptr = get_object_ptr_unsafe();
            void* new_ptr = other.get_object_ptr_unsafe();
            set_object_ptr_unsafe(new_ptr);
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
            void* old_ptr = get_object_ptr_unsafe();
            void* new_ptr = other.get_object_ptr_unsafe();
            set_object_ptr_unsafe(new_ptr);
            other.set_object_ptr_unsafe(nullptr);
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
        void* old_ptr = get_object_ptr_unsafe();
        set_object_ptr_unsafe(nullptr);
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
        void* old_ptr = get_object_ptr_unsafe();

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

        set_object_ptr_unsafe(new_ptr);
        destroy_object_if_last(old_ptr);
    }

    T* release() {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        T* old_ptr = static_cast<T*>(get_object_ptr_unsafe());
        if (old_ptr) {
            set_object_ptr_unsafe(nullptr);
            unregister_gc_object(old_ptr);
        }
        return old_ptr;
    }

    void swap(GcPtr& other) noexcept {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        void* tmp = get_object_ptr_unsafe();
        set_object_ptr_unsafe(other.get_object_ptr_unsafe());
        other.set_object_ptr_unsafe(tmp);
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

    T* get() { return static_cast<T*>(get_object_ptr()); }
    const T* get() const {
        return static_cast<const T*>(get_object_ptr());
    }

    explicit operator bool() const {
        return get_object_ptr() != nullptr;
    }

    void gc() { GcPtrBase::collect(); }

    static void set_gc_interval(std::chrono::seconds t) {
        GcPtrBase::set_gc_interval(t);
    }
    static void set_destruct_threshold(std::size_t n) {
        GcPtrBase::set_destruct_threshold(n);
    }
    static void disable_auto_gc() { GcPtrBase::disable_auto_gc(); }
    static void enable_auto_gc() { GcPtrBase::enable_auto_gc(); }
    static bool is_auto_gc_enabled() { return GcPtrBase::is_auto_gc_enabled(); }
    static std::size_t gc_object_count() {
        return GcPtrBase::gc_object_count();
    }

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
        gc_objects().erase(it);
        gc_ranges_dirty() = true;
        if (invoke) {
            invoke(ctx);
        }
        if (destroy) {
            destroy(ctx);
        }
    }
};

template <typename T, typename... Args>
GcPtr<T> make_gc(Args&&... args) {
    return GcPtr<T>(std::in_place, std::forward<Args>(args)...);
}

template <typename T, typename Deleter>
GcPtr<T> make_gc_with_deleter(T* ptr, Deleter deleter) {
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
        std::lock_guard<std::recursive_mutex> lock(GcPtrBase::gc_mutex());
        return ptr;
    }

    operator value_type() const noexcept {
        return load();
    }

    void store(value_type desired,
               std::memory_order order = std::memory_order_seq_cst) noexcept {
        (void)order;
        std::lock_guard<std::recursive_mutex> lock(GcPtrBase::gc_mutex());
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
        std::lock_guard<std::recursive_mutex> lock(GcPtrBase::gc_mutex());
        value_type old = ptr;
        ptr = std::move(desired);
        return old;
    }

    bool compare_exchange_weak(
        value_type& expected, value_type desired,
        std::memory_order success = std::memory_order_seq_cst,
        std::memory_order failure = std::memory_order_seq_cst) noexcept {
        (void)success;
        (void)failure;
        std::lock_guard<std::recursive_mutex> lock(GcPtrBase::gc_mutex());
        if (ptr == expected) {
            ptr = std::move(desired);
            return true;
        }
        expected = ptr;
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