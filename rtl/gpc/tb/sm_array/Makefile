# Verilator 仿真：sm_array（在 rtl/gpc/tb/sm_array 目录执行 make / make run）
VERILATOR = verilator
SM        = ../../../sm
LIB       = ../../../lib
GPC       = ../..

VFLAGS = --cc --exe --build --trace -j 0 -Wall \
         -Wno-UNUSEDPARAM \
         -Wno-UNUSEDSIGNAL \
         -Wno-PINMISSING \
         -CFLAGS -std=c++14

RTL_SRCS = \
	$(LIB)/priority_encoder.v \
	$(LIB)/sram_2p.v \
	$(LIB)/sync_fifo.v \
	$(LIB)/rr_arbiter.v \
	$(SM)/warp_scheduler.v \
	$(SM)/scoreboard.v \
	$(SM)/register_file.v \
	$(SM)/sm_frontend.v \
	$(SM)/sm_backend.v \
	$(SM)/alu_pipeline.v \
	$(SM)/sfu.v \
	$(SM)/lsu.v \
	$(SM)/lds_unit.v \
	$(SM)/sm_core_top.v \
	$(GPC)/sm_array.v

TB = tb_sm_array.cpp
TOP = sm_array

.PHONY: all clean run

all: obj_dir/V$(TOP)

obj_dir/V$(TOP): $(RTL_SRCS) $(TB)
	$(VERILATOR) $(VFLAGS) $(RTL_SRCS) $(TB) --top-module $(TOP)

run: obj_dir/V$(TOP)
	./obj_dir/V$(TOP)

clean:
	rm -rf obj_dir *.vcd
