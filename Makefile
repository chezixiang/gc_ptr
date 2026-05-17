CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -I. -DGPTR_THREAD
LDFLAGS = -lgtest -lgtest_main -pthread
COVFLAGS = --coverage

TEST_DIR = tests
TARGET = gc_ptr_test.exe
TEST_SRC = $(TEST_DIR)/gc_ptr_test.cpp

.PHONY: all test clean coverage

all: $(TARGET)

$(TARGET): $(TEST_SRC) gc_ptr.hpp
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -o $(TARGET) $(LDFLAGS)

test: $(TARGET)
	./$(TARGET)

coverage: CXXFLAGS += $(COVFLAGS)
coverage: LDFLAGS += $(COVFLAGS)
coverage: clean $(TARGET)
	./$(TARGET) || true
	gcov -b -c $(TEST_SRC)
	gcov -b -c $(TEST_SRC) | findstr "gc_ptr.hpp"

clean:
	rm -f $(TARGET) *.gcda *.gcno *.gcov tests/*.gcda tests/*.gcno tests/*.gcov
