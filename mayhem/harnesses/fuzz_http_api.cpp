// Fuzz the OpenSprinkler local HTTP API: raw request bytes -> OTF::Request parser -> the
// firmware's OTF route table (api/routes.cpp) -> every /xx handler in api/ (options, programs,
// stations, run-once, manual runs, logs, sensors, ...).
//
// The bytes are what a LAN client sends to the controller's web port. The device's socket loop
// (OpenThingsFramework::localServerLoop) hands the received request to OTF::Request and
// fillResponse(); this harness does the same on the firmware's own `otf` instance.
#include "otf_access.h"
#include "os_fuzz_env.h"

extern OTF::OpenThingsFramework *otf;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	osfuzz::init();
	osfuzz::reset();
	if (!otf) abort();
	// The socket loop NUL-terminates what it received (OpenThingsFramework.cpp localServerLoop).
	char *buf = static_cast<char *>(malloc(size + 1));
	memcpy(buf, data, size);
	buf[size] = 0;
	{
		OTF::Request req(buf, size, false);
		OTF::Response res;
		otf->fillResponse(req, res);
	}
	free(buf);
	return 0;
}
