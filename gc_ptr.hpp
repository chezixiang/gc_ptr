#ifndef GC_PTR_HPP
#define GC_PTR_HPP

// GC scanning limitation:
// This GC is a precise, conservative-style collector that scans only the
// contiguous memory region of each managed object (real_size bytes starting
// from the object's address). It discovers GcPtr members by their addresses
// falling within that memory range.
//
// IMPORTANT SAFETY RULES:
// 1. Do NOT store GcPtr inside any dynamically allocated container/object
//    that is NOT itself managed by the GC. This includes:
//      - std::vector, std::map, std::unordered_map, std::string, etc.
//      - Objects allocated with 'new' but NOT via make_gc/make_gc_with_deleter
//      - Global or static objects
//    The only safe places are:
//      - Stack variables (they become GC roots automatically)
//      - Inside other GC-managed objects (i.e., struct/class allocated with make_gc)
//      - As direct member of a GC-managed object's memory layout
// 2. In multithreaded mode (-DGPTR_THREAD), the pointer returned by get() or
//    operator-> is valid only as long as you don't trigger a GC. Avoid holding
//    such raw pointers across GC collection points (e.g., function calls that
//    might allocate new objects, or explicit gc() calls).
//
// Safe patterns:
//   struct Node { GcPtr<Node> next; };                    // OK
//   auto node = make_gc<Node>();                          // OK
//   GcPtr<int> on_stack = make_gc<int>(42);               // OK (root)
//   struct Bad { std::vector<GcPtr<int>> items; };        // WRONG
//   auto* bad = new Bad;                                   // WRONG (Bad not GC-managed)
//
// Polymorphic objects:
//   If a GcPtr<Base> actually points to a Derived object, you must pass the
//   correct real size (sizeof(Derived)) when registering, otherwise the GC
//   will not scan the Derived parts. Use the overloads that accept a real_size
//   argument, e.g.:
//     GcPtr<Base> ptr(derived_raw_ptr, [](Base* p){ delete static_cast<Derived*>(p); },
//                      sizeof(Derived));

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    // All access to object_ptr is now protected by the global gc_mutex.
    // The threadsafe version uses a single recursive mutex to avoid deadlocks.
    // If you need to read object_ptr while already holding gc_mutex, use the _unsafe versions.
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

    struct GcNode {
        void* address;
        std::size_t size;        // actual size of the managed object
        bool marked = false;
        std::function<void()> deleter;
        bool under_construction = true;
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

    static void register_gc_object(void* p, std::size_t sz,
                                std::function<void()> d) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        gc_objects().emplace(p, GcNode{p, sz, false, std::move(d), true});
        gc_ranges_dirty() = true;
    }

    static void unregister_gc_object(void* p) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        gc_objects().erase(p);
        gc_ranges_dirty() = true;
    }

    static int count_references(void* obj) {
        int count = 0;
        // This function is always called while holding gc_mutex,
        // so we can use the unsafe versions to avoid extra locking.
        for (const auto& [_, base] : all_ptrs()) {
            if (base->get_object_ptr_unsafe() == obj)
                ++count;
        }
        return count;
    }

    static void rebuild_ranges() {
        gc_ranges().clear();
        for (const auto& [addr, node] : gc_objects()) {
            auto begin = reinterpret_cast<uintptr_t>(addr);
            gc_ranges().emplace_back(begin, begin + node.size);
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

#ifdef GPTR_THREAD
    static std::atomic<bool> gc_in_progress;
#else
    static bool gc_in_progress;
#endif

    static void check_auto_gc() {
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

    template<typename Func>
    static void* allocate_with_retry(Func alloc_func) {
        void* p = nullptr;
        try {
            p = alloc_func();
        } catch (const std::bad_alloc&) {
            collect();
            p = alloc_func();
        }
        return p;
    }

    static void reset_all_marks() {
        for (auto& [_, node] : gc_objects())
            node.marked = false;
    }

    static std::vector<void*> collect_root_objects() {
        std::vector<void*> queue;
        for (const auto& [_, base] : all_ptrs()) {
            void* obj = base->get_object_ptr_unsafe();
            if (base->is_root && obj) {
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
            if (it == gc_objects().end()) continue;

            auto begin_addr = reinterpret_cast<uintptr_t>(cur);
            auto end_addr = begin_addr + it->second.size;

            auto ptr_it = all_ptrs().lower_bound(begin_addr);
            for (; ptr_it != all_ptrs().end(); ++ptr_it) {
                if (ptr_it->first >= end_addr) break;

                GcPtrBase* inner = ptr_it->second;
                void* inner_obj = inner->get_object_ptr_unsafe();
                if (!inner_obj) continue;

                auto inner_it = gc_objects().find(inner_obj);
                if (inner_it != gc_objects().end() && !inner_it->second.marked) {
                    inner_it->second.marked = true;
                    queue.push_back(inner_obj);
                }
            }
        }
    }

    static std::vector<void*> collect_unmarked_objects() {
        std::vector<void*> to_delete;
        for (const auto& [addr, node] : gc_objects())
            if (!node.marked && !node.under_construction)
                to_delete.push_back(addr);
        return to_delete;
    }

public:
    static void collect() {
#ifdef GPTR_THREAD
        // Prevent multiple concurrent collections
        if (gc_in_progress.exchange(true, std::memory_order_acq_rel))
            return;
        // Single recursive lock protects all GC metadata and object_ptr values.
        // Using a recursive mutex allows deleters (called below) to safely
        // perform operations that also acquire gc_mutex, e.g., unregister_this().
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());

        try {
            std::vector<std::function<void()>> deleters = collect_core();
            // Call all deleters while still holding the lock. Recursive mutex
            // guarantees no deadlock if a deleter triggers further GC activity.
            for (auto& deleter : deleters) {
                deleter();
            }
        } catch (...) {
            gc_in_progress.store(false, std::memory_order_release);
            throw;
        }

        gc_in_progress.store(false, std::memory_order_release);
#else
        if (gc_in_progress)
            return;
        gc_in_progress = true;

        try {
            std::vector<std::function<void()>> deleters = collect_core();
            for (auto& deleter : deleters) {
                deleter();
            }
        } catch (...) {
            gc_in_progress = false;
            throw;
        }

        gc_in_progress = false;
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

private:
    static std::vector<std::function<void()>> collect_core() {
        reset_all_marks();

        // 1. Mark all objects that are still under construction as alive,
        //    and scan their internal GcPtr members so that objects referenced
        //    inside constructors are not prematurely collected.
        std::vector<void*> queue;
        for (auto& [addr, node] : gc_objects()) {
            if (node.under_construction && !node.marked) {
                node.marked = true;
                queue.push_back(addr);
            }
        }

        // 2. Collect root objects (stack-based GcPtrs with is_root == true)
        std::vector<void*> roots = collect_root_objects();
        queue.insert(queue.end(), roots.begin(), roots.end());

        // 3. Mark all objects reachable from the initial queue
        mark_reachable_objects(queue);

        // 4. Collect unmarked (and not under construction) objects
        std::vector<void*> to_delete = collect_unmarked_objects();

        // 5. Extract deleters and remove objects from the registry
        std::vector<std::function<void()>> deleters;
        for (void* addr : to_delete) {
            auto it = gc_objects().find(addr);
            if (it != gc_objects().end()) {
                deleters.push_back(std::move(it->second.deleter));
                gc_objects().erase(it);
                gc_ranges_dirty() = true;
            }
        }
        return deleters;
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

// Static member definitions
inline std::size_t GcPtrBase::destruct_count = 0;
inline std::size_t GcPtrBase::destruct_threshold = 40;
inline std::chrono::seconds GcPtrBase::gc_interval(120);
inline std::chrono::steady_clock::time_point GcPtrBase::last_gc_time{};
inline bool GcPtrBase::time_initialized = false;

#ifdef GPTR_THREAD
inline std::atomic<bool> GcPtrBase::gc_in_progress{false};
#else
inline bool GcPtrBase::gc_in_progress = false;
#endif

template <typename T>
class GcPtr : private GcPtrBase {
public:
    GcPtr() = default;

    // Construct in-place using arguments (determines size via sizeof(T))
    template <typename... Args>
    explicit GcPtr(std::in_place_t, Args&&... args) {
        void* raw = allocate_with_retry([] { return operator new(sizeof(T)); });
        T* ptr = static_cast<T*>(raw);

        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            gc_objects().emplace(ptr, GcNode{ptr, sizeof(T), false, [ptr] { delete ptr; }, true});
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

    // Constructor taking a raw pointer, a deleter, and the real object size.
    // This overload supports polymorphic pointers (e.g., GcPtr<Base> pointing to Derived).
    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter, std::size_t real_size) {
        if (p) {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            if (gc_objects().find(p) != gc_objects().end()) {
                throw std::runtime_error("Pointer already registered with GC");
            }
            gc_objects().emplace(p, GcNode{p, real_size, false,
                                           [p, deleter]() mutable { deleter(p); }, false});
            gc_ranges_dirty() = true;
        }
        set_object_ptr_unsafe(p);
    }

    // Constructor with custom deleter, size defaults to sizeof(T).
    template <typename Deleter>
    explicit GcPtr(T* p, Deleter deleter)
        : GcPtr(p, deleter, sizeof(T)) {}

    // Constructor with default deleter, size defaults to sizeof(T).
    explicit GcPtr(T* p) : GcPtr(p, [](T* ptr) { delete ptr; }, sizeof(T)) {}

    // Copy / move
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
            set_object_ptr(other.get_object_ptr());
        }
        return *this;
    }

    GcPtr& operator=(GcPtr&& other) noexcept {
        if (this != &other) {
            void* ptr = other.get_object_ptr();
            set_object_ptr(ptr);
            other.set_object_ptr(nullptr);
        }
        return *this;
    }

    ~GcPtr() = default;

    // Reset to empty (deletes managed object if no other references exist,
    // using the registered deleter).
    void reset() {
        void* old_ptr_raw = nullptr;
        {
#ifdef GPTR_THREAD
            std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
            old_ptr_raw = get_object_ptr_unsafe();
            set_object_ptr_unsafe(nullptr);

            if (old_ptr_raw) {
                if (count_references(old_ptr_raw) <= 0) { // the current ptr is already cleared
                    auto it = gc_objects().find(old_ptr_raw);
                    if (it != gc_objects().end()) {
                        // Capture the deleter before erasing
                        auto deleter = std::move(it->second.deleter);
                        gc_objects().erase(it);
                        gc_ranges_dirty() = true;
                        // Call deleter while still holding the lock (recursive mutex allows re-entrancy)
                        if (deleter) deleter();
                    }
                }
            }
        }
    }

    // Reset to a new raw pointer (takes ownership, with default deleter and size = sizeof(T))
    void reset(T* new_ptr) {
        reset_with_deleter(new_ptr, [](T* p) { delete p; }, sizeof(T));
    }

    // Reset to a new raw pointer with custom deleter and real size.
    template <typename Deleter>
    void reset_with_deleter(T* new_ptr, Deleter deleter, std::size_t real_size) {
#ifdef GPTR_THREAD
        std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
        void* old_ptr_raw = get_object_ptr_unsafe();

        if (static_cast<T*>(old_ptr_raw) == new_ptr)
            return;

        // Set new pointer first
        set_object_ptr_unsafe(new_ptr);

        if (new_ptr) {
            auto [it, inserted] = gc_objects().emplace(new_ptr,
                GcNode{new_ptr, real_size, false,
                       [new_ptr, deleter]() mutable { deleter(new_ptr); }, false});
            if (!inserted) {
                throw std::runtime_error("Pointer already registered with GC");
            }
            gc_ranges_dirty() = true;
        }

        // Clean up old object if no other references remain
        if (old_ptr_raw) {
            if (count_references(old_ptr_raw) <= 0) { // the current ptr was cleared
                auto it = gc_objects().find(old_ptr_raw);
                if (it != gc_objects().end()) {
                    auto old_deleter = std::move(it->second.deleter);
                    gc_objects().erase(it);
                    gc_ranges_dirty() = true;
                    // Call deleter (safe because we hold gc_mutex, recursive)
                    if (old_deleter) old_deleter();
                }
            }
        }
    }

    // Release ownership and return the raw pointer (removes from GC).
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
        void* ptr = get_object_ptr();   // acquires lock if threaded
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
    const T* get() const { return static_cast<const T*>(get_object_ptr()); }

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

    friend bool operator==(const GcPtr& a, const GcPtr& b) {
        return a.get_object_ptr() == b.get_object_ptr();
    }
    friend bool operator!=(const GcPtr& a, const GcPtr& b) {
        return a.get_object_ptr() != b.get_object_ptr();
    }
};

// Factory: in-place construction (size = sizeof(T))
template <typename T, typename... Args>
GcPtr<T> make_gc(Args&&... args) {
    return GcPtr<T>(std::in_place, std::forward<Args>(args)...);
}

// Factory: wrap raw pointer with custom deleter (size = sizeof(T))
template <typename T, typename Deleter>
GcPtr<T> make_gc_with_deleter(T* ptr, Deleter deleter) {
    return GcPtr<T>(ptr, deleter, sizeof(T));
}

// Factory: wrap raw pointer with custom deleter and explicit real object size.
// Use this when the pointed-to object is larger than sizeof(T), e.g., a base
// class pointer to a derived object.
template <typename T, typename Deleter>
GcPtr<T> make_gc_with_deleter(T* ptr, Deleter deleter, std::size_t real_size) {
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
}

#endif // GC_PTR_HPP