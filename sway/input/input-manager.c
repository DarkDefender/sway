#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <libinput.h>
#include <math.h>
#include <wlr/backend/libinput.h>
#include <wlr/types/wlr_input_inhibitor.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include "sway/config.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/server.h"
#include "stringop.h"
#include "list.h"
#include "log.h"

#define DEFAULT_SEAT "seat0"

struct input_config *current_input_config = NULL;
struct seat_config *current_seat_config = NULL;

struct sway_seat *input_manager_current_seat(void) {
	struct sway_seat *seat = config->handler_context.seat;
	if (!seat) {
		seat = input_manager_get_default_seat();
	}
	return seat;
}

struct sway_seat *input_manager_get_default_seat(void) {
	return input_manager_get_seat(DEFAULT_SEAT, true);
}

struct sway_seat *input_manager_get_seat(const char *seat_name, bool create) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &server.input->seats, link) {
		if (strcmp(seat->wlr_seat->name, seat_name) == 0) {
			return seat;
		}
	}

	return create ? seat_create(seat_name) : NULL;
}

char *input_device_get_identifier(struct wlr_input_device *device) {
	int vendor = device->vendor;
	int product = device->product;
	char *name = strdup(device->name);
	strip_whitespace(name);

	char *p = name;
	for (; *p; ++p) {
		if (*p == ' ') {
			*p = '_';
		}
	}

	const char *fmt = "%d:%d:%s";
	int len = snprintf(NULL, 0, fmt, vendor, product, name) + 1;
	char *identifier = malloc(len);
	if (!identifier) {
		sway_log(SWAY_ERROR, "Unable to allocate unique input device name");
		return NULL;
	}

	snprintf(identifier, len, fmt, vendor, product, name);
	free(name);
	return identifier;
}

static bool device_is_touchpad(struct sway_input_device *device) {
	if (device->wlr_device->type != WLR_INPUT_DEVICE_POINTER ||
			!wlr_input_device_is_libinput(device->wlr_device)) {
		return false;
	}

	struct libinput_device *libinput_device =
		wlr_libinput_get_device_handle(device->wlr_device);

	return libinput_device_config_tap_get_finger_count(libinput_device) > 0;
}

const char *input_device_get_type(struct sway_input_device *device) {
	switch (device->wlr_device->type) {
	case WLR_INPUT_DEVICE_POINTER:
		if (device_is_touchpad(device)) {
			return "touchpad";
		} else {
			return "pointer";
		}
	case WLR_INPUT_DEVICE_KEYBOARD:
		return "keyboard";
	case WLR_INPUT_DEVICE_TOUCH:
		return "touch";
	case WLR_INPUT_DEVICE_TABLET_TOOL:
		return "tablet_tool";
	case WLR_INPUT_DEVICE_TABLET_PAD:
		return "tablet_pad";
	case WLR_INPUT_DEVICE_SWITCH:
		return "switch";
	}
	return "unknown";
}

static void apply_input_type_config(struct sway_input_device *input_device) {
	const char *device_type = input_device_get_type(input_device);
	struct input_config *type_config = NULL;
	for (int i = 0; i < config->input_type_configs->length; i++) {
		struct input_config *ic = config->input_type_configs->items[i];
		if (strcmp(ic->identifier + 5, device_type) == 0) {
			type_config = ic;
			break;
		}
	}
	if (type_config == NULL) {
		return;
	}

	for (int i = 0; i < config->input_configs->length; i++) {
		struct input_config *ic = config->input_configs->items[i];
		if (strcmp(input_device->identifier, ic->identifier) == 0) {
			struct input_config *current = new_input_config(ic->identifier);
			merge_input_config(current, type_config);
			merge_input_config(current, ic);

			current->input_type = device_type;
			config->input_configs->items[i] = current;
			free_input_config(ic);
			ic = NULL;

			break;
		}
	}
}

static struct sway_input_device *input_sway_device_from_wlr(
		struct wlr_input_device *device) {
	struct sway_input_device *input_device = NULL;
	wl_list_for_each(input_device, &server.input->devices, link) {
		if (input_device->wlr_device == device) {
			return input_device;
		}
	}
	return NULL;
}

