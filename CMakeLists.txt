# The following four lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../esp-idf-lib/components)
set(EXCLUDE_COMPONENTS max7219 mcp23x17 led_strip max31865 ls7366r)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(temphumid-sensor)
