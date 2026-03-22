# ============================================================================
# NeoGPU Verilator Makefile
# ============================================================================

# 参数设置
VERILATOR = verilator
VL_FLAGS = --cc --exe --build --trace \
           -Wall -Wno-DECLFILENAME -Wno-MODDUP \
           -CFLAGS "-std=c++17 -O2" \
           -LDFLAGS "-lpthread" \
           --timescale 1ps/1ps \
           --x-assign unique \
           --x-initial unique \
           --clk clk_sys \
           --clk clk_pcie

# 源文件
VERILOG_SRC = ../neo_gpu_top.v
CPP_SRC = tb_neo_gpu.cpp

# 输出目录
OBJ_DIR = obj_dir
EXE = $(OBJ_DIR)/Vneo_gpu_top

# 默认目标
all: $(EXE)

# 编译规则
$(EXE): $(VERILOG_SRC) $(CPP_SRC)
	@echo ">> Verilating..."
	$(VERILATOR) $(VL_FLAGS) $(VERILOG_SRC) $(CPP_SRC)
	@echo ">> Build complete: $(EXE)"

# 运行仿真
run: $(EXE)
	@echo ">> Running simulation..."
	./$(EXE)
	@echo ">> Waveform saved to waveform.vcd"

# 查看波形 (GTKWave)
wave: run
	gtkwave neo_gpu_waveform.vcd &

# 清理
clean:
	rm -rf $(OBJ_DIR)
	rm -f *.vcd
	rm -f *.log

# 调试编译
debug: VL_FLAGS += -DDEBUG -CFLAGS "-g -O0"
debug: all

.PHONY: all run clean wave debug