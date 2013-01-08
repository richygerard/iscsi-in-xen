# For Backend Compilation - uncomment before use
#obj-m  := front.o

# For Frontend Compilation - uncomment before use
#obj-m := back.o


#else
# normal makefile
    KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

#endif

clean:
	make -C /lib/modules/`uname -r`/build/ M=$(PWD) clean