static bool input_has_seat_fallback_configuration(void) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &server.input->seats, link) {
		struct seat_config *seat_config = seat_get_config(seat);
		if (seat_config && strcmp(seat_config->name, "*") != 0
				&& seat_config->fallback != -1) {
			return true;
		}
	}

	return false;
}

void input_manager_verify_fallback_seat(void) {
	struct sway_seat *seat = NULL;
	if (!input_has_seat_fallback_configuration()) {
		sway_log(SWAY_DEBUG, "no fallback seat config - creating default");
		seat = input_manager_get_default_seat();
		struct seat_config *sc = new_seat_config(seat->wlr_seat->name);
		sc->fallback = true;
		sc = store_seat_config(sc);
		input_manager_apply_seat_config(sc);
	}
}

static void log_libinput_config_status(enum libinput_config_status status) {
	if (status != LIBINPUT_CONFIG_STATUS_SUCCESS) {
		sway_log(SWAY_ERROR, "Failed to apply libinput config: %s",
			libinput_config_status_to_str(status));
	}
}

static void input_manager_libinput_config_keyboard(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;
	struct input_config *ic = input_device_get_config(input_device);
	struct libinput_device *libinput_device;

	if (!ic || !wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	libinput_device = wlr_libinput_get_device_handle(wlr_device);
	sway_log(SWAY_DEBUG, "input_manager_libinput_config_keyboard(%s)",
		ic->identifier);

	if (ic->send_events != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_keyboard(%s) send_events_set_mode(%d)",
			ic->identifier, ic->send_events);
		log_libinput_config_status(libinput_device_config_send_events_set_mode(
					libinput_device, ic->send_events));
	}
}

