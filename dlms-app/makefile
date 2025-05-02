
# Where is the sdk checkouted (submodule)
SDK_PATH := SDK
# Where is gurux lib checkouted (submodule)
DLMS_LIB:= DLMS_LIB
# How to go back to the ROOT directory (the build pathes will be relative to the SDK directory)
RELATIVE_ROOT_PATH := ..

# Apply required patch(s)
PATCH_PATH := patch
sdk_patch_filename?=sdk.patch

target_board?=radientum_wp_v1_0
app_name?=dlms_app
board_folder?=board/


# Specify absolute custom directory
build_directory=build/

.DEFAULT_GOAL := all

SHORT_SHA1 := $(shell git log -1 --pretty=format:"%H" | cut -c1-8)

.DEFAULT:
	+make -C $(SDK_PATH) -f makefile $@ \
		target_board=$(target_board) \
		build_directory=$(RELATIVE_ROOT_PATH)/$(build_directory) \
		app_name=$(app_name) \
		app_extra_folder=$(RELATIVE_ROOT_PATH) \
		board_directory=$(RELATIVE_ROOT_PATH)/$(board_folder) \
		SHORT_SHA1=$(SHORT_SHA1)

install:
	+git submodule init
	+git submodule update
	# Copy binaries if available
	+cp wirepas_binaries/* $(SDK_PATH)/image 2>/dev/null || :
	# Apply required patch(s) to SDK
	+git apply $(PATCH_PATH)/$(sdk_patch_filename) --directory $(SDK_PATH)
