# Compile RCP-only version
rcp:
	verilator --cc --exe --build -Wall \
		-GNUM_LANES=32 -GDATA_W=32 \
		-CFLAGS "-std=c++14 -O2" \
		--trace -Mdir obj_dir \
		sfu.v tb_sfu_rcp.cpp
	./obj_dir/Vsfu

# Run with trace
rcp_trace:
	verilator --cc --exe --build -Wall -DTRACE \
		-GNUM_LANES=32 -GDATA_W=32 \
		-CFLAGS "-std=c++14 -O2" \
		--trace -Mdir obj_dir \
		sfu_rcp_only.v tb_sfu_rcp.cpp
	./obj_dir/Vsfu
	gtkwave sfu_rcp.vcd &