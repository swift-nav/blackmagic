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

/* This file implements a basic command interpreter for GDB 'monitor'
 * commands.
 */

#include "general.h"
#include "exception.h"
#include "command.h"
#include "gdb_packet.h"
#include "target.h"
#include "morse.h"
#include "version.h"
#include "coredump.h"

#ifdef PLATFORM_HAS_TRACESWO
#	include "traceswo.h"
#endif

typedef bool (*cmd_handler)(target *t, int argc, const char **argv);

struct command_s {
	const char *cmd;
	cmd_handler handler;

	const char *help;
};

enum assert_srst_t{
	ASSERT_NEVER = 0,
	ASSERT_UNTIL_SCAN,
	ASSERT_UNTIL_ATTACH
};

static bool cmd_version(void);
static bool cmd_help(target *t);

static bool cmd_targets(void);
static bool cmd_morse(void);
static bool cmd_assert_srst(target *t, int argc, const char **argv);
static bool cmd_halt_timeout(target *t, int argc, const char **argv);
static bool cmd_hard_srst(void);
static bool cmd_gcore(target *t);
#ifdef PLATFORM_HAS_POWER_SWITCH
static bool cmd_target_power(target *t, int argc, const char **argv);
#endif
#ifdef PLATFORM_HAS_TRACESWO
static bool cmd_traceswo(target *t, int argc, const char **argv);
#endif
#ifdef PLATFORM_HAS_DEBUG
static bool cmd_debug_bmp(target *t, int argc, const char **argv);
#endif

const struct command_s cmd_list[] = {
	{"version", (cmd_handler)cmd_version, "Display firmware version info"},
	{"help", (cmd_handler)cmd_help, "Display help for monitor commands"},
	{"targets", (cmd_handler)cmd_targets, "Display list of available targets" },
	{"morse", (cmd_handler)cmd_morse, "Display morse error message" },
	{"assert_srst", (cmd_handler)cmd_assert_srst, "Assert SRST until:(never(default)| scan | attach)" },
	{"halt_timeout", (cmd_handler)cmd_halt_timeout, "Timeout (ms) to wait until Cortex-M is halted: (Default 2000)" },
	{"hard_srst", (cmd_handler)cmd_hard_srst, "Force a pulse on the hard SRST line - disconnects target" },
	{"gcore", (cmd_handler)cmd_gcore, "Generate a core dump" },
#ifdef PLATFORM_HAS_POWER_SWITCH
	{"tpwr", (cmd_handler)cmd_target_power, "Supplies power to the target: (enable|disable)"},
#endif
#ifdef PLATFORM_HAS_TRACESWO
	{"traceswo", (cmd_handler)cmd_traceswo, "Start trace capture [(baudrate) for async swo]" },
#endif
#ifdef PLATFORM_HAS_DEBUG
	{"debug_bmp", (cmd_handler)cmd_debug_bmp, "Output BMP \"debug\" strings to the second vcom: (enable|disable)"},
#endif
	{NULL, NULL, NULL}
};

static enum assert_srst_t assert_srst;
#ifdef PLATFORM_HAS_DEBUG
bool debug_bmp;
#endif
long cortexm_wait_timeout = 2000; /* Timeout to wait for Cortex to react on halt command. */

int command_process(target *t, char *cmd)
{
	const struct command_s *c;
	int argc = 0;
	const char **argv;

	/* Initial estimate for argc */
	for(char *s = cmd; *s; s++)
		if((*s == ' ') || (*s == '\t')) argc++;

	argv = alloca(sizeof(const char *) * argc);

	/* Tokenize cmd to find argv */
	for(argc = 0, argv[argc] = strtok(cmd, " \t");
		argv[argc]; argv[++argc] = strtok(NULL, " \t"));

	/* Look for match and call handler */
	for(c = cmd_list; c->cmd; c++) {
		/* Accept a partial match as GDB does.
		 * So 'mon ver' will match 'monitor version'
		 */
		if ((argc == 0) || !strncmp(argv[0], c->cmd, strlen(argv[0])))
			return !c->handler(t, argc, argv);
	}

	if (!t)
		return -1;

	return target_command(t, argc, argv);
}

