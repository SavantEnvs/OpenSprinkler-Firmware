// Access to the OTF dispatch entry points the firmware itself uses.
//
// OTF::OpenThingsFramework::fillResponse (local HTTP dispatch) and handleCloudMessage (OTC
// websocket frame dispatch), and the OTF::Response constructor, are private: on a device they are
// only reached from the socket / websocket loops. The harnesses call them directly on the
// firmware's own `otf` instance, so this header re-declares the OTF classes with public access.
// Only access control changes (no layout or code change), and only inside the harness TU.
#pragma once

// Pull in every system/library header the OTF headers include first, so the access override
// below only applies to the OTF declarations.
#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <new>
#include <string>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#define private public
#include "external/OpenThings-Framework-Firmware-Library/OpenThingsFramework.h"
#undef private
