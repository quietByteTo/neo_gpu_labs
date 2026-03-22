# Verilator 仿真：sm_core_top（在 tb/sm_core_top 目录执行 make / make run）
VERILATOR = verilator
SM        = ../..

VFLAGS = --cc --exe --build --trace -j 0 -Wall \
         -Wno-UNUSEDPARAM \
         -Wno-UNUSEDSIGNAL \
         -Wno-PINMISSING \
         -CFLAGS -std=c++14

# 依赖顺序：子模块在前可减少部分工具告警；也可仅用 -y 搜索
# priority_encoder 在 rtl/lib（warp_scheduler 依赖）；路径相对本 Makefile
LIB = ../../../lib
RTL_SRCS = \
	$(LIB)/priority_encoder.v \
	$(LIB)/sram_2p.v \
	$(SM)/warp_scheduler.v \
	$(SM)/scoreboard.v \
	$(SM)/register_file.v \
	$(SM)/sm_frontend.v \
	$(SM)/sm_backend.v \
	$(SM)/alu_pipeline.v \
	$(SM)/sfu.v \
	$(SM)/lsu.v \
	$(SM)/lds_unit.v \
	$(SM)/sm_core_top.v

TB = tb_sm_core_top.cpp
TOP = sm_core_top

.PHONY: all clean run

all: obj_dir/V$(TOP)

obj_dir/V$(TOP): $(RTL_SRCS) $(TB)
	$(VERILATOR) $(VFLAGS) $(RTL_SRCS) $(TB) --top-module $(TOP)

run: obj_dir/V$(TOP)
	./obj_dir/V$(TOP)

clean:
	rm -rf obj_dir *.vcd
