obj-m := limit_rate.o

KERNELDIR ?= /usr/src/linux-headers-2.6.18-6-686/

PWD :=  $(shell pwd)

all:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
