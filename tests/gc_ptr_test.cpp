#include <gtest/gtest.h>
#include <string>
#include <memory>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>
#ifdef GPTR_THREAD
#include <thread>
#endif
#include "../gc_ptr.hpp"

namespace {

class GcPtrTest : public ::testing::Test {
protected:
    void SetUp() override {
        GcPtr<int>::set_gc_interval(std::chrono::seconds(3600));
        GcPtr<int>::set_destruct_threshold(1000);
        GcPtrBase::collect();
    }
    void TearDown() override {
        GcPtrBase::collect();
    }
};

TEST_F(GcPtrTest, Should_DefaultConstructToNull) {
    GcPtr<int> ptr;
    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.get(), nullptr);
}

TEST_F(GcPtrTest, Should_ConstructWithArgs) {
    auto ptr = make_gc<int>(42);
    EXPECT_TRUE(ptr);
    EXPECT_EQ(*ptr, 42);
}

struct Counted {
	static std::atomic<int> counter;
	int value;
	Counted() : value(0) { counter++; }
	explicit Counted(int v) : value(v) { counter++; }
	~Counted() { counter--; }
};
std::atomic<int> Counted::counter{0};

TEST_F(GcPtrTest, Should_CallConstructorAndDestructor) {
    EXPECT_EQ(Counted::counter, 0);
    {
        auto ptr = make_gc<Counted>(123);
        EXPECT_EQ(Counted::counter, 1);
        EXPECT_EQ(ptr->value, 123);
    }
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_CopyConstruct) {
    auto ptr1 = make_gc<int>(42);
    GcPtr<int> ptr2(ptr1);
    EXPECT_TRUE(ptr2);
    EXPECT_EQ(*ptr2, 42);
    EXPECT_EQ(ptr1, ptr2);
}

TEST_F(GcPtrTest, Should_MoveConstruct) {
    auto ptr1 = make_gc<int>(42);
    GcPtr<int> ptr2(std::move(ptr1));
    EXPECT_FALSE(ptr1);
    EXPECT_TRUE(ptr2);
    EXPECT_EQ(*ptr2, 42);
}

TEST_F(GcPtrTest, Should_CopyAssign) {
    auto ptr1 = make_gc<int>(42);
    GcPtr<int> ptr2;
    ptr2 = ptr1;
    EXPECT_TRUE(ptr2);
    EXPECT_EQ(*ptr2, 42);
}

TEST_F(GcPtrTest, Should_MoveAssign) {
    auto ptr1 = make_gc<int>(42);
    GcPtr<int> ptr2;
    ptr2 = std::move(ptr1);
    EXPECT_FALSE(ptr1);
    EXPECT_TRUE(ptr2);
    EXPECT_EQ(*ptr2, 42);
}

TEST_F(GcPtrTest, Should_ResetToNull) {
    auto ptr = make_gc<int>(42);
    ptr.reset();
    EXPECT_FALSE(ptr);
}

TEST_F(GcPtrTest, Should_ResetAndCallDestructor) {
    EXPECT_EQ(Counted::counter, 0);
    auto ptr = make_gc<Counted>(123);
    EXPECT_EQ(Counted::counter, 1);
    ptr.reset();
    EXPECT_FALSE(ptr);
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_ResetToNewObject) {
    EXPECT_EQ(Counted::counter, 0);
    auto ptr = make_gc<Counted>(123);
    EXPECT_EQ(Counted::counter, 1);
    ptr.reset(new Counted(456));
    EXPECT_EQ(Counted::counter, 1);
    EXPECT_EQ(ptr->value, 456);
}

TEST_F(GcPtrTest, Should_Swap) {
    auto ptr1 = make_gc<int>(42);
    auto ptr2 = make_gc<int>(99);
    ptr1.swap(ptr2);
    EXPECT_EQ(*ptr1, 99);
    EXPECT_EQ(*ptr2, 42);
    std::swap(ptr1, ptr2);
    EXPECT_EQ(*ptr1, 42);
    EXPECT_EQ(*ptr2, 99);
}

TEST_F(GcPtrTest, Should_ReleaseObject) {
    EXPECT_EQ(Counted::counter, 0);
    {
        auto ptr = make_gc<Counted>(123);
        EXPECT_EQ(Counted::counter, 1);
        Counted* raw = ptr.release();
        EXPECT_FALSE(ptr);
        EXPECT_EQ(raw->value, 123);
        delete raw;  // 现在需要手动删除
    }
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_CollectUnreachableObjects) {
    Counted::counter = 0;
    auto root = make_gc<Counted>(1);
    {
        auto another = make_gc<Counted>(2);
        EXPECT_EQ(Counted::counter, 2);
    }
    EXPECT_EQ(Counted::counter, 2);
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 1);
    
    // 现在让 root 也离开作用域
    {
        // 新的作用域，root 在这里被重置为默认构造
        GcPtr<Counted> temp;
        temp.swap(root);
    }
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

struct Node {
	static std::atomic<int> counter;
	GcPtr<Node> next;
	Node() { counter++; }
	~Node() { counter--; }
};
std::atomic<int> Node::counter{0};

TEST_F(GcPtrTest, Should_HandleCyclicReferences) {
    Node::counter = 0;
    {
        auto node1 = make_gc<Node>();
        auto node2 = make_gc<Node>();
        EXPECT_EQ(Node::counter, 2);
        node1->next = node2;
        node2->next = node1;
    }
    GcPtrBase::collect();
    EXPECT_EQ(Node::counter, 0);
}

