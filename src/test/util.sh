#
# This file is included by foo.run test-driver files.  It provides
# some helpers for common test operations.  A driver file foo.run
# will want to include this file as follows
#
#  source util.sh foo "$@"
#
# Most tests are either "compare_test"s, which check that record and
# replay successfully complete and the output is the same, or,
# "debug_test"s, which launch a debugger script.  So the remainder of
# your test runner probably looks like
#
#  compare_test  # or, |debug_test|
#
# Test runners may set the environment variable $RECORD_ARGS to pass
# arguments to rr for recording.  This is only useful for tweaking the
# scheduler, don't use it for anything else.
#

#  delay_kill <sig> <delay_secs> <proc>
#
# Deliver the signal |sig|, after waiting |delay_secs| seconds, to the
# process named |proc|.  If there's more than |proc|, the signal is
# not delivered.
function delay_kill { sig=$1; delay_secs=$2; proc=$3
    sleep $delay_secs

    pid=""
    for i in `seq 1 5`; do
	live=`ps ax -o 'pid= cmd=' | awk '{print $1 " " $2}' | grep $proc`
	num=`echo "$live" | wc -l`
	if [[ "$num" -eq 1 ]]; then
	    pid=`echo "$live" | awk '{print $1}'`
	    break
	fi
	sleep 0.1
    done

    if [[ "$num" -gt 1 ]]; then
	leave_data=y
	echo FAILED: "$num" of "'$proc'" >&2
	exit 1
    elif [[ -z "$pid" ]]; then
	leave_data=y
	echo FAILED: process "'$proc'" not located >&2
	exit 1
    fi

    kill -s $sig $pid
    if [[ $? != 0 ]]; then
	leave_data=y
	echo FAILED: signal $sig not delivered to "'$proc'" >&2
	exit 1
    fi

    echo Successfully delivered signal $sig to "'$proc'"
}

function fatal { #...
    echo "$@" >&2
    exit 1
}

function onexit {
    cd
    if [[ "$leave_data" != "y" ]]; then
	rm -rf $workdir
    else
	echo Test $TESTNAME failed, leaving behind $workdir.
	echo To replay the failed test, run
	echo " " _RR_TRACE_DIR="$workdir" rr replay
    fi
}

function parent_pid_of { pid=$1
    ps -p $pid -o ppid=
}

function usage {
    echo Usage: "util.sh TESTNAME [LIB_ARG] [OBJDIR]"
}

# Don't bind record/replay tracees to the same logical CPU.  When we
# do that, the tests take impractically long to run.
#
# TODO: find a way to run faster with CPU binding
GLOBAL_OPTIONS="-u --check-cached-mmaps"

TESTNAME=$1
LIB_ARG=$2
OBJDIR=$3

# The temporary directory we create for this test run.
workdir=
# Did the test pass?  If not, then we'll leave the recording and
# output around for developers to debug.
leave_data=n
# The unique ID allocated to this test directory.
nonce=

# Set up the environment and working directory.
if [[ "$TESTNAME" == "" ]]; then
    usage
    fatal FAILED: test name not passed to script
fi
if [[ "$OBJDIR" == "" ]]; then
    # Default to assuming that the user's working directory is the
    # test/ directory within the rr clone.
    OBJDIR=`realpath ../../../obj`
fi
SRCDIR="${OBJDIR}/../rr"
TESTDIR="${SRCDIR}/src/test"

export PATH="${OBJDIR}/bin:${PATH}"
export LD_LIBRARY_PATH="${OBJDIR}/lib:/usr/local/lib:${LD_LIBRARY_PATH}"

which rr >/dev/null 2>&1
if [[ "$?" != "0" ]]; then
    fatal FAILED: rr not found in PATH "($PATH)"
fi

# NB: must set up the trap handler *before* mktemp
trap onexit EXIT
workdir=`mktemp -dt rr-test-$TESTNAME-XXXXXXXXX`
cd $workdir

