LOCAL_PATH := $(call my-dir)

# Forcefully disable PIE globally. This makes it possible to
# build some binaries without PIE by adding the necessary flags
# manually. These will not get reset by $(CLEAR_VARS).
TARGET_PIE := false
NDK_APP_PIE := false

include $(CLEAR_VARS)

# Enable PIE manually. Will get reset on $(CLEAR_VARS).
LOCAL_CFLAGS += -fPIE
LOCAL_LDFLAGS += -fPIE -pie

LOCAL_MODULE := minitouch

LOCAL_SRC_FILES := \
	minitouch.c \

LOCAL_STATIC_LIBRARIES := libevdev

include $(BUILD_EXECUTABLE)

include $(CLEAR_VARS)

LOCAL_MODULE := minitouch-nopie

LOCAL_SRC_FILES := \
	minitouch.c \

LOCAL_STATIC_LIBRARIES := libevdev

include $(BUILD_EXECUTABLE)
