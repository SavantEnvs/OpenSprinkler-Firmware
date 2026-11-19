// Fuzz the weather-service parsers (services/weather.cpp). The weather service is a remote HTTP
// server (default weather.opensprinkler.com, or any SOPT_WEATHERURL). The harness points the
// weather URL at a loopback listener that answers with the fuzz bytes and calls the firmware's
// own GetWeather(): request build, os.send_http_request() response read, peel_http_header() and
// getweather_callback() (errCode/scale/sunrise/tz/rd/rawData/scales -> parseMdScalesArray, option
// and nvdata updates). The same bytes are also fed to the two other parsers of weather data:
// weather_sensor_parse_response() (weather-sensor JSON response) and parse_wto() (the weather
// options string, also set remotely through /co?wto=).
#include "os_fuzz_env.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include <algorithm>

namespace {

int g_listen = -1;
uint16_t g_port = 0;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
std::string g_reply;

// Minimal HTTP server: read the request headers, send the current reply, close.
void *serve(void *) {
	for (;;) {
		int c = accept(g_listen, nullptr, nullptr);
		if (c < 0) continue;
		char buf[4096];
		std::string req;
		while (req.find("\r\n\r\n") == std::string::npos) {
			ssize_t n = recv(c, buf, sizeof(buf), 0);
			if (n <= 0) break;
			req.append(buf, (size_t)n);
		}
		pthread_mutex_lock(&g_mu);
		std::string reply = g_reply;
		pthread_mutex_unlock(&g_mu);
		size_t off = 0;
		while (off < reply.size()) {
			ssize_t n = send(c, reply.data() + off, reply.size() - off, MSG_NOSIGNAL);
			if (n <= 0) break;
			off += (size_t)n;
		}
		shutdown(c, SHUT_WR);
		while (recv(c, buf, sizeof(buf), 0) > 0) {}
		close(c);
	}
	return nullptr;
}

void start_server() {
	if (g_listen >= 0) return;
	g_listen = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	if (bind(g_listen, (sockaddr *)&a, sizeof(a)) != 0 || listen(g_listen, 16) != 0) { perror("bind"); abort(); }
	socklen_t l = sizeof(a);
	getsockname(g_listen, (sockaddr *)&a, &l);
	g_port = ntohs(a.sin_port);
	pthread_t t;
	pthread_create(&t, nullptr, serve, nullptr);
	pthread_detach(t);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	osfuzz::init();
	start_server();
	osfuzz::reset();

	// Point the weather service at the loopback listener (a user-settable option, /co?wto / wsp).
	char url[64];
	snprintf(url, sizeof(url), "http://127.0.0.1:%u", (unsigned)g_port);
	os.sopt_save(SOPT_WEATHERURL, url);
	os.iopts[IOPT_USE_WEATHER] = 1;

	pthread_mutex_lock(&g_mu);
	g_reply.assign((const char *)data, size);
	pthread_mutex_unlock(&g_mu);
	GetWeather();

	// Weather-sensor JSON response (NUL-terminated, as the HTTP reader delivers it).
	std::vector<char> s(size + 3, 0);
	memcpy(s.data(), data, size);
	weather_sensor_parse_response(s.data(), 0x7, 0xffff);
	// Weather-options string: callers load it at buffer+1, leaving room for the '{' and '}'
	// parse_wto wraps it in (OpenSprinkler.cpp iopts_load, api/server.cpp /co?wto=).
	if (!memchr(data, 0, size)) {
		std::fill(s.begin(), s.end(), 0);
		memcpy(s.data() + 1, data, size);
		parse_wto(s.data());
	}
	return 0;
}
