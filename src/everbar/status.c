/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "everbar.h"
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t
monotime_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* Python's round(): nearest integer, ties to even (including negative values). */
int
round_even(double v)
{
	if (!isfinite(v) || v < INT_MIN || v > INT_MAX) return -1;
	double f = floor(v), d = v - f;
	return (int)(d < .5 ? f : d > .5 ? f + 1 : fmod(f, 2) == 0 ? f : f + 1);
}

static FILE *
open_file(const char *root, const char *path)
{
	char full[PATH_MAX];
	int n = snprintf(full, sizeof full, "%s%s", root, path);
	return n < 0 || (size_t)n >= sizeof full ? NULL : fopen(full, "re");
}

static bool
number(const char *root, const char *base, const char *name, double *out)
{
	char path[PATH_MAX];
	int n = snprintf(path, sizeof path, "%s/%s", base, name);
	if (n < 0 || (size_t)n >= sizeof path) return false;
	FILE *f = open_file(root, path);
	if (!f) return false;
	char text[128], *end;
	bool ok = fgets(text, sizeof text, f) != NULL;
	fclose(f);
	if (!ok) return false;
	errno = 0;
	long long value = strtoll(text, &end, 10);
	if (end == text || errno == ERANGE) return false;
	while (isspace((unsigned char)*end)) end++;
	if (*end) return false;
	*out = (double)value;
	return ok;
}

struct cpu_sample
read_cpu(const char *root)
{
	struct cpu_sample s = {0};
	FILE *f = open_file(root, "/proc/stat");
	if (!f) return s;
	char line[1024];
	unsigned long long v[8] = {0};
	if (fgets(line, sizeof line, f) && sscanf(line,
	    "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
	    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) >= 5) {
		for (size_t i = 0; i < 8; i++) s.total += v[i];
		s.idle = v[3] + v[4];
		s.valid = true;
	}
	fclose(f);
	return s;
}

int
cpu_percent(struct cpu_sample a, struct cpu_sample b)
{
	if (!a.valid || !b.valid || b.total <= a.total || b.idle < a.idle) return -1;
	uint64_t total = b.total - a.total, idle = b.idle - a.idle;
	return idle > total ? -1 : round_even(100.0 * (double)(total - idle) / (double)total);
}

static void
ram(struct status *s, const char *root)
{
	strcpy(s->ram, "--");
	FILE *f = open_file(root, "/proc/meminfo");
	if (!f) return;
	char line[256];
	unsigned long long total = 0, available = 0, value;
	bool have_available = false;
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "MemTotal: %llu", &value) == 1) total = value;
		if (sscanf(line, "MemAvailable: %llu", &value) == 1) {
			available = value;
			have_available = true;
		}
	}
	fclose(f);
	if (!total || !have_available || available > total) return;
	double used = (double)(total - available);
	if (total >= 1024 * 1024)
		snprintf(s->ram, sizeof s->ram, "%.1f GiB", used / (1024 * 1024));
	else
		snprintf(s->ram, sizeof s->ram, "%.0f MiB", used / 1024);
}

static void
temperature(struct status *s, const char *root)
{
	s->temp = -1;
	char full[PATH_MAX];
	if (snprintf(full, sizeof full, "%s/sys/class/thermal", root) >= (int)sizeof full) return;
	DIR *dir = opendir(full);
	if (!dir) return;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "thermal_zone", 12)) continue;
		char base[512], path[528], type[64];
		snprintf(base, sizeof base, "/sys/class/thermal/%s", de->d_name);
		snprintf(path, sizeof path, "%s/type", base);
		FILE *f = open_file(root, path);
		if (!f) continue;
		bool match = fscanf(f, "%63s", type) == 1 && !strcmp(type, "x86_pkg_temp");
		fclose(f);
		if (match) {
			double value;
			if (number(root, base, "temp", &value)) s->temp = round_even(value / 1000);
			break;
		}
	}
	closedir(dir);
}

static void
battery(struct status *s, const char *root)
{
	s->battery = -1;
	s->charging = false;
	char full[PATH_MAX];
	if (snprintf(full, sizeof full, "%s/sys/class/power_supply", root) >= (int)sizeof full) return;
	DIR *dir = opendir(full);
	if (!dir) return;
	struct dirent *de;
	double sum_now = 0, sum_full = 0, sum_percent = 0;
	unsigned count = 0, pairs = 0, units = 0;
	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "BAT", 3)) continue;
		char base[512], path[528], state[32];
		snprintf(base, sizeof base, "/sys/class/power_supply/%s", de->d_name);
		double present, now, full_value, voltage, capacity;
		if (number(root, base, "present", &present) && present == 0) continue;
		snprintf(path, sizeof path, "%s/status", base);
		FILE *f = open_file(root, path);
		if (f) {
			if (fscanf(f, "%31s", state) == 1 && !strcmp(state, "Charging")) s->charging = true;
			fclose(f);
		}
		unsigned unit = 1;
		bool pair = number(root, base, "energy_now", &now) && number(root, base, "energy_full", &full_value);
		if (!pair) {
			unit = 2;
			pair = number(root, base, "charge_now", &now) && number(root, base, "charge_full", &full_value);
			if (pair && number(root, base, "voltage_min_design", &voltage) && voltage > 0) {
				now *= voltage / 1000000;
				full_value *= voltage / 1000000;
				unit = 1;
			}
		}
		if (pair && now >= 0 && full_value > 0) {
			sum_now += now;
			sum_full += full_value;
			sum_percent += 100 * now / full_value;
			units |= unit;
			pairs++;
			count++;
		} else if (number(root, base, "capacity", &capacity)) {
			sum_percent += capacity;
			count++;
		}
	}
	closedir(dir);
	if (!count) return;
	double percent = pairs == count && units != 3 ? 100 * sum_now / sum_full : sum_percent / count;
	s->battery = round_even(fmax(0, fmin(100, percent)));
}

