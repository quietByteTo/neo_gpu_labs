
---

## 3. Makefile

```makefile
# Makefile for SM Frontend Verilator Testbench

#=============================================================================
# Configuration
#=============================================================================
VERILATOR   = verilator
TARGET      = sm_frontend_tb
BUILD_DIR   = build

# RTL files (adjust paths as needed)
RTL_DIR     = ../rtl
RTL_FILES   = $(RTL_DIR)/sm_frontend.v \
              $(RTL_DIR)/warp_scheduler.v \
              $(RTL_DIR)/scoreboard.v \
              $(RTL_DIR)/priority_encoder.v

# Testbench
TB_CPP      = tb_sm_frontend.cpp

# Verilator flags
VFLAGS      = --cc --exe --build \
              --trace \
              --trace-structs \
              --trace-params \
              -Wall \
              -Wno-UNUSED \
              -Wno-DECLFILENAME \
              -Wno-BLKSEQ \
              -O2 \
              -x-assign fast \
              -x-initial fast \
              --noassert \
              -Mdir $(BUILD_DIR) \
              --top-module sm_frontend

# C++ flags
CXXFLAGS    = -std=c++17 -O2 -I$(VERILATOR_ROOT)/include

#=============================================================================
# Targets
#=============================================================================

.PHONY: all build run wave clean debug coverage help

all: build

$(BUILD_DIR)/Vsm_frontend.mk: $(RTL_FILES) $(TB_CPP)
	@mkdir -p $(BUILD_DIR)
	$(VERILATOR) $(VFLAGS) $(RTL_FILES) $(TB_CPP) -o ../$(TARGET)

build: $(BUILD_DIR)/Vsm_frontend.mk
	@echo "Building testbench..."
	@make -C $(BUILD_DIR) -f Vsm_frontend.mk

run: build
	@echo "Running tests..."
	./$(TARGET)

wave: run
	@echo "Opening waveform..."
	gtkwave sm_frontend.vcd -a gtkwave_config.gtkw &

# Debug build with assertions and no optimization
debug:
	@mkdir -p $(BUILD_DIR)_debug
	$(VERILATOR) --cc --exe --build --trace \
		-CFLAGS "-std=c++17 -O0 -g" \
		-Mdir $(BUILD_DIR)_debug \
		--top-module sm_frontend \
		$(RTL_FILES) $(TB_CPP) -o ../$(TARGET)_debug
	./$(TARGET)_debug

# Coverage build
coverage:
	@mkdir -p $(BUILD_DIR)_cov
	$(VERILATOR) --cc --exe --build --trace --coverage \
		-CFLAGS "-std=c++17 -O2" \
		-Mdir $(BUILD_DIR)_cov \
		--top-module sm_frontend \
		$(RTL_FILES) $(TB_CPP) -o ../$(TARGET)_cov
	./$(TARGET)_cov
	verilator_coverage --write-info coverage.info $(BUILD_DIR)_cov/*.dat
	genhtml coverage.info --output-directory coverage_report

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR)_debug $(BUILD_DIR)_cov
	rm -f $(TARGET) $(TARGET)_debug $(TARGET)_cov
	rm -f sm_frontend.vcd coverage.info
	rm -rf coverage_report

help:
	@echo "SM Frontend Testbench Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  make build    - Build the testbench"
	@echo "  make run      - Build and run all tests"
	@echo "  make wave     - Run tests and open waveform viewer"
	@echo "  make debug    - Build with debug symbols and run"
	@echo "  make coverage - Build with coverage and generate report"
	@echo "  make clean    - Remove all build artifacts"
	@echo "  make help     - Show this help message"