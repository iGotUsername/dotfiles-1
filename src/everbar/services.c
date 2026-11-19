/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "everbar.h"
#include <dbus/dbus.h>
#include <errno.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/param/route.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define BZ "org.bluez"
#define PROPS "org.freedesktop.DBus.Properties"
#define OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"

enum request_kind { NM_ROOT, NM_ACTIVE, BZ_OBJECTS, SET_PROPERTY };
struct request {
	struct request *next;
	struct services *s;
	DBusPendingCall *pending;
	enum request_kind kind;
};
struct level { bool known, muted; int volume; };
struct route {
	struct route *next;
	int index, profile;
	struct level level;
	int generation;
};
struct device {
	struct device *next;
	struct services *s;
	uint32_t id;
	struct pw_device *proxy;
	struct spa_hook listener;
	struct route *routes;
	int generation;
};
struct sink {
	struct sink *next;
	struct services *s;
	uint32_t id;
	struct pw_node *node;
	struct spa_hook listener;
	char *name;
	uint32_t device_id;
	int profile;
	struct level level;
};
struct services {
	struct status *status;
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct spa_hook core_listener, registry_listener, metadata_listener;
	struct pw_metadata *metadata;
	uint32_t metadata_id;
	struct sink *sinks;
	struct device *devices;
	char default_sink[1024];
	bool pw_failed;
	bool bus_warned;
	uint64_t pw_retry, bus_retry, nm_due, bz_due;
	DBusConnection *bus;
	struct bus_io *bus_ios;
	struct request *requests;
	unsigned nm_pending, bz_pending;
	bool nm_dirty, bz_dirty;
	bool nm_on, nm_connected;
	char adapter[1024];
};

/* libdbus watches and timers share PipeWire's epoll loop. No threads, helper
 * processes, fixed-frequency service polling, or second event loop. */
struct bus_source { struct services *s; struct spa_source *source; void *object; };
struct bus_watch { struct bus_watch *next; DBusWatch *watch; struct bus_io *io; };
struct bus_io {
	struct bus_io *next;
	struct services *s;
	int fd;
	struct spa_source *source;
	struct bus_watch *watches;
};

static void
source_free(void *data)
{
	struct bus_source *b = data;
	if (b->source) pw_loop_destroy_source(b->s->loop, b->source);
	free(b);
}

static uint32_t
watch_mask(struct bus_io *io)
{
	uint32_t mask = 0;
	for (struct bus_watch *w = io->watches; w; w = w->next) {
		if (!dbus_watch_get_enabled(w->watch)) continue;
		unsigned f = dbus_watch_get_flags(w->watch);
		mask |= (f & DBUS_WATCH_READABLE ? SPA_IO_IN : 0) |
		    (f & DBUS_WATCH_WRITABLE ? SPA_IO_OUT : 0) | SPA_IO_ERR | SPA_IO_HUP;
	}
	return mask;
}

static void
watch_ready(void *data, int fd, uint32_t mask)
{
	(void)fd;
	struct bus_io *io = data;
	unsigned flags = (mask & SPA_IO_IN ? DBUS_WATCH_READABLE : 0) |
	    (mask & SPA_IO_OUT ? DBUS_WATCH_WRITABLE : 0) |
	    (mask & SPA_IO_ERR ? DBUS_WATCH_ERROR : 0) |
	    (mask & SPA_IO_HUP ? DBUS_WATCH_HANGUP : 0);
	for (struct bus_watch *w = io->watches; w; w = w->next) {
		if (!dbus_watch_get_enabled(w->watch)) continue;
		unsigned ready = flags & (dbus_watch_get_flags(w->watch) | DBUS_WATCH_ERROR | DBUS_WATCH_HANGUP);
		/* Handling a watch may remove/free any of the watches, including io.
		 * Level-triggered readiness will deliver any remaining work next turn. */
		if (ready) { dbus_watch_handle(w->watch, ready); return; }
	}
}

