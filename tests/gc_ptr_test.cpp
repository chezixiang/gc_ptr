#include <gtest/gtest.h>
#include "../gc_ptr.hpp"
#include <string>
#include <memory>

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
    static int counter;
    int value;
    Counted() : value(0) { counter++; }
    explicit Counted(int v) : value(v) { counter++; }
    ~Counted() { counter--; }
};
int Counted::counter = 0;

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
    static int counter;
    GcPtr<Node> next;
    Node() { counter++; }
    ~Node() { counter--; }
};
int Node::counter = 0;

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
    static int counter;
    int data;
    Complex() : data(0) { counter++; }
    explicit Complex(int d) : data(d) { counter++; }
    ~Complex() { counter--; }
};
int Complex::counter = 0;

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
    static int counter;
    GcPtr<WithInnerPtr> child;
    WithInnerPtr() { counter++; }
    ~WithInnerPtr() { counter--; }
};
int WithInnerPtr::counter = 0;

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
    GcPtr<int> ptr(nullptr);
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
    static int throw_count;
    ThrowInConstructor() { throw_count++; throw std::runtime_error("test"); }
};
int ThrowInConstructor::throw_count = 0;

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
#include <thread>
#include <vector>

TEST_F(GcPtrTest, Should_WorkInMultiThreadedEnvironment) {
    Counted::counter = 0;
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([] {
            for (int j = 0; j < 100; ++j) {
                auto ptr = make_gc<Counted>(j);
                GcPtr<Counted> copy(ptr);
                GcPtr<Counted> moved(std::move(copy));
            }
            GcPtrBase::collect();
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

}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
