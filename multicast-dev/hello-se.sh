../build/X86/gem5.debug \
	--stats-file=se-test-stats \
	--debug-flags=GarnetMulticast \
	../configs/deprecated/example/se.py \
	--ruby \
	--network=garnet \
	--cpu-type=TimingSimpleCPU \
	--caches \
	--l2cache \
	--num-l2cache=16 \
	--num-cpus=16 \
	--num-dirs=16 \
	--topology=Mesh_XY \
	--mesh-rows=4 \
	--multicast \
	--cmd="../tests/test-progs/hello/bin/x86/linux/hello"
