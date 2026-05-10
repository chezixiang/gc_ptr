#ifndef GC_PTR_HPP
#define GC_PTR_HPP

#include <chrono>
#include <functional>
#include <map>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

class GcPtrBase {
protected:
	void* object_ptr = nullptr;
	bool is_root = false;

	GcPtrBase() {
		is_root = !is_within_any_gc_object(this);
		register_this();
		check_auto_gc();
	}

	GcPtrBase(const GcPtrBase&) {
		is_root = !is_within_any_gc_object(this);
		register_this();
		check_auto_gc();
	}

	GcPtrBase(GcPtrBase&&) noexcept {
		is_root = !is_within_any_gc_object(this);
		register_this();
		check_auto_gc();
	}

	GcPtrBase& operator=(const GcPtrBase&) = default;
	GcPtrBase& operator=(GcPtrBase&&) = default;

	~GcPtrBase() {
		unregister_this();
		auto_collect_on_destruct();
	}

	struct GcNode {
		void* address;
		std::size_t size;
		bool marked = false;
		std::function<void()> deleter;
	};

	static std::map<void*, GcNode>& gc_objects() {
		static std::map<void*, GcNode> objects;
		return objects;
	}

	static std::map<void*, GcPtrBase*>& all_ptrs() {
		static std::map<void*, GcPtrBase*> pointers;
		return pointers;
	}

	static void register_gc_object(void* p, std::size_t sz,
								   std::function<void()> d) {
		gc_objects().emplace(p, GcNode{p, sz, false, std::move(d)});
	}

	static void unregister_gc_object(void* p) {
		gc_objects().erase(p);
	}

	static bool is_within_any_gc_object(const void* addr) {
		for (const auto& [obj_addr, node] : gc_objects()) {
			const char* begin = static_cast<const char*>(obj_addr);
			const char* end = begin + node.size;
			const char* caddr = static_cast<const char*>(addr);
			if (caddr >= begin && caddr < end)
				return true;
		}
		return false;
	}

	static std::size_t destruct_count;
	static std::size_t destruct_threshold;
	static std::chrono::seconds gc_interval;
	static std::chrono::steady_clock::time_point last_gc_time;
	static bool time_initialized;

	static bool gc_in_progress;

	static void check_auto_gc() {
		if (!time_initialized) {
			last_gc_time = std::chrono::steady_clock::now();
			time_initialized = true;
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now - last_gc_time >= gc_interval) {
			collect();
			last_gc_time = now;
		}
	}

	static void auto_collect_on_destruct() {
		++destruct_count;
		if (destruct_count >= destruct_threshold) {
			collect();
			destruct_count = 0;
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

			const char* begin = static_cast<const char*>(cur);
			const char* end = begin + it->second.size;

			auto ptr_it = all_ptrs().lower_bound(const_cast<char*>(begin));
			for (; ptr_it != all_ptrs().end(); ++ptr_it) {
				const char* ptr_addr = static_cast<const char*>(ptr_it->first);
				if (ptr_addr >= end) break;

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
			if (!node.marked)
				to_delete.push_back(addr);
		return to_delete;
	}

	static void delete_unmarked_objects(const std::vector<void*>& to_delete) {
		for (void* addr : to_delete) {
			auto it = gc_objects().find(addr);
			if (it != gc_objects().end()) {
				it->second.deleter();
				gc_objects().erase(it);
			}
		}
	}

public:
	static void collect() {
		if (gc_in_progress) return;
		gc_in_progress = true;

		try {
			reset_all_marks();
			std::vector<void*> queue = collect_root_objects();
			mark_reachable_objects(queue);
			std::vector<void*> to_delete = collect_unmarked_objects();
			delete_unmarked_objects(to_delete);
		} catch (...) {
			gc_in_progress = false;
			throw;
		}
		gc_in_progress = false;
	}

	static void set_gc_interval(std::chrono::seconds interval) {
		gc_interval = interval;
		time_initialized = false;
	}

	static void set_destruct_threshold(std::size_t threshold) {
		destruct_threshold = threshold;
	}

private:
	void register_this() {
		all_ptrs().emplace(static_cast<void*>(this), this);
	}
	void unregister_this() {
		all_ptrs().erase(static_cast<void*>(this));
	}
};

std::size_t GcPtrBase::destruct_count = 0;
std::size_t GcPtrBase::destruct_threshold = 40;
std::chrono::seconds GcPtrBase::gc_interval(120);
std::chrono::steady_clock::time_point GcPtrBase::last_gc_time{};
bool GcPtrBase::time_initialized = false;
bool GcPtrBase::gc_in_progress = false;

template <typename T>
class GcPtr : private GcPtrBase {
public:
	GcPtr() = default;

	template <typename... Args>
	GcPtr(std::in_place_t, Args&&... args) {
		void* raw = allocate_with_retry([] { return operator new(sizeof(T)); });
		T* ptr = static_cast<T*>(raw);

		register_gc_object(ptr, sizeof(T), [ptr] { delete ptr; });

		try {
			new (ptr) T(std::forward<Args>(args)...);
		} catch (...) {
			unregister_gc_object(ptr);
			operator delete(ptr);
			throw;
		}

		object_ptr = ptr;
		is_root = true;
	}

	explicit GcPtr(const T* p) {
		auto* mp = const_cast<T*>(p);
		if (mp) {
			register_gc_object(mp, sizeof(T), [mp] { delete mp; });
		}
		object_ptr = mp;
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

	T* reset() {
		T* p = static_cast<T*>(object_ptr);
		if (p) {
			unregister_gc_object(p);
			object_ptr = nullptr;
		}
		return p;
	}

	T* reset(T* new_ptr) {
		T* old = reset();
		if (new_ptr) {
			register_gc_object(new_ptr, sizeof(T), [new_ptr] { delete new_ptr; });
			object_ptr = new_ptr;
		}
		return old;
	}

	void swap(GcPtr& other) noexcept {
		using std::swap;
		swap(object_ptr, other.object_ptr);
	}

	T& operator*() { return *static_cast<T*>(object_ptr); }
	const T& operator*() const { return *static_cast<T*>(object_ptr); }

	T* operator->() { return static_cast<T*>(object_ptr); }
	const T* operator->() const { return static_cast<T*>(object_ptr); }

	T* get() { return static_cast<T*>(object_ptr); }
	const T* get() const { return static_cast<T*>(object_ptr); }

	explicit operator bool() const { return object_ptr != nullptr; }

	void gc() { GcPtrBase::collect(); }

	void release() {
		if (object_ptr) {
			T* p = static_cast<T*>(object_ptr);
			unregister_gc_object(p);
			delete p;
			object_ptr = nullptr;
		}
	}

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
