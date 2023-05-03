obj-m += pantry.o

all: kmod format_disk_as_pantryfs

format_disk_as_pantryfs: CC = gcc
format_disk_as_pantryfs: CFLAGS = -g -Wall

PHONY += kmod
kmod:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

PHONY += clean
clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f format_disk_as_pantryfs

.PHONY: $(PHONY)
