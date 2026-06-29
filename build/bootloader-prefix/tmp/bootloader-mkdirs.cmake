# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "E:/Tools/ESP_IDF/Espressif/frameworks/esp-idf-v5.3.1/components/bootloader/subproject"
  "E:/Work/CW/CW_project/build/bootloader"
  "E:/Work/CW/CW_project/build/bootloader-prefix"
  "E:/Work/CW/CW_project/build/bootloader-prefix/tmp"
  "E:/Work/CW/CW_project/build/bootloader-prefix/src/bootloader-stamp"
  "E:/Work/CW/CW_project/build/bootloader-prefix/src"
  "E:/Work/CW/CW_project/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "E:/Work/CW/CW_project/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "E:/Work/CW/CW_project/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
