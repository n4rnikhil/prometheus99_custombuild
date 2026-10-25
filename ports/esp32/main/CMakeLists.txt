# The HMI runtime is the same C99 code as on Windows and Linux; only main_esp32.c is ESP-specific.
set(R "../../..")

idf_component_register(
    SRCS
        "main_esp32.c"
        "${R}/src/app.c"
        "${R}/src/core/sha256.c" "${R}/src/core/tags.c" "${R}/src/core/alarms.c" "${R}/src/core/alarm_log.c"
        "${R}/src/core/watchdog.c" "${R}/src/core/security.c" "${R}/src/core/crc32.c"
        "${R}/src/renderer/micro_renderer.c" "${R}/src/renderer/layout.c" "${R}/src/renderer/font_inter.c"
        "${R}/src/drivers/device_profiles.c" "${R}/src/drivers/profiles/profile_generic.c"
        "${R}/src/drivers/profiles/profile_tank_skid.c" "${R}/src/drivers/profiles/profile_drive.c"
        "${R}/src/drivers/profiles/profile_conveyor.c" "${R}/src/drivers/profiles/profile_boiler.c"
        "${R}/src/drivers/profiles/profile_pump_station.c" "${R}/src/drivers/profiles/profile_compressor.c"
        "${R}/src/drivers/profiles/profile_ahu.c" "${R}/src/drivers/profiles/profile_abb_drive.c"
        "${R}/src/drivers/plc_sim.c"
        "${R}/src/drivers/device_manager.c" "${R}/src/drivers/discovery.c" "${R}/src/drivers/modbus_proto.c"
        "${R}/src/drivers/driver_host.c"
        "${R}/src/hal/net_ip.c" "${R}/src/hal/net_posix.c" "${R}/src/hal/timer_posix.c" "${R}/src/hal/sys_posix.c"
        "${R}/src/hal/fs_stdio.c" "${R}/src/hal/thread_posix.c" "${R}/src/hal/gesture.c"
    INCLUDE_DIRS "${R}/include" "."
    REQUIRES esp_lcd esp_wifi esp_netif esp_event nvs_flash driver lwip pthread esp_hw_support heap
)

# Small-target profile (include/profile_mcu.h). To drop a driver module, remove its source above
# and add HMI_DRIVER_DRIVE=0 or HMI_DRIVER_TANK_SKID=0 here.
target_compile_definitions(${COMPONENT_LIB} PRIVATE HMI_PROFILE_MCU HMI_BUILD_ID="esp32s3")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
