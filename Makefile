#V := 1
PROJECT_NAME := temphumid-sensor

EXTRA_COMPONENT_DIRS := $(CURDIR)/esp-idf-lib/components
EXCLUDE_COMPONENTS := max7219 mcp23x17 led_strip max31865 ls7366r ads111x ads130e08 max31855

nvs.bin: nvs.csv web/index.html web/done.html
	$(IDF_PATH)/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py generate nvs.csv nvs.bin 12288

# esptool.py --chip esp8266 -p /dev/ttyUSB0 -b 460800 write_flash --flash_mode dio --flash_freq 40m --flash_size 2MB 0x8000 partition_table/partition-table.bin 0x0 bootloader/bootloader.bin 0x10000 ethermine.bin
nvs: nvs.bin
	${IDF_PATH}/components/partition_table/parttool.py -b 460800 --port ${ESPPORT} write_partition --partition-name=nvs --input "nvs.bin"

include $(IDF_PATH)/make/project.mk
