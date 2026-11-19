// Fuzz an MQTT command payload. The firmware subscribes to its command topic and passes each
// received message, unmodified, to dispatch_mqtt_command() (services/mqtt.cpp, mosquitto
// subscribe callback): a 2-char command (cv/cm/cr/mp) followed by query parameters, executed by
// the same command code as the HTTP API (api/commands.cpp).
#include "os_fuzz_env.h"
#include "services/mqtt.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	osfuzz::init();
	osfuzz::reset();
	dispatch_mqtt_command(data, size);
	return 0;
}