static dbus_bool_t
watch_add(DBusWatch *w, void *data)
{
	struct services *s = data;
	struct bus_watch *entry = calloc(1, sizeof *entry);
	if (!entry) return FALSE;
	int fd = dbus_watch_get_unix_fd(w);
	struct bus_io *io;
	for (io = s->bus_ios; io; io = io->next) if (io->fd == fd) break;
	if (!io) {
		io = calloc(1, sizeof *io);
		if (!io) { free(entry); return FALSE; }
		io->s = s; io->fd = fd;
		io->source = pw_loop_add_io(s->loop, fd, 0, false, watch_ready, io);
		if (!io->source) { free(io); free(entry); return FALSE; }
		io->next = s->bus_ios; s->bus_ios = io;
	}
	entry->watch = w; entry->io = io;
	entry->next = io->watches; io->watches = entry;
	dbus_watch_set_data(w, entry, NULL);
	pw_loop_update_io(s->loop, io->source, watch_mask(io));
	return TRUE;
}

static void
watch_remove(DBusWatch *w, void *data)
{
	struct services *s = data;
	struct bus_watch *entry = dbus_watch_get_data(w);
	if (!entry) return;
	struct bus_io *io = entry->io;
	struct bus_watch **it = &io->watches;
	while (*it && *it != entry) it = &(*it)->next;
	if (*it) *it = entry->next;
	free(entry);
	dbus_watch_set_data(w, NULL, NULL);
	if (io->watches) pw_loop_update_io(s->loop, io->source, watch_mask(io));
	else {
		struct bus_io **next = &s->bus_ios;
		while (*next && *next != io) next = &(*next)->next;
		if (*next) *next = io->next;
		pw_loop_destroy_source(s->loop, io->source);
		free(io);
	}
}

static void
watch_toggle(DBusWatch *w, void *data)
{
	(void)data;
	struct bus_watch *entry = dbus_watch_get_data(w);
	if (entry) pw_loop_update_io(entry->io->s->loop, entry->io->source, watch_mask(entry->io));
}

static void
timeout_ready(void *data, uint64_t expirations)
{
	(void)expirations;
	struct bus_source *b = data;
	dbus_timeout_handle(b->object);
}

static void
timeout_toggle(DBusTimeout *t, void *data)
{
	(void)data;
	struct bus_source *b = dbus_timeout_get_data(t);
	if (!b) return;
	int ms = dbus_timeout_get_interval(t);
	if (ms < 1) ms = 1;
	struct timespec interval = {ms / 1000, (ms % 1000) * 1000000L};
	struct timespec zero = {0};
	pw_loop_update_timer(b->s->loop, b->source,
	    dbus_timeout_get_enabled(t) ? &interval : &zero, &interval, false);
}

static dbus_bool_t
timeout_add(DBusTimeout *t, void *data)
{
	struct bus_source *b = calloc(1, sizeof *b);
	if (!b) return FALSE;
	b->s = data;
	b->object = t;
	b->source = pw_loop_add_timer(b->s->loop, timeout_ready, b);
	if (!b->source) { free(b); return FALSE; }
	dbus_timeout_set_data(t, b, source_free);
	timeout_toggle(t, data);
	return TRUE;
}

static void
timeout_remove(DBusTimeout *t, void *data)
{
	(void)data;
	dbus_timeout_set_data(t, NULL, NULL);
}

static bool
lookup(DBusMessageIter array, const char *key, DBusMessageIter *value)
{
	if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_ARRAY) return false;
	DBusMessageIter it;
	dbus_message_iter_recurse(&array, &it);
	while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry;
		const char *name;
		dbus_message_iter_recurse(&it, &entry);
		int type = dbus_message_iter_get_arg_type(&entry);
		if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH) return false;
		dbus_message_iter_get_basic(&entry, &name);
		dbus_message_iter_next(&entry);
		if (!strcmp(key, name)) {
			if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT)
				dbus_message_iter_recurse(&entry, value);
			else *value = entry;
			return true;
		}
		dbus_message_iter_next(&it);
	}
	return false;
}

