# ===========================================================================
#  PLC Simulator - Linux / Raspberry Pi build (GNU make, any C99 compiler)
#
#    make            plc_simulator_cli (headless: devices on the network, log on stdout)
#    make test       build and run the device-model tests
#    make CC=aarch64-linux-gnu-gcc   cross-compile for a 64-bit Raspberry Pi OS
#
#  The window version (plc_simulator.exe) is Windows only: build.bat.
# ===========================================================================

CC      ?= cc
WARN    := -std=c99 -Wall -Wextra -Wshadow -Werror
DEFS    := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CORE    := src/plc_models.c src/modbus_server.c

.PHONY: all test clean

all: plc_simulator_cli

plc_simulator_cli: src/main_console.c $(CORE)
	$(CC) $(WARN) -Os $(DEFS) -Iinclude $^ -o $@

tests/test_plc_models: tests/test_plc_models.c src/plc_models.c
	$(CC) $(WARN) -O1 -g $(DEFS) -Iinclude $^ -o $@

test: tests/test_plc_models
	./tests/test_plc_models

clean:
	rm -f plc_simulator_cli tests/test_plc_models
