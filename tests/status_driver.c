/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "everbar.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	char out[TEXT_CAP];
	if (argc == 14 && !strcmp(argv[1], "render")) {
		struct status s = {.cpu = atoi(argv[2]), .temp = atoi(argv[3]),
		    .battery = atoi(argv[4]), .volume = atoi(argv[5]), .charging = atoi(argv[6]),
		    .wifi_on = atoi(argv[7]), .wifi_connected = atoi(argv[8]),
		    .bt_on = atoi(argv[9]), .bt_connected = atoi(argv[10]), .muted = atoi(argv[11])};
		snprintf(s.ram, sizeof s.ram, "%s", argv[12]);
		assert(render_status(out, sizeof out, &s, argv[13]));
		puts(out);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "sensors")) {
		struct status s = {0};
		read_sensors(&s, argv[2], true);
		struct cpu_sample c = read_cpu(argv[2]);
		printf("%s\t%d\t%d\t%d\t%llu\t%llu\t%d\n", s.ram, s.temp, s.battery, s.charging,
		    (unsigned long long)c.total, (unsigned long long)c.idle, c.valid);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "clock")) {
		setenv("TZ", EVERBAR_TIMEZONE, 1); tzset();
		render_clock(out, sizeof out, (time_t)strtoll(argv[2], NULL, 10));
		puts(out); return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "quote")) {
		if (!quote_action(out, sizeof out, argv[2])) return 1;
		puts(out); return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "selftest")) {
		assert(round_even(0.5) == 0 && round_even(1.5) == 2 && round_even(2.5) == 2);
		assert(round_even(-1.5) == -2 && round_even(-2.5) == -2);
		struct cpu_sample a = {100, 50, true}, b = {200, 75, true};
		assert(cpu_percent(a, b) == 75);
		assert(cpu_percent(a, a) == -1 && cpu_percent(b, a) == -1);
		b.idle = 500; assert(cpu_percent(a, b) == -1);
		b.valid = false; assert(cpu_percent(a, b) == -1);
		struct status s = {.ram = "--"};
		assert(!render_status(out, 1, &s, "/x"));
		assert(!render_status(out, 0, &s, "/x"));
		assert(!quote_action(out, 1, "/x"));
		puts("ok"); return 0;
	}
	return 2;
}