static bool
boolean(DBusMessageIter props, const char *key)
{
	DBusMessageIter v;
	dbus_bool_t b = FALSE;
	if (lookup(props, key, &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN)
		dbus_message_iter_get_basic(&v, &b);
	return b;
}

static bool
string_is(DBusMessageIter props, const char *key, const char *expected)
{
	DBusMessageIter v;
	const char *str;
	if (!lookup(props, key, &v) || dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_STRING) return false;
	dbus_message_iter_get_basic(&v, &str);
	return !strcmp(str, expected);
}

static void request_done(DBusPendingCall *pending, void *data);

static void
request_free(void *data)
{
	struct request *r = data, **it = &r->s->requests;
	while (*it && *it != r) it = &(*it)->next;
	if (*it) *it = r->next;
	free(r);
}

static bool
send_request(struct services *s, DBusMessage *message, enum request_kind kind)
{
	if (!message) return false;
	struct request *r = calloc(1, sizeof *r);
	if (!r) { dbus_message_unref(message); return false; }
	r->s = s;
	r->kind = kind;
	bool ok = dbus_connection_send_with_reply(s->bus, message, &r->pending, 2000) && r->pending;
	dbus_message_unref(message);
	if (!ok) { free(r); return false; }
	r->next = s->requests;
	s->requests = r;
	if (!dbus_pending_call_set_notify(r->pending, request_done, r, request_free)) {
		dbus_pending_call_cancel(r->pending);
		dbus_pending_call_unref(r->pending);
		request_free(r);
		return false;
	}
	if (kind == NM_ROOT || kind == NM_ACTIVE) s->nm_pending++;
	if (kind == BZ_OBJECTS) s->bz_pending++;
	return true;
}

static void
get_all(struct services *s, const char *path, const char *interface, enum request_kind kind)
{
	DBusMessage *m = dbus_message_new_method_call(NM, path, PROPS, "GetAll");
	if (m && !dbus_message_append_args(m, DBUS_TYPE_STRING, &interface, DBUS_TYPE_INVALID)) {
		dbus_message_unref(m);
		return;
	}
	send_request(s, m, kind);
}

static void
bluez_snapshot(struct services *s, DBusMessageIter objects)
{
	s->status->bt_on = s->status->bt_connected = false;
	s->adapter[0] = 0;
	if (dbus_message_iter_get_arg_type(&objects) != DBUS_TYPE_ARRAY) return;
	DBusMessageIter it;
	dbus_message_iter_recurse(&objects, &it);
	/* bluetoothctl's default controller is normally hci0. Select a stable
 * controller, and never mix connection state from another controller. */
	for (; dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&it)) {
		DBusMessageIter entry, props;
		const char *path;
		dbus_message_iter_recurse(&it, &entry);
		dbus_message_iter_get_basic(&entry, &path);
		dbus_message_iter_next(&entry);
		if (lookup(entry, "org.bluez.Adapter1", &props) &&
		    (!s->adapter[0] || strcmp(path, s->adapter) < 0) && strlen(path) < sizeof s->adapter) {
			strcpy(s->adapter, path);
			s->status->bt_on = boolean(props, "Powered");
		}
	}
	if (!s->adapter[0] || !s->status->bt_on) return;
	dbus_message_iter_recurse(&objects, &it);
	for (; dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&it)) {
		DBusMessageIter entry, props;
		const char *path;
		dbus_message_iter_recurse(&it, &entry);
		dbus_message_iter_get_basic(&entry, &path);
		dbus_message_iter_next(&entry);
		size_t n = strlen(s->adapter);
		if (!strncmp(path, s->adapter, n) && path[n] == '/' &&
		    lookup(entry, "org.bluez.Device1", &props) && boolean(props, "Connected"))
			s->status->bt_connected = true;
	}
}

