# SPDX-License-Identifier: GPL-2.0
#
# Makefile for MT7927 WiFi driver
#

obj-m := mt7927.o

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

# Build options
ccflags-y += -DDEBUG

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a $(KVER)

uninstall:
	rm -f /lib/modules/$(KVER)/extra/mt7927.ko
	depmod -a $(KVER)

.PHONY: all modules clean install uninstall
