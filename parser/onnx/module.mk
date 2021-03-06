
LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := libfmk_onnx_parser

LOCAL_CFLAGS += -DPROTOBUF_INLINE_NOT_IN_HEADERS=0
LOCAL_CFLAGS += -Werror -Wno-deprecated-declarations -Dgoogle=ascend_private
ifeq ($(DEBUG), 1)
LOCAL_CFLAGS += -g -O0
endif

PARSER_ONNX_SRC_FILES := \
    onnx_custom_parser_adapter.cc \
    onnx_parser.cc \
    onnx_data_parser.cc \
    onnx_util.cc \
    onnx_constant_parser.cc \
    proto/onnx/ge_onnx.proto \
    proto/om.proto \

LOCAL_SRC_FILES := $(PARSER_ONNX_SRC_FILES)

LOCAL_C_INCLUDES := \
    $(LOCAL_PATH) \
    $(LOCAL_PATH)/../../ \
    $(TOPDIR)inc \
    $(TOPDIR)metadef/inc \
    $(TOPDIR)graphengine/inc \
    $(TOPDIR)parser/inc \
    $(TOPDIR)inc/external \
    $(TOPDIR)metadef/inc/external \
    $(TOPDIR)graphengine/inc/external \
    $(TOPDIR)parser/inc/external \
    $(TOPDIR)metadef/inc/external/graph \
    $(TOPDIR)graphengine/inc/framework \
    $(TOPDIR)parser \
    $(TOPDIR)parser/parser \
    $(TOPDIR)graphengine/ge \
    libc_sec/include \
    third_party/protobuf/include \
    third_party/json/include \
    third_party/openssl/include/x86/include \

LOCAL_SHARED_LIBRARIES := \
    libascend_protobuf \
    libslog \
    libc_sec \
    libparser_common \
    libgraph \
    libregister \

LOCAL_STATIC_LIBRARIES += libmmpa   

LOCAL_LDFLAGS := -lrt -ldl

include $(BUILD_HOST_SHARED_LIBRARY)