static void
request_done(DBusPendingCall *pending, void *data)
{
	struct request *r = data;
	struct services *s = r->s;
	DBusMessage *m = dbus_pending_call_steal_reply(pending);
	DBusMessageIter props;
	if (r->kind == NM_ROOT || r->kind == NM_ACTIVE) s->nm_pending--;
	if (r->kind == BZ_OBJECTS) s->bz_pending--;
	bool ok = m && dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_METHOD_RETURN && dbus_message_iter_init(m, &props);
	if (r->kind == NM_ROOT) {
		s->nm_on = s->nm_connected = false;
		if (ok) {
			s->nm_on = boolean(props, "WirelessEnabled");
			DBusMessageIter paths, it;
			if (s->nm_on && lookup(props, "ActiveConnections", &paths) && dbus_message_iter_get_arg_type(&paths) == DBUS_TYPE_ARRAY) {
				dbus_message_iter_recurse(&paths, &it);
				while (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_OBJECT_PATH) {
					const char *path;
					dbus_message_iter_get_basic(&it, &path);
					get_all(s, path, NM ".Connection.Active", NM_ACTIVE);
					dbus_message_iter_next(&it);
				}
			}
		}
	} else if (r->kind == NM_ACTIVE && ok) {
		DBusMessageIter v;
		dbus_uint32_t state = 0;
		if (lookup(props, "State", &v) && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_UINT32)
			dbus_message_iter_get_basic(&v, &state);
		if (state == 2 && string_is(props, "Type", "802-11-wireless") && s->nm_on)
			s->nm_connected = true;
	} else if (r->kind == BZ_OBJECTS) {
		s->status->bt_on = s->status->bt_connected = false;
		s->adapter[0] = 0;
		if (ok) bluez_snapshot(s, props);
	} else if (r->kind == SET_PROPERTY && m && dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_ERROR) {
		fprintf(stderr, "everbar: action rejected: %s\n", dbus_message_get_error_name(m));
	}
	if ((r->kind == NM_ROOT || r->kind == NM_ACTIVE) && !s->nm_pending) {
		s->status->wifi_on = s->nm_on;
		s->status->wifi_connected = s->nm_connected;
	}
	if (m) dbus_message_unref(m);
	dbus_pending_call_unref(pending); /* frees r */
}

static void
dirty(struct services *s, bool network)
{
	bool *flag = network ? &s->nm_dirty : &s->bz_dirty;
	uint64_t *due = network ? &s->nm_due : &s->bz_due;
	if (!*flag) *due = monotime_ms() + 40; /* coalesce a burst, without starvation */
	*flag = true;
}

