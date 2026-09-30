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
BETAMAX_SOURCE := sources/betamax/main.cpp
EREPAIR_SOURCE := sources/epsilonrepair/erepair2.cpp
FORMATS := date time url isbn ipv4 ipv6
PATCHOULI := $(BUILD_DIR)/patchouli$(EXE_SUFFIX)
BETAMAX := $(BUILD_DIR)/betamax$(EXE_SUFFIX)
EREPAIR := $(BUILD_DIR)/erepair$(EXE_SUFFIX)
VALIDATORS := $(addprefix $(BUILD_DIR)/validate_,$(addsuffix $(EXE_SUFFIX),$(FORMATS)))
FILE_VALIDATORS := $(addprefix $(BUILD_DIR)/validate_betamax_,$(addsuffix $(EXE_SUFFIX),$(FORMATS)))
BOUNDARY_VALIDATORS := $(addprefix $(BUILD_DIR)/boundary_validate_,$(addsuffix $(EXE_SUFFIX),$(FORMATS)))
TEST_CASES := shared-suite/test-cases/test-cases.json
COMMON_HEADER := sources/common/oracle_process.hpp
HEADERS := $(COMMON_HEADER) $(wildcard $(PATCHOULI_DIR)/include/patchouli/*.hpp) $(PATCHOULI_DIR)/third_party/nlohmann/json.hpp
CPPFLAGS := -Isources/common -I$(PATCHOULI_DIR)/include -I$(PATCHOULI_DIR)/third_party
CXXFLAGS ?= -O2 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic

.PHONY: all patchouli betamax erepair generate smoke smoke-patchouli smoke-betamax smoke-erepair test test-patchouli test-betamax test-erepair check clean
all: $(PATCHOULI) $(VALIDATORS)

patchouli: $(PATCHOULI) $(VALIDATORS)

betamax: $(BETAMAX) $(FILE_VALIDATORS) $(VALIDATORS)

erepair: $(EREPAIR) $(BOUNDARY_VALIDATORS) $(VALIDATORS)

$(BUILD_DIR):
	$(PYTHON) -c "from pathlib import Path; Path(r'$@').mkdir(parents=True, exist_ok=True)"

$(PATCHOULI): $(PATCHOULI_DIR)/src/main.cpp $(HEADERS) Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

$(BETAMAX): $(BETAMAX_SOURCE) $(COMMON_HEADER) Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(EREPAIR): $(EREPAIR_SOURCE) $(COMMON_HEADER) Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(BUILD_DIR)/validate_%$(EXE_SUFFIX): validators/validator.cpp $(PATCHOULI_DIR)/third_party/nlohmann/json.hpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(FILE_VALIDATORS): $(BUILD_DIR)/validate_betamax_%$(EXE_SUFFIX): validators/betamax_validator.cpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(BUILD_DIR)/boundary_validate_%$(EXE_SUFFIX): validators/boundary_validator.cpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@

$(BUILD_DIR)/patchouli_tests$(EXE_SUFFIX): $(PATCHOULI_DIR)/tests/patchouli_tests.cpp $(HEADERS) Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -UNDEBUG $< $(LDFLAGS) $(LDLIBS) -o $@

generate:
	$(PYTHON) shared-suite/generate_cases.py

$(TEST_CASES): shared-suite/suite-config.json shared-suite/generate_cases.py
	$(PYTHON) shared-suite/generate_cases.py

smoke: patchouli betamax erepair $(TEST_CASES)
	$(PYTHON) run_smoke_tests.py

smoke-patchouli: patchouli $(TEST_CASES)
	$(PYTHON) run_patchouli.py --smoke

smoke-betamax: betamax $(TEST_CASES)
	$(PYTHON) run_betamax.py --smoke

smoke-erepair: erepair $(TEST_CASES)
	$(PYTHON) run_erepair.py --smoke

test: patchouli betamax erepair $(TEST_CASES)
	$(PYTHON) run_tests.py

test-patchouli: patchouli $(TEST_CASES)
	$(PYTHON) run_patchouli.py

test-betamax: betamax $(TEST_CASES)
	$(PYTHON) run_betamax.py

test-erepair: erepair $(TEST_CASES)
	$(PYTHON) run_erepair.py

check: $(BUILD_DIR)/patchouli_tests$(EXE_SUFFIX)
	./$(BUILD_DIR)/patchouli_tests$(EXE_SUFFIX)

clean:
	$(PYTHON) -c "import shutil; shutil.rmtree('build', ignore_errors=True); shutil.rmtree('results', ignore_errors=True)"
