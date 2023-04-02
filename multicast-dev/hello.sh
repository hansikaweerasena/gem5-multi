PATH_TO_GEM5='../gem5-multicast'

$PATH_TO_GEM5/build/X86/gem5.opt \
	--stats-file=hello-stats \
	$PATH_TO_GEM5/configs/example/se.py \
	--ruby \
	--network=garnet \
	--cpu-type=TimingSimpleCPU \
	--caches \
	--l2cache \
	--num-l2cache=16 \
	--num-cpus=16 \
	--num-dirs=4 \
	--topology=MeshDirCorners_XY \
	--mesh-rows=4 \
	--cmd=$PATH_TO_GEM5/tests/test-progs/hello/bin/x86/linux/hello