static DBusHandlerResult
bus_filter(DBusConnection *connection, DBusMessage *m, void *data)
{
	(void)connection;
	struct services *s = data;
	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *path = dbus_message_get_path(m);
	if (path && !strncmp(path, NM_PATH, strlen(NM_PATH))) dirty(s, true);
	else if (path && !strncmp(path, "/org/bluez", 10)) dirty(s, false);
	else if (dbus_message_is_signal(m, OBJECT_MANAGER, "InterfacesAdded") ||
	         dbus_message_is_signal(m, OBJECT_MANAGER, "InterfacesRemoved")) dirty(s, false);
	else if (dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
		const char *name;
		DBusMessageIter it;
		if (dbus_message_iter_init(m, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING) {
			dbus_message_iter_get_basic(&it, &name);
			if (!strcmp(name, NM)) dirty(s, true);
			if (!strcmp(name, BZ)) dirty(s, false);
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void
bus_disconnect(struct services *s)
{
	while (s->requests) {
		DBusPendingCall *p = s->requests->pending;
		dbus_pending_call_cancel(p);
		dbus_pending_call_unref(p);
	}
	s->nm_pending = s->bz_pending = 0;
	if (s->bus) {
		dbus_connection_close(s->bus);
		dbus_connection_unref(s->bus);
		s->bus = NULL;
	}
	s->status->wifi_on = s->status->wifi_connected = false;
	s->status->bt_on = s->status->bt_connected = false;
	s->adapter[0] = 0;
}

static void
bus_connect(struct services *s)
{
	s->bus_retry = monotime_ms() + 5000;
	DBusError error;
	dbus_error_init(&error);
	s->bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (!s->bus) {
		if (!s->bus_warned) fprintf(stderr, "everbar: system D-Bus unavailable: %s (retrying)\n", error.message ? error.message : "unknown error");
		s->bus_warned = true;
		dbus_error_free(&error); return;
	}
	s->bus_warned = false;
	dbus_connection_set_exit_on_disconnect(s->bus, FALSE);
	if (!dbus_connection_set_watch_functions(s->bus, watch_add, watch_remove, watch_toggle, s, NULL) ||
	    !dbus_connection_set_timeout_functions(s->bus, timeout_add, timeout_remove, timeout_toggle, s, NULL) ||
	    !dbus_connection_add_filter(s->bus, bus_filter, s, NULL)) {
		bus_disconnect(s);
		return;
	}
	const char *matches[] = {
		"type='signal',sender='" NM "',interface='" PROPS "'",
		"type='signal',sender='" BZ "',interface='" PROPS "'",
		"type='signal',sender='" BZ "',interface='org.freedesktop.DBus.ObjectManager'",
		"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" NM "'",
		"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='" BZ "'"
	};
	/* NULL error makes AddMatch nonblocking; the bus queues each request. */
	for (size_t i = 0; i < sizeof matches / sizeof *matches; i++) dbus_bus_add_match(s->bus, matches[i], NULL);
	s->nm_dirty = s->bz_dirty = true;
	s->nm_due = s->bz_due = 0;
}

static struct sink *
default_sink(struct services *s)
{
	for (struct sink *n = s->sinks; n; n = n->next)
		if (!strcmp(n->name, s->default_sink)) return n;
	/* No guessed fallback: an unknown default is safer than muting the wrong
 * output. WirePlumber publishes the effective default.audio.sink metadata. */
	return NULL;
}

static void
parse_level(struct level *level, const struct spa_pod *param)
{
	if (!param || !spa_pod_is_object(param)) return;
	const struct spa_pod_prop *p;
	float scalar = -1, first = -1;
	SPA_POD_OBJECT_FOREACH((const struct spa_pod_object *)param, p) {
		if (p->key == SPA_PROP_mute) spa_pod_get_bool(&p->value, &level->muted);
		else if (p->key == SPA_PROP_volume) spa_pod_get_float(&p->value, &scalar);
		else if (p->key == SPA_PROP_channelVolumes && spa_pod_is_array(&p->value) &&
		         SPA_POD_ARRAY_VALUE_TYPE(&p->value) == SPA_TYPE_Float &&
		         SPA_POD_ARRAY_VALUE_SIZE(&p->value) == sizeof(float)) {
			uint32_t count = SPA_POD_ARRAY_N_VALUES(&p->value);
			const float *values = SPA_POD_ARRAY_VALUES(&p->value);
			/* WirePlumber 0.5's mixer-api reports the first channel, not the
			 * maximum. Preserve that convention for an unbalanced output. */
			if (count && isfinite(values[0]) && values[0] >= 0) first = values[0];
		}
	}
	if (first < 0) first = scalar;
	if (isfinite(first) && first >= 0) {
		/* wpctl uses cubic volume and prints two decimals before Python rounds
		 * to percent. Reproduce that visible precision, including ties. */
		level->volume = round_even(cbrt((double)first) * 100);
		level->known = level->volume >= 0;
	}
}

static struct route *
sink_route(struct services *s, struct sink *n, struct device **device)
{
	if (!n) return NULL;
	for (struct device *d = s->devices; d; d = d->next) {
		if (d->id != n->device_id) continue;
		for (struct route *r = d->routes; r; r = r->next) {
			if (r->profile == n->profile && r->generation == d->generation && r->level.known) {
				if (device) *device = d;
				return r;
			}
		}
	}
	return NULL;
}

static void
audio_status(struct services *s)
{
	struct sink *n = default_sink(s);
	struct route *r = sink_route(s, n, NULL);
	struct level *level = r ? &r->level : n ? &n->level : NULL;
	s->status->volume = level && level->known ? level->volume : -1;
	s->status->muted = level && level->muted;
}

static void
node_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param)
{
	(void)seq; (void)index; (void)next;
	struct sink *n = data;
	if (id != SPA_PARAM_Props) return;
	parse_level(&n->level, param);
	audio_status(n->s);
}

static const struct pw_node_events node_events = { PW_VERSION_NODE_EVENTS, .param = node_param };

static void
device_info(void *data, const struct pw_device_info *info)
{
	struct device *d = data;
	if (!(info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS)) return;
	/* Re-enumerate only when the server announces changed route parameters.
	 * Generation tags prevent an old profile/port from remaining effective. */
	d->generation = -1;
	for (uint32_t i = 0; i < info->n_params; i++) {
		if (info->params[i].id == SPA_PARAM_Route && (info->params[i].flags & SPA_PARAM_INFO_READ))
			/* Native protocol replaces the supplied seq: use the returned
			 * asynchronous sequence, not our argument, to match replies. */
			d->generation = pw_device_enum_params(d->proxy, 0, SPA_PARAM_Route, 0, UINT32_MAX, NULL);
	}
	audio_status(d->s);
}

static void
device_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param)
{
	(void)index; (void)next;
	struct device *d = data;
	if (id != SPA_PARAM_Route || !param || seq != d->generation || seq < 0) return;
	int route_index, profile;
	const struct spa_pod *props;
	if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
	    SPA_PARAM_ROUTE_index, SPA_POD_Int(&route_index),
	    SPA_PARAM_ROUTE_device, SPA_POD_Int(&profile),
	    SPA_PARAM_ROUTE_props, SPA_POD_Pod(&props)) < 0) return;
	struct route *r;
	for (r = d->routes; r; r = r->next) if (r->profile == profile) break;
	if (!r) {
		r = calloc(1, sizeof *r);
		if (!r) return;
		r->next = d->routes; d->routes = r;
	}
	r->index = route_index;
	r->profile = profile;
	/* Match the sequence returned by the native protocol enumeration. */
	r->generation = seq;
	parse_level(&r->level, props);
	audio_status(d->s);
}

