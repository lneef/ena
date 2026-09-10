#!/usr/bin/env python3
"""Multi-queue system test for the ENA emulation.

Boots the miniosv pong benchmark with several vCPUs; the bench runs one lcore per
vCPU, each owning an RX/TX queue pair, with RSS spreading flows across them.
Host threads inject many distinct UDP flows concurrently. Every echo must come
back intact with valid checksums, and the guest's per-queue counters must match
the Toeplitz distribution computed here. Exit 0 on success.
"""
import argparse, os, socket, struct, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

# RSS_DEFAULT_KEY of miniosv/app/bench.cc; the device hashes with this byte order.
RSS_KEY = bytes([
    0xbe, 0xac, 0x01, 0xfa, 0x6a, 0x42, 0xb7, 0x3b, 0x80, 0x30,
    0xf2, 0x0c, 0x77, 0xcb, 0x2d, 0xa3, 0xae, 0x7b, 0x30, 0xb4,
    0xd0, 0xca, 0x2b, 0xcb, 0x43, 0xa3, 0x8f, 0xb0, 0x41, 0x67,
    0x25, 0x3d, 0x25, 0x5b, 0x0e, 0xc2, 0x6d, 0x5a, 0x56, 0xda])
RETA_SIZE = 128


def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def toeplitz(key, data):
    result = 0
    key_int = int.from_bytes(key, "big")
    key_bits = len(key) * 8
    for i, byte in enumerate(data):
        for b in range(8):
            if byte & (0x80 >> b):
                shift = key_bits - 32 - (i * 8 + b)
                result ^= (key_int >> shift) & 0xFFFFFFFF
    return result


def mac(s):
    return bytes(int(x, 16) for x in s.split(":"))


class Flow:
    def __init__(self, idx, dip, dport, nrx):
        self.idx = idx
        self.sip = "10.0.%d.%d" % (1 + idx // 200, 1 + idx % 200)
        self.dip = dip
        self.sport = 20000 + idx
        self.dport = dport
        tuple4 = socket.inet_aton(self.sip) + socket.inet_aton(dip) + \
            struct.pack("!HH", self.sport, dport)
        self.hash = toeplitz(RSS_KEY, tuple4)
        self.queue = (self.hash % RETA_SIZE) % nrx


def frame(src_mac, dst_mac, flow, seq, ip_len):
    payload = struct.pack("<II", flow.idx, seq).ljust(ip_len - 28, b"\0")
    udp = struct.pack("!HHHH", flow.sport, flow.dport, 8 + len(payload), 0) + payload
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, ip_len, 0, 0, 64, 17, 0,
                     socket.inet_aton(flow.sip), socket.inet_aton(flow.dip))
    ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
    return dst_mac + src_mac + b"\x08\x00" + ip + udp


def check_echo(echo, src_mac, dst_mac, flows):
    if len(echo) < 42 or echo[12:14] != b"\x08\x00":
        return None, "not ipv4"
    if echo[0:6] != src_mac or echo[6:12] != dst_mac:
        return None, "mac not swapped"
    ip = echo[14:34]
    sport, dport, ulen, ucsum = struct.unpack("!HHHH", echo[34:42])
    fidx, seq = struct.unpack("<II", echo[42:50])
    if fidx >= len(flows):
        return None, "unknown flow"
    f = flows[fidx]
    if socket.inet_ntoa(ip[12:16]) != f.dip or socket.inet_ntoa(ip[16:20]) != f.sip:
        return None, "ip not swapped"
    if sport != f.dport or dport != f.sport:
        return None, "ports not swapped"
    if csum(ip) != 0:
        return None, "ip checksum bad"
    pseudo = ip[12:20] + struct.pack("!BBH", 0, 17, ulen)
    if ucsum == 0 or csum(pseudo + echo[34:34 + ulen]) != 0:
        return None, "udp checksum bad"
    return (fidx, seq), None


