# ============================================================================
# Verilator Makefile for rr_arbiter
# ============================================================================

MODULE = rr_arbiter
VERILOG_SRC = ../../$(MODULE).v
CPP_SRC = tb_$(MODULE).cpp
OBJ_DIR = obj_dir
TARGET = $(MODULE)_sim

# Verilator 选项
VERILATOR_FLAGS = --cc --exe --build \
                  -Wall \
                  -Wno-UNUSED \
                  -Wno-DECLFILENAME \
                  --trace \
                  -CFLAGS "-std=c++14 -O2" \
                  -Mdir $(OBJ_DIR)

all: $(TARGET)

$(TARGET): $(VERILOG_SRC) $(CPP_SRC)
	verilator $(VERILATOR_FLAGS) $(VERILOG_SRC) $(CPP_SRC)
	cp $(OBJ_DIR)/V$(MODULE) $(TARGET)

run: $(TARGET)
	./$(TARGET)

wave: run
	gtkwave waveform.vcd &

clean:
	rm -rf $(OBJ_DIR) $(TARGET) waveform.vcd

lint:
	verilator --lint-only -Wall $(VERILOG_SRC)

.PHONY: all run wave clean lint