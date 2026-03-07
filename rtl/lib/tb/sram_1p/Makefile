# ============================================================================
# Verilator Makefile for sram_1p
# ============================================================================

MODULE = sram_1p
VERILOG_SRC = ../../$(MODULE).v
CPP_SRC = tb_$(MODULE).cpp
OBJ_DIR = obj_dir
TARGET = $(MODULE)_sim

# Verilator 选项（使用小参数便于测试）
VERILATOR_FLAGS = --cc --exe --build \
                  -Wall \
                  -Wno-UNUSED \
                  -Wno-DECLFILENAME \
                  --trace \
                  -GADDR_W=8 \
                  -GDATA_W=32 \
                  -GDEPTH=256 \
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