// Fuzz the on-disk sprinkler log ring (storage/sprinkler_log.cpp): the ring index file
// logs/spr.hdr and the segment files logs/spr.logNNN are read back on every boot and every log
// query (/jl), so a corrupted or crafted flash/SD image goes through the index decode, segment
// scan, recovery/rebuild, cursor iteration, append/rotation and delete paths.
//
// Input container (little endian):
//   u8  nseg                      number of segment files that follow (low 3 bits)
//   u16 len, len bytes            contents of logs/spr.hdr
//   nseg x { u8 slot, u16 len, len bytes }   contents of logs/spr.log<slot>
// A truncated container simply ends early (the remaining files are absent).
#include "os_fuzz_env.h"

namespace {

struct Reader {
	const uint8_t *p;
	size_t n;
	bool u8(uint8_t &v) { if (n < 1) return false; v = *p++; n--; return true; }
	bool u16(uint16_t &v) { if (n < 2) return false; v = (uint16_t)(p[0] | (p[1] << 8)); p += 2; n -= 2; return true; }
	std::string bytes(uint16_t len) { size_t k = len < n ? len : n; std::string s((const char *)p, k); p += k; n -= k; return s; }
};

bool visit(const SprinklerLogRecord &record, bool live, void *context) {
	uint32_t *acc = static_cast<uint32_t *>(context);
	*acc += record.timestamp ^ record.value ^ record.aux ^ record.type ^ (live ? 1u : 0u);
	return true;
}

void query_all(uint32_t start_day, uint32_t end_day) {
	SprinklerLogCursor cursor = sprinkler_log_cursor_begin();
	uint32_t acc = 0;
	for (int page = 0; page < 64; page++) {
		SprinklerLogCursor next = cursor;
		uint32_t scanned = 0;
		bool done = true;
		if (!sprinkler_log_query(start_day, end_day, cursor, 100, visit, &acc, next, scanned, done)) break;
		if (done) break;
		char text[32];
		sprinkler_log_cursor_format(next, text, sizeof(text));
		if (!sprinkler_log_cursor_parse(text, cursor)) break;
	}
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	osfuzz::init();
	osfuzz::reset();

	Reader r{data, size};
	uint8_t nseg = 0;
	if (!r.u8(nseg)) return 0;
	nseg &= 7;
	const std::string logs = osfuzz::data_dir() + "/" + LOG_DIR;
	mkdir(logs.c_str(), 0700);
	uint16_t len = 0;
	if (r.u16(len)) osfuzz::spit(logs + "spr.hdr", r.bytes(len));
	for (uint8_t i = 0; i < nseg; i++) {
		uint8_t slot = 0;
		if (!r.u8(slot) || !r.u16(len)) break;
		char name[32];
		snprintf(name, sizeof(name), "spr.log%03u", slot);
		osfuzz::spit(logs + name, r.bytes(len));
	}

	sprinkler_log_invalidate_cache();
	sprinkler_log_prepare();
	sprinkler_log_exists();
	query_all(0, UINT32_MAX);

	SprinklerLogRecord rec = {};
	rec.timestamp = 1700000000u;
	rec.value = 600;
	rec.type = 1;
	rec.station = 1;
	sprinkler_log_append(rec);
	query_all(1700000000u / 86400u, UINT32_MAX);
	sprinkler_log_delete_day(1700000000u / 86400u);
	sprinkler_log_delete_before(19000);
	sprinkler_log_remove_oldest_completed();
	sprinkler_log_allocated_bytes(4096);
	query_all(0, UINT32_MAX);
	return 0;
}
