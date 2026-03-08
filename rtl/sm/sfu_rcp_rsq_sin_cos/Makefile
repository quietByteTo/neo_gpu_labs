all:
	verilator --cc --exe --build -Wall \
		-Wno-CASEINCOMPLETE -Wno-UNUSEDPARAM -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
		-GNUM_LANES=32 -GDATA_W=32 \
		-CFLAGS "-std=c++14 -O2" \
		--trace -Mdir obj_dir \
		sfu.v tb_sfu.cpp
	
	./obj_dir/Vsfu

clean:
	rm -rf obj_dir sfu.vcd

.PHONY: all clean