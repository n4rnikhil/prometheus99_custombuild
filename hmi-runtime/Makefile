# ===========================================================================
#  Prometheus HMI Runtime - Linux / Raspberry Pi build (GNU make, any C99 compiler)
#
#    make                      industrial_hmi (desktop window via libX11 at run time, or framebuffer + evdev)
#    make test                 every suite: unit, MCU profile, integration, Modbus faults,
#                              all under AddressSanitizer + UBSan
#    make test-fast            unit tests only, no sanitizers
#    make STATIC=1             fully static binary (copy one file to the target)
#    make CC=aarch64-linux-gnu-gcc     cross-compile for Raspberry Pi 3/4/5 (64-bit OS)
#    make CC=arm-linux-gnueabihf-gcc   cross-compile for Raspberry Pi Zero/2/3 (32-bit OS)
#    make clean
#
#  Windows builds use build.bat; ESP32 uses ports/esp32 (ESP-IDF).
# ===========================================================================

CC      ?= cc
BUILD_ID := $(shell date +%Y%m%d.%H%M%S 2>/dev/null || echo dev)

WARN    := -std=c99 -Wall -Wextra -Wshadow -Werror
DEFS    := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
OPT     := -Os -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
LIBS    := -lpthread -ldl
ifeq ($(STATIC),1)
LDFLAGS += -static
endif

CORE_SRC := src/core/sha256.c src/core/tags.c src/core/alarms.c src/core/alarm_log.c \
            src/core/watchdog.c src/core/security.c src/core/crc32.c src/core/device_key.c \
            src/renderer/micro_renderer.c src/renderer/layout.c src/renderer/font_inter.c \
            src/devices/device_registry.c src/devices/altivar_drive.c src/devices/em6400_meter.c \
            src/devices/tank_skid.c src/devices/conveyor.c src/devices/boiler.c \
            src/devices/pump_station.c src/devices/compressor.c src/devices/air_handler.c \
            src/devices/abb_drive.c src/devices/generic.c \
            src/fieldbus/device_manager.c src/fieldbus/discovery.c src/fieldbus/modbus_proto.c \
            src/fieldbus/driver_host.c
HAL_SRC  := src/hal/net_ip.c src/hal/net_posix.c src/hal/timer_posix.c src/hal/sys_posix.c \
            src/hal/fs_stdio.c src/hal/thread_posix.c src/hal/gesture.c src/hal/utc_format.c src/hal/alarm_tone.c
APP_SRC  := src/app/app.c src/platform/linux/main_linux.c src/platform/linux/x11_window.c src/platform/linux/alsa_sound.c

SAN     := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
NET_TEST_SRC := src/hal/net_ip.c src/hal/net_posix.c src/hal/timer_posix.c \
            src/fieldbus/device_manager.c src/fieldbus/discovery.c src/fieldbus/modbus_proto.c \
            $(filter src/devices/%,$(CORE_SRC)) src/core/tags.c src/core/alarms.c src/core/crc32.c

.PHONY: all test test-fast clean

all: industrial_hmi

industrial_hmi: $(APP_SRC) $(CORE_SRC) $(HAL_SRC)
	$(CC) $(WARN) $(OPT) $(DEFS) -DHMI_BUILD_ID=\"$(BUILD_ID)-linux\" -Iinclude $^ -o $@ $(LDFLAGS) $(LIBS)
	@ls -l $@ | awk '{print "[OK] " $$NF "  " $$5 " bytes"}'

tests/test_core: tests/test_core.c $(CORE_SRC) $(HAL_SRC)
	$(CC) $(WARN) -O1 -g $(DEFS) -Iinclude $^ -o $@ $(LIBS)

tests/test_core_san: tests/test_core.c $(CORE_SRC) $(HAL_SRC)
	$(CC) $(WARN) -O1 -g $(SAN) $(DEFS) -Iinclude $^ -o $@ $(LIBS)

tests/test_core_mcu: tests/test_core.c $(CORE_SRC) $(HAL_SRC)
	$(CC) $(WARN) -O1 -g $(SAN) $(DEFS) -DHMI_PROFILE_MCU -Iinclude $^ -o $@ $(LIBS)

tests/test_integration: tests/test_integration.c tests/fixtures/device_models.c $(NET_TEST_SRC)
	$(CC) $(WARN) -O1 -g $(SAN) $(DEFS) -Iinclude -Itests $^ -o $@ $(LIBS)

tests/test_modbus_faults: tests/test_modbus_faults.c $(NET_TEST_SRC)
	$(CC) $(WARN) -O1 -g $(SAN) $(DEFS) -Iinclude $^ -o $@ $(LIBS)

test-fast: tests/test_core
	./tests/test_core

test: industrial_hmi tests/test_core_san tests/test_core_mcu tests/test_integration tests/test_modbus_faults
	./tests/test_core_san
	./tests/test_core_mcu
	./tests/test_integration
	./tests/test_modbus_faults

clean:
	rm -f industrial_hmi tests/test_core tests/test_core_san tests/test_core_mcu \
	      tests/test_integration tests/test_modbus_faults hmi_test_*.log* hmi_test_fs_*.bin