static void input_manager_libinput_reset_keyboard(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;
	struct libinput_device *libinput_device;

	if (!wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	libinput_device = wlr_libinput_get_device_handle(wlr_device);

	uint32_t send_events =
		libinput_device_config_send_events_get_default_mode(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_keyboard(%s) send_events_set_mode(%d)",
		input_device->identifier, send_events);
	log_libinput_config_status(libinput_device_config_send_events_set_mode(
				libinput_device, send_events));
}

static void input_manager_libinput_config_switch(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;
	struct input_config *ic = input_device_get_config(input_device);
	struct libinput_device *libinput_device;

	if (!ic || !wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	libinput_device = wlr_libinput_get_device_handle(wlr_device);
	sway_log(SWAY_DEBUG, "input_manager_libinput_config_switch(%s)",
		ic->identifier);

	if (ic->send_events != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_switch(%s) send_events_set_mode(%d)",
			ic->identifier, ic->send_events);
		log_libinput_config_status(libinput_device_config_send_events_set_mode(
				libinput_device, ic->send_events));
	}
}

static void input_manager_libinput_reset_switch(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;
	struct libinput_device *libinput_device;

	if (!wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	libinput_device = wlr_libinput_get_device_handle(wlr_device);

	uint32_t send_events =
		libinput_device_config_send_events_get_default_mode(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_switch(%s) send_events_set_mode(%d)",
		input_device->identifier, send_events);
	log_libinput_config_status(libinput_device_config_send_events_set_mode(
				libinput_device, send_events));
}

static void input_manager_libinput_config_touch(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;
	struct input_config *ic = input_device_get_config(input_device);
	struct libinput_device *libinput_device;

	if (!ic || !wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	libinput_device = wlr_libinput_get_device_handle(wlr_device);
	sway_log(SWAY_DEBUG, "input_manager_libinput_config_touch(%s)",
		ic->identifier);

	if (ic->send_events != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_touch(%s) send_events_set_mode(%d)",
			ic->identifier, ic->send_events);
		log_libinput_config_status(libinput_device_config_send_events_set_mode(
					libinput_device, ic->send_events));
	}
	float *m = ic->calibration_matrix.matrix;
	if (ic->calibration_matrix.configured) {
		sway_log(SWAY_DEBUG, "libinput_config_touch(%s) calibration_set_matrix(%f %f %f %f %f %f)",
			ic->identifier, m[0], m[1], m[2], m[3], m[4], m[5]);
		log_libinput_config_status(libinput_device_config_calibration_set_matrix(
					libinput_device, ic->calibration_matrix.matrix));
	}
}

static void input_manager_libinput_reset_touch(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;
	struct libinput_device *libinput_device;

	if (!wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	libinput_device = wlr_libinput_get_device_handle(wlr_device);

	uint32_t send_events =
		libinput_device_config_send_events_get_default_mode(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_touch(%s) send_events_set_mode(%d)",
		input_device->identifier, send_events);
	log_libinput_config_status(libinput_device_config_send_events_set_mode(
				libinput_device, send_events));
	float m[6];
	libinput_device_config_calibration_get_default_matrix(libinput_device, m);
	sway_log(SWAY_DEBUG, "libinput_reset_touch(%s) calibration_set_matrix(%f %f %f %f %f %f)",
		input_device->identifier, m[0], m[1], m[2], m[3], m[4], m[5]);
	log_libinput_config_status(libinput_device_config_calibration_set_matrix(
				libinput_device, m));
}

static void input_manager_libinput_config_pointer(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;
	struct input_config *ic = input_device_get_config(input_device);
	struct libinput_device *libinput_device;

	if (!ic || !wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	libinput_device = wlr_libinput_get_device_handle(wlr_device);
	sway_log(SWAY_DEBUG, "input_manager_libinput_config_pointer(%s)",
		ic->identifier);

	if (ic->accel_profile != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) accel_set_profile(%d)",
			ic->identifier, ic->accel_profile);
		log_libinput_config_status(libinput_device_config_accel_set_profile(
					libinput_device, ic->accel_profile));
	}
	if (ic->click_method != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) click_set_method(%d)",
			ic->identifier, ic->click_method);
		log_libinput_config_status(libinput_device_config_click_set_method(
					libinput_device, ic->click_method));
	}
	if (ic->drag != INT_MIN) {
		sway_log(SWAY_DEBUG,
			"libinput_config_pointer(%s) tap_set_drag_enabled(%d)",
			ic->identifier, ic->drag);
		log_libinput_config_status(libinput_device_config_tap_set_drag_enabled(
					libinput_device, ic->drag));
	}
	if (ic->drag_lock != INT_MIN) {
		sway_log(SWAY_DEBUG,
			"libinput_config_pointer(%s) tap_set_drag_lock_enabled(%d)",
			ic->identifier, ic->drag_lock);
		log_libinput_config_status(
				libinput_device_config_tap_set_drag_lock_enabled(
					libinput_device, ic->drag_lock));
	}
	if (ic->dwt != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) dwt_set_enabled(%d)",
			ic->identifier, ic->dwt);
		log_libinput_config_status(libinput_device_config_dwt_set_enabled(
					libinput_device, ic->dwt));
	}
	if (ic->left_handed != INT_MIN) {
		sway_log(SWAY_DEBUG,
			"libinput_config_pointer(%s) left_handed_set_enabled(%d)",
			ic->identifier, ic->left_handed);
		log_libinput_config_status(libinput_device_config_left_handed_set(
					libinput_device, ic->left_handed));
	}
	if (ic->middle_emulation != INT_MIN) {
		sway_log(SWAY_DEBUG,
			"libinput_config_pointer(%s) middle_emulation_set_enabled(%d)",
			ic->identifier, ic->middle_emulation);
		log_libinput_config_status(
				libinput_device_config_middle_emulation_set_enabled(
					libinput_device, ic->middle_emulation));
	}
	if (ic->natural_scroll != INT_MIN) {
		sway_log(SWAY_DEBUG,
			"libinput_config_pointer(%s) natural_scroll_set_enabled(%d)",
			ic->identifier, ic->natural_scroll);
		log_libinput_config_status(
				libinput_device_config_scroll_set_natural_scroll_enabled(
					libinput_device, ic->natural_scroll));
	}
	if (ic->pointer_accel != FLT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) accel_set_speed(%f)",
			ic->identifier, ic->pointer_accel);
		log_libinput_config_status(libinput_device_config_accel_set_speed(
					libinput_device, ic->pointer_accel));
	}
	if (ic->scroll_button != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) scroll_set_button(%d)",
			ic->identifier, ic->scroll_button);
		log_libinput_config_status(libinput_device_config_scroll_set_button(
					libinput_device, ic->scroll_button));
	}
	if (ic->scroll_method != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) scroll_set_method(%d)",
			ic->identifier, ic->scroll_method);
		log_libinput_config_status(libinput_device_config_scroll_set_method(
					libinput_device, ic->scroll_method));
	}
	if (ic->send_events != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) send_events_set_mode(%d)",
			ic->identifier, ic->send_events);
		log_libinput_config_status(libinput_device_config_send_events_set_mode(
					libinput_device, ic->send_events));
	}
	if (ic->tap != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) tap_set_enabled(%d)",
			ic->identifier, ic->tap);
		log_libinput_config_status(libinput_device_config_tap_set_enabled(
					libinput_device, ic->tap));
	}
	if (ic->tap_button_map != INT_MIN) {
		sway_log(SWAY_DEBUG, "libinput_config_pointer(%s) tap_set_button_map(%d)",
			ic->identifier, ic->tap_button_map);
		log_libinput_config_status(libinput_device_config_tap_set_button_map(
					libinput_device, ic->tap_button_map));
	}
}