static const struct pw_device_events device_events = { PW_VERSION_DEVICE_EVENTS, .info = device_info, .param = device_param };

static int
metadata_property(void *data, uint32_t subject, const char *key, const char *type, const char *value)
{
	(void)type;
	struct services *s = data;
	if (subject != PW_ID_CORE || (key && strcmp(key, "default.audio.sink"))) return 0;
	s->default_sink[0] = 0;
	if (key && value) {
		struct spa_json it[2];
		char name[128];
		spa_json_init(&it[0], value, strlen(value));
		if (spa_json_enter_object(&it[0], &it[1]) > 0) {
			while (spa_json_get_string(&it[1], name, sizeof name) > 0) {
				if (!strcmp(name, "name")) {
					if (spa_json_get_string(&it[1], s->default_sink, sizeof s->default_sink) <= 0) s->default_sink[0] = 0;
					break;
				}
				const char *ignored;
				if (spa_json_next(&it[1], &ignored) <= 0) break;
			}
		}
	}
	audio_status(s);
	return 0;
}

static const struct pw_metadata_events metadata_events = { PW_VERSION_METADATA_EVENTS, .property = metadata_property };

static void
registry_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props)
{
	(void)permissions;
	struct services *s = data;
	if (!props) return;
	const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	const char *media = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (!strcmp(type, PW_TYPE_INTERFACE_Node) && name && media && !strcmp(media, "Audio/Sink")) {
		struct sink *n = calloc(1, sizeof *n);
		if (!n) return;
		n->s = s;
		n->id = id;
		const char *device_id = spa_dict_lookup(props, PW_KEY_DEVICE_ID);
		const char *profile = spa_dict_lookup(props, "card.profile.device");
		n->device_id = device_id ? (uint32_t)strtoul(device_id, NULL, 10) : SPA_ID_INVALID;
		n->profile = profile ? atoi(profile) : -1;
		n->name = strdup(name);
		n->node = pw_registry_bind(s->registry, id, type, version < PW_VERSION_NODE ? version : PW_VERSION_NODE, 0);
		if (!n->name || !n->node) {
			if (n->node) pw_proxy_destroy((struct pw_proxy *)n->node);
			free(n->name); free(n); return;
		}
		n->next = s->sinks;
		s->sinks = n;
		pw_node_add_listener(n->node, &n->listener, &node_events, n);
		uint32_t param = SPA_PARAM_Props;
		pw_node_subscribe_params(n->node, &param, 1);
	} else if (!strcmp(type, PW_TYPE_INTERFACE_Device) && media && !strcmp(media, "Audio/Device")) {
		struct device *d = calloc(1, sizeof *d);
		if (!d) return;
		d->s = s; d->id = id;
		d->proxy = pw_registry_bind(s->registry, id, type, version < PW_VERSION_DEVICE ? version : PW_VERSION_DEVICE, 0);
		if (!d->proxy) { free(d); return; }
		d->next = s->devices; s->devices = d;
		pw_device_add_listener(d->proxy, &d->listener, &device_events, d);
	} else if (!strcmp(type, PW_TYPE_INTERFACE_Metadata) && !s->metadata) {
		const char *metadata_name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
		if (!metadata_name || strcmp(metadata_name, "default")) return;
		s->metadata = pw_registry_bind(s->registry, id, type, version < PW_VERSION_METADATA ? version : PW_VERSION_METADATA, 0);
		if (s->metadata) {
			s->metadata_id = id;
			pw_metadata_add_listener(s->metadata, &s->metadata_listener, &metadata_events, s);
		}
	}
}

