#!/usr/bin/env bash
# The network: a device, a lease, a reachable gateway, and an exchange with
# the host.
#
# The IPv4 stack under this is the **ARM project's, carried over unchanged** —
# it is pure, it includes only `core/` and `lib/`, and its host tests
# (`net_test`, `tcp_test`, `tcp_hostile_test`) passed here on the first build.
# So what this test proves is the part that is genuinely new: the virtio-net
# adapter beneath it, and the wiring in the composition root.
#
# Three things, in order of what they rule out:
#
#   - **A lease**, which needs transmit, receive, ARP and a DHCP exchange of
#     four packets to all work. An address is the first thing that cannot be
#     faked by a half-working driver.
#   - **A ping the gateway answers**, which proves the path end to end and
#     back: a driver that transmits into a void still gets a lease in some
#     models, but nothing replies to it.
#   - **A datagram from the host that comes back unchanged**, through the
#     emulator's port forwarding — the guest's own echo service, so the frame
#     crosses the adapter in both directions with the kernel choosing the
#     reply's source address and checksum.
set -euo pipefail

image="$1"
tmp="${TEST_TMPDIR:-$(mktemp -d)}"
log="$tmp/console.log"

command -v qemu-system-riscv64 >/dev/null || { echo "qemu is not installed" >&2; exit 1; }

fail() { cat "$log" 2>/dev/null; echo "FAIL: $1" >&2; exit 1; }

# A port nobody else is on. The pid is as good a source of one as any here,
# and two copies of this test on one machine must not collide.
port=$((20000 + ($$ % 20000)))

# `force-legacy=false` is not optional: QEMU's MMIO transport defaults to
# **version 1** for a device added this way, and this driver refuses a legacy
# transport rather than guessing at a layout it does not implement. Without
# it the device is found and declined, which reads as "no network device".
qemu-system-riscv64 -M virt -smp 4 -accel tcg,thread=multi -m 1G -nographic \
    -bios default -kernel "$image" \
    -netdev "user,id=n0,hostfwd=udp:127.0.0.1:$port-:7" \
    -device virtio-net-device,netdev=n0 -global virtio-mmio.force-legacy=false \
    -append "console=sbi" > "$log" 2>&1 &
qemu=$!
cleanup() { kill "$qemu" 2>/dev/null || true; wait "$qemu" 2>/dev/null || true; }
trap cleanup EXIT

wait_for() {
    for _ in $(seq 1 150); do
        tr -d '\r' < "$log" 2>/dev/null | sed -E 's/^\[[0-9:.]+\] [A-Z] //' | grep -qE -- "$1" && return 0
        sleep 0.1
    done
    fail "the console never showed: $1"
}

# The device, with the hardware address it claims rather than one invented
# here: a frame from an address the model does not expect is dropped on the
# host's side, which looks exactly like a driver that cannot transmit.
wait_for '^net: /soc/virtio_mmio@[0-9a-f]+, mac ([0-9a-f]{2}:){5}[0-9a-f]{2}, 8 receive buffers$'

# The lease, with the whole configuration the emulator's DHCP server hands
# out. Four packets had to cross the adapter in both directions for this.
wait_for '^net: eth0 10\.0\.2\.15/24 gw 10\.0\.2\.2 dns 10\.0\.2\.3 \(dhcp, lease 86400 s\)$'

# And something answered. The gateway is the emulator itself, and it only
# replies to a well-formed ICMP echo with a correct checksum.
wait_for '^net: 10\.0\.2\.2 answered ping seq [0-9]+$'

# The host's turn: a datagram into the guest's echo service, through the
# forwarded port, and back unchanged.
payload="riscv-minikern-echo-$port"
exec 3<>"/dev/udp/127.0.0.1/$port"
printf '%s' "$payload" >&3
reply=""
read -r -t 10 -N "${#payload}" reply <&3 || true
exec 3<&-
tr -d '\r' < "$log" | sed -E 's/^\[[0-9:.]+\] [A-Z] //' > "$log.clean" && mv "$log.clean" "$log"
cat "$log"
[ "$reply" = "$payload" ] || fail "the echo service returned '$reply', want '$payload'"
! grep -qF "PANIC" "$log" || fail "the kernel panicked"
echo "ok: a lease, a gateway that answers, and a datagram echoed back through the adapter"
echo PASS
