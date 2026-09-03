CXX ?= c++
TEST_BINARY := /tmp/wheel-wrecker-dial-tests
SANITIZED_TEST_BINARY := /tmp/wheel-wrecker-dial-tests-sanitized
TEST_FLAGS := -std=c++11 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion

.PHONY: build test test-sanitize

build:
	pio run

test:
	$(CXX) $(TEST_FLAGS) -Iinclude src/DialMath.cpp \
		test/native/test_dial_math.cpp -o $(TEST_BINARY)
	$(TEST_BINARY)

test-sanitize:
	$(CXX) $(TEST_FLAGS) -fsanitize=address,undefined,float-cast-overflow \
		-fno-omit-frame-pointer -Iinclude src/DialMath.cpp \
		test/native/test_dial_math.cpp -o $(SANITIZED_TEST_BINARY)
	$(SANITIZED_TEST_BINARY)
