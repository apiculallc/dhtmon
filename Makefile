#V := 1
PROJECT_NAME := temphumid-sensor

EXTRA_COMPONENT_DIRS := $(CURDIR)/../esp-idf-lib/components
EXCLUDE_COMPONENTS := max7219 mcp23x17 led_strip max31865 ls7366r

include $(IDF_PATH)/make/project.mk
