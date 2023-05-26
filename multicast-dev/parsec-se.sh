../build/X86/gem5.debug \
    --debug-break=2673000 \
    --stats-file=parsec-se-stats.txt \
    --debug-flags='GarnetMulticast' \
    ../configs/deprecated/example/se.py \
    --ruby \
    --network=garnet \
    --cpu-type=TimingSimpleCPU \
    --caches \
    --l1d_size=16kB \
    --l1i_size=16kB \
    --l2cache \
    --num-l2cache=16 \
    --num-cpus=16 \
    --num-dirs=16 \
    --topology=Mesh_XY \
    --mesh-rows=4 \
    --cpu-clock=2GHz \
    --mem-size=4GB \
    --cmd=../../parsec-benchmark/pkgs/apps/blackscholes/inst/amd64-linux.gcc/bin/blackscholes \
    --options="4 ../../parsec-benchmark/pkgs/apps/blackscholes/run/in_4.txt prices.txt" \
    --multicast \

