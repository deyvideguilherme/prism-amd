#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 5 || $# -gt 6 ]]; then
    echo "Usage: $0 <vmlinuz> <chroma_amd.ko> <static busybox> <static prisma-ctl> <static test_kernel_abi> [drm_kms_helper.ko]" >&2
    exit 2
fi

kernel_image="$(realpath "$1")"
module="$(realpath "$2")"
busybox="$(realpath "$3")"
cli="$(realpath "$4")"
abi_test="$(realpath "$5")"
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
initrd="$scratch/initrd"
mkdir -p "$initrd"/{bin,dev,proc,sys,tmp,root}
install -m755 "$busybox" "$initrd/bin/busybox"
install -m755 "$cli" "$initrd/bin/prisma-ctl"
install -m755 "$abi_test" "$initrd/bin/test_kernel_abi"
install -m644 "$module" "$initrd/chroma_amd.ko"
if [[ $# == 6 ]]; then install -m644 "$6" "$initrd/drm_kms_helper.ko"; fi

cat > "$initrd/init" <<'GUEST'
#!/bin/busybox sh
set -eu
export PATH=/bin HOME=/root
/bin/busybox --install -s /bin
fail() {
    echo "FAIL kernel integration"
    dmesg | tail -n 120
    poweroff -f
}
trap fail EXIT
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
uname -r
prisma-ctl --version
if prisma-ctl status --json || prisma-ctl doctor; then fail; fi
prisma-ctl telemetry --json | grep -F '"card": null'

if test -f /drm_kms_helper.ko; then insmod /drm_kms_helper.ko; fi
insmod /chroma_amd.ko
test "$(cat /sys/kernel/chroma_amd/abi_version)" = 1
test_kernel_abi
prisma-ctl status --json
prisma-ctl set -s 165 -c 115 -b -20 -e 1
test "$(cat /sys/kernel/chroma_amd/saturation)" = 165
test "$(cat /sys/kernel/chroma_amd/contrast)" = 115
test "$(cat /sys/kernel/chroma_amd/brightness)" = -20
prisma-ctl profile save gaming
prisma-ctl set --enabled 0
test "$(cat /sys/kernel/chroma_amd/enabled)" = 0
prisma-ctl profile apply gaming
prisma-ctl profile list | grep -F '[sat=165, con=115, bri=-20, en=1]'
test "$(cat /sys/kernel/chroma_amd/enabled)" = 1
for arg in 165bad junk 99999999999999999999 401; do
    if prisma-ctl set -s "$arg"; then fail; fi
    test "$(cat /sys/kernel/chroma_amd/saturation)" = 165
done
if prisma-ctl doctor; then fail; fi

if test -w /sys/power/pm_test && grep -qw devices /sys/power/pm_test &&
   grep -qw freeze /sys/power/state; then
    echo devices > /sys/power/pm_test
    echo freeze > /sys/power/state
    test "$(cat /sys/kernel/chroma_amd/saturation)" = 165
    echo "PASS PM devices suspend/resume test"
fi
rmmod chroma_amd
test ! -e /dev/chroma_amd
insmod /chroma_amd.ko
test "$(cat /sys/kernel/chroma_amd/saturation)" = 100
prisma-ctl profile apply gaming
test "$(cat /sys/kernel/chroma_amd/saturation)" = 165
prisma-ctl profile reset
test "$(cat /sys/kernel/chroma_amd/saturation)" = 100
prisma-ctl profile delete gaming
test ! -e /root/.config/prisma/profiles/gaming.conf
rmmod chroma_amd
if dmesg | grep -E '(\] |^)(BUG:|WARNING:|Oops:|Call Trace:)'; then fail; fi
echo "PASS kernel integration"
trap - EXIT
poweroff -f
GUEST

chmod +x "$initrd/init"
(cd "$initrd" && find . -print0 | cpio -0 -o -H newc --owner=0:0 --quiet | gzip) > "$scratch/initramfs.gz"
timeout 90 "${QEMU:-qemu-system-x86_64}" ${QEMU_ACCEL:--accel kvm -cpu host} \
    -machine q35 -m 512M -smp 4 -nic none -display none -serial stdio -monitor none -no-reboot \
    -kernel "$kernel_image" -initrd "$scratch/initramfs.gz" \
    -append 'console=ttyS0 quiet panic=-1' | tee "$scratch/guest.log"
grep -Fxq 'PASS kernel integration' <(tr -d '\r' < "$scratch/guest.log")