static void input_manager_libinput_reset_pointer(
		struct sway_input_device *input_device) {
	struct wlr_input_device *wlr_device = input_device->wlr_device;

	if (!wlr_input_device_is_libinput(wlr_device)) {
		return;
	}

	struct libinput_device *libinput_device =
		wlr_libinput_get_device_handle(wlr_device);

	enum libinput_config_accel_profile accel_profile =
		libinput_device_config_accel_get_default_profile(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) accel_set_profile(%d)",
			input_device->identifier, accel_profile);
	log_libinput_config_status(libinput_device_config_accel_set_profile(
				libinput_device, accel_profile));

	enum libinput_config_click_method click_method =
		libinput_device_config_click_get_default_method(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) click_set_method(%d)",
		input_device->identifier, click_method);
	log_libinput_config_status(libinput_device_config_click_set_method(
				libinput_device, click_method));

	enum libinput_config_drag_state drag =
		libinput_device_config_tap_get_default_drag_enabled(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) tap_set_drag_enabled(%d)",
			input_device->identifier, drag);
	log_libinput_config_status(libinput_device_config_tap_set_drag_enabled(
				libinput_device, drag));

	enum libinput_config_drag_lock_state drag_lock =
		libinput_device_config_tap_get_default_drag_lock_enabled(
				libinput_device);
	sway_log(SWAY_DEBUG,
			"libinput_reset_pointer(%s) tap_set_drag_lock_enabled(%d)",
			input_device->identifier, drag_lock);
	log_libinput_config_status(
			libinput_device_config_tap_set_drag_lock_enabled(
				libinput_device, drag_lock));

	enum libinput_config_dwt_state dwt =
		libinput_device_config_dwt_get_default_enabled(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) dwt_set_enabled(%d)",
		input_device->identifier, dwt);
	log_libinput_config_status(libinput_device_config_dwt_set_enabled(
				libinput_device, dwt));

	int left_handed =
		libinput_device_config_left_handed_get_default(libinput_device);
	sway_log(SWAY_DEBUG,
		"libinput_reset_pointer(%s) left_handed_set_enabled(%d)",
		input_device->identifier, left_handed);
	log_libinput_config_status(libinput_device_config_left_handed_set(
				libinput_device, left_handed));

	enum libinput_config_middle_emulation_state middle_emulation =
		libinput_device_config_middle_emulation_get_default_enabled(
				libinput_device);
	sway_log(SWAY_DEBUG,
		"libinput_reset_pointer(%s) middle_emulation_set_enabled(%d)",
		input_device->identifier, middle_emulation);
	log_libinput_config_status(
			libinput_device_config_middle_emulation_set_enabled(
				libinput_device, middle_emulation));

	int natural_scroll =
		libinput_device_config_scroll_get_default_natural_scroll_enabled(
				libinput_device);
	sway_log(SWAY_DEBUG,
		"libinput_reset_pointer(%s) natural_scroll_set_enabled(%d)",
		input_device->identifier, natural_scroll);
	log_libinput_config_status(
			libinput_device_config_scroll_set_natural_scroll_enabled(
				libinput_device, natural_scroll));

	double pointer_accel =
		libinput_device_config_accel_get_default_speed(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) accel_set_speed(%f)",
		input_device->identifier, pointer_accel);
	log_libinput_config_status(libinput_device_config_accel_set_speed(
				libinput_device, pointer_accel));

	uint32_t scroll_button =
		libinput_device_config_scroll_get_default_button(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) scroll_set_button(%d)",
		input_device->identifier, scroll_button);
	log_libinput_config_status(libinput_device_config_scroll_set_button(
				libinput_device, scroll_button));

	enum libinput_config_scroll_method scroll_method =
		libinput_device_config_scroll_get_default_method(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) scroll_set_method(%d)",
		input_device->identifier, scroll_method);
	log_libinput_config_status(libinput_device_config_scroll_set_method(
				libinput_device, scroll_method));

	uint32_t send_events =
		libinput_device_config_send_events_get_default_mode(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) send_events_set_mode(%d)",
		input_device->identifier, send_events);
	log_libinput_config_status(libinput_device_config_send_events_set_mode(
				libinput_device, send_events));

	enum libinput_config_tap_state tap =
		libinput_device_config_tap_get_default_enabled(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) tap_set_enabled(%d)",
		input_device->identifier, tap);
	log_libinput_config_status(libinput_device_config_tap_set_enabled(
				libinput_device, tap));

	enum libinput_config_tap_button_map tap_button_map =
		libinput_device_config_tap_get_button_map(libinput_device);
	sway_log(SWAY_DEBUG, "libinput_reset_pointer(%s) tap_set_button_map(%d)",
		input_device->identifier, tap_button_map);
	log_libinput_config_status(libinput_device_config_tap_set_button_map(
				libinput_device, tap_button_map));
}

