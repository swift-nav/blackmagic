/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "general.h"
#include "gdb_if.h"
#include "version.h"

#include <assert.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdarg.h>

#include "target/adiv5.h"

ADIv5_AP_t franken_fake_ap;

/*
Description of DAP ROM table, from Zynq 7000 TRM, Table 25-5

DAP ROM 0xF880_0000
ETB 0xF880_1000
CTI (connected to ETB, TPIU) 0xF880_2000
TPIU 0xF880_3000
Funnel 0xF880_4000
ITM 0xF880_5000
CTI (connected to FTM) 0xF880_9000
FTM 0xF880_B000
Cortex-A9 ROM 0xF888_0000
CPU0 debug logic 0xF889_0000
CPU0 PMU 0xF889_1000
CPU1 debug logic 0xF889_2000
CPU1 PMU 0xF889_3000
CTI (connected to CPU0, PTM0) 0xF889_8000
CTI (connected to CPU1, PTM1) 0xF889_9000
PTM0 (for CPU0) 0xF889_C000
PTM1 (for CPU1) 0xF889_D000
 */
void platform_init(void)
{
	printf("\nBlack Magic Probe (" FIRMWARE_VERSION ")\n");
	printf("Copyright (C) 2015  Black Sphere Technologies Ltd.\n");
	printf("License GPLv3+: GNU GPL version 3 or later "
	       "<http://gnu.org/licenses/gpl.html>\n\n");

	extern bool cortexa_probe(volatile uint32_t *dbg, volatile uint32_t *slcr);
	int pmem = open("/dev/mem", O_RDWR | O_SYNC);
	volatile uint32_t *dbg;
	volatile uint32_t *slcr = NULL;
	dbg = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
	           pmem, 0xfebf0000);
	cortexa_probe(dbg, slcr);
	dbg = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED,
	           pmem, 0xfebf2000);
	cortexa_probe(dbg, slcr);

	assert(gdb_if_init() == 0);
}

void platform_srst_set_val(bool assert)
{
	(void)assert;
}

bool platform_srst_get_val(void) { return false; }

const char *platform_target_voltage(void)
{
	return "not supported";
}

void platform_delay(uint32_t ms)
{
	usleep(ms * 1000);
}

uint32_t platform_time_ms(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

void piksi_log(char *fmt, ...)
{
	FILE *piksi_log = popen("/usr/bin/sbp_log --error", "w");
	va_list ap;

	va_start(ap, fmt);
	vfprintf(piksi_log, fmt, ap);
	va_end(ap);

	pclose(piksi_log);
}
