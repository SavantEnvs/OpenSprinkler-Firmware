// Shared set-up for the whole-firmware OpenSprinkler harnesses (Linux DEMO build).
//
// The firmware is linked as-is (main.cpp compiled with -Dmain=opensprinkler_main) and booted once
// with do_setup() against a private data directory under $TMPDIR. Password checking is turned off
// through the firmware's own option (IOPT_IGNORE_PASSWORD) so the fuzzer reaches the handlers.
//
// Per-input isolation: the firmware keeps its configuration in files in the data directory and
// mirrors part of it in memory. os_fuzz_reset() restores every file in the data directory to the
// snapshot taken right after boot (deleting files an input created), and reloads the in-memory
// copies the firmware itself loads at boot (integer options, non-volatile data, station attributes,
// programs, sensors, sprinkler-log cache, weather state) plus the controller status struct, so each
// input starts from the same state. Process globals a handler can write are snapshotted after boot
// and restored too: the notification queue (emptied with NotifQueue::clear(); the harness never runs
// the main loop that drains it), reboot_timer, the flow-sensor counters, curr_alert_sid,
// currpoll_timeout and the weather globals (wt_monthly, md_scales, wt_rawData).
// Kept across inputs on purpose: the OTF route table and the mosquitto library init (both set up
// once at boot and never changed by a request), and the scratch buffers tmp_buffer/ether_buffer
// (each use writes them before reading).
#pragma once

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

#include "OpenSprinkler.h"
#include "core/program.h"
#include "sensors/sensor.h"
#include "storage/files.h"
#include "storage/sprinkler_log.h"
#include "services/notifier.h"
#include "services/weather.h"

extern OpenSprinkler os;
extern ProgramData pd;
void do_setup();
extern uint32_t flow_begin, flow_start, flow_stop, flow_gallons, flow_count;
extern float flow_last_gpm;
extern uint32_t reboot_timer;
extern unsigned char curr_alert_sid;
extern uint32_t currpoll_timeout;
extern char wt_rawData[];

namespace osfuzz {

static std::string g_dir;
static std::vector<std::pair<std::string, std::string>> g_files;  // relative path -> contents
static std::vector<std::string> g_dirs;                            // relative dirs
static ConStatus g_status;

// Post-boot values of the process globals (main.cpp, services/weather.cpp) a handler can write.
struct Globals {
	uint32_t flow_begin, flow_start, flow_stop, flow_gallons, flow_count;
	float flow_last_gpm;
	uint32_t reboot_timer;
	unsigned char curr_alert_sid;
	uint32_t currpoll_timeout;
	unsigned char wt_monthly[12];
	unsigned char md_scales[MAX_N_MD_SCALES];
	char wt_rawData[TMP_BUFFER_SIZE];
};
static Globals g_globals;

static void save_globals(Globals &g) {
	g.flow_begin = flow_begin; g.flow_start = flow_start; g.flow_stop = flow_stop;
	g.flow_gallons = flow_gallons; g.flow_count = flow_count; g.flow_last_gpm = flow_last_gpm;
	g.reboot_timer = reboot_timer;
	g.curr_alert_sid = curr_alert_sid;
	g.currpoll_timeout = currpoll_timeout;
	memcpy(g.wt_monthly, wt_monthly, sizeof(g.wt_monthly));
	memcpy(g.md_scales, md_scales, sizeof(g.md_scales));
	memcpy(g.wt_rawData, wt_rawData, sizeof(g.wt_rawData));
}

static void restore_globals(const Globals &g) {
	flow_begin = g.flow_begin; flow_start = g.flow_start; flow_stop = g.flow_stop;
	flow_gallons = g.flow_gallons; flow_count = g.flow_count; flow_last_gpm = g.flow_last_gpm;
	reboot_timer = g.reboot_timer;
	curr_alert_sid = g.curr_alert_sid;
	currpoll_timeout = g.currpoll_timeout;
	memcpy(wt_monthly, g.wt_monthly, sizeof(g.wt_monthly));
	memcpy(md_scales, g.md_scales, sizeof(g.md_scales));
	memcpy(wt_rawData, g.wt_rawData, sizeof(g.wt_rawData));
}

static void walk(const std::string &rel, std::vector<std::string> &files, std::vector<std::string> &dirs) {
	std::string full = g_dir + "/" + rel;
	DIR *d = opendir(full.c_str());
	if (!d) return;
	while (struct dirent *e = readdir(d)) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		std::string r = rel.empty() ? std::string(e->d_name) : rel + "/" + e->d_name;
		struct stat st;
		if (lstat((g_dir + "/" + r).c_str(), &st) != 0) continue;
		if (S_ISDIR(st.st_mode)) {
			dirs.push_back(r);
			walk(r, files, dirs);
		} else {
			files.push_back(r);
		}
	}
	closedir(d);
}

static std::string slurp(const std::string &path) {
	std::string out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
	fclose(f);
	return out;
}

static void spit(const std::string &path, const std::string &data) {
	FILE *f = fopen(path.c_str(), "wb");
	if (!f) return;
	if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
	fclose(f);
}

static const std::string &data_dir() { return g_dir; }

static void remove_tree() {
	std::vector<std::string> files, dirs;
	walk("", files, dirs);
	for (auto &r : files) unlink((g_dir + "/" + r).c_str());
	for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) rmdir((g_dir + "/" + *it).c_str());
}

static void cleanup_at_exit() {
	if (g_dir.empty()) return;
	remove_tree();
	rmdir(g_dir.c_str());
}

// Boot the firmware once against a fresh data directory (removed again at exit).
static void init() {
	if (!g_dir.empty()) return;
	const char *tmp = getenv("TMPDIR");
	std::string templ = std::string(tmp && *tmp ? tmp : "/tmp") + "/osfuzz.XXXXXX";
	std::vector<char> buf(templ.begin(), templ.end());
	buf.push_back(0);
	if (!mkdtemp(buf.data())) { perror("mkdtemp"); abort(); }
	g_dir = buf.data();
	atexit(cleanup_at_exit);
	static std::string dir_slash = g_dir + "/";
	set_data_dir(dir_slash.c_str());

	// Silence the firmware's chatty stdout so libFuzzer's output stays readable.
	if (!getenv("OSFUZZ_VERBOSE")) {
		if (!freopen("/dev/null", "w", stdout)) {}
	}

	do_setup();
	os.iopts[IOPT_IGNORE_PASSWORD] = 1;
	os.iopts_save();

	std::vector<std::string> files;
	walk("", files, g_dirs);
	for (auto &r : files) g_files.emplace_back(r, slurp(g_dir + "/" + r));
	g_status = os.status;
	NotifQueue::clear();
	save_globals(g_globals);
}

// Restore the data directory and the firmware's loaded state to the post-boot snapshot.
static void reset() {
	remove_tree();
	for (auto &d : g_dirs) mkdir((g_dir + "/" + d).c_str(), 0700);
	for (auto &f : g_files) spit(g_dir + "/" + f.first, f.second);

	os.status = g_status;
	NotifQueue::clear();
	restore_globals(g_globals);
	os.iopts_load();
	os.nvdata_load();
	os.attribs_load();
	os.clear_all_station_bits();
	pd.init();
	Sensor::load_all();
	sprinkler_log_invalidate_cache();
	md_N = 0;
	mda = 0;
	wt_errCode = HTTP_RQT_NOT_RECEIVED;
	wt_restricted = 0;
	weather_sensor_reset_cache();
	weather_response_reset();
}

}  // namespace osfuzz
