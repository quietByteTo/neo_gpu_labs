# Makefile for Scoreboard Verilator Test

VERILATOR = verilator
VERILATOR_FLAGS = -Wall --trace --cc --exe --build -j 0 -Wno-WIDTH

.PHONY: all clean run wave

all: Vscoreboard

Vscoreboard: ../../scoreboard.v tb_scoreboard.cpp
	$(VERILATOR) $(VERILATOR_FLAGS) ../../scoreboard.v tb_scoreboard.cpp --top-module scoreboard

run: Vscoreboard
	./obj_dir/Vscoreboard

wave: run
	gtkwave scoreboard.vcd &

clean:
	rm -rf obj_dir scoreboard.vcd