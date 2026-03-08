# Makefile for Verilator Simulation

# Verilator flags
VERILATOR = verilator
VERILATOR_FLAGS = -Wall --trace --cc --exe --build -j 0

# Source files
VERILOG_SRC = ../../../lib/priority_encoder.v ../../warp_scheduler.v
CPP_SRC = tb_warp_scheduler.cpp

# Output
TARGET = Vwarp_scheduler

.PHONY: all clean run wave

all: $(TARGET)

$(TARGET): $(VERILOG_SRC) $(CPP_SRC)
	$(VERILATOR) $(VERILATOR_FLAGS) $(VERILOG_SRC) $(CPP_SRC) --top-module warp_scheduler

run: $(TARGET)
	./obj_dir/$(TARGET)

wave: run
	gtkwave waveform.vcd &

clean:
	rm -rf obj_dir waveform.vcd