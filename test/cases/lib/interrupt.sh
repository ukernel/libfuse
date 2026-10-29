# lib/interrupt.sh - body for the misc/interrupt cases.
#
# Caller sets FS_OPTS before sourcing:
#   FS_OPTS   -o option list for test/test_intr_ll, empty for none
#
# test_intr_ll enables interrupts and registers a callback only in its
# blocking setlk (RED selective interrupts, D3). Three times over, a SETLKW
# is interrupted by SIGALRM and must fail with EINTR within 5 s, and a stat()
# of the file must then still succeed.

. "$TEST_LIB/common.sh"

_require_linux "FUSE_INTERRUPT for a blocked SETLKW"
_require_cap FUSE_CAP_POSIX_LOCKS

opts=()
[ "${FUSE_URING_ENABLE:-0}" != 1 ] || opts+=(-o io_uring)
[ -z "${FS_OPTS:-}" ] || opts+=(-o "$FS_OPTS")

fuse_mount test/test_intr_ll "${opts[@]}" >/dev/null

python3 - "$TEST_MNT/f" <<'PY'
import fcntl, os, signal, sys, time

class Alarm(Exception):
    pass

def on_alarm(*a):
    raise Alarm()

# Python installs handlers with SA_RESTART and retries a syscall that fails
# with EINTR (PEP 475) unless the handler raises. siginterrupt() drops
# SA_RESTART so the kernel returns EINTR, and raising stops the retry: the
# handler runs only once F_SETLKW has returned.
signal.signal(signal.SIGALRM, on_alarm)
signal.siginterrupt(signal.SIGALRM, True)

fd = os.open(sys.argv[1], os.O_RDWR)
for attempt in range(3):
    signal.alarm(1)
    t = time.time()
    try:
        fcntl.lockf(fd, fcntl.LOCK_EX)      # F_SETLKW on the whole file
        sys.exit("F_SETLKW returned without error")
    except Alarm:
        pass
    if time.time() - t > 5:
        sys.exit("interrupt took too long")
    os.stat(sys.argv[1])
os.close(fd)
PY

fuse_umount
