# Makefile for Register File Verilator Test

VERILATOR = verilator
VERILATOR_FLAGS = -Wall --trace --cc --exe --build -j 0 -Wno-WIDTH -Wno-UNUSEDSIGNAL  -Wno-UNUSEDPARAM

.PHONY: all clean run wave

all: Vregister_file

Vregister_file: ../../register_file.v ../../../lib/sram_2p.v tb_register_file.cpp
	$(VERILATOR) $(VERILATOR_FLAGS) ../../register_file.v ../../../lib/sram_2p.v tb_register_file.cpp --top-module register_file

run: Vregister_file
	./obj_dir/Vregister_file

wave: run
	gtkwave register_file.vcd &

clean:
	rm -rf obj_dir register_file.vcd