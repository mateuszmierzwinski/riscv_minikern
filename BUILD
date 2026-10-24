load("@rules_cc//cc:defs.bzl", "cc_library", "cc_test")
load("@rules_shell//shell:sh_test.bzl", "sh_test")
load("//:version.bzl", "MINIKERN_VERSION")
load(":riscv_minikern.bzl", "kernel_binary", "kernel_image", "user_binary", "user_image")

# minikern for RISC-V — a from-scratch kernel for the StarFive JH7110
# (SiFive U74), implementing Linux's syscall interface so that Go binaries
# run unchanged. The migration of github.com/mateuszmierzwinski/arm_minikern
# off 32-bit ARM; see README.md and TASKS.md.
package(default_visibility = ["//visibility:private"])

exports_files(["src/arch/riscv/link.ld"])

# ── portable code: the same sources build for the host and for the board ───
#
# Every target here is architecture-neutral by construction — src/core
# includes nothing from arch/ or board/ — which is why the host tests below
# run on day one of the port rather than at the end of it.

cc_library(
    name = "kfmt",
    srcs = ["src/lib/kfmt.c"],
    hdrs = ["src/lib/kfmt.h"],
    strip_include_prefix = "src",
)

cc_library(
    name = "core_boot",
    srcs = [
        "src/core/bootinfo.c",
        "src/core/cmdline.c",
        "src/core/fdt.c",
    ],
    hdrs = [
        "src/core/bootinfo.h",
        "src/core/cmdline.h",
        "src/core/errno.h",
        "src/core/fdt.h",
        "src/core/types.h",
    ],
    strip_include_prefix = "src",
)

# The arithmetic of proportional-share scheduling: weights, virtual time and
# slices. Pure and host-tested on purpose — every mistake in it is a silent
# wrong answer that reads as "the scheduler is unfair" rather than a failure.
cc_library(
    name = "fair",
    srcs = ["src/core/fair.c"],
    hdrs = ["src/core/fair.h"],
    strip_include_prefix = "src",
)

cc_test(
    name = "fair_test",
    size = "small",
    srcs = ["test/fair_test.c"],
    deps = [
        ":fair",
        ":ktest",
    ],
)

# The line discipline: what turns bytes from a wire into the lines a program
# expects from a terminal. Pure and host-tested, because every mistake in it
# is a plausible behaviour that needs a human's fingers to notice — a
# backspace that erases the prompt, an echo that prints a control character
# as itself, a ^D that throws away the line it was meant to deliver.
# The permission rule: three bits, three classes, and an exclusivity that is
# easy to get backwards. Pure, because the cases that matter are the ones
# nobody produces by hand — an owner denied by their own bits, root against
# a mode that denies everybody.
cc_library(
    name = "access",
    srcs = ["src/core/access.c"],
    hdrs = ["src/core/access.h"],
    strip_include_prefix = "src",
)

cc_test(
    name = "access_test",
    size = "small",
    srcs = ["test/access_test.c"],
    deps = [
        ":access",
        ":ktest",
    ],
)

cc_library(
    name = "tty",
    srcs = ["src/core/tty.c"],
    hdrs = ["src/core/tty.h"],
    strip_include_prefix = "src",
)

cc_test(
    name = "tty_test",
    size = "small",
    srcs = ["test/tty_test.c"],
    deps = [
        ":ktest",
        ":tty",
    ],
)

cc_library(
    name = "console",
    srcs = ["src/core/console.c"],
    hdrs = [
        "src/core/console.h",
        "src/core/ports.h",
    ],
    strip_include_prefix = "src",
    deps = [":kfmt"],
)

