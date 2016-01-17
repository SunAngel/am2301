
obj-m := am2301.o
#dht22.o

KERNEL_SRC=/lib/modules/$(shell uname -r)/build
#KERNEL_SRC=/mnt/nanoStorage/src/pi-kernel
#KERNEL_SRC=/mnt/MyBook/Etna/src/pi-kernel

all:
	$(MAKE) -C ${KERNEL_SRC} M=$(PWD) modules

clean:
	$(MAKE) -C ${KERNEL_SRC} M=$(PWD) clean
