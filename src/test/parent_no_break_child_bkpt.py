from rrutil import *

def observe_normal_parent_exit():
    expect_rr('EXIT-SUCCESS')
    expect_gdb(r'Inferior 1 \(process \d+\) exited normally')

send_gdb('b breakpoint\n')
expect_gdb('Breakpoint 1')

send_gdb('c\n')
observe_normal_parent_exit()

restart_replay_at_end()
observe_normal_parent_exit()

ok()
