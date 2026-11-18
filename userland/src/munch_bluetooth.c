/* BlueTool uses the controller initialized by the munch kernel driver. */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef mach_port_t io_service_t;
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
struct _xpc_type_s;

extern CFMutableDictionaryRef IOServiceMatching(const char *);
extern io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef);
extern kern_return_t IOObjectRelease(io_service_t);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary;
extern const struct _xpc_type_s *xpc_get_type(xpc_object_t);
extern xpc_connection_t xpc_connection_create_mach_service(const char *, dispatch_queue_t, uint64_t);
extern void xpc_connection_set_event_handler(xpc_connection_t, void (^)(xpc_object_t));
extern void xpc_connection_resume(xpc_connection_t);
extern void xpc_connection_send_message(xpc_connection_t, xpc_object_t);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t);
extern void xpc_dictionary_set_int64(xpc_object_t, const char *, int64_t);
extern void xpc_dictionary_set_string(xpc_object_t, const char *, const char *);
extern void xpc_release(xpc_object_t);

static bool
controller_ready(void)
{
	io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("MunchBluetooth"));
	if (service == MACH_PORT_NULL) {
		return false;
	}
	IOObjectRelease(service);
	return true;
}

static void
serve_bluetool(void)
{
	xpc_connection_t service = xpc_connection_create_mach_service("com.apple.BlueTool", NULL, 1);
	if (service == NULL) {
		return;
	}
	xpc_connection_set_event_handler(service, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t request) {
			if (xpc_get_type(request) != &_xpc_type_dictionary) {
				return;
			}
			xpc_object_t reply = xpc_dictionary_create_reply(request);
			if (reply == NULL) {
				return;
			}
			bool ready = controller_ready();
			xpc_dictionary_set_int64(reply, "result", ready ? 0 : ENXIO);
			if (ready) {
				/* Manufacturer 15 selects iOS 16.5's Broadcom H4BC interface. */
				xpc_dictionary_set_int64(reply, "manufacturer", 15);
				xpc_dictionary_set_string(reply, "firmware", "QCA6390");
				xpc_dictionary_set_string(reply, "output", "");
			}
			xpc_connection_send_message(peer, reply);
			xpc_release(reply);
			fprintf(stderr, "munch-bt: BlueTool controller ready=%d\n", ready);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(service);
	dispatch_main();
}

__attribute__((constructor))
static void
munch_bluetooth_init(void)
{
	const char *program = getprogname();
	if (strcmp(program, "BlueTool") == 0) {
		serve_bluetool();
	} else if (strcmp(program, "bluetoothd") == 0) {
		/* The QCA patch download starts after Wi-Fi has powered the shared rails. */
		for (unsigned attempt = 0; attempt < 425; attempt++) {
			if (controller_ready()) {
				fprintf(stderr, "munch-bt: controller visible to bluetoothd\n");
				return;
			}
			usleep(200000);
		}
		fprintf(stderr, "munch-bt: bluetoothd timed out waiting for the controller\n");
	}
}
