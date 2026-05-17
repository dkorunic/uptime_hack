# SPDX-License-Identifier: GPL-2.0-or-later
ifneq ($(KERNELRELEASE),)
obj-m += uptime_hack.o
obj-m += unhide.o

else
KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) PAHOLE=/usr/bin/pahole modules

modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) PAHOLE=/usr/bin/pahole modules_install

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean

endif
