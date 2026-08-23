obj-m += ndt_cache.o
ccflags-y += -I$(src)/../../include

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

.PHONY: all clean

all: module raw_extent_test

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

raw_extent_test: raw_extent_test.c ../../include/ndt_cache_ioctl.h
	$(CC) -O2 -Wall -Wextra -Werror -I../../include -o $@ raw_extent_test.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) raw_extent_test
