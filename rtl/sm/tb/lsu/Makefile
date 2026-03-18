# ============================================================================
# Makefile for LSU Verilator Simulation
# ============================================================================

TOP = lsu
SRC = ../../lsu.v
CSRC = sim_main.cpp

OBJ_DIR = obj_dir
EXEC = $(OBJ_DIR)/V$(TOP)

.PHONY: all clean sim wave

all: $(EXEC)

$(EXEC): $(SRC) $(CSRC)
	@echo "Building with Verilator..."
	verilator --cc --trace --exe --build -j 0 -Wall \
		-Wno-UNUSED -Wno-DECLFILENAME \
		--top-module $(TOP) \
		$(SRC) $(CSRC) \
		-CFLAGS "-std=c++17"
	@echo "Build complete: $(EXEC)"

sim: $(EXEC)
	@echo "Running simulation..."
	./$(EXEC)
	@echo ""
	@echo "Waveform generated: lsu_waveform.vcd"

wave: sim
	@if command -v gtkwave >/dev/null 2>&1; then \
		echo "Opening GTKWave..."; \
		gtkwave lsu_waveform.vcd & \
	else \
		echo "GTKWave not found. Install with: sudo apt-get install gtkwave"; \
	fi

clean:
	rm -rf $(OBJ_DIR) *.vcd
	@echo "Cleaned build files"

help:
	@echo "Targets:"
	@echo "  all    - Build simulation (default)"
	@echo "  sim    - Build and run simulation"
	@echo "  wave   - Run simulation and open waveform viewer"
	@echo "  clean  - Remove build files"