#  Dinko Korunic 'kreator', 2012.
#  Uptime hack LKM
# 
#  Copyright (C) 2012  Dinko Korunic

DEBUG=n
KVERSION=$(shell uname -r)

ifeq ($(DEBUG),y)
	ccflags-y += -DDEBUG
endif

obj-m += uptime_hack.o

all: 
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean: 
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
