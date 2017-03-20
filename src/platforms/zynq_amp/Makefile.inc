P = ~/Projects/piksi_buildroot/buildroot/output/host/usr/bin/
CROSS_COMPILE ?= $(P)arm-linux-gnueabihf-
CC = $(CROSS_COMPILE)gcc

SRC += 	timing.c	\
        coredump.c  \
        crash_watchdog.c \
