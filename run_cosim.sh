#!/bin/bash
# run_cosim.sh - Launch co-simulation environment

# 1. Compile Verilator model
verilator --cc --exe --build \
    -CFLAGS "-std=c++17 -pthread" \
    -LDFLAGS "-lrt -pthread" \
    --top-module neo_gpu_top \
    -Mdir obj_dir \
    rtl/top/neo_gpu_top.v \
    rtl/sm/*.v rtl/gpc/*.v rtl/hub/*.v  rtl/mem/*.v rtl/lib/*.v \
    tb/tb_neo_gpu_top.cpp

# 2. Start Verilator (background)
#./obj_dir/Vneo_gpu_top &

#VERILATOR_PID=$!

# 3. Start QEMU with NeoGPU device (after Verilator initializes SHM)
#sleep 2
#qemu-system-x86_64 \
#    -m 4G \
#    -device neogpu \
#    -kernel linux_image.bin \
#    -append "console=ttyS0 root=/dev/sda" \
#    -serial stdio
#
## 4. Cleanup
#kill $VERILATOR_PID 2>/dev/null
#rm -f /dev/shm/gpu_cosim_shm