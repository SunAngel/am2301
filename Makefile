MOD=am2301

obj-m := $(MOD).o

KERNEL_SRC=/lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C ${KERNEL_SRC} M=$(PWD) modules

clean:
	$(MAKE) -C ${KERNEL_SRC} M=$(PWD) clean
