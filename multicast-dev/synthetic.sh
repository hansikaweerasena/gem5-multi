../build/NULL/gem5.debug \
	--stats-file=synthetic-multicast-stats.txt \
	--debug-flags=GarnetMulticast \
	../configs/example/garnet_synth_traffic.py \
	--network=garnet \
	--num-cpus=16 \
	--num-dirs=16 \
	--topology=Mesh_XY \
	--mesh-rows=4 \
	--multicast \
	--sim-cycles=1000000000 \
	--synthetic=uniform_random \
	--injectionrate=0.01 \
	--debug-break=2673000 \
	