def start_guest(a):
    cmd = [os.path.join(a.miniosv, "scripts", "run.py"), "--novnc", "--nogdb",
           "-c", str(a.vcpus), "--qemu-path", a.qemu,
           "--pass-args=-device ena,netdev=n0,mac=%s" % a.dst_mac,
           "--pass-args=-netdev socket,id=n0,udp=127.0.0.1:%d,localaddr=127.0.0.1:%d"
           % (a.listen_port, a.peer_port)]
    proc = subprocess.Popen(cmd, cwd=a.miniosv, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, errors="replace")
    lines = []
    lock = threading.Lock()

    def reader():
        for line in proc.stdout:
            with lock:
                lines.append(line.rstrip("\r\n"))

    threading.Thread(target=reader, daemon=True).start()

    def wait_for(pred, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with lock:
                for l in lines:
                    if pred(l):
                        return l
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        return None

    return proc, lines, lock, wait_for


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=os.path.join(ROOT, "qemu/build/qemu-system-x86_64"))
    ap.add_argument("--miniosv", default=os.path.join(ROOT, "miniosv"))
    ap.add_argument("--vcpus", type=int, default=4)
    ap.add_argument("--flows", type=int, default=64)
    ap.add_argument("--frames", type=int, default=16, help="frames per flow")
    ap.add_argument("--senders", type=int, default=4, help="host sender threads")
    ap.add_argument("--dst-mac", default="52:54:00:00:00:02")
    ap.add_argument("--src-mac", default="52:54:00:00:00:01")
    ap.add_argument("--dip", default="10.0.0.2")
    ap.add_argument("--dport", type=int, default=1234)
    ap.add_argument("--ip-len", type=int, default=128)
    ap.add_argument("--listen-port", type=int, default=1235)
    ap.add_argument("--peer-port", type=int, default=1234)
    a = ap.parse_args()

    proc, lines, lock, wait_for = start_guest(a)
    try:
        q = wait_for(lambda l: l.startswith("queues: "), 60)
        if not q:
            print("FAIL: guest did not report its queue count"); return 1
        nrx = int(q.split()[1])
        if not wait_for(lambda l: l.strip() == "0, 0, 0, 0", 60):
            print("FAIL: device did not start"); return 1
        bad_boot = [l for l in lines if any(k in l for k in
                    ("ERR", "CRIT", "no dev", "configure failed", "setup failed",
                     "Starting dev failed"))]
        if bad_boot:
            print("FAIL: bring-up errors:", bad_boot[:3]); return 1
        print("guest up with %d queue pairs" % nrx)
        if nrx < 2:
            print("FAIL: need at least 2 queues (vcpus=%d)" % a.vcpus); return 1
        time.sleep(1)

        src, dst = mac(a.src_mac), mac(a.dst_mac)
        flows = [Flow(i, a.dip, a.dport, nrx) for i in range(a.flows)]
        expected = [0] * nrx
        for f in flows:
            expected[f.queue] += a.frames
        if min(expected) == 0:
            print("FAIL: flow set does not cover every queue:", expected); return 1

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", a.listen_port))
        sock.settimeout(1.0)
        peer = ("127.0.0.1", a.peer_port)

        def sender(fl):
            for seq in range(a.frames):
                for f in fl:
                    sock.sendto(frame(src, dst, f, seq, a.ip_len), peer)
                time.sleep(0.001)

        groups = [flows[i::a.senders] for i in range(a.senders)]
        threads = [threading.Thread(target=sender, args=(g,)) for g in groups]
        for t in threads:
            t.start()

        want = a.flows * a.frames
        got, bad = set(), {}
        deadline = time.time() + 15
        while len(got) < want and time.time() < deadline:
            try:
                echo, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            key, err = check_echo(echo, src, dst, flows)
            if err:
                bad[err] = bad.get(err, 0) + 1
            else:
                got.add(key)
        for t in threads:
            t.join()
        print("sent=%d echoed=%d bad=%s" % (want, len(got), bad or 0))

        # the guest reports "queue i rx= tx=" every 2 s; take the last full set
        time.sleep(3)
        with lock:
            reports = [l for l in lines if l.startswith("queue ")]
        last = {}
        for l in reports:
            parts = l.split()
            last[int(parts[1])] = (int(parts[2][3:]), int(parts[3][3:]))
        rx = [last.get(i, (0, 0))[0] for i in range(nrx)]
        tx = [last.get(i, (0, 0))[1] for i in range(nrx)]
        print("expected per queue:", expected)
        print("guest rx per queue: ", rx)
        print("guest tx per queue: ", tx)

        ok = len(got) == want and not bad and rx == expected and tx == expected
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
