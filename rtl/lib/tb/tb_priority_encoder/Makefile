# ============================================================================
# Verilator Makefile for priority_encoder
# ============================================================================

# 模块名称
MODULE = priority_encoder

# 源文件
VERILOG_SRC = ../../$(MODULE).v
CPP_SRC = tb_$(MODULE).cpp

# 输出目录
OBJ_DIR = obj_dir

# 可执行文件
TARGET = $(MODULE)_sim

# Verilator 选项
VERILATOR_FLAGS = --cc --exe --build \
                  -Wall \
                  -Wno-UNUSED \
                  -Wno-DECLFILENAME \
                  --trace \
                  -CFLAGS "-std=c++14 -O2" \
                  -Mdir $(OBJ_DIR)

# 默认目标
all: $(TARGET)

# 生成 C++ 并编译
$(TARGET): $(VERILOG_SRC) $(CPP_SRC)
	verilator $(VERILATOR_FLAGS) $(VERILOG_SRC) $(CPP_SRC)
	cp $(OBJ_DIR)/V$(MODULE) $(TARGET)

# 运行测试
run: $(TARGET)
	./$(TARGET)

# 查看波形（如果有 gtkwave）
wave: run
	gtkwave waveform.vcd &

# 清理
clean:
	rm -rf $(OBJ_DIR) $(TARGET) waveform.vcd

# 仅进行 lint 检查
lint:
	verilator --lint-only -Wall $(VERILOG_SRC)

.PHONY: all run wave clean lints