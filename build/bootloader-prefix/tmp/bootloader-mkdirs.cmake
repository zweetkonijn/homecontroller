# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "C:/esp/esp-idf-v5.3.1/components/bootloader/subproject"
  "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader"
  "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader-prefix"
  "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader-prefix/tmp"
  "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader-prefix/src/bootloader-stamp"
  "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader-prefix/src"
  "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "C:/esp/esp-zigbee-sdk/examples/sleepy_devices/light_sleep_end_device/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
