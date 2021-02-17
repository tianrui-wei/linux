#!/bin/bash
~/research/qemu/objdir/riscv64-softmmu/qemu-system-riscv64 \
-M virt-cosim \
-smp 4 \
-serial stdio \
-display none \
-m 2G \
-dtb virt.dtb \
-bios opensbi-riscv64-virt-fw_jump.bin \
-kernel Image \
-drive file=~/research/busybox/rootfs.img,format=raw,id=hd0 \
-device virtio-blk-device,drive=hd0 \
-append "root=/dev/vda ro console=ttyS0 init=/sbin/init" \
-object rng-random,filename=/dev/urandom,id=rng0 \
-device virtio-rng-device,rng=rng0 \
-device virtio-net-device,netdev=usernet \
-netdev user,id=usernet \
-netdev user,id=net4 \
-device remote-port-net,rp-adaptor0=/machine/cosim,rp-chan0=256,rp-chan1=266,netdev=net4 \
-machine-path /tmp/machine-riscv64