# XXX technically the trailing -XXXXXXXXXX isn't unique, since there
# could be "foo-123456789" and "bar-123456789", but if that happens,
# buy me a lottery ticket.
nonce=${workdir#/tmp/rr-test-$TESTNAME-}

##--------------------------------------------------
## Now we come to the helpers available to test runners.  This is the
## testing "API".
##

function fails { why=$1;
    echo NOTE: Skipping "'$TESTNAME'" because it fails: $why
    exit 0
}

# If the test takes too long to run without the syscallbuf enabled,
# use this to prevent it from running when that's the case.
function skip_if_no_syscall_buf {
    if [[ "-n" == "$LIB_ARG" ]]; then
	echo NOTE: Skipping "'$TESTNAME'" because syscallbuf is disabled
	exit 0
    fi
}

# If the test is causing an unrealistic failure when the syscallbuf is
# enabled, skip it.  This better be a temporary situation!
function skip_if_syscall_buf {
    if [[ "-b" == "$LIB_ARG" || "" == "$LIB_ARG" ]]; then
	echo NOTE: Skipping "'$TESTNAME'" because syscallbuf is enabled
	exit 0
    fi
}

function just_record { exe=$1; exeargs=$2;
    _RR_TRACE_DIR="$workdir" \
	rr $GLOBAL_OPTIONS record $LIB_ARG $RECORD_ARGS $exe $exeargs 1> record.out
}

function save_exe { exe=$1;
    cp ${OBJDIR}/bin/$exe $exe-$nonce
}    

function record { exe=$1; exeargs=$2;
    save_exe $exe
    just_record ./$exe-$nonce "$exeargs"
}

#  record_async_signal <signal> <delay-secs> <test>
#
# Record $test, delivering $signal to it after $delay-secs.
function record_async_signal { sig=$1; delay_secs=$2; exe=$3; exeargs=$4;
    delay_kill $sig $delay_secs $exe-$nonce &
    record $exe $exeargs
    wait
}

function replay { replayflags=$1
    _RR_TRACE_DIR="$workdir" \
	rr $GLOBAL_OPTIONS replay -a $replayflags 1> replay.out 2> replay.err
}

#  debug <exe> <expect-script-name> [replay-args]
#
# Load the "expect" script to drive replay of the recording of |exe|.
function debug { exe=$1; expectscript=$2; replayargs=$3
    _RR_TRACE_DIR="$workdir" \
	python $TESTDIR/$expectscript.py $exe-$nonce \
	rr $GLOBAL_OPTIONS replay -x $TESTDIR/test_setup.gdb $replayargs
    if [[ $? == 0 ]]; then
	echo "Test '$TESTNAME' PASSED"
    else
	leave_data=y
	echo "Test '$TESTNAME' FAILED"
    fi
}

# Check that (i) no error during replay; (ii) recorded and replayed
# output match; (iii) the supplied token was found in the output.
# Otherwise the test fails.
function check { token=$1;
    if [ ! -f record.out -o ! -f replay.err -o ! -f replay.out ]; then
	leave_data=y
	echo "Test '$TESTNAME' FAILED: output files not found."
    elif [[ $(cat replay.err) != "" ]]; then
	leave_data=y
	echo "Test '$TESTNAME' FAILED: error during replay:"
	echo "--------------------------------------------------"
	cat replay.err
	echo "--------------------------------------------------"
    elif [[ $(diff record.out replay.out) != "" ]]; then
	leave_data=y
	echo "Test '$TESTNAME' FAILED: output from recording different than replay"
	echo "Output from recording:"
	echo "--------------------------------------------------"
	cat record.out
	echo "--------------------------------------------------"
	echo "Output from replay:"
	echo "--------------------------------------------------"
	cat replay.out
	echo "--------------------------------------------------"
    elif [[ "$token" != "" && "record.out" != $(grep -l "$token" record.out) ]]; then
	leave_data=y
	echo "Test '$TESTNAME' FAILED: token '$token' not in output:"
	echo "--------------------------------------------------"
	cat record.out
	echo "--------------------------------------------------"
    else
	echo "Test '$TESTNAME' PASSED"
    fi
}

#  compare_test <token> [<replay-flags>]
#
# Record the test name passed to |util.sh|, then replay it (optionally
# with $replayflags) and verify record/replay output match and $token
# appears in the output.
function compare_test { token=$1; replayflags=$2;
    test=$TESTNAME
    if [[ $token == "" ]]; then
	leave_data=y
	fatal "FAILED: Test $test didn't pass an exit token"
    fi
    record $test
    replay $replayflags
    check $token
}

#  debug_test
#
# Record the test name passed to |util.sh|, then replay the recording
# using the "expect" script $test-name.py, which is responsible for
# computing test pass/fail.
function debug_test {
    test=$TESTNAME
    record $test
    debug $test $test
}

# Return the number of events in the most recent local recording.
function count_events {
    local events=$(rr dump -r latest-trace | wc -l)
    # The |simple| test is just about the simplest possible C program,
    # and has around 180 events (when recorded on a particular
    # developer's machine).  If we count a number of events
    # significalty less than that, almost certainly something has gone
    # wrong.
    if [ "$events" -le 150 ]; then
        leave_data=y
        fatal "FAILED: Recording for $test had too few events.  Is |rr dump -r| broken?"
    fi
    # This event count is used to loop over attaching the debugger.
    # The tests assume that the debugger can be attached at all
    # events, but at the very last events, EXIT and so forth, rr can't
    # attach the debugger.  So we fudge the event count down to avoid
    # that edge case.
    let "events -= 10"
    echo $events
}

# Return a random number from the range [min, max], inclusive.
function rand_range { min=$1; max=$2
    local num=$RANDOM
    local range=""
    let "range = 1 + $max - $min"
    let "num %= $range"
    let "num += $min"
    echo $num
}

# Record |exe|, then replay it using the |restart_finish| debugger
# script attaching at every recorded event.  To make the
# debugger-replays more practical, the events are strided between at a
# random interval between [min, max], inclusive.
#
# So for example, |checkpoint_test simple 3 5| means to record the
# "simple" test, and attach the debugger at every X'th event, where X
# is a random number in [3, 5].
function checkpoint_test { exe=$1; min=$2; max=$3;
    record $exe
    num_events=$(count_events)
    stride=$(rand_range $min $max)
    for i in $(seq 1 $stride $num_events); do
        echo Checkpointing at event $i ...
        debug $exe restart_finish "-g $i"
    done
}
