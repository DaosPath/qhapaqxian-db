#!/usr/bin/env bash
set -euo pipefail

echo "== QhapaqXian KVM probe =="

if [[ ! -e /dev/kvm ]]; then
	echo "KVM device missing: /dev/kvm"
	exit 1
fi

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
	echo "qemu-system-x86_64 not found on PATH"
	exit 1
fi

if ! qemu-system-x86_64 -accel kvm -machine help >/dev/null 2>&1; then
	echo "QEMU build does not expose KVM acceleration"
	exit 1
fi

echo "KVM probe ok"