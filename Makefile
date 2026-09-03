CXX ?= c++
PIO ?= pio
ARDUINO_CLI ?= arduino-cli
ARDUINO_PROFILE := uno_r4_wifi
ARDUINO_BUILD_DIR ?= /tmp/wheel-wrecker-arduino-build
SKETCH_DIR := arduino/WheelWrecker
TEST_BINARY := /tmp/wheel-wrecker-dial-tests
SANITIZED_TEST_BINARY := /tmp/wheel-wrecker-dial-tests-sanitized
TEST_FLAGS := -std=c++11 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion

.PHONY: build arduino-build test test-sanitize verify

build:
	$(PIO) run

arduino-build:
	$(ARDUINO_CLI) compile --clean --jobs 1 \
		--build-path $(ARDUINO_BUILD_DIR) --warnings all \
		--profile $(ARDUINO_PROFILE) \
		$(SKETCH_DIR)

test:
	$(CXX) $(TEST_FLAGS) -I$(SKETCH_DIR) $(SKETCH_DIR)/DialMath.cpp \
		test/native/test_dial_math.cpp -o $(TEST_BINARY)
	$(TEST_BINARY)

test-sanitize:
	$(CXX) $(TEST_FLAGS) -fsanitize=address,undefined,float-cast-overflow \
		-fno-omit-frame-pointer -I$(SKETCH_DIR) \
		$(SKETCH_DIR)/DialMath.cpp \
		test/native/test_dial_math.cpp -o $(SANITIZED_TEST_BINARY)
	$(SANITIZED_TEST_BINARY)

verify: test test-sanitize build arduino-build
