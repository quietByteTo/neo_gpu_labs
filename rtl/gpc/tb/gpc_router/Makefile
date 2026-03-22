# Verilator 仿真：gpc_router（在 rtl/gpc/tb/gpc_router 目录执行 make / make run）
VERILATOR = verilator
GPC       = ../..

VFLAGS = --cc --exe --build --trace -j 0 -Wall \
         -Wno-UNUSEDPARAM \
         -Wno-UNUSEDSIGNAL \
         -Wno-PINMISSING \
         -CFLAGS -std=c++14

RTL_SRCS = $(GPC)/gpc_router.v
TB       = tb_gpc_router.cpp
TOP      = gpc_router

.PHONY: all clean run

all: obj_dir/V$(TOP)

obj_dir/V$(TOP): $(RTL_SRCS) $(TB)
	$(VERILATOR) $(VFLAGS) $(RTL_SRCS) $(TB) --top-module $(TOP)

run: obj_dir/V$(TOP)
	./obj_dir/V$(TOP)

clean:
	rm -rf obj_dir *.vcd
