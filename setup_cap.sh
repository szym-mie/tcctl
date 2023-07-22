# !/bin/sh

usermod $(whoami) kmem
mkdir /etc/udev/rules.d
echo 'SUBSYSTEM="mem", KERNEL="mem", GROUP="kmem", MODE="0660"' | tee /etc/udev/rules.d/98-mem.rules
echo reboot system for changes to take effect