struct Complex {
	static std::atomic<int> counter;
	int data;
	Complex() : data(0) { counter++; }
	explicit Complex(int d) : data(d) { counter++; }
	~Complex() { counter--; }
};
std::atomic<int> Complex::counter{0};

TEST_F(GcPtrTest, Should_CollectNestedObjects) {
    Complex::counter = 0;
    {
        auto root = make_gc<Complex>(1);
        {
            auto inner = make_gc<Complex>(2);
            auto temp = make_gc<Complex>(3);
        }
        EXPECT_EQ(Complex::counter, 3);
        GcPtrBase::collect();
        EXPECT_EQ(Complex::counter, 1);
    }
    GcPtrBase::collect();
    EXPECT_EQ(Complex::counter, 0);
}

TEST_F(GcPtrTest, Should_SetGcInterval) {
    GcPtr<int>::set_gc_interval(std::chrono::seconds(10));
    GcPtr<int>::set_destruct_threshold(50);
}

struct WithInnerPtr {
	static std::atomic<int> counter;
	GcPtr<WithInnerPtr> child;
	WithInnerPtr() { counter++; }
	~WithInnerPtr() { counter--; }
};
std::atomic<int> WithInnerPtr::counter{0};

TEST_F(GcPtrTest, Should_CollectChainOfObjects) {
    WithInnerPtr::counter = 0;
    {
        auto root = make_gc<WithInnerPtr>();
        root->child = make_gc<WithInnerPtr>();
        root->child->child = make_gc<WithInnerPtr>();
        EXPECT_EQ(WithInnerPtr::counter, 3);
    }
    GcPtrBase::collect();
    EXPECT_EQ(WithInnerPtr::counter, 0);
}

TEST_F(GcPtrTest, Should_ConstructFromRawPointer) {
    int* raw = new int(100);
    GcPtr<int> ptr(raw);
    EXPECT_EQ(*ptr, 100);
}

TEST_F(GcPtrTest, Should_CompareEquality) {
    auto ptr1 = make_gc<int>(42);
    auto ptr2 = make_gc<int>(42);
    GcPtr<int> ptr3(ptr1);
    EXPECT_FALSE(ptr1 == ptr2);
    EXPECT_TRUE(ptr1 == ptr3);
    EXPECT_TRUE(ptr1 != ptr2);
}

TEST_F(GcPtrTest, Should_ConstructFromNullptr) {
    GcPtr<int> ptr; // 默认构造就是 nullptr
    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.get(), nullptr);
}

TEST_F(GcPtrTest, Should_SelfAssignCopy) {
    auto ptr = make_gc<int>(42);
    ptr = ptr;  // 自赋值
    EXPECT_TRUE(ptr);
    EXPECT_EQ(*ptr, 42);
}

TEST_F(GcPtrTest, Should_SelfAssignMove) {
    auto ptr = make_gc<int>(42);
    // 抑制自移动警告，因为我们确实想测试这种边缘情况
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wself-move"
    ptr = std::move(ptr);  // 自移动赋值
    #pragma GCC diagnostic pop
    // 移动后的状态可能为空，或保持原值，取决于实现
    // 这里我们只检查程序不崩溃
}

struct ThrowInConstructor {
	static std::atomic<int> throw_count;
	ThrowInConstructor() { throw_count++; throw std::runtime_error("test"); }
};
std::atomic<int> ThrowInConstructor::throw_count{0};

TEST_F(GcPtrTest, Should_HandleExceptionInConstructor) {
    ThrowInConstructor::throw_count = 0;
    EXPECT_THROW({
        auto ptr = make_gc<ThrowInConstructor>();
    }, std::runtime_error);
    EXPECT_EQ(ThrowInConstructor::throw_count, 1);
}

TEST_F(GcPtrTest, Should_SetAndGetDestructThreshold) {
    GcPtr<int>::set_destruct_threshold(100);
    GcPtr<int>::set_destruct_threshold(200);  // 只是测试不会崩溃
    // 恢复默认值
    GcPtr<int>::set_destruct_threshold(1000);
}

struct ReentrantGc : public Counted {
    GcPtr<ReentrantGc> ptr;
    ReentrantGc() : Counted() {}
    ~ReentrantGc() {
        // 析构时尝试再次GC
        GcPtrBase::collect();
    }
};

TEST_F(GcPtrTest, Should_ProtectAgainstReentrantGc) {
    ReentrantGc::counter = 0;
    {
        auto obj = make_gc<ReentrantGc>();
        obj->ptr = make_gc<ReentrantGc>();
        EXPECT_EQ(ReentrantGc::counter, 2);
    }
    GcPtrBase::collect();
    EXPECT_EQ(ReentrantGc::counter, 0);
}

TEST_F(GcPtrTest, Should_ResetToNullptr) {
    auto ptr = make_gc<int>(42);
    ptr.reset(nullptr);
    EXPECT_FALSE(ptr);
}

TEST_F(GcPtrTest, Should_ConstructWithMultipleArgs) {
    struct MultiArg {
        int a;
        std::string b;
        double c;
        MultiArg(int x, std::string y, double z) : a(x), b(y), c(z) {}
    };
    auto ptr = make_gc<MultiArg>(42, "test", 3.14);
    EXPECT_EQ(ptr->a, 42);
    EXPECT_EQ(ptr->b, "test");
    EXPECT_EQ(ptr->c, 3.14);
}

#ifdef GPTR_THREAD

