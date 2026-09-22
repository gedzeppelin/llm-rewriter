BUILD_DIR ?= build
BUILD_TYPE ?= Debug
CXX_COMPILER ?= clang++
GTK4 ?= OFF

CMAKE_FLAGS := -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_CXX_COMPILER=$(CXX_COMPILER)
FEATURE_FLAGS := -DLLM_REWRITER_BUILD_GTK4=$(GTK4)

.PHONY: configure build test clean

configure:
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS) $(FEATURE_FLAGS) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	cmake --build $(BUILD_DIR) --target clean
