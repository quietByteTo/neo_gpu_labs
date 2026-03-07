verilator --lint-only ../priority_encoder.v 
verilator --lint-only -Wno-WIDTHTRUNC ../rr_arbiter.v 
verilator --lint-only ../sram_1p.v
verilator --lint-only ../sram_2p.v
verilator --lint-only ../sync_fifo.v

