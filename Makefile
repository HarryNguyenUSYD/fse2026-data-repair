CXX ?= c++
ifeq ($(OS),Windows_NT)
  PYTHON ?= python
else
  PYTHON ?= python3
endif

BUILD_DIR := build
PATCHOULI_DIR := sources/patchouli
BETAMAX_OLD_SOURCE := sources/betamax/main.cpp
EPSILONREPAIR_SOURCE := sources/epsilonrepair/erepair2.cpp
FORMATS := date time url isbn ipv4 ipv6
PATCHOULI := $(BUILD_DIR)/patchouli
BETAMAX_OLD := $(BUILD_DIR)/betamax-old
EPSILONREPAIR := $(BUILD_DIR)/epsilonrepair
STDIN_VALIDATORS := $(addprefix $(BUILD_DIR)/validate_,$(FORMATS))
FILE_VALIDATORS := $(addprefix $(BUILD_DIR)/validate_old_,$(FORMATS))
BOUNDARY_VALIDATORS := $(addprefix $(BUILD_DIR)/boundary_validate_,$(FORMATS))
VALIDATOR_SOURCE := validators/validator.cpp
BOUNDARY_VALIDATOR_SOURCE := validators/boundary_validator.cpp
PATCHOULI_HEADERS := $(wildcard $(PATCHOULI_DIR)/include/patchouli/*.hpp)
CPPFLAGS := -I$(PATCHOULI_DIR)/include -I$(PATCHOULI_DIR)/third_party
CXXFLAGS ?= -O2 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic

ifeq ($(OS),Windows_NT)
  EXE_SUFFIX := .exe
  LDFLAGS := -static
  PATCHOULI_LDLIBS := -lpsapi
endif

PATCHOULI_TARGET := $(PATCHOULI)$(EXE_SUFFIX)
BETAMAX_OLD_TARGET := $(BETAMAX_OLD)$(EXE_SUFFIX)
EPSILONREPAIR_TARGET := $(EPSILONREPAIR)$(EXE_SUFFIX)
STDIN_VALIDATOR_TARGETS := $(addsuffix $(EXE_SUFFIX),$(STDIN_VALIDATORS))
FILE_VALIDATOR_TARGETS := $(addsuffix $(EXE_SUFFIX),$(FILE_VALIDATORS))
BOUNDARY_VALIDATOR_TARGETS := $(addsuffix $(EXE_SUFFIX),$(BOUNDARY_VALIDATORS))

.PHONY: all generate smoke test clean
all: $(PATCHOULI_TARGET) $(BETAMAX_OLD_TARGET) $(EPSILONREPAIR_TARGET) $(STDIN_VALIDATOR_TARGETS) $(FILE_VALIDATOR_TARGETS) $(BOUNDARY_VALIDATOR_TARGETS)

$(BUILD_DIR):
	$(PYTHON) -c "from pathlib import Path; Path(r'$@').mkdir(parents=True, exist_ok=True)"

$(PATCHOULI_TARGET): $(PATCHOULI_DIR)/src/main.cpp $(PATCHOULI_HEADERS) $(PATCHOULI_DIR)/third_party/nlohmann/json.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(PATCHOULI_LDLIBS) -o $@

$(BETAMAX_OLD_TARGET): $(BETAMAX_OLD_SOURCE) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(EPSILONREPAIR_TARGET): $(EPSILONREPAIR_SOURCE) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -include algorithm $< $(LDFLAGS) -o $@

$(BUILD_DIR)/validate_old_%$(EXE_SUFFIX): $(VALIDATOR_SOURCE) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -DVALIDATOR_FILE_INPUT=1 $< $(LDFLAGS) -o $@

$(BUILD_DIR)/validate_%$(EXE_SUFFIX): $(VALIDATOR_SOURCE) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(BUILD_DIR)/boundary_validate_%$(EXE_SUFFIX): $(BOUNDARY_VALIDATOR_SOURCE) Makefile | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< $(LDFLAGS) -o $@

generate:
	$(PYTHON) shared-suite/generate_cases.py

smoke: all generate
	$(PYTHON) run_smoke_tests.py

test: all generate
	$(PYTHON) run_tests.py

clean:
	$(PYTHON) -c "import shutil; shutil.rmtree(r'$(BUILD_DIR)', ignore_errors=True); shutil.rmtree('results', ignore_errors=True)"
