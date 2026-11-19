#!/usr/bin/env bash
#
# mayhem/build.sh — build the OpenSprinkler fuzz harnesses and the upstream unit tests.
#
# Runs inside the commit image as `mayhem` in /mayhem (re-runnable, offline). The base image
# provides CC/CXX/SANITIZER_FLAGS/LIB_FUZZING_ENGINE/STANDALONE_FUZZ_MAIN.
#
#  1. The whole Linux DEMO firmware (upstream Makefile, VERSION=DEMO) compiled with the sanitizers
#     and DWARF-3, objects in place (upstream's *.o, gitignored). main.cpp is compiled separately
#     with -Dmain=opensprinkler_main so the harnesses can call do_setup().
#  2. Each harness in mayhem/harnesses linked twice: libFuzzer binary + standalone reproducer,
#     into mayhem/out/.
#  3. The upstream C++ unit tests (the Makefile's test-* recipes) with normal flags, into
#     mayhem/out/tests/, for mayhem/test.sh to run.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

SRC="${SRC:-/mayhem}"
cd "$SRC"
OUT="$SRC/mayhem/out"
mkdir -p "$OUT/obj" "$OUT/tests"

# SanCov for the firmware objects only when building with libFuzzer.
COV_FLAGS=""
case "$LIB_FUZZING_ENGINE" in *fuzzer*) COV_FLAGS="-fsanitize=fuzzer-no-link" ;; esac
FUZZ_FLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS $COV_FLAGS -DHTTP_PORT=0"

# Same flags as upstream's Makefile CXXFLAGS for VERSION=DEMO.
OS_CXXFLAGS=(-std=gnu++14 -DDEMO -DSMTP_OPENSSL -Wall -I. -include string.h -include cstdint
	-Iexternal/TinyWebsockets/tiny_websockets_lib/include
	-Iexternal/OpenThings-Framework-Firmware-Library/)
OS_LIBS=(-lpthread -lmosquitto -lssl -lcrypto)