void
read_sensors(struct status *s, const char *root, bool slow)
{
	ram(s, root);
	temperature(s, root);
	if (slow) battery(s, root);
}

/* dwlb parses ')' and '^' before the shell sees the command. Refuse paths
 * it cannot represent instead of emitting broken/injectable markup. */
bool
quote_action(char *out, size_t cap, const char *path)
{
	size_t n = 0;
	if (!cap) return false;
	bool plain = *path != 0;
	for (const unsigned char *p = (const unsigned char *)path; *p; p++)
		if (!isalnum(*p) && !strchr("/_-.", *p)) plain = false;
	if (plain) {
		if (strlen(path) >= cap) return false;
		strcpy(out, path);
		return true;
	}
	if (cap < 3) return false;
	out[n++] = '\'';
	for (; *path; path++) {
		if (*path == ')' || *path == '^' || (unsigned char)*path < 32) return false;
		const char *part = *path == '\'' ? "'\\''" : NULL;
		size_t len = part ? 4 : 1;
		if (n + len + 2 > cap) return false;
		if (part) memcpy(out + n, part, len); else out[n] = *path;
		n += len;
	}
	out[n++] = '\'';
	out[n] = 0;
	return true;
}

struct buffer { char *p; size_t left; bool ok; };
static void
append(struct buffer *b, const char *fmt, ...)
{
	if (!b->ok) return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b->p, b->left, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= b->left) { b->ok = false; return; }
	b->p += n;
	b->left -= (size_t)n;
}

static void
item(struct buffer *b, const char *text, const char *color, const char *action, const char *name)
{
	if (name) append(b, "^lm(%s %s)", action, name);
	append(b, "^fg(%s)%s^fg(" FG ")", color, text);
	if (name) append(b, "^lm()");
}

bool
render_status(char *out, size_t cap, const struct status *s, const char *action)
{
	struct buffer b = {out, cap, true};
	char text[128];
	append(&b, "   ");
	if (s->temp == -1) strcpy(text, "\uf2c9 --"); else snprintf(text, sizeof text, "\uf2c9 %d°C", s->temp);
	item(&b, text, s->temp >= 85 ? RED : YELLOW, NULL, NULL);
	append(&b, "  │  ");
	if (s->cpu < 0) strcpy(text, "\uf2db --"); else snprintf(text, sizeof text, "\uf2db %d%%", s->cpu);
	item(&b, text, GREEN, NULL, NULL);
	append(&b, "  │  ");
	snprintf(text, sizeof text, "\uefc5 %s", s->ram);
	item(&b, text, BLUE, NULL, NULL);
	append(&b, "  │  ");
	item(&b, s->wifi_on ? "\U000f05a9" : "\U000f05aa", s->wifi_connected ? GREEN : GREY, action, "wifi");
	append(&b, "  │  ");
	item(&b, s->bt_on ? "\U000f00af" : "\U000f00b2", s->bt_connected ? BLUE : GREY, action, "bluetooth");
	append(&b, "  │  ");
	if (s->volume < 0) strcpy(text, "\uf026 --");
	else snprintf(text, sizeof text, "%s %d%%", s->muted || s->volume == 0 ? "\U000f075f" : s->volume < 50 ? "\U000f057f" : "\U000f057e", s->volume);
	item(&b, text, s->volume < 0 || s->muted ? GREY : FG, action, "mute");
	append(&b, "  │  ");
	const char *color = GREY;
	if (s->battery < 0) strcpy(text, "\uf244 --");
	else {
		const char *icon = s->charging ? "\U000f0084" : s->battery >= 90 ? "\U000f0079" : s->battery >= 65 ? "\U000f0080" : s->battery >= 40 ? "\U000f007d" : s->battery >= 15 ? "\U000f007b" : "\U000f007a";
		snprintf(text, sizeof text, "%s %d%%", icon, s->battery);
		color = s->charging ? GREEN : s->battery < 15 ? RED : YELLOW;
	}
	item(&b, text, color, NULL, NULL);
	append(&b, "  │  ");
	item(&b, "\uf011", RED, action, "power");
	append(&b, "  ");
	return b.ok;
}

void
render_clock(char *out, size_t cap, time_t now)
{
	static const char *months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
	static const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
	struct tm tm;
	if (!localtime_r(&now, &tm)) { if (cap) *out = 0; return; }
	snprintf(out, cap, "%s %d %s %02d:%02d", months[tm.tm_mon], tm.tm_mday, days[tm.tm_wday], tm.tm_hour, tm.tm_min);
}
