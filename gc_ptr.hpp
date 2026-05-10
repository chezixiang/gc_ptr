#ifndef GC_PTR_HPP
#define GC_PTR_HPP

// GC scanning limitation:
// This GC is a precise, conservative-style collector that scans only the
// contiguous memory region of each managed object (sizeof(T) bytes starting
// from the object's address). It discovers GcPtr members by their addresses
// falling within that memory range.
//
// IMPORTANT: Do NOT store GcPtr inside dynamically allocated containers
// such as std::vector, std::map, std::unordered_map, std::string, etc.
// These containers allocate their elements on the heap, outside the scanned
// sizeof(T) region, so GcPtr stored there will NOT be found by the GC,
// causing reachable objects to be incorrectly collected.
//
// Safe patterns:
//   struct Node { GcPtr<Node> next; };                    // OK - direct member
//   struct Tree { GcPtr<Tree> left; GcPtr<Tree> right; };  // OK - direct members
//   struct Bad { std::vector<GcPtr<int>> items; };         // WRONG - heap storage

#include <algorithm>
#include <cassert>
#include <chrono>
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
#include <shared_mutex>
#endif

class GcPtrBase {
protected:
	void* object_ptr = nullptr;
	bool is_root = false;

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

	~GcPtrBase() {
		auto_collect_on_destruct();
		unregister_this();
	}

