import os, stat, sys

# Creates a FIFO via mkfifo (a mknod(S_IFIFO) through FUSE), verifies it is visible as a fifo
# through the virtualized FUSE view, and round-trips data through it.
FIFO_PATH = "myfifo"

os.mkfifo(FIFO_PATH)

if not stat.S_ISFIFO(os.stat(FIFO_PATH).st_mode):
    print("FAIL: stat() does not report a fifo")
    sys.exit(1)

pid = os.fork()
if pid == 0:
    # Child: writer.
    fd = os.open(FIFO_PATH, os.O_WRONLY)
    os.write(fd, b"fifo-ok")
    os.close(fd)
    os._exit(0)

# Parent: reader.
fd = os.open(FIFO_PATH, os.O_RDONLY)
data = os.read(fd, 64)
os.close(fd)
os.waitpid(pid, 0)

if data == b"fifo-ok":
    print("FIFO_ROUNDTRIP_OK")
else:
    print("FAIL: unexpected data %r" % data)
    sys.exit(1)
