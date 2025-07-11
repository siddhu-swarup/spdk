#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2017 Intel Corporation
#  Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.
#
testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../../..)
source $rootdir/test/common/autotest_common.sh
source $rootdir/test/nvmf/common.sh

MALLOC_BDEV_SIZE=64
MALLOC_BLOCK_SIZE=512

function starttarget() {
	nvmftestinit

	# Start the target
	nvmfappstart -m 0x1E

	$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

	num_subsystems=({1..10})

	timing_enter create_subsystems
	# Create subsystems
	rm -rf $testdir/rpcs.txt
	for i in "${num_subsystems[@]}"; do
		cat <<- EOL >> $testdir/rpcs.txt
			bdev_malloc_create $MALLOC_BDEV_SIZE $MALLOC_BLOCK_SIZE -b Malloc$i
			nvmf_create_subsystem nqn.2016-06.io.spdk:cnode$i -a -s SPDK$i
			nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode$i Malloc$i
			nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode$i -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
		EOL
	done
	$rpc_py < $testdir/rpcs.txt
	timing_exit create_subsystems

}

function stoptarget() {
	rm -f ./local-job0-0-verify.state
	rm -rf $testdir/bdevperf.conf
	rm -rf $testdir/rpcs.txt

	nvmftestfini
}

function waitforio() {
	# $1 = RPC socket
	if [ -z "$1" ]; then
		exit 1
	fi
	# $2 = bdev name
	if [ -z "$2" ]; then
		exit 1
	fi
	local ret=1
	local i
	for ((i = 10; i != 0; i--)); do
		read_io_count=$($rpc_py -s $1 bdev_get_iostat -b $2 | jq -r '.bdevs[0].num_read_ops')
		# A few I/O will happen during initial examine.  So wait until at least 100 I/O
		#  have completed to know that bdevperf is really generating the I/O.
		if [ $read_io_count -ge 100 ]; then
			ret=0
			break
		fi
		sleep 0.25
	done
	return $ret
}

# Test 1: Kill the initiator unexpectedly with no I/O outstanding
function nvmf_shutdown_tc1() {
	starttarget

	# Run bdev_svc, which connects but does not issue I/O
	$rootdir/test/app/bdev_svc/bdev_svc -m 0x1 -i 1 -r /var/tmp/bdevperf.sock --json <(gen_nvmf_target_json "${num_subsystems[@]}") &
	perfpid=$!
	waitforlisten $perfpid /var/tmp/bdevperf.sock
	$rpc_py -s /var/tmp/bdevperf.sock framework_wait_init

	# Kill bdev_svc
	kill -9 $perfpid || true
	rm -f /var/run/spdk_bdev1

	# Verify the target stays up
	sleep 1
	kill -0 $nvmfpid

	# Connect with bdevperf and confirm it works
	$rootdir/build/examples/bdevperf --json <(gen_nvmf_target_json "${num_subsystems[@]}") \
		-q 64 -o 65536 -w verify -t 1

	stoptarget
}

# Test 2: Kill initiator unexpectedly with I/O outstanding
function nvmf_shutdown_tc2() {
	starttarget

	# Run bdevperf
	$rootdir/build/examples/bdevperf -r /var/tmp/bdevperf.sock --json <(gen_nvmf_target_json "${num_subsystems[@]}") -q 64 -o 65536 -w verify -t 10 &
	perfpid=$!
	waitforlisten $perfpid /var/tmp/bdevperf.sock
	$rpc_py -s /var/tmp/bdevperf.sock framework_wait_init

	waitforio /var/tmp/bdevperf.sock Nvme1n1

	# Kill bdevperf half way through
	killprocess $perfpid

	# Verify the target stays up
	sleep 1
	kill -0 $nvmfpid

	stoptarget
}

# Test 3: Kill the target unexpectedly with I/O outstanding
function nvmf_shutdown_tc3() {
	starttarget

	# Run bdevperf
	$rootdir/build/examples/bdevperf -r /var/tmp/bdevperf.sock --json <(gen_nvmf_target_json "${num_subsystems[@]}") -q 64 -o 65536 -w verify -t 10 &
	perfpid=$!
	waitforlisten $perfpid /var/tmp/bdevperf.sock
	$rpc_py -s /var/tmp/bdevperf.sock framework_wait_init

	# Expand the trap to clean up bdevperf if something goes wrong
	trap 'process_shm --id $NVMF_APP_SHM_ID; kill -9 $perfpid || true; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

	waitforio /var/tmp/bdevperf.sock Nvme1n1

	# Kill the target half way through
	killprocess $nvmfpid
	sleep 1
	NOT wait $perfpid

	stoptarget
}

# Test 4: Kill the target unexpectedly with I/O outstanding, highly fragmented payload
function nvmf_shutdown_tc4() {
	starttarget

	# Run nvme_perf with highly fragmented payload
	$rootdir/build/bin/spdk_nvme_perf -q 128 -o 45056 -O 4096 -w randwrite -t 20 -r "trtype:$TEST_TRANSPORT adrfam:IPV4 traddr:$NVMF_FIRST_TARGET_IP trsvcid:$NVMF_PORT" -P 4 &
	perfpid=$!
	sleep 5
	# Expand the trap to clean up bdevperf if something goes wrong
	trap 'process_shm --id $NVMF_APP_SHM_ID; kill -9 $perfpid || true; nvmftestfini; exit 1' SIGINT SIGTERM EXIT

	# Kill the target half way through
	killprocess $nvmfpid
	sleep 1
	# Due to IOs are completed with errors, perf exits with bad status
	NOT wait $perfpid
	stoptarget
}

run_test "nvmf_shutdown_tc1" nvmf_shutdown_tc1
run_test "nvmf_shutdown_tc2" nvmf_shutdown_tc2
run_test "nvmf_shutdown_tc3" nvmf_shutdown_tc3

# Temporarily disable for tcp due to issue #3522
if [[ "$TEST_TRANSPORT" != "tcp" ]]; then
	run_test "nvmf_shutdown_tc4" nvmf_shutdown_tc4
fi

trap - SIGINT SIGTERM EXIT
