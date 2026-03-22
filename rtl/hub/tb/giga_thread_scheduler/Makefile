# Makefile for Verilator simulation

# Verilator 配置
VERILATOR = verilator
VERILATOR_FLAGS = -Wall -Wno-UNUSEDSIGNAL --trace --cc --exe --build \
                  --top-module giga_thread_scheduler

# 源文件
VERILOG_SRC = ../../giga_thread_scheduler.v
CPP_SRC = tb_main.cpp

# 目标
TARGET = Vgiga_thread_scheduler

.PHONY: all clean run wave

all: $(TARGET)

$(TARGET): $(VERILOG_SRC) $(CPP_SRC)
	$(VERILATOR) $(VERILATOR_FLAGS) $(VERILOG_SRC) $(CPP_SRC)
	./obj_dir/$(TARGET)

run: $(TARGET)
	./obj_dir/$(TARGET)

wave: run
	gtkwave waveform.vcd &

clean:
	rm -rf obj_dir waveform.vcd