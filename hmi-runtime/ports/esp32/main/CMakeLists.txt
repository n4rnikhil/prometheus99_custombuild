# The HMI runtime is the same C99 code as on Windows and Linux; only main_esp32.c is ESP-specific.
set(R "../../..")

idf_component_register(
    SRCS
        "main_esp32.c"
        "${R}/src/app/app.c"
        "${R}/src/core/sha256.c" "${R}/src/core/tags.c" "${R}/src/core/alarms.c" "${R}/src/core/alarm_log.c"
        "${R}/src/core/watchdog.c" "${R}/src/core/security.c" "${R}/src/core/crc32.c" "${R}/src/core/device_key.c"
        "${R}/src/renderer/micro_renderer.c" "${R}/src/renderer/layout.c" "${R}/src/renderer/font_inter.c"
        "${R}/src/devices/device_registry.c" "${R}/src/devices/generic.c"
        "${R}/src/devices/altivar_drive.c" "${R}/src/devices/em6400_meter.c"
        "${R}/src/devices/tank_skid.c" "${R}/src/devices/conveyor.c" "${R}/src/devices/boiler.c"
        "${R}/src/devices/pump_station.c" "${R}/src/devices/compressor.c"
        "${R}/src/devices/air_handler.c" "${R}/src/devices/abb_drive.c"
        "${R}/src/fieldbus/device_manager.c" "${R}/src/fieldbus/discovery.c" "${R}/src/fieldbus/modbus_proto.c"
        "${R}/src/fieldbus/driver_host.c"
        "${R}/src/hal/net_ip.c" "${R}/src/hal/net_posix.c" "${R}/src/hal/timer_posix.c" "${R}/src/hal/sys_posix.c"
        "${R}/src/hal/fs_stdio.c" "${R}/src/hal/thread_posix.c" "${R}/src/hal/gesture.c" "${R}/src/hal/utc_format.c"
    INCLUDE_DIRS "${R}/include" "."
    REQUIRES esp_lcd esp_wifi esp_eth esp_netif esp_event nvs_flash driver lwip pthread esp_hw_support esp_psram heap
)

# Small-target profile (include/profile_mcu.h). To drop a driver module, remove its source above
# and add e.g. HMI_DRIVER_ALTIVAR=0 or HMI_DRIVER_TANK_SKID=0 here.
target_compile_definitions(${COMPONENT_LIB} PRIVATE HMI_PROFILE_MCU HMI_BUILD_ID="esp32s3")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow -Werror)
