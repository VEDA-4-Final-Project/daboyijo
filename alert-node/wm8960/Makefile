TARGET = wm8960_custum_daboyjo
obj-m += $(TARGET).o
DTBO = $(TARGET).dtbo

KDIR=/lib/modules/$(shell uname -r)/build
PWD=$(shell pwd)

all: modules dtbo

clean:
	make -C $(KDIR) M=$(PWD) clean
	rm -f *.dtbo

modules:
	make -C $(KDIR) M=$(PWD) modules

dtbo: $(TARGET).dts
	dtc -@ -I dts -O dtb -o $(DTBO) $<



	