static void handle_device_destroy(struct wl_listener *listener, void *data) {
	struct wlr_input_device *device = data;

	struct sway_input_device *input_device = input_sway_device_from_wlr(device);

	if (!sway_assert(input_device, "could not find sway device")) {
		return;
	}

	sway_log(SWAY_DEBUG, "removing device: '%s'",
		input_device->identifier);

	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_remove_device(seat, input_device);
	}

	wl_list_remove(&input_device->link);
	wl_list_remove(&input_device->device_destroy.link);
	free(input_device->identifier);
	free(input_device);
}

static void handle_new_input(struct wl_listener *listener, void *data) {
	struct sway_input_manager *input =
		wl_container_of(listener, input, new_input);
	struct wlr_input_device *device = data;

	struct sway_input_device *input_device =
		calloc(1, sizeof(struct sway_input_device));
	if (!sway_assert(input_device, "could not allocate input device")) {
		return;
	}
	device->data = input_device;

	input_device->wlr_device = device;
	input_device->identifier = input_device_get_identifier(device);
	wl_list_insert(&input->devices, &input_device->link);

	sway_log(SWAY_DEBUG, "adding device: '%s'",
		input_device->identifier);

	apply_input_type_config(input_device);

	if (input_device->wlr_device->type == WLR_INPUT_DEVICE_POINTER ||
			input_device->wlr_device->type == WLR_INPUT_DEVICE_TABLET_TOOL) {
		input_manager_libinput_config_pointer(input_device);
	} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		input_manager_libinput_config_keyboard(input_device);
	} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_SWITCH) {
		input_manager_libinput_config_switch(input_device);
	} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_TOUCH) {
		input_manager_libinput_config_touch(input_device);
	}

	wl_signal_add(&device->events.destroy, &input_device->device_destroy);
	input_device->device_destroy.notify = handle_device_destroy;

	input_manager_verify_fallback_seat();

	bool added = false;
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &input->seats, link) {
		struct seat_config *seat_config = seat_get_config(seat);
		bool has_attachment = seat_config &&
			(seat_config_get_attachment(seat_config, input_device->identifier) ||
			 seat_config_get_attachment(seat_config, "*"));

		if (has_attachment) {
			seat_add_device(seat, input_device);
			added = true;
		}
	}

	if (!added) {
		wl_list_for_each(seat, &input->seats, link) {
			struct seat_config *seat_config = seat_get_config(seat);
			if (seat_config && seat_config->fallback == 1) {
				seat_add_device(seat, input_device);
				added = true;
			}
		}
	}

	if (!added) {
		sway_log(SWAY_DEBUG,
			"device '%s' is not configured on any seats",
			input_device->identifier);
	}
}

static void handle_inhibit_activate(struct wl_listener *listener, void *data) {
	struct sway_input_manager *input_manager = wl_container_of(
			listener, input_manager, inhibit_activate);
	struct sway_seat *seat;
	wl_list_for_each(seat, &input_manager->seats, link) {
		seat_set_exclusive_client(seat, input_manager->inhibit->active_client);
	}
}

