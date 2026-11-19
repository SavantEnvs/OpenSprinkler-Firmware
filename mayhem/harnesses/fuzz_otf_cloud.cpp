// Fuzz an OpenThings Cloud (OTC) websocket frame: "FWD: <id>\r\n<forwarded HTTP request>".
// OpenThingsFramework::handleCloudMessage parses the forwarded frame (ForwardedRequest.cpp),
// builds an OTF::Request marked as a cloud request, and dispatches it through the firmware's
// route table to the api/ handlers. The cloud server relays these frames from any remote client.
// The websocket itself is not connected (cloud status NOT_ENABLED), so the response stream is
// built but not sent, exactly as handleCloudMessage does when the link drops.
#include "otf_access.h"
#include "os_fuzz_env.h"

extern OTF::OpenThingsFramework *otf;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	osfuzz::init();
	osfuzz::reset();
	if (!otf) abort();
	// The websocket layer delivers the frame in a heap buffer that handleCloudMessage edits in place.
	uint8_t *buf = static_cast<uint8_t *>(malloc(size + 1));
	memcpy(buf, data, size);
	buf[size] = 0;
	otf->handleCloudMessage(buf, size);
	free(buf);
	return 0;
}