# The physical page allocator and the kernel heap: carried over, pure, and
# host-tested. Widened to 64-bit physical addresses, because DRAM on this
# board starts at 0x4000_0000 and can run past 0x2_0000_0000.
cc_library(
    name = "memory",
    srcs = [
        "src/core/kheap.c",
        "src/core/pmm.c",
    ],
    hdrs = [
        "src/core/kheap.h",
        "src/core/pmm.h",
        "src/core/ports.h",
    ],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

# The SD protocol's arithmetic, with no controller: the capacity a CSD
# reports, in both of the layouts a card can use.
# Partition tables: both formats, and the protective MBR that must not be
# followed.
# Where every mapping goes. Mostly about addresses above 4 GiB, which is
# what had kept this file unused on a 64-bit machine.
cc_test(
    name = "vma_test",
    size = "small",
    srcs = ["test/vma_test.c"],
    deps = [
        ":ktest",
        ":vma",
    ],
)

cc_test(
    name = "partition_test",
    size = "small",
    srcs = ["test/partition_test.c"],
    deps = [
        ":ktest",
        ":partition",
    ],
)

cc_test(
    name = "sdcard_test",
    size = "small",
    srcs = ["test/sdcard_test.c"],
    deps = [
        ":drivers_sdcard",
        ":ktest",
    ],
)

cc_test(
    name = "pmm_test",
    size = "small",
    srcs = ["test/pmm_test.c"],
    deps = [
        ":ktest",
        ":memory",
    ],
)

# The block layer and the filesystem, carried over whole: a block device
# port, a buffer cache over it, and XFS. All pure, all host-tested.
cc_library(
    name = "storage",
    srcs = [
        "src/core/bcache.c",
        "src/core/blockscale.c",
    ],
    hdrs = [
        "src/core/bcache.h",
        "src/core/blockdev.h",
        "src/core/blockscale.h",
    ],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

# XFS, carried over whole: mount, read, logged writes, create, remove, grow.
# Pure over the buffer cache, and host-tested against filesystems mkfs.xfs
# made — which is the property that matters, because the authority on whether
# this implementation is right is the vendor's own checker, not this project.
cc_library(
    name = "xfs",
    srcs = glob(["src/core/xfs/*.c"]),
    hdrs = glob(["src/core/xfs/*.h"]),
    strip_include_prefix = "src",
    deps = [
        ":console",
        ":core_boot",
        ":crypto",
        ":storage",
    ],
)

cc_library(
    name = "crypto",
    srcs = ["src/lib/crc32c.c"],
    hdrs = ["src/lib/crc32c.h"],
    strip_include_prefix = "src",
)

cc_test(
    name = "xfs_test",
    size = "small",
    srcs = ["test/xfs_test.c"],
    deps = [
        ":ktest",
        ":xfs",
    ],
)

cc_test(
    name = "blockscale_test",
    size = "small",
    srcs = ["test/blockscale_test.c"],
    deps = [
        ":ktest",
        ":storage",
    ],
)

cc_test(
    name = "bcache_test",
    size = "small",
    srcs = ["test/bcache_test.c"],
    deps = [
        ":ktest",
        ":storage",
    ],
)

# Sv39 translation, as pure arithmetic: no CSR is touched here, which is what
# lets it be tested on the host before any hardware installs a table.
# Loading a program, and the state it starts in. Both pure, both
# host-tested: an ELF64 parser that refuses what it cannot run, and the
# argc/argv/envp/auxv block a C runtime reads before main.
cc_library(
    name = "exec",
    srcs = [
        "src/core/elf.c",
        "src/core/ustack.c",
    ],
    hdrs = [
        "src/core/elf.h",
        "src/core/ustack.h",
    ],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

# The IPv4 stack: Ethernet, ARP, IPv4 with a loopback path, ICMP echo, UDP, a
# DHCP client and TCP. **Carried over unchanged from the ARM project** — it is
# pure, it includes only core/ and lib/, and it was already host-tested with
# crafted and hostile frames, so the architecture it runs on is not a fact it
# knows. That is what a hexagon is for, and this is the largest piece of
# evidence that this one holds.
# The adapter under the stack: a virtio network device, which the emulator
# models. Two queues, because a receive buffer waits in the device's hands
# for a frame that may never come and must not hold up a transmit.
# The live interface: the device's frames into the stack, the stack's 10 ms
# tick, a DHCP client and a boot-time ping. Carried over from the ARM
# project; what changed is where time comes from and that waiting for a
# lease is a `wfi` rather than a kernel service this kernel does not have.
cc_library(
    name = "kernel_net",
    srcs = ["src/kernel/net.c"],
    hdrs = ["src/kernel/net.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":kernel_random",
        ":net",
        ":virtio_net",
    ],
)

cc_library(
    name = "virtio_net",
    srcs = ["src/drivers/virtio_net.c"],
    hdrs = ["src/drivers/virtio_net.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":kernel_mem",
        ":net",
        ":drivers_virtio",
    ],
)

cc_library(
    name = "net",
    srcs = [
        "src/core/net/net_arp.c",
        "src/core/net/net_dhcp.c",
        "src/core/net/net_echo.c",
        "src/core/net/net_eth.c",
        "src/core/net/net_ipv4.c",
        "src/core/net/net_tcp.c",
        "src/core/net/net_tcp_in.c",
        "src/core/net/net_tcp_out.c",
        "src/core/net/net_udp.c",
        "src/core/net/net_util.c",
    ],
    hdrs = [
        # The two private headers are `hdrs` rather than `srcs` because
        # `strip_include_prefix` only rewrites the include path of headers it
        # is told about — a private header left in `srcs` is compiled against
        # its own path and is then unfindable by `#include "core/net/..."`.
        "src/core/net/net.h",
        "src/core/net/net_internal.h",
        "src/core/net/tcp.h",
        "src/core/net/tcp_internal.h",
        "src/core/ports.h",
    ],
    strip_include_prefix = "src",
    deps = [
        ":core_boot",
        ":kfmt",
    ],
)

cc_test(
    name = "net_test",
    size = "small",
    srcs = [
        "test/net_frames.h",
        "test/net_test.c",
    ],
    deps = [
        ":ktest",
        ":net",
    ],
)

cc_test(
    name = "tcp_test",
    size = "small",
    srcs = [
        "test/net_frames.h",
        "test/tcp_test.c",
    ],
    deps = [
        ":ktest",
        ":net",
    ],
)

cc_test(
    name = "tcp_hostile_test",
    size = "small",
    srcs = [
        "test/net_frames.h",
        "test/tcp_hostile_test.c",
    ],
    deps = [
        ":ktest",
        ":net",
    ],
)

cc_library(
    name = "pgtable",
    srcs = ["src/arch/riscv/pgtable.c"],
    hdrs = ["src/arch/riscv/pgtable.h"],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

# ── host tests, which need no board and no emulator ────────────────────────

cc_test(
    name = "kfmt_test",
    size = "small",
    srcs = ["test/kfmt_test.c"],
    deps = [
        ":kfmt",
        ":ktest",
    ],
)

cc_library(
    name = "ktest",
    testonly = True,
    hdrs = [
        "test/fdt_build.h",
        "test/ktest.h",
    ],
    strip_include_prefix = "test",
)

cc_test(
    name = "cmdline_test",
    size = "small",
    srcs = ["test/cmdline_test.c"],
    deps = [
        ":core_boot",
        ":ktest",
    ],
)

cc_test(
    name = "fdt_test",
    size = "small",
    srcs = ["test/fdt_test.c"],
    deps = [
        ":core_boot",
        ":ktest",
    ],
)

cc_test(
    name = "elf_test",
    size = "small",
    srcs = ["test/elf_test.c"],
    deps = [
        ":exec",
        ":ktest",
    ],
)

cc_test(
    name = "pgtable_test",
    size = "small",
    srcs = ["test/pgtable_test.c"],
    deps = [
        ":ktest",
        ":pgtable",
    ],
)

cc_test(
    name = "bootinfo_test",
    size = "small",
    srcs = ["test/bootinfo_test.c"],
    deps = [
        ":core_boot",
        ":ktest",
    ],
)

# The live memory: one frame allocator and one heap over what the device tree
# described. The pure parts are :memory above, which the host tests cover.
# One lock around the kernel: one hart inside it at a time, which preserves
# the property the single-hart design relied on without auditing every
# structure. The parallelism it buys is in user space.
cc_library(
    name = "klock",
    srcs = ["src/kernel/klock.c"],
    hdrs = ["src/kernel/klock.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [":arch_riscv"],
)

cc_library(
    name = "kernel_mem",
    srcs = ["src/kernel/mem.c"],
    hdrs = ["src/kernel/mem.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":memory",
        ":pgtable",
    ],
)

# Drivers for devices an emulator models faithfully. They exist so that the
# storage and network stacks — carried over with host tests but never run
# against a real device on this architecture — are exercised before the
# board's own controllers are written, which means a later failure in an SD
# or NVMe driver is in that driver rather than in the filesystem above it.
# PCI Express and the M.2 slot behind it. The bus walker assigns the memory
# windows itself, because no firmware here has: a device whose BARs are still
# zero answers configuration reads perfectly and ignores its registers.
# The board's pins. The arithmetic that maps a pin to a register is pure and
# host-tested, because the JH7110 uses a *byte* per pin where a bit would be
# conventional — and writing a bit where a byte belongs lands on a
# neighbouring pin, which no boot log would show. The register *behaviour* is
# not tested and cannot be: no emulator models this controller.
cc_library(
    name = "drivers_gpio",
    srcs = [
        "src/drivers/crg_jh7110.c",
        "src/drivers/gpio_jh7110.c",
    ],
    hdrs = [
        "src/drivers/crg_jh7110.h",
        "src/drivers/gpio_jh7110.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
    ],
)

# The pure halves, which the host can judge.
cc_library(
    name = "gpio_offsets",
    hdrs = [
        "src/drivers/crg_jh7110.h",
        "src/drivers/gpio_jh7110.h",
    ],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

# The frequency governor's policy: pure, and until Phase 3 went looking for
# it, **not in the build at all** — carried over from the ARM project,
# compiled nowhere and tested nowhere. Its test is the policy's half of the
# ARM lesson that a fake proves the policy and nothing about the registers.
cc_library(
    name = "cpufreq",
    srcs = ["src/core/cpufreq.c"],
    hdrs = ["src/core/cpufreq.h"],
    strip_include_prefix = "src",
)

# ── Rule 25: the CVE regression suite ────────────────────────────────────
#
# The product class is a kernel that parses untrusted input, and it has four
# such surfaces: a filesystem on a removable card, an ELF a user supplies,
# packets off a network, and syscall arguments. Each test here is written
# from the attacker's side and asserts a *refusal*, not an absence of
# obvious breakage — a test satisfied by "it did not crash" would pass
# against a parser that read out of bounds and got away with it.
#
# Run as `bazel test //:cve`.
cc_test(
    name = "elf_cve_test",
    size = "small",
    srcs = ["test/cve/elf_cve_test.c"],
    deps = [
        ":exec",
        ":ktest",
    ],
)

test_suite(
    name = "cve",
    tests = [
        ":elf_cve_test",
        ":fdt_test",
        ":partition_test",
        ":tcp_hostile_test",
        ":xfs_test",
    ],
)

cc_test(
    name = "cpufreq_test",
    size = "small",
    srcs = ["test/cpufreq_test.c"],
    deps = [
        ":cpufreq",
        ":ktest",
    ],
)

# The DesignWare I2C master, polled. Seven of them on this board; the one
# that matters holds the PMIC that holds the core voltage. It checks the
# block's own identity register before writing anything, which is the
# cheapest possible defence against the mistake this project has made twice
# with memory-mapped registers.
cc_library(
    name = "drivers_i2c",
    srcs = ["src/drivers/i2c_dw.c"],
    hdrs = ["src/drivers/i2c_dw.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":core_boot",
        ":drivers_gpio",
    ],
)

# Frequency scaling's hardware half: one register, and the measurement that
# proved which one. The policy it serves is `core/cpufreq.c`, which is pure
# and host-tested and knows nothing about this board.
cc_library(
    name = "drivers_cpufreq",
    srcs = ["src/drivers/cpufreq_jh7110.c"],
    hdrs = ["src/drivers/cpufreq_jh7110.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":core_boot",
        ":cpufreq",
        ":drivers_gpio",
    ],
)

cc_test(
    name = "crg_jh7110_test",
    size = "small",
    srcs = ["test/crg_jh7110_test.c"],
    deps = [
        ":gpio_offsets",
        ":ktest",
    ],
)

cc_test(
    name = "gpio_jh7110_test",
    size = "small",
    srcs = ["test/gpio_jh7110_test.c"],
    deps = [
        ":gpio_offsets",
        ":ktest",
    ],
)

cc_library(
    name = "drivers_pci",
    srcs = [
        "src/drivers/nvme.c",
        "src/drivers/pci.c",
        "src/drivers/sdhci.c",
    ],
    hdrs = [
        "src/drivers/nvme.h",
        "src/drivers/pci.h",
        "src/drivers/sdhci.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":drivers_sdcard",
        ":kernel_mem",
        ":storage",
    ],
)

# The SD controller a VisionFive 2 actually has: a DesignWare mobile-storage
# controller, not the Cadence SDHCI the documentation claimed. The card's side
# of the protocol is shared with the SDHCI path.
# The SD card's side of the conversation, with no controller in it: one copy,
# shared by the two transports that have nothing else in common.
# One partition of a disk, as a disk: GPT and MBR, so a filesystem that does
# not start at sector 0 is found.
# The regions of one user address space: what is mapped where, and with what
# rights. Pure and host-tested — and widened to 64-bit addresses, which is
# what had kept it from being used on a machine with 256 GiB of user space.
cc_library(
    name = "vma",
    srcs = ["src/core/vma.c"],
    hdrs = ["src/core/vma.h"],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

cc_library(
    name = "partition",
    srcs = ["src/core/partition.c"],
    hdrs = ["src/core/partition.h"],
    strip_include_prefix = "src",
    deps = [":storage"],
)

cc_library(
    name = "drivers_sdcard",
    srcs = ["src/drivers/sdcard.c"],
    hdrs = ["src/drivers/sdcard.h"],
    strip_include_prefix = "src",
    deps = [":core_boot"],
)

cc_library(
    name = "drivers_dwmmc",
    srcs = ["src/drivers/dwmmc.c"],
    hdrs = ["src/drivers/dwmmc.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":core_boot",
        ":drivers_gpio",
        ":drivers_sdcard",
        ":storage",
    ],
)

# A block device over memory the boot loader fetched: the fixture that lets a
# test filesystem be mounted on real hardware without rewriting the card.
cc_library(
    name = "drivers_ramdisk",
    srcs = ["src/drivers/ramdisk.c"],
    hdrs = ["src/drivers/ramdisk.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [":storage"],
)

cc_library(
    name = "drivers_virtio",
    srcs = [
        "src/drivers/virtio.c",
        "src/drivers/virtio_blk.c",
    ],
    hdrs = [
        "src/drivers/virtio.h",
        "src/drivers/virtio_blk.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":kernel_mem",
        ":storage",
    ],
)

# User mode: an address space of its own, and the way in and out of it.
cc_library(
    name = "kernel_uspace",
    srcs = [
        "src/arch/riscv/switch.S",
        "src/arch/riscv/uspace_entry.S",
        "src/kernel/sched.c",
        "src/kernel/fdobjects.c",
        "src/kernel/tty.c",
        "src/kernel/sock_tcp.c",
        "src/kernel/sys_proc.c",
        "src/kernel/sys_socket.c",
        "src/kernel/sock_udp.c",
        "src/kernel/socket.c",
        "src/kernel/file.c",
        "src/kernel/signal.c",
        "src/kernel/syscall.c",
        "src/kernel/uspace.c",
    ],
    hdrs = [
        "src/arch/riscv/sigframe.h",
        "src/kernel/fdobjects.h",
        "src/kernel/tty.h",
        "src/kernel/socket.h",
        "src/kernel/file.h",
        "src/kernel/signal.h",
        "src/kernel/sched.h",
        "src/kernel/syscall.h",
        "src/kernel/uspace.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":vma",
        ":access",
        ":arch_riscv",
        ":tty",
        ":console",
        ":exec",
        ":fair",
        ":kernel_mem",
        ":kernel_swap",
        ":kernel_net",
        ":kernel_xfs",
        ":kernel_random",
        ":klock",
        ":net",
        ":pgtable",
    ],
)


# A generator for AT_RANDOM and anything else that wants unpredictable
# bytes. Carried over; what seeds it is a question for later (the ARM
# project ended up harvesting interrupt arrival times, because its seed was
# constant under emulation).
cc_library(
    name = "kernel_random",
    srcs = [
        "src/kernel/entropy.c",
        "src/kernel/random.c",
    ],
    hdrs = [
        "src/kernel/entropy.h",
        "src/kernel/random.h",
    ],
    strip_include_prefix = "src",
    deps = [":crypto_hash"],
)

# SHA-256, which the entropy pool reads itself through.
cc_library(
    name = "crypto_hash",
    srcs = ["src/lib/sha256.c"],
    hdrs = ["src/lib/sha256.h"],
    strip_include_prefix = "src",
)

# The live block device and the cache over it.
# What this machine actually is, measured: the numbers the scheduler's
# design depends on (docs/ROADMAP.md, and the scheduler plan's Phase 0).
cc_library(
    name = "kernel_probe",
    srcs = ["src/kernel/probe.c"],
    hdrs = ["src/kernel/probe.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":core_boot",
        ":cpufreq",
        ":drivers_cpufreq",
        ":drivers_gpio",
        ":drivers_i2c",
    ],
)

cc_library(
    name = "kernel_disk",
    srcs = ["src/kernel/disk.c"],
    hdrs = ["src/kernel/disk.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":drivers_dwmmc",
        ":drivers_pci",
        ":drivers_ramdisk",
        ":drivers_virtio",
        ":kernel_mem",
        ":partition",
        ":storage",
    ],
)

# The board's own Ethernet. Everything its register map, PHY and clocks
# needed was measured by the probe below first — CLAUDE.md §3 records what,
# and why a driver written before that would have been the wrong driver.
cc_library(
    name = "drivers_dwmac",
    srcs = ["src/drivers/dwmac.c"],
    hdrs = ["src/drivers/dwmac.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":arch_riscv",
        ":console",
        ":core_boot",
        ":drivers_gpio",
    ],
)

# Reconnaissance for the board's own Ethernet: which of the two controllers,
# which DesignWare register generation, which PHY, and whether it clocks.
# Reads only; the driver follows once the board has answered.
cc_library(
    name = "dwmac_probe",
    srcs = ["src/drivers/dwmac_probe.c"],
    hdrs = ["src/drivers/dwmac_probe.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":core_boot",
        ":drivers_gpio",
    ],
)

# The mount table: which filesystem is mounted where, and automount by the
# filesystem's label. ROOT becomes /, anything else a volume under /volumes.
cc_library(
    name = "kernel_volumes",
    srcs = ["src/kernel/volumes.c"],
    hdrs = ["src/kernel/volumes.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":kernel_disk",
        ":kernel_mem",
        ":xfs",
    ],
)

# The filesystem on the disk: the carried-over XFS over the live cache.
cc_library(
    name = "kernel_xfs",
    srcs = ["src/kernel/xfsmount.c"],
    hdrs = ["src/kernel/xfsmount.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":crypto_hash",
        ":kernel_disk",
        ":kernel_mem",
        ":kernel_volumes",
        ":xfs",
    ],
)

# Swap: pages kept in a file on the filesystem, the way Windows keeps a page
# file. It needs the filesystem only to find the file's blocks — swapping
# overwrites them and changes no metadata — and the block cache to move the
# pages.
cc_library(
    name = "kernel_swap",
    srcs = ["src/kernel/swap.c"],
    hdrs = ["src/kernel/swap.h"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":kernel_disk",
        ":kernel_xfs",
    ],
)

# ── the kernel ─────────────────────────────────────────────────────────────

# memset and friends: the kernel links no libc, and the compiler emits calls
# to these for a structure initialiser. Kernel-only — the host tests above
# use the host's, which is why this is not in :kfmt.
cc_library(
    name = "kmem",
    srcs = ["src/lib/kmem.c"],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
)

cc_library(
    name = "arch_riscv",
    srcs = [
        "src/arch/riscv/mmu.c",
        "src/arch/riscv/plic.c",
        "src/arch/riscv/fp.S",
        "src/arch/riscv/fp.c",
        "src/arch/riscv/sbi.c",
        "src/arch/riscv/smp.c",
        "src/arch/riscv/start.S",
        "src/arch/riscv/timer.c",
        "src/arch/riscv/trap.c",
        "src/arch/riscv/trap_entry.S",
        "src/kernel/irq.c",
    ],
    hdrs = [
        "src/arch/riscv/frame.h",
        "src/arch/riscv/layout.h",
        "src/arch/riscv/mmu.h",
        "src/arch/riscv/plic.h",
        "src/arch/riscv/cache.h",
        "src/arch/riscv/fp.h",
        "src/arch/riscv/sbi.h",
        "src/arch/riscv/smp.h",
        "src/arch/riscv/spinlock.h",
        "src/arch/riscv/timer.h",
        "src/arch/riscv/trap.h",
        "src/kernel/irq.h",
    ],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":console",
        ":pgtable",
    ],
    # _start is referenced by nothing, so without this the linker drops it
    # from the archive and the image boots into whatever follows.
    alwayslink = True,
)

cc_library(
    name = "board_jh7110",
    srcs = [
        "src/board/jh7110/console.c",
        "src/board/jh7110/main.c",
        "src/board/jh7110/selftest.c",
        "src/board/jh7110/uart.c",
        "src/board/jh7110/usertest.S",
    ],
    hdrs = [
        "src/board/jh7110/console.h",
        "src/board/jh7110/selftest.h",
        "src/board/jh7110/uart.h",
        "src/board/jh7110/version.h",
    ],
    # The series comes from the monorepo's single constant (Rule 28). It has
    # to be defined on the target that compiles main.c, not on the binary:
    # a local_define does not reach a dependency.
    local_defines = ['MINIKERN_VERSION=\\"%s\\"' % MINIKERN_VERSION],
    strip_include_prefix = "src",
    target_compatible_with = ["//platforms:riscv64"],
    deps = [
        ":kernel_probe",
        ":crypto_hash",
        ":arch_riscv",
        ":console",
        ":core_boot",
        ":drivers_gpio",
        ":drivers_pci",
        ":drivers_virtio",
        ":kernel_disk",
        ":kernel_mem",
        ":kernel_net",
        ":kernel_swap",
        ":kernel_uspace",
        ":virtio_net",
        ":drivers_dwmac",
        ":dwmac_probe",
        ":kernel_volumes",
        ":kernel_xfs",
    ],
    alwayslink = True,
)

kernel_binary(
    name = "kernel",
    srcs = [],
    deps = [
        ":arch_riscv",
        ":board_jh7110",
        ":drivers_gpio",
        ":drivers_pci",
        ":drivers_virtio",
        ":kernel_disk",
        ":kernel_mem",
        ":kernel_swap",
        ":kernel_uspace",
        ":kernel_xfs",
        ":kmem",
    ],
)

# The same kernel for a real VisionFive 2: linked at DRAM's base plus 2 MiB,
# which on this board is 0x40200000 rather than QEMU's 0x80200000, and emitted
# as a flat image because that is what a loader's `booti` takes.
kernel_binary(
    name = "kernel_board",
    srcs = [],
    binary = True,
    load_address = "0x40200000",
    tags = ["manual"],
    deps = [
        ":arch_riscv",
        ":board_jh7110",
        ":drivers_gpio",
        ":drivers_pci",
        ":drivers_virtio",
        ":kernel_disk",
        ":kernel_mem",
        ":kernel_swap",
        ":kernel_uspace",
        ":kernel_xfs",
        ":kmem",
    ],
)

kernel_image(
    name = "minikern_board",
    kernel = ":kernel_board",
    tags = ["manual"],
)

kernel_image(
    name = "minikern",
    kernel = ":kernel",
    tags = ["manual"],
)

# ── a user program ─────────────────────────────────────────────────────────
#
# An ordinary static ELF64, built with the same toolchain at a user address
# and depending on nothing from the kernel. It reads the state the kernel
# started it in — argc, argv, the environment, the auxiliary vector — and
# reports it, so the test checks the kernel's work from outside rather than
# trusting the kernel's own account of it.

# Four threads and a shared counter, with no runtime between the kernel and
# the result: the reproducer for the four-hart race (CLAUDE.md, docs/ROADMAP.md
# §0). It **passes** — raw clone, atomics and a futex mutex are all correct
# at this scale — which is what narrowed the remaining fault to something a
# Go runtime does that this does not.
user_binary(
    name = "race",
    srcs = [
        "src/userland/race/clone.S",
        "src/userland/race/main.c",
        "src/userland/start.S",
    ],
)

user_image(
    name = "race.elf",
    program = ":race",
)

# Does a real fork isolate the two processes? Written to be the proof rather
# than a demonstration: both sides write the same addresses and each checks
# it sees only its own.
user_binary(
    name = "forktest",
    srcs = [
        "src/userland/forktest/main.c",
        "src/userland/start.S",
    ],
)

user_image(
    name = "forktest.elf",
    program = ":forktest",
)

# One busy thread and nothing else, for measuring the preemption model: with
# no competition there is no slice to end, so the timer interrupt count this
# produces is the model's own overhead.
user_binary(
    name = "spin",
    srcs = [
        "src/userland/spin/main.c",
        "src/userland/start.S",
    ],
)

user_image(
    name = "spin.elf",
    program = ":spin",
)

user_binary(
    name = "init",
    srcs = [
        "src/userland/init.c",
        "src/userland/start.S",
    ],
)

user_image(
    name = "init.elf",
    program = ":init",
)

# ── the emulator ───────────────────────────────────────────────────────────
#
# Unlike the board this kernel targets, QEMU's virt machine models everything
# the kernel needs: SBI (through OpenSBI), the PLIC, the CLINT and virtio. So
# the whole bring-up is verifiable here, which is the opposite of the
# position the ARM project ended in with three stubbed registers.

sh_test(
    name = "boot_qemu_test",
    size = "medium",
    srcs = ["test/boot_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P0.3: the trap path, proved by taking deliberate faults.
sh_test(
    name = "selftest_qemu_test",
    size = "medium",
    srcs = ["test/selftest_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P4.1: a real block device, and the first thing on this architecture to
# exercise the carried-over buffer cache against one.
sh_test(
    name = "disk_qemu_test",
    size = "medium",
    srcs = ["test/disk_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P4.4: XFS on a real device — the whole stack, judged against filesystems
# the vendor's own tools made and call clean.
sh_test(
    name = "xfs_qemu_test",
    size = "large",
    srcs = ["test/xfs_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P2: user mode — a process, its own address space, and syscalls.
sh_test(
    name = "user_qemu_test",
    size = "medium",
    srcs = ["test/user_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P2.1: the whole machine — firmware to a program's own account of how it
# was started.
sh_test(
    name = "init_qemu_test",
    size = "large",
    srcs = ["test/init_qemu_test.sh"],
    args = [
        "$(rootpath :minikern)",
        "$(rootpath :init.elf)",
    ],
    data = [
        ":init.elf",
        ":minikern",
    ],
    tags = ["manual"],
)

# P2.4: the goal. A stock gc Go binary for linux/riscv64, built by the
# ordinary Go rules and knowing nothing about this kernel, running unchanged.
sh_test(
    name = "go_qemu_test",
    size = "large",
    srcs = ["test/go_qemu_test.sh"],
    args = [
        "$(rootpath :minikern)",
        "$(rootpath //src/userland/gotest:gotest)",
    ],
    data = [
        ":minikern",
        "//src/userland/gotest:gotest",
    ],
    tags = ["manual"],
)

# §0's reproducer: the four-hart race, hammered until it shows.
#
# Not a gate — the thing it looks for is known to be there — but the
# instrument that makes a question about it answerable in minutes rather than
# a day. See `test/race_qemu_test.sh`.
sh_test(
    name = "race_qemu_test",
    size = "large",
    srcs = ["test/race_qemu_test.sh"],
    args = [
        "$(rootpath :minikern)",
        "$(rootpath //src/userland/racestress:racestress)",
    ],
    data = [
        ":minikern",
        "//src/userland/racestress:racestress",
    ],
    tags = ["manual"],
)

# P3: four harts, and a Go program using all of them.
sh_test(
    name = "smp_qemu_test",
    size = "large",
    srcs = ["test/smp_qemu_test.sh"],
    args = [
        "$(rootpath :minikern)",
        "$(rootpath //src/userland/gotest:gotest)",
    ],
    data = [
        ":minikern",
        "//src/userland/gotest:gotest",
    ],
    tags = ["manual"],
)

# P5: the network — the carried-over stack on a new adapter.
sh_test(
    name = "net_qemu_test",
    size = "large",
    srcs = ["test/net_qemu_test.sh"],
    args = ["$(rootpath :minikern)"],
    data = [":minikern"],
    tags = ["manual"],
)

# P7: Go's net package, which needs the descriptor layer, a real epoll and
# sockets over the carried-over stack.
sh_test(
    name = "netgo_qemu_test",
    size = "large",
    srcs = ["test/netgo_qemu_test.sh"],
    args = [
        "$(rootpath :minikern)",
        "$(rootpath //src/userland/netgo:netgo)",
    ],
    data = [
        ":minikern",
        "//src/userland/netgo:netgo",
    ],
    tags = ["manual"],
)

# Static analysis, in SonarQube's place — and a better fit than it, because
# these can be told the target triple and the absence of a libc, where Sonar's
# rules are tuned for application code and argue with memory-mapped registers,
# inline assembly and bare-metal patterns. (The server also has no C analyser
# at all: 27 languages, none of them C.)
#
# Tagged `manual` for the same reason the packaging targets are: they shell out
# to host tools that are not on every machine, and the whole-tree green check
# has to stay green. The consequence is the same too — run them explicitly,
# with `bazel test //:analysis`.
sh_test(
    name = "clang_tidy_test",
    size = "large",
    srcs = ["test/clang_tidy_test.sh"],
    data = glob([
        "src/**/*.c",
        "src/**/*.h",
    ]),
    tags = [
        "external",
        "manual",
    ],
)

sh_test(
    name = "clang_analyze_test",
    size = "large",
    srcs = ["test/clang_analyze_test.sh"],
    data = glob([
        "src/**/*.c",
        "src/**/*.h",
    ]),
    tags = [
        "external",
        "manual",
    ],
)

sh_test(
    name = "cppcheck_test",
    size = "large",
    srcs = ["test/cppcheck_test.sh"],
    data = glob([
        "src/**/*.c",
        "src/**/*.h",
    ]),
    tags = [
        "external",
        "manual",
    ],
)

test_suite(
    name = "analysis",
    tags = ["manual"],
    tests = [
        ":clang_analyze_test",
        ":clang_tidy_test",
        ":cppcheck_test",
    ],
)

# The board suite: every self-test this kernel has, on the real VisionFive 2.
#
# **`manual`, and for a stronger reason than the QEMU targets.** Those need a
# host tool; this needs the physical board attached to this machine, powered
# through the operator's relay on `/dev/ttyACM0`, its console on
# `/dev/ttyUSB0`, and a web server handing out the image. It is the gate that
# matters — the emulator agrees with whatever you believed — and it is the one
# a wildcard build must never reach for.
#
# `test/hil/board.py` is one boot; `test/hil/suite.py` is the cases. Python
# and `pyserial` because the harness talks to two serial ports and waits on
# prompts, which is not work for the kernel's own toolchain; it is an external
# instrument in the same category as `sonar-scanner`, and nothing built or
# tested here depends on it.
sh_test(
    name = "hil_test",
    size = "enormous",
    srcs = ["test/hil_test.sh"],
    data = [
        "test/hil/board.py",
        "test/hil/image.py",
        "test/hil/suite.py",
    ],
    tags = [
        "external",
        "manual",
    ],
)

test_suite(
    name = "qemu_tests",
    tags = ["manual"],
    tests = [
        ":boot_qemu_test",
        ":disk_qemu_test",
        ":go_qemu_test",
        ":init_qemu_test",
        ":net_qemu_test",
        ":netgo_qemu_test",
        ":selftest_qemu_test",
        ":smp_qemu_test",
        ":user_qemu_test",
        ":xfs_qemu_test",
    ],
)
