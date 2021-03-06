set(PROTO_LIST
    "${METADEF_DIR}/proto/onnx/ge_onnx.proto"
    "${METADEF_DIR}/proto/om.proto"
)

set(SRC_LIST
    "onnx_custom_parser_adapter.cc"
    "onnx_parser.cc"
    "onnx_data_parser.cc"
    "onnx_util.cc"
    "onnx_constant_parser.cc" 
)

protobuf_generate(ge PROTO_SRCS PROTO_HDRS ${PROTO_LIST})

############ libfmk_onnx_parser.so ############
add_library(fmk_onnx_parser SHARED ${SRC_LIST} ${PROTO_HDRS})

target_compile_options(fmk_onnx_parser PRIVATE
    -Werror
    -Wno-deprecated-declarations
)

target_compile_definitions(fmk_onnx_parser PRIVATE
    PROTOBUF_INLINE_NOT_IN_HEADERS=0
    google=ascend_private
)

target_include_directories(fmk_onnx_parser PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${PARSER_DIR}
    ${PARSER_DIR}/inc
    ${PARSER_DIR}/parser
    ${METADEF_DIR}/inc
    ${METADEF_DIR}/inc/graph
    ${METADEF_DIR}/inc/register
    ${METADEF_DIR}/inc/external
    ${METADEF_DIR}/inc/external/graph
    ${METADEF_DIR}/inc/external/register
    #### temp ####
    ${PARSER_DIR}/../graphengine/inc/common/util
    ${PARSER_DIR}/../graphengine/inc/external
    ${PARSER_DIR}/../graphengine/inc/framework
    ${PARSER_DIR}/../graphengine/inc
    ${PARSER_DIR}/../graphengine/ge
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/proto/ge
    #### blue zone compile #####
    ${PARSER_DIR}/../ge
    ${PARSER_DIR}/../inc
    ${PARSER_DIR}/../inc/common/util
    ${PARSER_DIR}/../inc/framework
    ${PARSER_DIR}/../inc/external
    ${PARSER_DIR}/../third_party/fwkacllib/inc
    #### independent compile #####
    ${METADEF_DIR}/third_party/graphengine/ge
    ${METADEF_DIR}/third_party/graphengine/inc
    ${METADEF_DIR}/third_party/graphengine/inc/framework
    ${METADEF_DIR}/third_party/graphengine/inc/external
    ${METADEF_DIR}/third_party/fwkacllib/inc
)

target_link_libraries(fmk_onnx_parser PRIVATE
    $<BUILD_INTERFACE:intf_pub>
    static_mmpa
    -Wl,--no-as-needed
    ascend_protobuf
    register
    c_sec
    parser_common 
    graph
    slog
    -Wl,--as-needed
    json
    -lrt
    -ldl
)

############ install ############
set(INSTALL_BASE_DIR "")
set(INSTALL_LIBRARY_DIR lib)

install(TARGETS fmk_onnx_parser OPTIONAL
    LIBRARY DESTINATION ${INSTALL_LIBRARY_DIR}
)