bool cmd_version(void)
{
	gdb_outf("Black Magic Probe (Firmware " FIRMWARE_VERSION ") (Hardware Version %d)\n", platform_hwversion());
	gdb_out("Copyright (C) 2015  Black Sphere Technologies Ltd.\n");
	gdb_out("License GPLv3+: GNU GPL version 3 or later "
		"<http://gnu.org/licenses/gpl.html>\n\n");

	return true;
}

bool cmd_help(target *t)
{
	const struct command_s *c;

	gdb_out("General commands:\n");
	for(c = cmd_list; c->cmd; c++)
		gdb_outf("\t%s -- %s\n", c->cmd, c->help);

	if (!t)
		return -1;

	target_command_help(t);

	return true;
}

static void display_target(int i, target *t, void *context)
{
	(void)context;
	gdb_outf("%2d   %c  %s\n", i, target_attached(t)?'*':' ', target_driver_name(t));
}

bool cmd_targets(void)
{
	gdb_out("Available Targets:\n");
	gdb_out("No. Att Driver\n");
	if (!target_foreach(display_target, NULL)) {
		gdb_out("No usable targets found.\n");
		return false;
	}

	return true;
}

bool cmd_morse(void)
{
	if(morse_msg)
		gdb_outf("%s\n", morse_msg);
	return true;
}

static bool cmd_assert_srst(target *t, int argc, const char **argv)
{
	(void)t;
	if (argc > 1) {
		if (!strcmp(argv[1], "attach"))
			assert_srst = ASSERT_UNTIL_ATTACH;
		else if (!strcmp(argv[1], "scan"))
			assert_srst = ASSERT_UNTIL_SCAN;
		else
			assert_srst = ASSERT_NEVER;
	}
	gdb_outf("Assert SRST %s\n",
			 (assert_srst == ASSERT_UNTIL_ATTACH) ? "until attach" :
			 (assert_srst == ASSERT_UNTIL_SCAN) ? "until scan" : "never");
	return true;
}

static bool cmd_halt_timeout(target *t, int argc, const char **argv)
{
	(void)t;
	if (argc > 1)
		cortexm_wait_timeout = atol(argv[1]);
	gdb_outf("Cortex-M timeout to wait for device haltes: %d\n",
				 cortexm_wait_timeout);
	return true;
}

static bool cmd_hard_srst(void)
{
	target_list_free();
	platform_srst_set_val(true);
	platform_srst_set_val(false);
	return true;
}

static bool cmd_gcore(target *t)
{
	if (t == NULL)
		return false;

	zynq_amp_core_dump(t);
	return true;
}

#ifdef PLATFORM_HAS_POWER_SWITCH
static bool cmd_target_power(target *t, int argc, const char **argv)
{
	(void)t;
	if (argc == 1)
		gdb_outf("Target Power: %s\n",
			 platform_target_get_power() ? "enabled" : "disabled");
	else
		platform_target_set_power(!strncmp(argv[1], "enable", strlen(argv[1])));
	return true;
}
#endif

#ifdef PLATFORM_HAS_TRACESWO
static bool cmd_traceswo(target *t, int argc, const char **argv)
{
#if defined(STM32L0) || defined(STM32F3) || defined(STM32F4)
	extern char serial_no[13];
#else
	extern char serial_no[9];
#endif
	uint32_t baudrate = 0;
	(void)t;

	if (argc > 1)
		baudrate = atoi(argv[1]);

	traceswo_init(baudrate);
	gdb_outf("%s:%02X:%02X\n", serial_no, 5, 0x85);
	return true;
}
#endif

#ifdef PLATFORM_HAS_DEBUG
static bool cmd_debug_bmp(target *t, int argc, const char **argv)
{
	(void)t;
	if (argc > 1) {
		debug_bmp = !strcmp(argv[1], "enable");
	}
	gdb_outf("Debug mode is %s\n",
		 debug_bmp ? "enabled" : "disabled");
	return true;
}
#endif
