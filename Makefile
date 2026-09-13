CXX = g++
ifeq ($(OS),Windows_NT)
  PYTHON ?= python
  EXE_SUFFIX := .exe
  LDFLAGS := -static
  LDLIBS := -lpsapi
else
  PYTHON ?= python3
endif
BUILD_DIR := build
PATCHOULI_DIR := sources/patchouli
FORMATS := date time url isbn ipv4 ipv6
PATCHOULI := $(BUILD_DIR)/patchouli$(EXE_SUFFIX)
VALIDATORS := $(addprefix $(BUILD_DIR)/validate_,$(addsuffix $(EXE_SUFFIX),$(FORMATS)))
HEADERS := $(wildcard $(PATCHOULI_DIR)/include/patchouli/*.hpp) $(PATCHOULI_DIR)/third_party/nlohmann/json.hpp
CPPFLAGS := -I$(PATCHOULI_DIR)/include -I$(PATCHOULI_DIR)/third_party
CXXFLAGS ?= -O2 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic

.PHONY: all generate smoke test check clean
all: $(PATCHOULI) $(VALIDATORS)

$(BUILD_DIR):
	$(PYTHON) -c "from pathlib import Path; Path(r'$@').mkdir(parents=True, exist_ok=True)"

$(PATCHOULI): $(PATCHOULI_DIR)/src/main.cpp $(HEADERS) Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/validate_%$(EXE_SUFFIX): validators/validator.cpp $(PATCHOULI_DIR)/third_party/nlohmann/json.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(BUILD_DIR)/patchouli_tests$(EXE_SUFFIX): $(PATCHOULI_DIR)/tests/patchouli_tests.cpp $(HEADERS) Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $< $(LDFLAGS) $(LDLIBS) -o $@

generate:
	$(PYTHON) shared-suite/generate_cases.py

smoke: all
	$(PYTHON) run_smoke_tests.py

test: all
	$(PYTHON) run_tests.py

$(BUILD_DIR)/oracle_driver$(EXE_SUFFIX): $(PATCHOULI_DIR)/tests/oracle_driver.cpp $(HEADERS) Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

check: all $(BUILD_DIR)/oracle_driver$(EXE_SUFFIX) $(BUILD_DIR)/patchouli_tests$(EXE_SUFFIX)
	$(PYTHON) -m unittest test_runner_support.py test_oracle_integration.py
	./$(BUILD_DIR)/patchouli_tests$(EXE_SUFFIX)

clean:
	$(PYTHON) -c "import shutil; shutil.rmtree('build', ignore_errors=True); shutil.rmtree('results', ignore_errors=True)"
