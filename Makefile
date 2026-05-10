CXX = g++
CXXFLAGS = -std=c++26 -Wall -Wextra -Wpedantic -g -I. -DGPTR_THREAD
LDFLAGS = -lgtest -lgtest_main -pthread

TEST_DIR = tests
TARGET = gc_ptr_test.exe
TEST_SRC = $(TEST_DIR)/gc_ptr_test.cpp

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(TEST_SRC) gc_ptr.hpp
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -o $(TARGET) $(LDFLAGS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