static void
sink_free(struct sink *n)
{
	spa_hook_remove(&n->listener);
	pw_proxy_destroy((struct pw_proxy *)n->node);
	free(n->name);
	free(n);
}

static void
device_free(struct device *d)
{
	spa_hook_remove(&d->listener);
	pw_proxy_destroy((struct pw_proxy *)d->proxy);
	while (d->routes) { struct route *r = d->routes; d->routes = r->next; free(r); }
	free(d);
}

static void
registry_remove(void *data, uint32_t id)
{
	struct services *s = data;
	struct sink **it = &s->sinks;
	while (*it) {
		if ((*it)->id == id) {
			struct sink *n = *it;
			*it = n->next;
			sink_free(n);
			break;
		}
		it = &(*it)->next;
	}
	struct device **di = &s->devices;
	while (*di) {
		if ((*di)->id == id) {
			struct device *d = *di; *di = d->next; device_free(d); break;
		}
		di = &(*di)->next;
	}
	if (s->metadata && s->metadata_id == id) {
		spa_hook_remove(&s->metadata_listener);
		pw_proxy_destroy((struct pw_proxy *)s->metadata);
		s->metadata = NULL;
		s->default_sink[0] = 0;
	}
	audio_status(s);
}

static const struct pw_registry_events registry_events = { PW_VERSION_REGISTRY_EVENTS, .global = registry_global, .global_remove = registry_remove };

static void
core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	(void)seq; (void)message;
	struct services *s = data;
	if (id == PW_ID_CORE && (res == -EPIPE || res == -ECONNRESET || res == -ENOTCONN)) s->pw_failed = true;
}

static const struct pw_core_events core_events = { PW_VERSION_CORE_EVENTS, .error = core_error };

static void
audio_disconnect(struct services *s)
{
	while (s->devices) {
		struct device *d = s->devices; s->devices = d->next; device_free(d);
	}
	while (s->sinks) {
		struct sink *n = s->sinks;
		s->sinks = n->next;
		sink_free(n);
	}
	if (s->metadata) {
		spa_hook_remove(&s->metadata_listener);
		pw_proxy_destroy((struct pw_proxy *)s->metadata);
		s->metadata = NULL;
	}
	if (s->registry) {
		spa_hook_remove(&s->registry_listener);
		pw_proxy_destroy((struct pw_proxy *)s->registry);
		s->registry = NULL;
	}
	if (s->core) {
		spa_hook_remove(&s->core_listener);
		pw_core_disconnect(s->core);
		s->core = NULL;
	}
	s->default_sink[0] = 0;
	s->status->volume = -1;
	s->status->muted = false;
	s->pw_failed = false;
}

static void
audio_connect(struct services *s)
{
	s->pw_retry = monotime_ms() + 5000;
	s->core = pw_context_connect(s->context, NULL, 0);
	if (!s->core) return;
	pw_core_add_listener(s->core, &s->core_listener, &core_events, s);
	s->registry = pw_core_get_registry(s->core, PW_VERSION_REGISTRY, 0);
	if (!s->registry) { audio_disconnect(s); return; }
	pw_registry_add_listener(s->registry, &s->registry_listener, &registry_events, s);
}

struct services *
services_new(struct status *status)
{
	pw_init(NULL, NULL);
	struct services *s = calloc(1, sizeof *s);
	if (!s) return NULL;
	s->status = status;
	s->loop = pw_loop_new(NULL);
	if (!s->loop) { free(s); return NULL; }
	s->context = pw_context_new(s->loop, pw_properties_new(PW_KEY_APP_NAME, "everbar", NULL), 0);
	if (!s->context) { pw_loop_destroy(s->loop); free(s); return NULL; }
	return s;
}

int
services_fd(struct services *s)
{
	return pw_loop_get_fd(s->loop);
}