static void handle_inhibit_deactivate(struct wl_listener *listener, void *data) {
	struct sway_input_manager *input_manager = wl_container_of(
			listener, input_manager, inhibit_deactivate);
	struct sway_seat *seat;
	wl_list_for_each(seat, &input_manager->seats, link) {
		seat_set_exclusive_client(seat, NULL);
		struct sway_node *previous = seat_get_focus(seat);
		if (previous) {
			// Hack to get seat to re-focus the return value of get_focus
			seat_set_focus(seat, NULL);
			seat_set_focus(seat, previous);
		}
	}
}

void handle_virtual_keyboard(struct wl_listener *listener, void *data) {
	struct sway_input_manager *input_manager =
		wl_container_of(listener, input_manager, virtual_keyboard_new);
	struct wlr_virtual_keyboard_v1 *keyboard = data;
	struct wlr_input_device *device = &keyboard->input_device;

	struct sway_seat *seat = input_manager_get_default_seat();

	// TODO: The user might want this on a different seat
	struct sway_input_device *input_device =
		calloc(1, sizeof(struct sway_input_device));
	if (!sway_assert(input_device, "could not allocate input device")) {
		return;
	}
	device->data = input_device;

	input_device->wlr_device = device;
	input_device->identifier = input_device_get_identifier(device);
	wl_list_insert(&input_manager->devices, &input_device->link);

	sway_log(SWAY_DEBUG, "adding virtual keyboard: '%s'",
		input_device->identifier);

	wl_signal_add(&device->events.destroy, &input_device->device_destroy);
	input_device->device_destroy.notify = handle_device_destroy;

	seat_add_device(seat, input_device);
}

struct sway_input_manager *input_manager_create(struct sway_server *server) {
	struct sway_input_manager *input =
		calloc(1, sizeof(struct sway_input_manager));
	if (!input) {
		return NULL;
	}

	wl_list_init(&input->devices);
	wl_list_init(&input->seats);

	input->new_input.notify = handle_new_input;
	wl_signal_add(&server->backend->events.new_input, &input->new_input);

	input->virtual_keyboard = wlr_virtual_keyboard_manager_v1_create(
		server->wl_display);
	wl_signal_add(&input->virtual_keyboard->events.new_virtual_keyboard,
		&input->virtual_keyboard_new);
	input->virtual_keyboard_new.notify = handle_virtual_keyboard;

	input->inhibit = wlr_input_inhibit_manager_create(server->wl_display);
	input->inhibit_activate.notify = handle_inhibit_activate;
	wl_signal_add(&input->inhibit->events.activate,
			&input->inhibit_activate);
	input->inhibit_deactivate.notify = handle_inhibit_deactivate;
	wl_signal_add(&input->inhibit->events.deactivate,
			&input->inhibit_deactivate);

	return input;
}

bool input_manager_has_focus(struct sway_node *node) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &server.input->seats, link) {
		if (seat_get_focus(seat) == node) {
			return true;
		}
	}

	return false;
}

void input_manager_set_focus(struct sway_node *node) {
	struct sway_seat *seat;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_set_focus(seat, node);
		seat_consider_warp_to_focus(seat);
	}
}

void input_manager_apply_input_config(struct input_config *input_config) {
	struct sway_input_device *input_device = NULL;
	bool wildcard = strcmp(input_config->identifier, "*") == 0;
	bool type_wildcard = strncmp(input_config->identifier, "type:", 5) == 0;
	wl_list_for_each(input_device, &server.input->devices, link) {
		bool type_matches = type_wildcard &&
			strcmp(input_device_get_type(input_device), input_config->identifier + 5) == 0;
		if (strcmp(input_device->identifier, input_config->identifier) == 0
				|| wildcard
				|| type_matches) {
			if (input_device->wlr_device->type == WLR_INPUT_DEVICE_POINTER ||
					input_device->wlr_device->type == WLR_INPUT_DEVICE_TABLET_TOOL) {
				input_manager_libinput_config_pointer(input_device);
			} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_KEYBOARD) {
				input_manager_libinput_config_keyboard(input_device);
			} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_SWITCH) {
				input_manager_libinput_config_switch(input_device);
			} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_TOUCH) {
				input_manager_libinput_config_touch(input_device);
			}

			struct sway_seat *seat = NULL;
			wl_list_for_each(seat, &server.input->seats, link) {
				seat_configure_device(seat, input_device);
			}
		}
	}
}