	struct GcNode {
		void* address;
		std::size_t size;
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
	static std::shared_mutex& gc_access_mutex() {
		static std::shared_mutex mtx;
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
		for (const auto& [_, base] : all_ptrs()) {
			if (base->object_ptr == obj)
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
			if (base->is_root && base->object_ptr) {
				auto it = gc_objects().find(base->object_ptr);
				if (it != gc_objects().end() && !it->second.marked) {
					it->second.marked = true;
					queue.push_back(base->object_ptr);
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
				if (!inner->object_ptr) continue;

				auto inner_it = gc_objects().find(inner->object_ptr);
				if (inner_it != gc_objects().end() && !inner_it->second.marked) {
					inner_it->second.marked = true;
					queue.push_back(inner->object_ptr);
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
		if (gc_in_progress.exchange(true, std::memory_order_acq_rel))
			return;
		std::unique_lock<std::shared_mutex> access_lock(gc_access_mutex());

		try {
			std::vector<std::function<void()>> deleters;
			{
				std::lock_guard<std::recursive_mutex> lock(gc_mutex());
				reset_all_marks();
				std::vector<void*> queue = collect_root_objects();
				mark_reachable_objects(queue);
				std::vector<void*> to_delete = collect_unmarked_objects();

				for (void* addr : to_delete) {
					auto it = gc_objects().find(addr);
					if (it != gc_objects().end()) {
						deleters.push_back(std::move(it->second.deleter));
						gc_objects().erase(it);
						gc_ranges_dirty() = true;
					}
				}
			}

			access_lock.unlock();

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
			std::vector<std::function<void()>> deleters;
			{
				reset_all_marks();
				std::vector<void*> queue = collect_root_objects();
				mark_reachable_objects(queue);
				std::vector<void*> to_delete = collect_unmarked_objects();

				for (void* addr : to_delete) {
					auto it = gc_objects().find(addr);
					if (it != gc_objects().end()) {
						deleters.push_back(std::move(it->second.deleter));
						gc_objects().erase(it);
						gc_ranges_dirty() = true;
					}
				}
			}

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

#ifdef GPTR_THREAD
inline std::atomic<bool> GcPtrBase::gc_in_progress{false};
#else
inline bool GcPtrBase::gc_in_progress = false;
#endif

template <typename T>
class GcPtr : private GcPtrBase {
public:
	GcPtr() = default;

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

		object_ptr = ptr;
		{
#ifdef GPTR_THREAD
			std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
			auto it = gc_objects().find(ptr);
			if (it != gc_objects().end()) {
				it->second.under_construction = false;
			}
		}
		is_root = true;
	}

	explicit GcPtr(T* p) {
		if (p) {
#ifdef GPTR_THREAD
			std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
			if (gc_objects().find(p) != gc_objects().end()) {
				throw std::runtime_error("Pointer already registered with GC");
			}
			gc_objects().emplace(p, GcNode{p, sizeof(T), false, [p] { delete p; }, false});
			gc_ranges_dirty() = true;
		}
		object_ptr = p;
		is_root = true;
	}

	GcPtr(const GcPtr& other) : GcPtrBase(other) {
		object_ptr = other.object_ptr;
	}

	GcPtr(GcPtr&& other) noexcept : GcPtrBase(std::move(other)) {
		object_ptr = other.object_ptr;
		other.object_ptr = nullptr;
	}

	GcPtr& operator=(const GcPtr& other) {
		if (this != &other) {
			object_ptr = other.object_ptr;
		}
		return *this;
	}

	GcPtr& operator=(GcPtr&& other) noexcept {
		if (this != &other) {
			object_ptr = other.object_ptr;
			other.object_ptr = nullptr;
		}
		return *this;
	}

	~GcPtr() = default;

	void reset() {
		T* old_ptr = static_cast<T*>(object_ptr);
		if (old_ptr) {
			bool should_delete = false;
			{
#ifdef GPTR_THREAD
				std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
				if (count_references(old_ptr) <= 1) {
					gc_objects().erase(old_ptr);
					gc_ranges_dirty() = true;
					should_delete = true;
				}
			}
			if (should_delete) {
#ifdef GPTR_THREAD
				std::shared_lock<std::shared_mutex> access_lock(gc_access_mutex());
#endif
				delete old_ptr;
			}
			object_ptr = nullptr;
		}
	}

	void reset(T* new_ptr) {
		if (object_ptr == new_ptr) {
			return;
		}

		T* old_ptr = static_cast<T*>(object_ptr);

		if (new_ptr) {
#ifdef GPTR_THREAD
			std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
			auto [it, inserted] = gc_objects().emplace(new_ptr, GcNode{new_ptr, sizeof(T), false, [new_ptr] { delete new_ptr; }, false});
			if (!inserted) {
				throw std::runtime_error("Pointer already registered with GC");
			}
			gc_ranges_dirty() = true;
		}

		object_ptr = new_ptr;

		if (old_ptr) {
			bool should_delete = false;
			{
#ifdef GPTR_THREAD
				std::lock_guard<std::recursive_mutex> lock(gc_mutex());
#endif
				if (count_references(old_ptr) <= 1) {
					gc_objects().erase(old_ptr);
					gc_ranges_dirty() = true;
					should_delete = true;
				}
			}
			if (should_delete) {
#ifdef GPTR_THREAD
				std::shared_lock<std::shared_mutex> access_lock(gc_access_mutex());
#endif
				delete old_ptr;
			}
		}
	}

	// WARNING: release() transfers ownership out of GC management.
	// The caller becomes responsible for manually deleting the returned pointer.
	// After release(), the object is no longer tracked by GC and will not be
	// collected automatically. Any other GcPtr pointing to the same object
	// will become dangling. Use with extreme caution.
	T* release() {
		T* old_ptr = static_cast<T*>(object_ptr);
		if (old_ptr) {
			unregister_gc_object(old_ptr);
			object_ptr = nullptr;
		}
		return old_ptr;
	}

	void swap(GcPtr& other) noexcept {
		using std::swap;
		swap(object_ptr, other.object_ptr);
	}

	T& operator*() { 
		assert(object_ptr && "dereferencing null GcPtr");
#ifdef GPTR_THREAD
		std::shared_lock<std::shared_mutex> lock(gc_access_mutex());
#endif
		return *static_cast<T*>(object_ptr); 
	}
	const T& operator*() const { 
		assert(object_ptr && "dereferencing null GcPtr");
#ifdef GPTR_THREAD
		std::shared_lock<std::shared_mutex> lock(gc_access_mutex());
#endif
		return *static_cast<const T*>(object_ptr); 
	}

	T* operator->() { 
		assert(object_ptr && "dereferencing null GcPtr");
#ifdef GPTR_THREAD
		std::shared_lock<std::shared_mutex> lock(gc_access_mutex());
#endif
		return static_cast<T*>(object_ptr); 
	}
	const T* operator->() const { 
		assert(object_ptr && "dereferencing null GcPtr");
#ifdef GPTR_THREAD
		std::shared_lock<std::shared_mutex> lock(gc_access_mutex());
#endif
		return static_cast<const T*>(object_ptr); 
	}

	T* get() { 
#ifdef GPTR_THREAD
		std::shared_lock<std::shared_mutex> lock(gc_access_mutex());
#endif
		return static_cast<T*>(object_ptr); 
	}
	const T* get() const { 
#ifdef GPTR_THREAD
		std::shared_lock<std::shared_mutex> lock(gc_access_mutex());
#endif
		return static_cast<const T*>(object_ptr); 
	}

	explicit operator bool() const { return object_ptr != nullptr; }

	void gc() { GcPtrBase::collect(); }

	static void set_gc_interval(std::chrono::seconds t) {
		GcPtrBase::set_gc_interval(t);
	}
	static void set_destruct_threshold(std::size_t n) {
		GcPtrBase::set_destruct_threshold(n);
	}

	friend bool operator==(const GcPtr& a, const GcPtr& b) {
		return a.object_ptr == b.object_ptr;
	}
	friend bool operator!=(const GcPtr& a, const GcPtr& b) {
		return a.object_ptr != b.object_ptr;
	}
};

template <typename T, typename... Args>
GcPtr<T> make_gc(Args&&... args) {
	return GcPtr<T>(std::in_place, std::forward<Args>(args)...);
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

#endif