uint64_t
services_deadline(struct services *s)
{
	uint64_t due = UINT64_MAX;
	if (s->bus && dbus_connection_get_dispatch_status(s->bus) == DBUS_DISPATCH_DATA_REMAINS) return 0;
	if (!s->bus && s->bus_retry < due) due = s->bus_retry;
	if (!s->core && s->pw_retry < due) due = s->pw_retry;
	if (s->bus && s->nm_dirty && !s->nm_pending && s->nm_due < due) due = s->nm_due;
	if (s->bus && s->bz_dirty && !s->bz_pending && s->bz_due < due) due = s->bz_due;
	return due;
}

void
services_step(struct services *s, uint64_t now)
{
	pw_loop_iterate(s->loop, 0);
	if (s->pw_failed) { audio_disconnect(s); s->pw_retry = now + 1000; }
	if (!s->core && now >= s->pw_retry) audio_connect(s);
	if (s->bus && !dbus_connection_get_is_connected(s->bus)) {
		bus_disconnect(s);
		s->bus_retry = now + 1000;
	}
	if (!s->bus && now >= s->bus_retry) bus_connect(s);
	if (!s->bus) return;
	for (unsigned i = 0; i < 64; i++)
		if (dbus_connection_dispatch(s->bus) != DBUS_DISPATCH_DATA_REMAINS) break;
	if (s->nm_dirty && !s->nm_pending && now >= s->nm_due) {
		s->nm_dirty = false;
		get_all(s, NM_PATH, NM, NM_ROOT);
	}
	if (s->bz_dirty && !s->bz_pending && now >= s->bz_due) {
		s->bz_dirty = false;
		send_request(s, dbus_message_new_method_call(BZ, "/", OBJECT_MANAGER, "GetManagedObjects"), BZ_OBJECTS);
	}
}

static void
set_boolean(struct services *s, const char *destination, const char *path, const char *interface, const char *property, bool value)
{
	DBusMessage *m = dbus_message_new_method_call(destination, path, PROPS, "Set");
	if (!m) return;
	DBusMessageIter it, variant;
	dbus_bool_t b = value;
	dbus_message_iter_init_append(m, &it);
	if (!dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &interface) ||
	    !dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &property) ||
	    !dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &variant)) {
		dbus_message_unref(m); return;
	}
	if (!dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &b)) {
		dbus_message_iter_abandon_container(&it, &variant);
		dbus_message_unref(m); return;
	}
	if (!dbus_message_iter_close_container(&it, &variant)) { dbus_message_unref(m); return; }
	send_request(s, m, SET_PROPERTY);
}

void
services_action(struct services *s, const char *action)
{
	if (!strcmp(action, "wifi") && s->bus) {
		set_boolean(s, NM, NM_PATH, NM, "WirelessEnabled", !s->status->wifi_on);
		dirty(s, true);
	} else if (!strcmp(action, "bluetooth") && s->bus && s->adapter[0]) {
		set_boolean(s, BZ, s->adapter, "org.bluez.Adapter1", "Powered", !s->status->bt_on);
		dirty(s, false);
	} else if (!strcmp(action, "mute")) {
		struct sink *n = default_sink(s);
		struct device *d = NULL;
		struct route *r = sink_route(s, n, &d);
		struct level *level = r ? &r->level : n ? &n->level : NULL;
		if (!level || !level->known) return;
		uint8_t buffer[512];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
		struct spa_pod *param = spa_pod_builder_add_object(&b,
		    SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(!level->muted));
		if (r) {
			struct spa_pod *route = spa_pod_builder_add_object(&b,
			    SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
			    SPA_PARAM_ROUTE_index, SPA_POD_Int(r->index),
			    SPA_PARAM_ROUTE_device, SPA_POD_Int(r->profile),
			    SPA_PARAM_ROUTE_props, SPA_POD_Pod(param),
			    SPA_PARAM_ROUTE_save, SPA_POD_Bool(true));
			pw_device_set_param(d->proxy, SPA_PARAM_Route, 0, route);
		} else pw_node_set_param(n->node, SPA_PARAM_Props, 0, param);
	}
}

void
services_free(struct services *s)
{
	if (!s) return;
	bus_disconnect(s);
	audio_disconnect(s);
	pw_context_destroy(s->context);
	pw_loop_destroy(s->loop);
	free(s);
	pw_deinit();
}
