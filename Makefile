#  Dinko Korunic 'kreator', 2012.
#  Uptime hack LKM
# 
#  Copyright (C) 2012  Dinko Korunic

KVERSION=$(shell uname -r)

obj-m += uptime_hack.o

all: 
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules

clean: 
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