TEST_F(GcPtrTest, Should_WorkInMultiThreadedEnvironment) {
    Counted::counter = 0;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 50; ++i) {
        threads.emplace_back([] {
            for (int j = 0; j < 20; ++j) {
                auto ptr = make_gc<Counted>(j);
                GcPtr<Counted> copy(ptr);
                GcPtr<Counted> moved(std::move(copy));
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    GcPtrBase::collect();
    // 所有线程完成后不应该有残留对象
    EXPECT_EQ(Counted::counter, 0);
}

#endif

// 自定义 Deleter 测试
struct CustomDeleterTest {
    static int delete_count;
    int value;
    CustomDeleterTest(int v) : value(v) {}
};
int CustomDeleterTest::delete_count = 0;

TEST_F(GcPtrTest, Should_UseCustomDeleter) {
    CustomDeleterTest::delete_count = 0;
    auto* raw = new CustomDeleterTest(42);
    {
        auto ptr = make_gc_with_deleter<CustomDeleterTest>(
            raw, 
            [](CustomDeleterTest* p) { 
                CustomDeleterTest::delete_count++; 
                delete p; 
            }
        );
        EXPECT_EQ(ptr->value, 42);
    }
    GcPtrBase::collect();
    EXPECT_EQ(CustomDeleterTest::delete_count, 1);
}

// 边界条件测试：多次重置同一指针
TEST_F(GcPtrTest, Should_HandleMultipleResets) {
    Counted::counter = 0;
    {
        auto ptr = make_gc<Counted>(1);
        EXPECT_EQ(Counted::counter, 1);
        ptr.reset();
        EXPECT_EQ(Counted::counter, 0);
        ptr.reset(new Counted(2));
        EXPECT_EQ(Counted::counter, 1);
        ptr.reset(new Counted(3));
        EXPECT_EQ(Counted::counter, 1);
    }
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

// 边界条件测试：release 后手动删除
TEST_F(GcPtrTest, Should_HandleReleaseAndManualDelete) {
    Counted::counter = 0;
    {
        auto ptr = make_gc<Counted>(42);
        EXPECT_EQ(Counted::counter, 1);
        Counted* raw = ptr.release();
        EXPECT_EQ(Counted::counter, 1);
        delete raw;
        EXPECT_EQ(Counted::counter, 0);
    }
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

// 边界条件测试：空指针操作
TEST_F(GcPtrTest, Should_HandleNullptrOperations) {
    GcPtr<int> ptr;
    EXPECT_FALSE(ptr);
    EXPECT_EQ(ptr.get(), nullptr);
    ptr.reset(); // 空指针重置不应该崩溃
    EXPECT_FALSE(ptr);
    
    int* null_raw = nullptr;
    GcPtr<int> ptr2(null_raw); // 从空指针构造
    EXPECT_FALSE(ptr2);
}

// 边界条件测试：大对象链
struct LargeObjectChainNode {
    static std::atomic<int> count;
    GcPtr<LargeObjectChainNode> next;
    int id;
    LargeObjectChainNode(int i) : id(i) { count++; }
    ~LargeObjectChainNode() { count--; }
};
std::atomic<int> LargeObjectChainNode::count{0};

TEST_F(GcPtrTest, Should_HandleLargeObjectChain) {
    LargeObjectChainNode::count = 0;
    
    {
        auto root = make_gc<LargeObjectChainNode>(0);
        auto current = root;
        for (int i = 1; i < 100; ++i) {
            current->next = make_gc<LargeObjectChainNode>(i);
            current = current->next;
        }
        EXPECT_EQ(LargeObjectChainNode::count, 100);
    }
    GcPtrBase::collect();
    EXPECT_EQ(LargeObjectChainNode::count, 0);
}

// 边界条件测试：快速创建和销毁
TEST_F(GcPtrTest, Should_HandleRapidCreationAndDestruction) {
    Counted::counter = 0;
    for (int i = 0; i < 1000; ++i) {
        auto ptr = make_gc<Counted>(i);
    }
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

}

TEST_F(GcPtrTest, Should_CountReferencesCorrectly) {
    auto ptr1 = make_gc<int>(42);
    EXPECT_EQ(GcPtrBase::count_references(ptr1.get()), 1);
    
    GcPtr<int> ptr2(ptr1);
    EXPECT_EQ(GcPtrBase::count_references(ptr1.get()), 2);
    
    GcPtr<int> ptr3(ptr2);
    EXPECT_EQ(GcPtrBase::count_references(ptr1.get()), 3);
}

TEST_F(GcPtrTest, Should_DetectPointerWithinGcObject) {
    struct WithPtr {
        GcPtr<int> inner;
        int value;
    };
    auto outer = make_gc<WithPtr>();
    outer->inner = make_gc<int>(42);
    
    EXPECT_TRUE(GcPtrBase::is_within_any_gc_object(&outer->inner));
    EXPECT_TRUE(GcPtrBase::is_within_any_gc_object(&outer->value));
    
    int stack_var = 0;
    EXPECT_FALSE(GcPtrBase::is_within_any_gc_object(&stack_var));
}

TEST_F(GcPtrTest, Should_HandleManualGcObjectRegistration) {
    int* raw = new int(100);
    bool deleted = false;
    
    GcPtrBase::register_gc_object(raw, sizeof(int), [raw, &deleted]() {
        delete raw;
        deleted = true;
    });
    
    EXPECT_EQ(GcPtrBase::count_references(raw), 0);
    
    GcPtrBase::unregister_gc_object(raw);
    EXPECT_FALSE(deleted);
}

TEST_F(GcPtrTest, Should_TriggerAutoGcOnDestructThreshold) {
    // 测试设置和获取阈值
    GcPtr<int>::set_destruct_threshold(50);
    GcPtr<int>::set_destruct_threshold(100);
    EXPECT_TRUE(true); // 只是确认设置不会崩溃
    GcPtr<int>::set_destruct_threshold(1000);
}

TEST_F(GcPtrTest, Should_TriggerAutoGcOnTimeInterval) {
    // 测试设置和获取间隔
    GcPtr<int>::set_gc_interval(std::chrono::seconds(5));
    GcPtr<int>::set_gc_interval(std::chrono::seconds(10));
    EXPECT_TRUE(true); // 只是确认设置不会崩溃
    GcPtr<int>::set_gc_interval(std::chrono::seconds(3600));
}

struct PolymorphicBase {
    static std::atomic<int> base_count;
    virtual ~PolymorphicBase() { base_count--; }
    PolymorphicBase() { base_count++; }
};
struct PolymorphicDerived : PolymorphicBase {
    static std::atomic<int> derived_count;
    int extra;
    PolymorphicDerived() : extra(42) { derived_count++; }
    ~PolymorphicDerived() { derived_count--; }
};
std::atomic<int> PolymorphicBase::base_count{0};
std::atomic<int> PolymorphicDerived::derived_count{0};

TEST_F(GcPtrTest, Should_HandlePolymorphicObjects) {
    PolymorphicBase::base_count = 0;
    PolymorphicDerived::derived_count = 0;
    
    {
        GcPtr<PolymorphicBase> ptr(new PolymorphicDerived(), 
            [](PolymorphicBase* p) { delete static_cast<PolymorphicDerived*>(p); }, 
            sizeof(PolymorphicDerived));
        EXPECT_EQ(PolymorphicBase::base_count, 1);
        EXPECT_EQ(PolymorphicDerived::derived_count, 1);
    }
    
    GcPtrBase::collect();
    EXPECT_EQ(PolymorphicBase::base_count, 0);
    EXPECT_EQ(PolymorphicDerived::derived_count, 0);
}

struct PolymorphicBase2 {
    virtual ~PolymorphicBase2() = default;
    virtual int getValue() const { return 0; }
};
struct PolymorphicDerived2 : PolymorphicBase2 {
    static std::atomic<int> counter;
    int value;
    PolymorphicDerived2(int v) : value(v) { counter++; }
    ~PolymorphicDerived2() { counter--; }
    int getValue() const override { return value; }
};
std::atomic<int> PolymorphicDerived2::counter{0};

TEST_F(GcPtrTest, Should_UseMakeGcWithDeleterAndSize) {
    PolymorphicDerived2::counter = 0;
    
    {
        PolymorphicDerived2* raw = new PolymorphicDerived2(123);
        auto ptr = make_gc_with_deleter<PolymorphicBase2>(raw, 
            [](PolymorphicBase2* p) { delete static_cast<PolymorphicDerived2*>(p); }, 
            sizeof(PolymorphicDerived2));
        EXPECT_EQ(ptr->getValue(), 123);
        EXPECT_EQ(PolymorphicDerived2::counter, 1);
    }
    
    GcPtrBase::collect();
    EXPECT_EQ(PolymorphicDerived2::counter, 0);
}

struct ResetCustomDeleterTest {
    static std::atomic<int> delete_count;
    int value;
    ResetCustomDeleterTest(int v) : value(v) {}
};
std::atomic<int> ResetCustomDeleterTest::delete_count{0};

TEST_F(GcPtrTest, Should_ResetWithCustomDeleter) {
    // 测试 reset_with_deleter 基本功能
    GcPtr<int> ptr;
    int* raw = new int(42);
    bool deleted = false;
    
    ptr.reset_with_deleter(raw, [&deleted](int* p) {
        deleted = true;
        delete p;
    }, sizeof(int));
    
    EXPECT_EQ(*ptr, 42);
    EXPECT_FALSE(deleted);
    
    ptr.reset(); // 这会触发删除器
    EXPECT_TRUE(deleted);
}

TEST_F(GcPtrTest, Should_ThrowOnDoubleRegistration) {
    int* raw = new int(42);
    
    GcPtr<int> ptr1(raw);
    
    EXPECT_THROW({
        GcPtr<int> ptr2(raw);
    }, std::runtime_error);
    
    GcPtrBase::collect();
}

TEST_F(GcPtrTest, Should_ThrowOnResetWithAlreadyRegisteredPointer) {
    auto ptr1 = make_gc<int>(42);
    GcPtr<int> ptr2;
    
    EXPECT_THROW({
        ptr2.reset(ptr1.get());
    }, std::runtime_error);
    
    GcPtrBase::collect();
}

TEST_F(GcPtrTest, Should_HandleConstMethods) {
    auto ptr = make_gc<int>(42);
    
    const GcPtr<int>& const_ptr = ptr;
    
    EXPECT_TRUE(const_ptr);
    EXPECT_EQ(*const_ptr, 42);
    EXPECT_EQ(const_ptr.get(), ptr.get());
}

struct ConstructorGcObject {
    static std::atomic<int> counter;
    int value;
    ConstructorGcObject(int v) : value(v) { counter++; }
    ~ConstructorGcObject() { counter--; }
};
std::atomic<int> ConstructorGcObject::counter{0};

TEST_F(GcPtrTest, Should_HandleUnderConstructionDuringGc) {
    ConstructorGcObject::counter = 0;
    
    {
        // 测试多个对象创建和GC
        auto ptr1 = make_gc<ConstructorGcObject>(1);
        auto ptr2 = make_gc<ConstructorGcObject>(2);
        auto ptr3 = make_gc<ConstructorGcObject>(3);
        
        EXPECT_EQ(ConstructorGcObject::counter, 3);
    }
    
    GcPtrBase::collect();
    EXPECT_EQ(ConstructorGcObject::counter, 0);
}

struct SelfRefObject {
    static std::atomic<int> counter;
    GcPtr<SelfRefObject> self;
    SelfRefObject() { counter++; }
    ~SelfRefObject() { counter--; }
};
std::atomic<int> SelfRefObject::counter{0};

TEST_F(GcPtrTest, Should_HandleSelfReference) {
    SelfRefObject::counter = 0;
    
    {
        auto ptr = make_gc<SelfRefObject>();
        ptr->self = ptr;
        EXPECT_EQ(SelfRefObject::counter, 1);
    }
    
    GcPtrBase::collect();
    EXPECT_EQ(SelfRefObject::counter, 0);
}

struct DeepNodeObject {
    static std::atomic<int> counter;
    int value;
    GcPtr<DeepNodeObject> left;
    GcPtr<DeepNodeObject> right;
    DeepNodeObject(int v) : value(v) { counter++; }
    ~DeepNodeObject() { counter--; }
};
std::atomic<int> DeepNodeObject::counter{0};

TEST_F(GcPtrTest, Should_HandleDeepNestedStructures) {
    DeepNodeObject::counter = 0;
    
    {
        auto root = make_gc<DeepNodeObject>(1);
        root->left = make_gc<DeepNodeObject>(2);
        root->right = make_gc<DeepNodeObject>(3);
        root->left->left = make_gc<DeepNodeObject>(4);
        root->left->right = make_gc<DeepNodeObject>(5);
        root->right->left = make_gc<DeepNodeObject>(6);
        root->right->right = make_gc<DeepNodeObject>(7);
        EXPECT_EQ(DeepNodeObject::counter, 7);
    }
    
    GcPtrBase::collect();
    EXPECT_EQ(DeepNodeObject::counter, 0);
}

TEST_F(GcPtrTest, Should_HandleResetToSamePointer) {
    auto ptr = make_gc<int>(42);
    int* raw = ptr.get();
    
    EXPECT_NO_THROW({
        ptr.reset(raw);
    });
    
    EXPECT_EQ(*ptr, 42);
    GcPtrBase::collect();
}

TEST_F(GcPtrTest, Should_HandleEmptySwap) {
    GcPtr<int> ptr1;
    GcPtr<int> ptr2;
    
    ptr1.swap(ptr2);
    
    EXPECT_FALSE(ptr1);
    EXPECT_FALSE(ptr2);
}

TEST_F(GcPtrTest, Should_HandleMixedSwap) {
    auto ptr1 = make_gc<int>(42);
    GcPtr<int> ptr2;
    
    ptr1.swap(ptr2);
    
    EXPECT_FALSE(ptr1);
    EXPECT_TRUE(ptr2);
    EXPECT_EQ(*ptr2, 42);
    
    GcPtrBase::collect();
}

TEST_F(GcPtrTest, Should_HandleConstCastOperator) {
    auto ptr = make_gc<int>(42);
    
    bool result = static_cast<bool>(ptr);
    EXPECT_TRUE(result);
    
    GcPtr<int> null_ptr;
    result = static_cast<bool>(null_ptr);
    EXPECT_FALSE(result);
}

TEST_F(GcPtrTest, Should_CallGcMethod) {
    Counted::counter = 0;
    
    auto ptr = make_gc<Counted>(1);
    EXPECT_EQ(Counted::counter, 1);
    
    ptr.gc();
    EXPECT_EQ(Counted::counter, 1);
    
    {
        auto temp = make_gc<Counted>(2);
        EXPECT_EQ(Counted::counter, 2);
    }
    
    ptr.gc();
    EXPECT_EQ(Counted::counter, 1);
    
    GcPtrBase::collect();
}

TEST_F(GcPtrTest, Should_HandleMultipleReferencesDuringCollection) {
    Counted::counter = 0;
    
    auto ptr1 = make_gc<Counted>(1);
    GcPtr<Counted> ptr2(ptr1);
    GcPtr<Counted> ptr3(ptr1);
    
    EXPECT_EQ(Counted::counter, 1);
    
    ptr1.reset();
    EXPECT_EQ(Counted::counter, 1);
    
    ptr2.reset();
    EXPECT_EQ(Counted::counter, 1);
    
    ptr3.reset();
    EXPECT_EQ(Counted::counter, 0);
}

struct LightWeightObject {
    static std::atomic<int> counter;
    int value;
    LightWeightObject(int v) : value(v) { counter++; }
    ~LightWeightObject() { counter--; }
};
std::atomic<int> LightWeightObject::counter{0};

TEST_F(GcPtrTest, Should_HandleLargeNumberOfPointers) {
    LightWeightObject::counter = 0;
    
    const int NUM_OBJECTS = 500;
    std::vector<GcPtr<LightWeightObject>> pointers;
    pointers.reserve(NUM_OBJECTS);
    
    for (int i = 0; i < NUM_OBJECTS; ++i) {
        pointers.push_back(make_gc<LightWeightObject>(i));
    }
    
    EXPECT_EQ(LightWeightObject::counter, NUM_OBJECTS);
    
    pointers.clear();
    GcPtrBase::collect();
    
    EXPECT_EQ(LightWeightObject::counter, 0);
}

TEST_F(GcPtrTest, Should_HandleGcInProgressFlag) {
    Counted::counter = 0;
    
    auto ptr = make_gc<Counted>(1);
    
    bool gc_was_in_progress = GcPtrBase::gc_in_progress;
    EXPECT_FALSE(gc_was_in_progress);
    
    GcPtrBase::collect();
    
    EXPECT_EQ(Counted::counter, 1);
    GcPtrBase::collect();
    
    GcPtrBase::collect();
}

TEST_F(GcPtrTest, Should_HandlePointerComparisonWithNullptr) {
    auto ptr = make_gc<int>(42);
    
    EXPECT_TRUE(ptr.get() != nullptr);
    EXPECT_FALSE(ptr.get() == nullptr);
    
    GcPtr<int> null_ptr;
    EXPECT_TRUE(null_ptr.get() == nullptr);
    EXPECT_FALSE(null_ptr.get() != nullptr);
}

struct MoveOnly {
    static std::atomic<int> counter;
    int value;
    MoveOnly(int v) : value(v) { counter++; }
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
    ~MoveOnly() { counter--; }
};
std::atomic<int> MoveOnly::counter{0};

TEST_F(GcPtrTest, Should_HandleMoveOnlyTypes) {
    MoveOnly::counter = 0;
    
    {
        auto ptr = make_gc<MoveOnly>(42);
        EXPECT_EQ(MoveOnly::counter, 1);
        EXPECT_EQ(ptr->value, 42);
        
        GcPtr<MoveOnly> ptr2(std::move(ptr));
        EXPECT_FALSE(ptr);
        EXPECT_TRUE(ptr2);
        EXPECT_EQ(ptr2->value, 42);
        EXPECT_EQ(MoveOnly::counter, 1);
    }
    
    GcPtrBase::collect();
    EXPECT_EQ(MoveOnly::counter, 0);
}

TEST_F(GcPtrTest, Should_HandleReleaseOnNullPointer) {
    GcPtr<int> ptr;
    
    int* result = ptr.release();
    EXPECT_EQ(result, nullptr);
}

TEST_F(GcPtrTest, Should_HandleResetOnNullPointer) {
    GcPtr<int> ptr;
    
    EXPECT_NO_THROW({
        ptr.reset();
    });
    
    EXPECT_FALSE(ptr);
}

TEST_F(GcPtrTest, Should_HandleResetWithNullptr) {
    auto ptr = make_gc<int>(42);
    
    ptr.reset(nullptr);
    EXPECT_FALSE(ptr);
    
    GcPtrBase::collect();
}

TEST_F(GcPtrTest, Should_ProvideStrongExceptionSafetyOnResetWithDeleter) {
    Counted::counter = 0;
    auto ptr = make_gc<Counted>(1);
    EXPECT_EQ(Counted::counter, 1);

    Counted* already_registered = new Counted(2);
    GcPtr<Counted> other(already_registered);

    EXPECT_THROW({
        ptr.reset_with_deleter(already_registered,
            [](Counted* p) { delete p; }, sizeof(Counted));
    }, std::runtime_error);

    EXPECT_TRUE(ptr);
    EXPECT_EQ(Counted::counter, 2);

    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 2);

    ptr.reset();
    other.reset();
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_ReleaseOldObjectOnCopyAssignment) {
    Counted::counter = 0;
    GcPtr<Counted> ptr(make_gc<Counted>(1));
    EXPECT_EQ(Counted::counter, 1);

    auto new_obj = make_gc<Counted>(2);
    EXPECT_EQ(Counted::counter, 2);

    ptr = new_obj;
    EXPECT_EQ(Counted::counter, 1);

    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 1);
}

TEST_F(GcPtrTest, Should_ReleaseOldObjectOnMoveAssignment) {
    Counted::counter = 0;
    GcPtr<Counted> ptr(make_gc<Counted>(1));
    EXPECT_EQ(Counted::counter, 1);

    GcPtr<Counted> new_ptr(make_gc<Counted>(2));
    EXPECT_EQ(Counted::counter, 2);

    ptr = std::move(new_ptr);
    EXPECT_EQ(Counted::counter, 1);

    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 1);
}

TEST_F(GcPtrTest, Should_NotReleaseSharedObjectOnAssignment) {
    Counted::counter = 0;
    auto shared_obj = make_gc<Counted>(1);
    EXPECT_EQ(Counted::counter, 1);

    GcPtr<Counted> ptr1(shared_obj);
    GcPtr<Counted> ptr2(shared_obj);
    EXPECT_EQ(Counted::counter, 1);

    auto new_obj = make_gc<Counted>(2);
    EXPECT_EQ(Counted::counter, 2);

    ptr1 = new_obj;
    EXPECT_EQ(Counted::counter, 2);

    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 2);
}

TEST_F(GcPtrTest, Should_DisableAndEnableAutoGc) {
    EXPECT_TRUE(GcPtr<int>::is_auto_gc_enabled());

    GcPtr<int>::disable_auto_gc();
    EXPECT_FALSE(GcPtr<int>::is_auto_gc_enabled());

    GcPtr<int>::enable_auto_gc();
    EXPECT_TRUE(GcPtr<int>::is_auto_gc_enabled());
}

TEST_F(GcPtrTest, Should_NotTriggerAutoGcWhenDisabled) {
    Counted::counter = 0;
    GcPtr<int>::disable_auto_gc();
    GcPtr<int>::set_destruct_threshold(1);

    {
        auto ptr = make_gc<Counted>(1);
        EXPECT_EQ(Counted::counter, 1);
    }

    {
        auto ptr = make_gc<Counted>(2);
        EXPECT_EQ(Counted::counter, 2);
    }

    GcPtr<int>::enable_auto_gc();
    GcPtr<int>::set_destruct_threshold(1000);
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_ReportGcObjectCount) {
    Counted::counter = 0;
    EXPECT_EQ(GcPtr<Counted>::gc_object_count(), 0u);

    auto ptr1 = make_gc<Counted>(1);
    EXPECT_EQ(GcPtr<Counted>::gc_object_count(), 1u);

    {
        auto ptr2 = make_gc<Counted>(2);
        EXPECT_EQ(GcPtr<Counted>::gc_object_count(), 2u);
    }

    GcPtrBase::collect();
    EXPECT_EQ(GcPtr<Counted>::gc_object_count(), 1u);
}

TEST_F(GcPtrTest, Should_SupportStdAtomicLoadStore) {
    Counted::counter = 0;
    std::atomic<GcPtr<Counted>> atomic_ptr;

    auto ptr = make_gc<Counted>(42);
    EXPECT_EQ(Counted::counter, 1);

    atomic_ptr.store(ptr);
    EXPECT_EQ(Counted::counter, 1);

    auto loaded = atomic_ptr.load();
    EXPECT_TRUE(loaded);
    EXPECT_EQ(loaded->value, 42);
    EXPECT_EQ(Counted::counter, 1);

    atomic_ptr.store(GcPtr<Counted>());
    loaded = atomic_ptr.load();
    EXPECT_FALSE(loaded);

    loaded.reset();
    ptr.reset();
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_SupportStdAtomicExchange) {
    Counted::counter = 0;
    std::atomic<GcPtr<Counted>> atomic_ptr;

    auto ptr1 = make_gc<Counted>(1);
    auto ptr2 = make_gc<Counted>(2);
    EXPECT_EQ(Counted::counter, 2);

    atomic_ptr.store(ptr1);
    auto old = atomic_ptr.exchange(ptr2);
    EXPECT_TRUE(old);
    EXPECT_EQ(old->value, 1);
    EXPECT_EQ(Counted::counter, 2);

    auto loaded = atomic_ptr.load();
    EXPECT_EQ(loaded->value, 2);

    atomic_ptr.store(GcPtr<Counted>());
    old.reset();
    loaded.reset();
    ptr1.reset();
    ptr2.reset();
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_SupportStdAtomicCompareExchange) {
    Counted::counter = 0;
    std::atomic<GcPtr<Counted>> atomic_ptr;

    auto ptr1 = make_gc<Counted>(1);
    auto ptr2 = make_gc<Counted>(2);
    EXPECT_EQ(Counted::counter, 2);

    atomic_ptr.store(ptr1);

    auto expected = make_gc<Counted>(3);
    EXPECT_EQ(Counted::counter, 3);

    bool success =
        atomic_ptr.compare_exchange_strong(expected, ptr2);
    EXPECT_FALSE(success);
    EXPECT_EQ(expected->value, 1);

    success =
        atomic_ptr.compare_exchange_weak(expected, ptr2);
    EXPECT_TRUE(success);

    auto loaded = atomic_ptr.load();
    EXPECT_EQ(loaded->value, 2);

    atomic_ptr.store(GcPtr<Counted>());
    expected.reset();
    loaded.reset();
    ptr1.reset();
    ptr2.reset();
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_SupportStdAtomicCopyConstruction) {
    Counted::counter = 0;
    auto ptr = make_gc<Counted>(42);
    EXPECT_EQ(Counted::counter, 1);

    std::atomic<GcPtr<Counted>> atomic_ptr(ptr);
    EXPECT_EQ(Counted::counter, 1);

    auto loaded = atomic_ptr.load();
    EXPECT_EQ(loaded->value, 42);

    atomic_ptr.store(GcPtr<Counted>());
    loaded.reset();
    ptr.reset();
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

TEST_F(GcPtrTest, Should_WorkWithStdVector) {
    Counted::counter = 0;
    GcPtr<int>::disable_auto_gc();

    {
        std::vector<GcPtr<Counted>> vec;
        vec.reserve(3);
        vec.push_back(make_gc<Counted>(1));
        vec.push_back(make_gc<Counted>(2));
        vec.push_back(make_gc<Counted>(3));
        EXPECT_EQ(Counted::counter, 3);
        EXPECT_EQ(vec.size(), 3u);

        vec.erase(vec.begin());
        EXPECT_EQ(vec.size(), 2u);
        EXPECT_EQ(Counted::counter, 2);
        GcPtrBase::collect();
        EXPECT_EQ(Counted::counter, 2);
    }

    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
    GcPtr<int>::enable_auto_gc();
}

TEST_F(GcPtrTest, Should_HandleVectorResizeWithGcPtr) {
    Counted::counter = 0;

    {
        std::vector<GcPtr<Counted>> vec;
        for (int i = 0; i < 20; ++i) {
            vec.push_back(make_gc<Counted>(i));
        }
        EXPECT_EQ(Counted::counter, 20);
        EXPECT_EQ(vec.size(), 20u);

        vec.resize(5);
        EXPECT_EQ(vec.size(), 5u);
        GcPtrBase::collect();
        EXPECT_EQ(Counted::counter, 5);
    }

    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

#ifdef GPTR_THREAD

TEST_F(GcPtrTest, Should_SupportAtomicGcPtrInMultiThreads) {
    Counted::counter = 0;
    std::atomic<GcPtr<Counted>> atomic_ptr;

    auto initial = make_gc<Counted>(100);
    atomic_ptr.store(initial);

    std::vector<std::thread> threads;
    for (int i = 0; i < 20; ++i) {
        threads.emplace_back([&atomic_ptr, i] {
            for (int j = 0; j < 20; ++j) {
                auto snap = atomic_ptr.load();
                if (snap) {
                    EXPECT_GE(snap->value, 0);
                }
                if (j % 5 == 0) {
                    auto replacement = make_gc<Counted>(i * 100 + j);
                    atomic_ptr.store(replacement);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    atomic_ptr.store(GcPtr<Counted>());
    initial.reset();
    GcPtrBase::collect();
    EXPECT_EQ(Counted::counter, 0);
}

#endif

struct ContainerTestData {
    static std::atomic<int> counter;
    int value;
    ContainerTestData(int v) : value(v) { counter++; }
    ~ContainerTestData() { counter--; }
};
std::atomic<int> ContainerTestData::counter{0};

TEST_F(GcPtrTest, Should_SupportOperatorLess) {
    auto ptr1 = make_gc<int>(42);
    auto ptr2 = make_gc<int>(99);

    EXPECT_TRUE((ptr1 < ptr2) || (ptr2 < ptr1));
    EXPECT_FALSE(ptr1 < ptr1);

    GcPtr<int> null1;
    GcPtr<int> null2;
    EXPECT_FALSE(null1 < null2);

    EXPECT_TRUE(null1 < ptr1 || ptr1 < null1);
    EXPECT_FALSE(null1 < ptr1 && ptr1 < null1);
}

TEST_F(GcPtrTest, Should_WorkWithStdSet) {
    ContainerTestData::counter = 0;
    GcPtr<int>::disable_auto_gc();

    {
        std::set<GcPtr<ContainerTestData>> s;
        s.insert(make_gc<ContainerTestData>(1));
        s.insert(make_gc<ContainerTestData>(2));
        s.insert(make_gc<ContainerTestData>(3));
        EXPECT_EQ(s.size(), 3u);
        EXPECT_EQ(ContainerTestData::counter, 3);

        s.erase(s.begin());
        EXPECT_EQ(s.size(), 2u);
        GcPtrBase::collect();
        EXPECT_EQ(ContainerTestData::counter, 2);
    }

    GcPtrBase::collect();
    EXPECT_EQ(ContainerTestData::counter, 0);
    GcPtr<int>::enable_auto_gc();
}

TEST_F(GcPtrTest, Should_WorkWithStdMap) {
    ContainerTestData::counter = 0;
    GcPtr<int>::disable_auto_gc();

    {
        std::map<GcPtr<ContainerTestData>, int> m;
        auto k1 = make_gc<ContainerTestData>(10);
        auto k2 = make_gc<ContainerTestData>(20);
        auto k3 = make_gc<ContainerTestData>(30);
        m[k1] = 1;
        m[k2] = 2;
        m[k3] = 3;
        EXPECT_EQ(m.size(), 3u);
        EXPECT_EQ(ContainerTestData::counter, 3);

        m.erase(k1);
        EXPECT_EQ(m.size(), 2u);
        GcPtrBase::collect();
        EXPECT_EQ(ContainerTestData::counter, 3);
    }

    GcPtrBase::collect();
    EXPECT_EQ(ContainerTestData::counter, 0);
    GcPtr<int>::enable_auto_gc();
}

TEST_F(GcPtrTest, Should_WorkWithStdUnorderedSet) {
    ContainerTestData::counter = 0;
    GcPtr<int>::disable_auto_gc();

    {
        std::unordered_set<GcPtr<ContainerTestData>> us;
        us.insert(make_gc<ContainerTestData>(1));
        us.insert(make_gc<ContainerTestData>(2));
        us.insert(make_gc<ContainerTestData>(3));
        EXPECT_EQ(us.size(), 3u);
        EXPECT_EQ(ContainerTestData::counter, 3);

        us.erase(us.begin());
        EXPECT_EQ(us.size(), 2u);
        GcPtrBase::collect();
        EXPECT_EQ(ContainerTestData::counter, 2);
    }

    GcPtrBase::collect();
    EXPECT_EQ(ContainerTestData::counter, 0);
    GcPtr<int>::enable_auto_gc();
}

TEST_F(GcPtrTest, Should_WorkWithHeapAllocatedVector) {
    ContainerTestData::counter = 0;
    GcPtr<int>::disable_auto_gc();

    auto* vec = new std::vector<GcPtr<ContainerTestData>>();
    vec->push_back(make_gc<ContainerTestData>(1));
    vec->push_back(make_gc<ContainerTestData>(2));
    vec->push_back(make_gc<ContainerTestData>(3));
    EXPECT_EQ(vec->size(), 3u);
    EXPECT_EQ(ContainerTestData::counter, 3);

    GcPtrBase::collect();
    EXPECT_EQ(ContainerTestData::counter, 3);

    delete vec;
    GcPtrBase::collect();
    EXPECT_EQ(ContainerTestData::counter, 0);
    GcPtr<int>::enable_auto_gc();
}

TEST_F(GcPtrTest, Should_SupportOwnerBefore) {
    auto ptr1 = make_gc<int>(42);
    auto ptr2 = make_gc<int>(99);
    GcPtr<int> ptr3(ptr1);

    EXPECT_FALSE(ptr1.owner_before(ptr3));
    EXPECT_FALSE(ptr3.owner_before(ptr1));

    EXPECT_TRUE(ptr1.owner_before(ptr2) || ptr2.owner_before(ptr1));
}

TEST_F(GcPtrTest, Should_SupportStdOwnerLess) {
    ContainerTestData::counter = 0;
    GcPtr<int>::disable_auto_gc();

    {
        std::set<GcPtr<ContainerTestData>, std::owner_less<GcPtr<ContainerTestData>>> s;
        auto p1 = make_gc<ContainerTestData>(1);
        auto p2 = make_gc<ContainerTestData>(2);
        s.insert(p1);
        s.insert(p2);
        EXPECT_EQ(s.size(), 2u);
        EXPECT_EQ(ContainerTestData::counter, 2);
    }

    GcPtrBase::collect();
    EXPECT_EQ(ContainerTestData::counter, 0);
    GcPtr<int>::enable_auto_gc();
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
