all:
	verilator --cc --exe --build -Wall -Wno-CASEINCOMPLETE \
		-GNUM_LANES=32 -GDATA_W=32 \
		-CFLAGS "-std=c++14 -O2" \
		--trace -Mdir obj_dir \
		cordic_sincos.v tb_cordic.cpp
	
	./obj_dir/Vcordic_sincos

clean:
	rm -rf obj_dir cordic.vcd

.PHONY: all clean