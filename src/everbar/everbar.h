/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef EVERBAR_H
#define EVERBAR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define TEXT_CAP 4096
#ifndef EVERBAR_TIMEZONE
#define EVERBAR_TIMEZONE "Europe/Oslo"
#endif
#define FG "D3C6AA"
#define GREEN "A7C080"
#define RED "E67E80"
#define YELLOW "DBBC7F"
#define BLUE "7FBBB3"
#define GREY "859289"

struct cpu_sample { uint64_t total, idle; bool valid; };
struct status {
	int cpu, temp, battery, volume;
	bool charging, wifi_on, wifi_connected, bt_on, bt_connected, muted;
	char ram[64];
};

uint64_t monotime_ms(void);
int round_even(double value);
struct cpu_sample read_cpu(const char *root);
int cpu_percent(struct cpu_sample previous, struct cpu_sample current);
void read_sensors(struct status *s, const char *root, bool slow);
bool render_status(char *out, size_t cap, const struct status *s, const char *action);
void render_clock(char *out, size_t cap, time_t now);
bool quote_action(char *out, size_t cap, const char *path);

struct services;
struct services *services_new(struct status *status);
int services_fd(struct services *services);
uint64_t services_deadline(struct services *services);
void services_step(struct services *services, uint64_t now);
void services_action(struct services *services, const char *action);
void services_free(struct services *services);
#endif
