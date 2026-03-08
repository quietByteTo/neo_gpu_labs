# Makefile for Tensor Core Verilator Test

VERILATOR = verilator
VERILATOR_FLAGS = -Wall --trace --cc --exe --build -j 0 -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM -Wno-BLKSEQ

.PHONY: all clean run wave

all: Vtensor_core

Vtensor_core: ../../tensor_core.v tb_tensor_core.cpp
	$(VERILATOR) $(VERILATOR_FLAGS) ../../tensor_core.v tb_tensor_core.cpp --top-module tensor_core

run: Vtensor_core
	./obj_dir/Vtensor_core

wave: run
	gtkwave tensor_core.vcd &

clean:
	rm -rf obj_dir tensor_core.vcd