void input_manager_reset_input(struct sway_input_device *input_device) {
	if (input_device->wlr_device->type == WLR_INPUT_DEVICE_POINTER ||
			input_device->wlr_device->type == WLR_INPUT_DEVICE_TABLET_TOOL) {
		input_manager_libinput_reset_pointer(input_device);
	} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_KEYBOARD) {
		input_manager_libinput_reset_keyboard(input_device);
	} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_SWITCH) {
		input_manager_libinput_reset_switch(input_device);
	} else if (input_device->wlr_device->type == WLR_INPUT_DEVICE_TOUCH) {
		input_manager_libinput_reset_touch(input_device);
	}

	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_reset_device(seat, input_device);
	}
}

void input_manager_reset_all_inputs() {
	struct sway_input_device *input_device = NULL;
	wl_list_for_each(input_device, &server.input->devices, link) {
		input_manager_reset_input(input_device);
	}
}


void input_manager_apply_seat_config(struct seat_config *seat_config) {
	sway_log(SWAY_DEBUG, "applying seat config for seat %s", seat_config->name);
	if (strcmp(seat_config->name, "*") == 0) {
		struct sway_seat *seat = NULL;
		wl_list_for_each(seat, &server.input->seats, link) {
			// Only apply the wildcard config directly if there is no seat
			// specific config
			struct seat_config *sc = seat_get_config(seat);
			if (!sc) {
				sc = seat_config;
			}
			seat_apply_config(seat, sc);
		}
	} else {
		struct sway_seat *seat =
			input_manager_get_seat(seat_config->name, true);
		if (!seat) {
			return;
		}
		seat_apply_config(seat, seat_config);
	}

	// for every device, try to add it to a seat and if no seat has it
	// attached, add it to the fallback seats.
	struct sway_input_device *input_device = NULL;
	wl_list_for_each(input_device, &server.input->devices, link) {
		list_t *seat_list = create_list();
		struct sway_seat *seat = NULL;
		wl_list_for_each(seat, &server.input->seats, link) {
			struct seat_config *seat_config = seat_get_config(seat);
			if (!seat_config) {
				continue;
			}
			if (seat_config_get_attachment(seat_config, "*") ||
					seat_config_get_attachment(seat_config,
						input_device->identifier)) {
				list_add(seat_list, seat);
			}
		}

		if (seat_list->length) {
			wl_list_for_each(seat, &server.input->seats, link) {
				bool attached = false;
				for (int i = 0; i < seat_list->length; ++i) {
					if (seat == seat_list->items[i]) {
						attached = true;
						break;
					}
				}
				if (attached) {
					seat_add_device(seat, input_device);
				} else {
					seat_remove_device(seat, input_device);
				}
			}
		} else {
			wl_list_for_each(seat, &server.input->seats, link) {
				struct seat_config *seat_config = seat_get_config(seat);
				if (seat_config && seat_config->fallback == 1) {
					seat_add_device(seat, input_device);
				} else {
					seat_remove_device(seat, input_device);
				}
			}
		}
		list_free(seat_list);
	}
}

void input_manager_configure_xcursor(void) {
	struct sway_seat *seat = NULL;
	wl_list_for_each(seat, &server.input->seats, link) {
		seat_configure_xcursor(seat);
	}
}

struct input_config *input_device_get_config(struct sway_input_device *device) {
	struct input_config *wildcard_config = NULL;
	struct input_config *input_config = NULL;
	for (int i = 0; i < config->input_configs->length; ++i) {
		input_config = config->input_configs->items[i];
		if (strcmp(input_config->identifier, device->identifier) == 0) {
			return input_config;
		} else if (strcmp(input_config->identifier, "*") == 0) {
			wildcard_config = input_config;
		}
	}

	const char *device_type = input_device_get_type(device);
	for (int i = 0; i < config->input_type_configs->length; ++i) {
		input_config = config->input_type_configs->items[i];
		if (strcmp(input_config->identifier + 5, device_type) == 0) {
			return input_config;
		}
	}

	return wildcard_config;
}
