import re

from rrutil import *

BAD_TOKEN = r'EXIT-SUCCESS'
GOOD_TOKEN = r'Inferior 1 \(process \d+\) exited normally'

def observe_child_crash_and_exit():
    expect_gdb('Program received signal SIGSEGV')

    send_gdb('c\n')
    for line in iterlines_both():
        m = re.search(BAD_TOKEN, line)
        if m:
            failed('Saw illegal token "'+ BAD_TOKEN +'"')
        m = re.search(GOOD_TOKEN, line)
        if m:
            return

send_gdb('c\n')
observe_child_crash_and_exit()

restart_replay_at_end()
observe_child_crash_and_exit()

ok()
