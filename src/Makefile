PROBE_HOST ?= zynq_amp
PLATFORM_DIR = platforms/$(PROBE_HOST)
VPATH += $(PLATFORM_DIR) platforms/common target
ENABLE_DEBUG ?=

ifneq ($(V), 1)
MAKEFLAGS += --no-print-dir
Q := @
endif

OPT_FLAGS ?= -O0

CFLAGS += -Wall -Wextra -Werror -Wno-char-subscripts\
	$(OPT_FLAGS) -std=gnu99 -g3 -MD \
	-I. -Iinclude -Iplatforms/common -I$(PLATFORM_DIR)
LDFLAGS += $(OPT_FLAGS)

ifeq ($(ENABLE_DEBUG), 1)
CFLAGS += -DENABLE_DEBUG
endif

SRC =			\
	command.c	\
	cortexa.c	\
	crc32.c		\
	exception.c	\
	gdb_if.c	\
	gdb_main.c	\
	gdb_hostio.c	\
	gdb_packet.c	\
	hex_utils.c	\
	main.c		\
	morse.c		\
	platform.c	\
	target.c	\

include $(PLATFORM_DIR)/Makefile.inc

OBJ = $(SRC:.c=.o)

blackmagic: include/version.h $(OBJ)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o:	%.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

%.bin:	%
	@echo "  OBJCOPY $@"
	$(Q)$(OBJCOPY) -O binary $^ $@

%.hex:	%
	@echo "  OBJCOPY $@"
	$(Q)$(OBJCOPY) -O ihex $^ $@

.PHONY:	clean host_clean all_platforms FORCE

clean:	host_clean
	$(Q)echo "  CLEAN"
	-$(Q)$(RM) -f *.o *.d *~ blackmagic $(HOSTFILES)
	-$(Q)$(RM) -f platforms/*/*.o platforms/*/*.d mapfile include/version.h

all_platforms:
	$(Q)set -e ;\
	mkdir -p artifacts/$(shell git describe --always) ;\
	echo "<html><body><ul>" > artifacts/index.html ;\
	for i in platforms/*/Makefile.inc ; do \
		export DIRNAME=`dirname $$i` ;\
		export PROBE_HOST=`basename $$DIRNAME` ;\
		export CFLAGS=-Werror ;\
		echo "Building for hardware platform: $$PROBE_HOST" ;\
		$(MAKE) $(MAKEFLAGS) clean ;\
		$(MAKE) $(MAKEFLAGS);\
		if [ -f blackmagic.bin ]; then \
			mv blackmagic.bin artifacts/blackmagic-$$PROBE_HOST.bin ;\
			echo "<li><a href='blackmagic-$$PROBE_HOST.bin'>$$PROBE_HOST</a></li>"\
				>> artifacts/index.html ;\
		fi ;\
	done ;\
	echo "</ul></body></html>" >> artifacts/index.html ;\
	cp artifacts/*.bin artifacts/$(shell git describe --always)

command.c: include/version.h

include/version.h: FORCE
	$(Q)echo "  GIT     include/version.h"
	$(Q)echo "#define FIRMWARE_VERSION \"`git describe --always --dirty`\"" > $@

-include *.d
