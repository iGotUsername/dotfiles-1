/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Include the implementation to exercise POD parsing and route selection
 * without a physical ALSA/BlueZ device or an additional public API. */
#include "../src/everbar/services.c"
#include <assert.h>

struct fake_device { struct spa_interface iface; bool muted, saved; int calls; };

static int
fake_enum(void *object, int seq, uint32_t id, uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	(void)object; (void)seq; (void)start; (void)num; (void)filter;
	assert(id == SPA_PARAM_Route);
	return 0x40000042; /* native protocol's replacement async sequence */
}

static int
fake_set(void *object, uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	(void)flags;
	struct fake_device *d = object;
	assert(id == SPA_PARAM_Route);
	int index, profile;
	const struct spa_pod *props;
	assert(spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
	    SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
	    SPA_PARAM_ROUTE_device, SPA_POD_Int(&profile),
	    SPA_PARAM_ROUTE_props, SPA_POD_Pod(&props),
	    SPA_PARAM_ROUTE_save, SPA_POD_Bool(&d->saved)) >= 0);
	assert(index == 5 && profile == 2);
	assert(spa_pod_parse_object(props, SPA_TYPE_OBJECT_Props, NULL,
	    SPA_PROP_mute, SPA_POD_Bool(&d->muted)) >= 0);
	d->calls++;
	return 0;
}

int
main(void)
{
	static const struct pw_device_methods methods = {
		PW_VERSION_DEVICE_METHODS, .enum_params = fake_enum, .set_param = fake_set,
	};
	struct fake_device fake = {0};
	fake.iface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, &methods, &fake);
	struct status status = {0};
	struct services s = {.status = &status};
	struct sink n = {.s = &s, .name = "sink", .device_id = 9, .profile = 2};
	struct device d = {.s = &s, .id = 9, .proxy = (struct pw_device *)&fake};
	s.sinks = &n; s.devices = &d;
	strcpy(s.default_sink, "sink");
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
	float volumes[] = {.125f, .729f};
	struct spa_pod *props = spa_pod_builder_add_object(&b,
	    SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
	    SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 2, volumes),
	    SPA_PROP_mute, SPA_POD_Bool(false));
	node_param(&n, 0, SPA_PARAM_Props, 0, 0, props);
	assert(status.volume == 50 && !status.muted);
	struct spa_param_info param_info = {.id = SPA_PARAM_Route, .flags = SPA_PARAM_INFO_READ};
	struct pw_device_info info = {.change_mask = PW_DEVICE_CHANGE_MASK_PARAMS, .params = &param_info, .n_params = 1};
	device_info(&d, &info);
	assert(d.generation == 0x40000042);
	struct spa_pod *route = spa_pod_builder_add_object(&b,
	    SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
	    SPA_PARAM_ROUTE_index, SPA_POD_Int(5),
	    SPA_PARAM_ROUTE_device, SPA_POD_Int(2),
	    SPA_PARAM_ROUTE_props, SPA_POD_Pod(props));
	device_param(&d, d.generation - 1, SPA_PARAM_Route, 0, 0, route);
	assert(!d.routes); /* obsolete reply must not change the current route */
	device_param(&d, d.generation, SPA_PARAM_Route, 0, 0, route);
	assert(d.routes && status.volume == 50 && !status.muted);
	n.level.volume = 99; audio_status(&s);
	assert(status.volume == 50); /* hardware route wins over node software gain */
	services_action(&s, "mute");
	assert(fake.calls == 1 && fake.saved && fake.muted);
	/* A removed/default-unknown output must never mute a different device. */
	s.default_sink[0] = 0;
	services_action(&s, "mute");
	assert(fake.calls == 1);
	strcpy(s.default_sink, "sink");
	d.generation++;
	audio_status(&s);
	assert(status.volume == 99); /* stale hardware route no longer wins */
	free(d.routes);
	puts("service POD/route tests: ok");
	return 0;
}
