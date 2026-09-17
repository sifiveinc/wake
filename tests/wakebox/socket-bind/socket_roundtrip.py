import os, socket, stat, sys

# Bind an AF_UNIX socket in the workspace (a mknod(S_IFSOCK) through FUSE), verify it is
# visible as a socket through the virtualized FUSE view, and round-trip data through it.
SOCK_PATH = "mysocket"

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
server.bind(SOCK_PATH)
server.listen(1)

if not stat.S_ISSOCK(os.stat(SOCK_PATH).st_mode):
    print("FAIL: stat() does not report a socket")
    sys.exit(1)

pid = os.fork()
if pid == 0:
    # Child: connect and read.
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(SOCK_PATH)
    data = client.recv(64)
    client.close()
    os._exit(0 if data == b"socket-ok" else 1)

# Parent: accept and write.
conn, _ = server.accept()
conn.sendall(b"socket-ok")
conn.close()
server.close()
_, status = os.waitpid(pid, 0)

if os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0:
    print("SOCKET_ROUNDTRIP_OK")
else:
    print("FAIL: socket round-trip failed")
    sys.exit(1)
