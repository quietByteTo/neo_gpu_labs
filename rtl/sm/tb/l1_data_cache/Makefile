MODULE = l1_data_cache
VERILOG_SRC = ../../$(MODULE).v
CPP_SRC = tb_$(MODULE).cpp
OBJ_DIR = obj_dir
TARGET = $(MODULE)_sim

VERILATOR_FLAGS = --cc --exe --build \
                  -Wall \
                  -Wno-UNUSED \
                  -Wno-DECLFILENAME \
                  --trace \
                  -GCACHE_SIZE_KB=64 \
                  -GLINE_SIZE=128 \
                  -GNUM_WAYS=4 \
                  -GADDR_W=64 \
                  -GDATA_W=128 \
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