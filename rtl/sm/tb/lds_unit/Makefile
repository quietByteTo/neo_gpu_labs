# Makefile for LDS Unit Verilator Test

VERILATOR = verilator
VERILATOR_FLAGS = -Wall --trace --cc --exe --build -j 0 -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM

.PHONY: all clean run wave

all: Vlds_unit

Vlds_unit: ../../lds_unit.v tb_lds_unit.cpp
	$(VERILATOR) $(VERILATOR_FLAGS) ../../lds_unit.v tb_lds_unit.cpp --top-module lds_unit

run: Vlds_unit
	./obj_dir/Vlds_unit

wave: run
	gtkwave lds_unit.vcd &

clean:
	rm -rf obj_dir lds_unit.vcd