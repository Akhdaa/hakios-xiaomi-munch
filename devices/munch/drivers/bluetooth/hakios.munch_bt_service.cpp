#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>

class MunchBluetooth : public IOService
{
	OSDeclareDefaultStructors(MunchBluetooth);
};

OSDefineMetaClassAndStructors(MunchBluetooth, IOService);

extern "C" bool
munch_bt_publish(const uint8_t *address)
{
	IOService *platform = IOService::getPlatform();
	if (platform == nullptr) {
		return false;
	}
	MunchBluetooth *node = OSTypeAlloc(MunchBluetooth);
	if (node == nullptr) {
		return false;
	}
	if (!node->init() || !node->attach(platform)) {
		node->release();
		return false;
	}

	/* The HCI address is little endian. IORegistry expects display order. */
	uint8_t mac[6];
	for (unsigned i = 0; i < sizeof(mac); i++) {
		mac[i] = address[sizeof(mac) - 1 - i];
	}
	OSData *property = OSData::withBytes(mac, sizeof(mac));
	node->setName("bluetooth");
	bool ready = property != nullptr && node->setProperty("local-mac-address", property) && node->start(platform);
	OSSafeReleaseNULL(property);
	if (ready) {
		node->registerService();
	} else {
		node->detach(platform);
	}
	node->release();
	return ready;
}
