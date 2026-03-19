# Makefile
VERILATOR = verilator
VFLAGS = --cc --exe --build --trace -j 0  -Wall \
         -Wno-UNUSEDPARAM \
         -Wno-UNUSEDSIGNAL
TOP = ../../sm_backend
TB = tb_sm_backend.cpp

.PHONY: all clean run

all: obj_dir/V$(TOP)

obj_dir/V$(TOP): $(TOP).v $(TB)
	$(VERILATOR) -Wall $(VFLAGS)  $(TOP).v $(TB)

run: obj_dir/V$(TOP)
	./obj_dir/V$(TOP)

clean:
	rm -rf obj_dir *.vcd