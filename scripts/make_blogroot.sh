#!/bin/sh
# make_blogroot.sh — compose a whole root filesystem for the board, served
# over netfs: QuokkaOS as PID 1, qmcd for the Quokka Management Console,
# statsd, and the mattmierzwinski.com blog listening on :8080.
#
#   scripts/make_blogroot.sh [DIR]          (default ~/vf2/blogroot)
#
# then export it and boot it:
#
#   netfsd -dir DIR -addr 192.168.86.119:8084
#   board.py --bare 'console=sbi minikern.disk=sd minikern.netfs=192.168.86.119:8084 minikern.netroot minikern.init'
#
# (`minikern.disk=sd` because the NVMe drive holds the operator's data.)
#
# Re-runnable: binaries and service files are replaced, the blog's data in
# /var/lib/blog and the logs in /var/log are kept.
#
# The blog runs with -journal-mode delete: its database lives on netfs, and
# SQLite's WAL coordinates through a shared-memory file that SQLite says does
# not work on a network filesystem (and MAP_SHARED is refused here anyway).
set -eu

root=${1:-$HOME/vf2/blogroot}
monorepo=$(cd "$(dirname "$0")/../../../../.." && pwd)

targets="//src/go/quokkaos/cmd/init:init_riscv64
//src/go/quokkaos/cmd/rc:rc_riscv64
//src/go/quokkaos/cmd/statsd:statsd_riscv64
//src/go/tools/qmc/cmd/qmcd:qmcd
//src/go/blog/mattmierzwinski_com:blog_riscv64"

# output prints the one file a target builds.
output() {
	(cd "$monorepo" && bazel cquery --output=files "$1" 2>/dev/null)
}

build() {
	# shellcheck disable=SC2086
	(cd "$monorepo" && bazel build $targets)
}

make_tree() {
	for d in bin sbin dev proc sys tmp root home run/quokka \
		etc/init.d etc/conf.d etc/runlevels/default \
		var/log var/tmp var/lib/blog; do
		mkdir -p "$root/$d"
	done
	chmod 1777 "$root/tmp" "$root/var/tmp"
	chmod 700 "$root/root"
}

install_binaries() {
	install -m 755 "$monorepo/$(output //src/go/quokkaos/cmd/init:init_riscv64)" "$root/init"
	install -m 755 "$monorepo/$(output //src/go/quokkaos/cmd/rc:rc_riscv64)" "$root/bin/rc"
	install -m 755 "$monorepo/$(output //src/go/quokkaos/cmd/statsd:statsd_riscv64)" "$root/bin/statsd"
	install -m 755 "$monorepo/$(output //src/go/tools/qmc/cmd/qmcd:qmcd)" "$root/bin/qmcd"
	install -m 755 "$monorepo/$(output //src/go/blog/mattmierzwinski_com:blog_riscv64)" "$root/bin/blog"
	ln -sf ../init "$root/sbin/init"
}

write_etc() {
	echo quokka > "$root/etc/hostname"
	printf 'root:x:0:0:root:/root:/bin/rc\n' > "$root/etc/passwd"
	printf 'root:x:0:\n' > "$root/etc/group"
	printf '127.0.0.1 localhost\n::1 localhost\n' > "$root/etc/hosts"
	printf 'nameserver 192.168.86.1\n' > "$root/etc/resolv.conf"
}

service() { # name, then the file's body on stdin
	cat > "$root/etc/init.d/$1.toml"
	ln -sf "../../init.d/$1.toml" "$root/etc/runlevels/default/$1.toml"
}

write_services() {
	service qmcd <<'EOF'
description = "the Quokka Management Console daemon"
command = ["/bin/qmcd", "-addr", ":9910"]
[restart]
policy = "always"
backoff = "500ms..5s"
max_in_window = "10/60s"
EOF
	service statsd <<'EOF'
description = "the board's state as a web page"
command = ["/bin/statsd", "-addr", ":8088"]
[restart]
policy = "always"
backoff = "500ms..5s"
max_in_window = "10/60s"
EOF
	service blog <<'EOF'
description = "the mattmierzwinski.com blog"
command = ["/bin/blog", "-addr", ":8080", "-data", "/var/lib/blog",
           "-journal-mode", "delete", "-baseurl", "http://192.168.86.247:8080"]
dirs = ["/var/lib/blog"]
[env]
TMPDIR = "/tmp"
HOME = "/root"
[restart]
policy = "always"
backoff = "1s..10s"
max_in_window = "5/60s"
EOF
}

build
make_tree
install_binaries
write_etc
write_services
echo "make_blogroot: $root ready"
