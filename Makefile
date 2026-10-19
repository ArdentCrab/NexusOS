CXX      := g++
LD       := ld
CXXFLAGS := -std=c++23 -O2 -g \
            -ffreestanding -fno-exceptions -fno-rtti \
            -fno-stack-protector -fno-pic -mno-red-zone \
            -mno-sse -mno-sse2 -mno-mmx -mno-80387 \
            -mcmodel=kernel -fno-omit-frame-pointer \
            -fno-threadsafe-statics -fno-use-cxa-atexit \
            -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
            -I.
LDFLAGS  := -nostdlib -static -z max-page-size=0x1000
SRCS     := $(wildcard *.cpp) $(wildcard kernel/*/*.cpp)
OBJS     := $(SRCS:.cpp=.o)

.PHONY: all clean run runkvm

all: nexus.elf

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

nexus.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o $@ $(OBJS)

run: nexus.elf
	rm -rf iso_root nexus.iso
	mkdir -p iso_root
	cp nexus.elf limine.conf iso_root/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	xorriso -as mkisofs -b limine-bios-cd.bin \
	        -no-emul-boot -boot-load-size 4 -boot-info-table \
	        --efi-boot limine-uefi-cd.bin \
	        -efi-boot-part --efi-boot-image --protective-msdos-label \
	        iso_root -o nexus.iso 2>&1 | tail -3
	./limine/limine bios-install nexus.iso 2>&1 | tail -2
	qemu-system-x86_64 -M q35 -drive if=none,id=d0,file=disk.img,format=raw -device virtio-blk-pci,drive=d0,disable-modern=on -cdrom nexus.iso -serial stdio -cpu max -m 512M -no-reboot

runkvm: nexus.elf
	rm -rf iso_root nexus.iso
	mkdir -p iso_root
	cp nexus.elf limine.conf iso_root/
	cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	xorriso -as mkisofs -b limine-bios-cd.bin \
	        -no-emul-boot -boot-load-size 4 -boot-info-table \
	        --efi-boot limine-uefi-cd.bin \
	        -efi-boot-part --efi-boot-image --protective-msdos-label \
	        iso_root -o nexus.iso 2>&1 | tail -3
	./limine/limine bios-install nexus.iso 2>&1 | tail -2
	qemu-system-x86_64 -cdrom nexus.iso -serial stdio -accel kvm -cpu host -m 512M -no-reboot

clean:
	rm -f *.o *.elf *.iso
	rm -rf iso_root