# ---- 1) firmware objects (Makefile SOURCES, minus main.cpp) --------------------------------------
shopt -s nullglob
sources=(OpenSprinkler.cpp api/*.cpp boards/*.cpp core/*.cpp drivers/*.cpp platform/*.cpp
	sensors/*.cpp storage/*.cpp util/*.cpp
	external/TinyWebsockets/tiny_websockets_lib/src/*.cpp
	external/OpenThings-Framework-Firmware-Library/*.cpp)
for s in services/*.cpp services/*.c; do
	case "$s" in services/EMailSender.cpp|services/espconnect.cpp) ;; *) sources+=("$s") ;; esac
done
shopt -u nullglob
objects=()
for s in "${sources[@]}"; do objects+=("${s%.*}.o"); done

# The flags are recorded so a re-run with different flags rebuilds instead of reusing objects.
stamp="$OUT/obj/flags.stamp"
if [ "$(cat "$stamp" 2>/dev/null)" != "$CXX $CC $FUZZ_FLAGS" ]; then
	rm -f "${objects[@]}" "$OUT"/obj/*.o
	printf '%s' "$CXX $CC $FUZZ_FLAGS" > "$stamp"
fi
make -j"$MAYHEM_JOBS" VERSION=DEMO CXX="$CXX" CC="$CC" \
	EXTRA_CXXFLAGS="$FUZZ_FLAGS -Wno-mismatched-tags" \
	CFLAGS="$FUZZ_FLAGS -DSMTP_OPENSSL -I." "${objects[@]}"

"$CXX" "${OS_CXXFLAGS[@]}" $FUZZ_FLAGS -Wno-mismatched-tags -Dmain=opensprinkler_main \
	-c main.cpp -o "$OUT/obj/main_fuzz.o"
rm -f "$OUT/obj/libopensprinkler.a"
ar rcs "$OUT/obj/libopensprinkler.a" "$OUT/obj/main_fuzz.o" "${objects[@]}"

# ---- 2) harnesses ---------------------------------------------------------------------------------
"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS -c mayhem/lsan_off.cc -o "$OUT/obj/lsan_off.o"
"$CC" $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OUT/obj/standalone_main.o"

targets=(fuzz_http_api fuzz_otf_cloud fuzz_mqtt_command fuzz_sprinkler_log fuzz_weather)
for t in "${targets[@]}"; do
	"$CXX" "${OS_CXXFLAGS[@]}" $FUZZ_FLAGS -Wno-mismatched-tags -Imayhem/harnesses \
		-c "mayhem/harnesses/$t.cpp" -o "$OUT/obj/$t.o"
	"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OUT/obj/$t.o" "$OUT/obj/lsan_off.o" \
		"$OUT/obj/libopensprinkler.a" "${OS_LIBS[@]}" -o "$OUT/$t"
	"$CXX" $SANITIZER_FLAGS $DEBUG_FLAGS "$OUT/obj/standalone_main.o" "$OUT/obj/$t.o" "$OUT/obj/lsan_off.o" \
		"$OUT/obj/libopensprinkler.a" "${OS_LIBS[@]}" -o "$OUT/$t-standalone"
done

# ---- 3) upstream unit tests (Makefile test-* recipes), normal flags ------------------------------
T="$OUT/tests"
INC=(-I. -Iexternal/TinyWebsockets/tiny_websockets_lib/include -Iexternal/OpenThings-Framework-Firmware-Library)
GC=(-ffunction-sections -fdata-sections -Wl,--gc-sections)
tc() { local out="$1"; shift; "$CXX" -std=gnu++14 -w $COVERAGE_FLAGS "$@" -o "$T/$out"; }
tc sprinkler_log -DDEMO -DSPRINKLER_LOG_TEST_SMALL_GEOMETRY -I. tests/sprinkler_log_test.cpp \
	storage/sprinkler_log.cpp storage/files.cpp storage/maintenance.cpp
tc storage_files -DDEMO -I. tests/storage_files_test.cpp storage/files.cpp storage/maintenance.cpp \
	storage/sprinkler_log.cpp platform/hostname.cpp
tc station_extra -DDEMO -I. tests/station_extra_test.cpp storage/station_extra.cpp core/cycle_soak.cpp \
	storage/files.cpp storage/maintenance.cpp storage/sprinkler_log.cpp
tc buffer_filler -DDEMO -I. tests/bfiller_test.cpp storage/files.cpp
tc bundle_codec -DDEMO "${INC[@]}" "${GC[@]}" tests/bundle_codec_test.cpp core/bundle.cpp
tc cycle_soak -I. tests/cycle_soak_test.cpp core/cycle_soak.cpp
tc cycle_scheduler -DDEMO "${INC[@]}" "${GC[@]}" tests/cycle_scheduler_test.cpp core/scheduler.cpp \
	core/program.cpp core/cycle_soak.cpp
tc runtime_queue -DDEMO "${INC[@]}" "${GC[@]}" tests/runtime_queue_test.cpp core/program.cpp
tc queue_capacity -DDEMO -DEXPECTED_QUEUE_CAPACITY=255 -I. tests/queue_capacity_test.cpp
tc output_sequencer -I. tests/output_sequencer_test.cpp core/output_sequencer.cpp
tc weather_failsafe -I. tests/weather_failsafe_test.cpp services/weather_failsafe.cpp
tc weather_sensor_cache -DDEMO "${INC[@]}" "${GC[@]}" tests/weather_sensor_cache_test.cpp \
	services/weather.cpp services/weather_failsafe.cpp platform/monotonic_clock.cpp
tc string_buffer -DDEMO "${INC[@]}" "${GC[@]}" tests/string_buffer_test.cpp util/utils.cpp api/http.cpp
tc board_profiles -I. tests/board_profile_test.cpp boards/board_profile.cpp
tc hardware_detection -I. tests/hardware_detection_test.cpp boards/hardware_detection.cpp
tc flow_rate_window -I. tests/flow_rate_window_test.cpp
tc sensor_units -DDEMO "${INC[@]}" "${GC[@]}" tests/sensor_unit_test.cpp sensors/sensor.cpp \
	sensors/weather_sensor.cpp
tc system_internal_sensor -DDEMO "${INC[@]}" "${GC[@]}" tests/system_internal_sensor_test.cpp \
	sensors/system_internal_sensor.cpp sensors/sensor.cpp storage/files.cpp
tc onboard_analog -DDEMO "${INC[@]}" "${GC[@]}" tests/onboard_analog_test.cpp sensors/analog_sensor.cpp \
	sensors/ads1115_sensor.cpp sensors/onboard_analog_sensor.cpp sensors/onboard_digital_sensor.cpp \
	sensors/sensor.cpp services/sensor_ports.cpp boards/board_profile.cpp storage/files.cpp
tc w5500_frame -I. tests/w5500_frame_test.cpp drivers/w5500_frame.cpp
tc ch390_health -I. tests/ch390_health_test.cpp
tc ch390_raw -DARDUINO -DESP8266 -Itests/ch390_stubs -I. tests/ch390_raw_test.cpp drivers/ch390_raw.cpp
tc ethernet_lifecycle -Itests/ethernet_stubs -I. tests/ethernet_lifecycle_test.cpp
tc hostname -DDEMO -I. tests/hostname_test.cpp platform/hostname.cpp
tc update_http_reader -I. tests/update_http_reader_test.cpp

echo "build.sh: built ${targets[*]} and $(ls "$T" | wc -l) unit tests"
