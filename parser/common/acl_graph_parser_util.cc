/**
 * Copyright 2020 Huawei Technologies Co., Ltd

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 * http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include "parser/common/acl_graph_parser_util.h"

#include <dlfcn.h>
#include <regex.h>

#include <cstdlib>
#include <ctime>
#include <fstream>

#include "common/debug/log.h"
#include "common/op/ge_op_utils.h"
#include "common/string_util.h"
#include "common/types.h"
#include "common/util.h"
#include "common/util/error_manager/error_manager.h"
#include "external/ge/ge_api_types.h"
#include "framework/common/debug/ge_log.h"
#include "framework/omg/parser/parser_types.h"
#include "ge/ge_api_types.h"
#include "google/protobuf/io/coded_stream.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "graph/opsproto_manager.h"
#include "graph/utils/type_utils.h"
#include "omg/parser/parser_inner_ctx.h"
#include "parser/common/register_tbe.h"
#include "tbe_plugin_loader.h"

using google::protobuf::io::CodedInputStream;
using google::protobuf::io::FileInputStream;
using google::protobuf::io::ZeroCopyInputStream;
using namespace ge::parser;

namespace {
static std::map<std::string, domiTensorFormat_t> kInputFormatStrToGeformat = {
    {"ND", domi::DOMI_TENSOR_ND},       {"NCHW", domi::DOMI_TENSOR_NCHW},       {"NHWC", domi::DOMI_TENSOR_NHWC},
    {"CHWN", domi::DOMI_TENSOR_CHWN},   {"NC1HWC0", domi::DOMI_TENSOR_NC1HWC0}, {"NHWC1C0", domi::DOMI_TENSOR_NHWC1C0},
    {"NCDHW", domi::DOMI_TENSOR_NCDHW}, {"NDHWC", domi::DOMI_TENSOR_NDHWC}};

// datatype/formats from user to GE, Unified to util interface file later
const std::map<std::string, ge::DataType> kOutputTypeSupportDatatype = {
    {"FP32", ge::DT_FLOAT}, {"FP16", ge::DT_FLOAT16}, {"UINT8", ge::DT_UINT8}};
const char *const kOutputTypeSupport = "only support FP32, FP16, UINT8";
const char *const kInputShapeSample1 = "\"input_name1:n1,c1,h1,w1\"";
const char *const kInputShapeSample2 = "\"input_name1:1,3,224,224\"";
const char *const kSplitError1 = "size not equal to 2 split by \":\"";
const char *const kEmptyError = "can not be empty";
const char *const kFloatNumError = "exist float number";
const char *const kDigitError = "is not digit";
const std::string kGraphDefaultName = "domi_default";
const char *const kOutputTypeSample = "correct sample is \"opname:index:dtype\"";
const char *const kOutputTypeError = "The multiple out nodes set in output_type must be found in out_nodes.";
static std::set<std::string> kCaffeSupportInputFormatSet = {"NCHW", "ND"};
static std::set<std::string> kTfSupportInputFormatSet = {"NCHW", "NHWC", "ND", "NCDHW", "NDHWC"};
const char *const kCaffeFormatSupport = "only support NCHW, ND in Caffe model";
const char *const kTFFormatSupport = "only support NCHW, NHWC, ND, NCDHW, NDHWC in TF model";
/// The maximum length of the file.
/// Based on the security coding specification and the current actual (protobuf) model size, it is determined as 2G-1
const int kMaxFileSizeLimit = INT_MAX;
const int kMaxBuffSize = 256;
const int kProtoReadBytesLimit = INT_MAX;    // Max size of 2 GB minus 1 byte.
const int kWarningThreshold = 536870912 * 2; // 536870912 represent 512M
const int kOutputTypeNode = 0;
const int kOutputTypeIndex = 1;
const int kOutputTypeDataType = 2;

vector<string> SplitInputShape(const std::string &input_shape) {
  vector<string> shape_pair_vec;
  size_t pos = input_shape.rfind(":");
  if (pos != std::string::npos) {
    shape_pair_vec.emplace_back(input_shape.substr(0, pos));
    shape_pair_vec.emplace_back(input_shape.substr(pos + 1, input_shape.size() - pos));
  }
  return shape_pair_vec;
}

static string GetSoPath() {
  Dl_info dl_info;
  if (dladdr(reinterpret_cast<void *>(&GetSoPath), &dl_info) == 0) {
    GELOGW("Failed to read so_path!");
    return string();
  } else {
    std::string so_path = dl_info.dli_fname;
    char path[PATH_MAX] = {0};
    if (so_path.length() >= PATH_MAX) {
      GELOGW("File path is too long!");
      return string();
    }
    if (realpath(so_path.c_str(), path) == nullptr) {
      GELOGW("Failed to get realpath of %s", so_path.c_str());
      return string();
    }

    so_path = path;
    so_path = so_path.substr(0, so_path.rfind('/') + 1);
    return so_path;
  }
}

static void GetOpsProtoPath(string &opsproto_path) {
  GELOGD("Start to get ops proto path schedule.");
  const char *path_env = std::getenv("ASCEND_OPP_PATH");
  if (path_env != nullptr) {
    string path = path_env;
    string file_path = ge::parser::RealPath(path.c_str());
    if (file_path.empty()) {
      GELOGE(ge::FAILED, "File path %s is invalid.", path.c_str());
      return;
    }
    opsproto_path = (path + "/op_proto/custom/" + ":") + (path + "/op_proto/built-in/");
    GELOGI("Get opsproto so path from env : %s", path.c_str());
    return;
  }
  string path_base = GetSoPath();
  GELOGI("path_base is %s", path_base.c_str());
  path_base = path_base.substr(0, path_base.rfind('/'));
  path_base = path_base.substr(0, path_base.rfind('/') + 1);
  opsproto_path = (path_base + "ops/op_proto/custom/" + ":") + (path_base + "ops/op_proto/built-in/");
}

static void GetAclParams(const std::map<ge::AscendString, ge::AscendString> &parser_params, const string &key,
                         string &value) {
  for (auto &ele : parser_params) {
    const char *key_ascend = ele.first.GetString();
    if (key_ascend == nullptr) {
      GELOGW("Input options key is null, Please check!");
      continue;
    }

    string key_str = key_ascend;
    if (key == key_str) {
      const char *value_ascend = ele.second.GetString();
      if (value_ascend == nullptr) {
        value = "";
      } else {
        value = value_ascend;
      }
      return;
    }
  }
  value = "";
  return;
}

static bool CheckDigitStr(std::string &str) {
  for (char c : str) {
    if (!isdigit(c)) {
      GELOGE(domi::FAILED, "Value[%s] is not positive integer", str.c_str());
      return false;
    }
  }
  return true;
}

// Remove the space and tab before and after the string
std::string TrimConf(const std::string &str) {
  if (str.empty()) {
    return str;
  }

  std::string::size_type start = str.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return str;
  }

  std::string::size_type end = str.find_last_not_of(" \t\r\n") + 1;
  return str.substr(start, end);
}

// Parsing the command line
bool ParseSingleLine(const std::string &line, std::map<std::string, std::string> &op_conf_map) {
  std::string temp = TrimConf(line);
  std::string delimiter = ":";
  // Comment or newline returns true directly
  if (temp.find_first_of('#') == 0 || *(temp.c_str()) == '\n') {
    return true;
  }

  if (!temp.empty()) {
    std::string::size_type pos = temp.find_first_of(delimiter);
    if (pos == std::string::npos) {
      GELOGE(ge::PARAM_INVALID, "Incorrect line [%s], it must include [%s].Perhaps you use illegal chinese symbol",
             line.c_str(), delimiter.c_str());
      return false;
    }

    std::string map_key = TrimConf(temp.substr(0, pos));
    std::string value = TrimConf(temp.substr(pos + 1));
    if (map_key.empty() || value.empty()) {
      GELOGE(ge::PARAM_INVALID, "Map_key or value empty. %s", line.c_str());
      return false;
    }

    op_conf_map[map_key] = value;
  }
  return true;
}

} // namespace

namespace ge {
static bool CheckInputTrueOrFalse(const std::string &s, const std::string &atc_param) {
  if ((s == "true") || (s == "false")) {
    return true;
  } else {
    ErrorManager::GetInstance().ATCReportErrMessage("E10005", {"parameter", "value"}, {atc_param, s});
    GELOGE(PARAM_INVALID, "Input parameter[%s]'s value[%s] must be true or false.", atc_param.c_str(), s.c_str());
    return false;
  }
}

static domi::Status CheckOutPutDataTypeSupport(const std::string &output_type) {
  auto it = kOutputTypeSupportDatatype.find(output_type);
  if (it == kOutputTypeSupportDatatype.end()) {
    ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                    {"output_type", output_type, kOutputTypeSupport});
    GELOGE(PARAM_INVALID, "Invalid value for output_type[%s], %s.", output_type.c_str(), kOutputTypeSupport);
    return domi::PARAM_INVALID;
  }
  return domi::SUCCESS;
}

static domi::Status StringToInt(std::string &str, int32_t &value) {
  try {
    if (!CheckDigitStr(str)) {
      GELOGE(PARAM_INVALID, "Invalid of digit string: %s ", str.c_str());
      ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                      {"output_type", str, "is not positive integer"});
      return PARAM_INVALID;
    }
    value = stoi(str);
  } catch (std::invalid_argument &) {
    GELOGE(PARAM_INVALID, "Invalid of digit string: %s, catch invalid_argument.", str.c_str());
    ErrorManager::GetInstance().ATCReportErrMessage("E10014", {"parameter", "value"}, {"output_type", str});
    return PARAM_INVALID;
  } catch (std::out_of_range &) {
    GELOGE(PARAM_INVALID, "Invalid of digit string: %s, catch out_of_range.", str.c_str());
    ErrorManager::GetInstance().ATCReportErrMessage("E10013", {"parameter", "value"}, {"output_type", str});
    return PARAM_INVALID;
  }
  return SUCCESS;
}

static Status CheckOutNode(ge::OpDescPtr op_desc, int32_t index) {
  int32_t out_size = op_desc->GetOutputsSize();
  if (index < 0 || index >= out_size) {
    GELOGE(domi::FAILED,
           "out_node [%s] output index:%d must be smaller "
           "than node output size:%d and can not be negative!",
           op_desc->GetName().c_str(), index, out_size);
    std::string fail_reason = "output index:" + to_string(index) +
                              " must be smaller than output size:" + to_string(out_size) + " and can not be negative!";
    ErrorManager::GetInstance().ATCReportErrMessage("E10003", {"parameter", "value", "reason"},
                                                    {"out_nodes", op_desc->GetName(), fail_reason});
    return domi::FAILED;
  }
  return domi::SUCCESS;
}

domi::Status VerifyOutputTypeAndOutNodes(std::vector<std::string> &out_type_vec) {
  std::vector<std::pair<std::string, int32_t>> user_out_nodes = ge::GetParserContext().user_out_nodes;
  std::set<std::string> out_nodes_info;
  for (uint32_t i = 0; i < user_out_nodes.size(); ++i) {
    // out_nodes set should include output_type and output_format
    std::string tmp = user_out_nodes[i].first + ":" + to_string(user_out_nodes[i].second);
    out_nodes_info.emplace(tmp);
  }
  for (uint32_t i = 0; i < out_type_vec.size(); ++i) {
    if (out_nodes_info.find(out_type_vec[i]) == out_nodes_info.end()) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                      {"output_type", out_type_vec[i], kOutputTypeError});
      GELOGE(domi::FAILED, "Invalid value for output_type[%s], %s.", out_type_vec[i].c_str(), kOutputTypeError);
      return domi::FAILED;
    }
  }
  return domi::SUCCESS;
}

domi::Status AclGrphParseUtil::LoadOpsProtoLib() {
  string opsproto_path;
  GetOpsProtoPath(opsproto_path);
  GELOGI("Get opsproto path is %s", opsproto_path.c_str());
  OpsProtoManager *manager = OpsProtoManager::Instance();
  map<string, string> option_tmp;
  option_tmp.emplace(std::pair<string, string>(string("ge.opsProtoLibPath"), opsproto_path));
  bool is_proto_init = manager->Initialize(option_tmp);
  if (!is_proto_init) {
    GELOGE(FAILED, "Load ops_proto lib failed, ops proto path is invalid.");
    return FAILED;
  }
  return SUCCESS;
}

void AclGrphParseUtil::SaveCustomCaffeProtoPath() {
  GELOGD("Enter save custom caffe proto path.");
  std::string path_base = GetSoPath();
  path_base = path_base.substr(0, path_base.rfind('/'));
  path_base = path_base.substr(0, path_base.rfind('/') + 1);
  ge::GetParserContext().caffe_proto_path = path_base + "include/proto/";

  string custom_op_path;
  const char *path_env = std::getenv("ASCEND_OPP_PATH");
  if (path_env != nullptr) {
    std::string path = path_env;
    custom_op_path = path + "/framework/custom/caffe/";
    GELOGI("Get custom proto path from env : %s", path_env);
    GetParserContext().custom_proto_path = custom_op_path;
    return;
  }
  custom_op_path = path_base + "ops/framework/custom/caffe/";
  ge::GetParserContext().custom_proto_path = custom_op_path;
  return;
}

// Initialize PARSER, load custom op plugin
// options will be used later for parser decoupling
domi::Status AclGrphParseUtil::AclParserInitialize(const std::map<std::string, std::string> &options) {
  GELOGT(TRACE_INIT, "AclParserInitialize start");
  // check init status
  if (parser_initialized) {
    GELOGW("AclParserInitialize is called more than once");
    return SUCCESS;
  }

  // load custom op plugin
  TBEPluginLoader::Instance().LoadPluginSo(options);

  // load and save custom op proto for prediction
  (void)LoadOpsProtoLib();
  SaveCustomCaffeProtoPath();

  auto op_registry = domi::OpRegistry::Instance();
  if (op_registry == nullptr) {
    GELOGE(FAILED, "Get OpRegistry instance failed");
    return FAILED;
  }

  std::vector<OpRegistrationData> registrationDatas = op_registry->registrationDatas;
  GELOGI("The size of registrationDatas in parser is: %zu", registrationDatas.size());
  for (OpRegistrationData &reg_data : registrationDatas) {
    (void)OpRegistrationTbe::Instance()->Finalize(reg_data, false);
    domi::OpRegistry::Instance()->Register(reg_data);
  }

  // set init status
  if (!parser_initialized) {
    // Initialize success, first time calling initialize
    parser_initialized = true;
  }

  GELOGT(TRACE_STOP, "AclParserInitialize finished");
  return SUCCESS;
}

bool AclGrphParseUtil::CheckAclInputFormat(string &input_format) {
  if (input_format.empty()) {
    // Set default format
    if (ge::GetParserContext().type == domi::TENSORFLOW) {
      input_format = "NHWC";
    } else {
      input_format = "NCHW";
    }
    return true;
  } else if (ge::GetParserContext().type == domi::CAFFE) { // caffe
    if (kCaffeSupportInputFormatSet.find(input_format) != kCaffeSupportInputFormatSet.end()) {
      return true;
    }
    // only support NCHW ND
    ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                    {"input_format", input_format, kCaffeFormatSupport});
    GELOGE(ge::FAILED, "Invalid value for input_format[%s], %s.", input_format.c_str(), kCaffeFormatSupport);
    return false;
  } else if (ge::GetParserContext().type == domi::TENSORFLOW) { // tf
    if (kTfSupportInputFormatSet.find(input_format) != kTfSupportInputFormatSet.end()) {
      return true;
    }
    // only support NCHW NHWC ND NCDHW NDHWC
    ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                    {"input_format", input_format, kTFFormatSupport});
    GELOGE(ge::FAILED, "Invalid value for input_format[%s], %s.", input_format.c_str(), kTFFormatSupport);
    return false;
  }
  return true;
}

domi::Status AclGrphParseUtil::ParseAclFormat(string &input_format) {
  ge::GetParserContext().format = domi::DOMI_TENSOR_ND;
  if (!CheckAclInputFormat(input_format)) {
    GELOGE(PARAM_INVALID, "Check input_format failed");
    return PARAM_INVALID;
  }
  if (!input_format.empty()) {
    auto iter = kInputFormatStrToGeformat.find(input_format);
    if (iter != kInputFormatStrToGeformat.end()) {
      ge::GetParserContext().format = iter->second;
    } else {
      GELOGE(PARAM_INVALID, "Input format %s not support , expect ND/NCHW/NHWC/CHWN/NC1HWC0/NHWC1C0.",
             input_format.c_str());
      return PARAM_INVALID;
    }
  }
  return SUCCESS;
}

bool AclGrphParseUtil::ParseInputShape(const string &input_shape,
                                       std::unordered_map<string, vector<int64_t>> &shape_map,
                                       vector<pair<string, vector<int64_t>>> &user_shape_map, bool is_dynamic_input) {
  vector<string> shape_vec = StringUtils::Split(input_shape, ';');
  const int DEFAULT_SHAPE_PAIR_SIZE = 2;
  for (const auto &shape : shape_vec) {
    vector<string> shape_pair_vec = SplitInputShape(shape);
    if (shape_pair_vec.size() != DEFAULT_SHAPE_PAIR_SIZE) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10002", {"shape", "reason", "sample"},
                                                      {shape, kSplitError1, kInputShapeSample1});
      GELOGW("Parse input parameter [input_shape]'s shape[%s] failed, reason: %s, correct sample is %s.", shape.c_str(),
             kSplitError1, kInputShapeSample1);
      return false;
    }
    if (shape_pair_vec[1].empty()) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10002", {"shape", "reason", "sample"},
                                                      {shape, kEmptyError, kInputShapeSample1});
      GELOGW("Parse input parameter [input_shape]'s shape[%s] failed, reason: %s, correct sample is %s.", shape.c_str(),
             kEmptyError, kInputShapeSample1);
      return false;
    }

    vector<string> shape_value_strs = StringUtils::Split(shape_pair_vec[1], ',');
    vector<int64_t> shape_values;
    for (auto &shape_value_str : shape_value_strs) {
      // stoul: The method may throw an exception: invalid_argument/out_of_range
      if (std::string::npos != shape_value_str.find('.')) {
        ErrorManager::GetInstance().ATCReportErrMessage("E10002", {"shape", "reason", "sample"},
                                                        {shape, kFloatNumError, kInputShapeSample2});
        GELOGW("Parse input parameter [input_shape]'s shape[%s] failed, reason: %s, correct sample is %s.",
               shape.c_str(), kFloatNumError, kInputShapeSample2);
        return false;
      }

      long left_result = 0;
      try {
        left_result = stol(StringUtils::Trim(shape_value_str));
        if (!shape_value_str.empty() && (shape_value_str.front() == '-')) {
          // The value maybe dynamic shape [-1], need substr it and verify isdigit.
          shape_value_str = shape_value_str.substr(1);
        }
        for (char c : shape_value_str) {
          if (!isdigit(c)) {
            ErrorManager::GetInstance().ATCReportErrMessage("E10002", {"shape", "reason", "sample"},
                                                            {shape, kDigitError, kInputShapeSample2});
            GELOGE(PARAM_INVALID, "input_shape's shape value[%s] is not digit", shape_value_str.c_str());
            return false;
          }
        }
      } catch (const std::out_of_range &) {
        ErrorManager::GetInstance().ATCReportErrMessage("E10013", {"parameter", "value"},
                                                        {"input_shape", shape_value_str});
        GELOGW("Input parameter[input_shape]â€™s value[%s] cause out of range execption!", shape_value_str.c_str());
        return false;
      } catch (const std::invalid_argument &) {
        ErrorManager::GetInstance().ATCReportErrMessage("E10014", {"parameter", "value"},
                                                        {"input_shape", shape_value_str});
        GELOGW("Input parameter[input_shape]â€™s value[%s] cause invalid argument!", shape_value_str.c_str());
        return false;
      } catch (...) {
        ErrorManager::GetInstance().ATCReportErrMessage("E10015", {"parameter", "value"},
                                                        {"input_shape", shape_value_str});
        GELOGW("Input parameter[input_shape]â€™s value[%s] cause unkown execption!", shape_value_str.c_str());
        return false;
      }
      int64_t result = left_result;
      // - 1 is not currently supported
      if (!is_dynamic_input && result <= 0) {
        ErrorManager::GetInstance().ATCReportErrMessage("E10011", {"shape", "result"}, {shape, std::to_string(result)});
        GELOGW(
            "Input parameter[input_shape]â€™s shape value[%s] is invalid, "
            "expect positive integer, but value is %ld.",
            shape.c_str(), result);
        return false;
      }
      shape_values.push_back(result);
    }

    shape_map.emplace(make_pair(StringUtils::Trim(shape_pair_vec[0]), shape_values));
    user_shape_map.push_back(make_pair(StringUtils::Trim(shape_pair_vec[0]), shape_values));
  }

  return true;
}

// Parse user input shape info
domi::Status AclGrphParseUtil::ParseAclShape(const string &input_shape, bool is_dynamic_input) {
  ge::GetParserContext().input_dims.clear();
  ge::GetParserContext().user_input_dims.clear();
  ge::GetParserContext().is_dynamic_input = is_dynamic_input;

  if (input_shape.empty()) {
    return SUCCESS;
  }

  std::unordered_map<string, vector<int64_t>> &shape_map = ge::GetParserContext().input_dims;
  if (!ParseInputShape(input_shape, ge::GetParserContext().input_dims, ge::GetParserContext().user_input_dims,
                       is_dynamic_input) ||
      shape_map.empty()) {
    GELOGE(PARAM_INVALID, "Failed to parse input shape: %s", input_shape.c_str());
    return PARAM_INVALID;
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseAclOutputNodes(const string &out_nodes) {
  try {
    // parse output node
    if (!out_nodes.empty()) {
      ge::GetParserContext().out_nodes_map.clear();
      ge::GetParserContext().user_out_nodes.clear();
      ge::GetParserContext().user_out_nodes_top_vec.clear();

      vector<string> nodes_v = StringUtils::Split(out_nodes, ';');
      for (const string &node : nodes_v) {
        vector<string> key_value_v = StringUtils::Split(node, ':');
        if (key_value_v.size() != 2) { // The size must be 2.
          if (key_value_v.size() == 1 && ge::GetParserContext().type == domi::CAFFE) {
            ge::GetParserContext().user_out_nodes_top_vec.push_back(node);
            continue;
          }
          ErrorManager::GetInstance().ATCReportErrMessage(
              "E10001", {"parameter", "value", "reason"},
              {"out_nodes", node, "the correct format is \"node_name1:0;node_name1:1;node_name2:0\""});
          GELOGE(PARAM_INVALID,
                 "The input format of out_nodes is invalid, the correct format is "
                 "\"node_name1:0;node_name1:1;node_name2:0\", while the actual input is %s.",
                 node.c_str());
          return PARAM_INVALID;
        }
        if (!ge::GetParserContext().user_out_nodes_top_vec.empty()) {
          ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                          {"out_nodes", out_nodes, "is not all index or top_name"});
          GELOGE(PARAM_INVALID, "This out_nodes str must be all index or top_name, while the actual input is %s",
                 out_nodes.c_str());
          return PARAM_INVALID;
        }
        // stoi: The method may throw an exception: invalid_argument/out_of_range
        if (!CheckDigitStr(key_value_v[1])) {
          ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                          {"out_nodes", out_nodes, "is not positive integer"});
          GELOGE(PARAM_INVALID, "This str must be digit string, while the actual input is %s", out_nodes.c_str());
          return PARAM_INVALID;
        }

        auto iter = ge::GetParserContext().out_nodes_map.find(key_value_v[0]);
        int32_t index = stoi(StringUtils::Trim(key_value_v[1]));
        GELOGD("Get output info: node[%s] and index[%ld]", key_value_v[0].c_str(), index);
        if (iter != ge::GetParserContext().out_nodes_map.end()) {
          iter->second.emplace_back(index);
        } else {
          std::vector<int32_t> index_v;
          index_v.emplace_back(index);
          ge::GetParserContext().out_nodes_map.emplace(key_value_v[0], index_v);
        }
        ge::GetParserContext().user_out_nodes.push_back(std::make_pair(key_value_v[0], index));
      }
    }
  } catch (std::invalid_argument &) {
    GELOGE(PARAM_INVALID, "Invalid of out_nodes: %s ", out_nodes.c_str());
    ErrorManager::GetInstance().ATCReportErrMessage("E10014", {"parameter", "value"}, {"out_nodes", out_nodes});
    return PARAM_INVALID;
  } catch (std::out_of_range &) {
    GELOGE(PARAM_INVALID, "Invalid of out_nodes: %s ", out_nodes.c_str());
    ErrorManager::GetInstance().ATCReportErrMessage("E10013", {"parameter", "value"}, {"out_nodes", out_nodes});
    return PARAM_INVALID;
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseAclOutputFp16NodesFormat(const string &is_output_fp16) {
  if (is_output_fp16.empty()) {
    return SUCCESS;
  }

  vector<domiTensorFormat_t> &output_formats = ge::GetParserContext().output_formats;
  output_formats.clear();
  vector<string> node_format_vec = StringUtils::Split(is_output_fp16, ',');
  for (auto &is_fp16 : node_format_vec) {
    StringUtils::Trim(is_fp16);
    if (!CheckInputTrueOrFalse(is_fp16, "is_output_adjust_hw_layout")) {
      GELOGE(PARAM_INVALID, "Invalid Param, is_output_adjust_hw_layout only support true/false: but is [%s]",
             is_output_fp16.c_str());
      return PARAM_INVALID;
    }
    if (is_fp16 == "false") {
      output_formats.push_back(DOMI_TENSOR_ND);
    } else if (is_fp16 == "true") {
      output_formats.push_back(domi::DOMI_TENSOR_NC1HWC0);
    }
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseAclOpConf(const std::string &op_conf) {
  if (op_conf.empty()) {
    return SUCCESS;
  }
  // Normalize the path
  string resolved_file_path = ge::parser::RealPath(op_conf.c_str());
  if (resolved_file_path.empty()) {
    ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"}, {"op_map_conf", op_conf});
    GELOGE(domi::FAILED, "Invalid input file path [%s], make sure that the file path is correct.", op_conf.c_str());
    return FAILED;
  }
  std::ifstream fs(resolved_file_path, std::ifstream::in);

  if (!fs.is_open()) {
    GELOGE(PARAM_INVALID, "Open %s failed.", op_conf.c_str());
    return FAILED;
  }

  std::string line;

  while (getline(fs, line)) { // line not with \n
    if (!ParseSingleLine(line, ge::GetParserContext().op_conf_map)) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"},
                                                      {"op_map_conf_line_info", line});
      GELOGE(PARAM_INVALID, "Parse line failed. content is [%s].", line.c_str());
      fs.close();
      return FAILED;
    }
  }
  fs.close(); // close the file

  GELOGI("LoadFileContent success.");
  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseAclEnableScope(const string &enable_scope_fusion_passes) {
  ge::GetParserContext().enable_scope_fusion_passes.clear();
  if (enable_scope_fusion_passes.empty()) {
    return SUCCESS;
  }
  ge::GetParserContext().enable_scope_fusion_passes = enable_scope_fusion_passes;
  return SUCCESS;
}

void AclGrphParseUtil::AddAttrsForInputNodes(const vector<string> &adjust_fp16_format_vec,
                                             const string &fp16_nodes_name, uint32_t index, OpDescPtr &op_desc) {
  if (AttrUtils::SetStr(op_desc, ATTR_ATC_USER_DEFINE_DATATYPE, TypeUtils::DataTypeToSerialString(DT_FLOAT16))) {
    if ((index < adjust_fp16_format_vec.size()) && (adjust_fp16_format_vec[index] == "true")) {
      GELOGI("This node [%s] should be set NC1HWC0", fp16_nodes_name.c_str());
      if (!AttrUtils::SetStr(op_desc, ATTR_ATC_USER_DEFINE_FORMAT, TypeUtils::FormatToSerialString(FORMAT_NC1HWC0))) {
        GELOGW("This node [%s] set NC1HWC0 failed", fp16_nodes_name.c_str());
      }
    }
  }
}

domi::Status AclGrphParseUtil::ParseAclInputFp16Nodes(const ComputeGraphPtr &graph, const string &input_fp16_nodes,
                                                      const string &is_input_adjust_hw_layout) {
  GE_CHECK_NOTNULL(graph);
  vector<string> adjust_fp16_format_vec;
  if (!is_input_adjust_hw_layout.empty()) {
    adjust_fp16_format_vec = StringUtils::Split(is_input_adjust_hw_layout, ',');
    for (auto &s : adjust_fp16_format_vec) {
      StringUtils::Trim(s);
      if (!CheckInputTrueOrFalse(s, "is_input_adjust_hw_layout")) {
        GELOGE(PARAM_INVALID, "Invalid Param, is_input_adjust_hw_layout only support true/false: but is [%s]",
               is_input_adjust_hw_layout.c_str());
        return PARAM_INVALID;
      }
    }
  }
  if (input_fp16_nodes.empty()) {
    return SUCCESS;
  }
  GELOGI("The input_fp16_nodes is set %s", input_fp16_nodes.c_str());
  vector<string> input_fp16_nodes_vec = StringUtils::Split(input_fp16_nodes, ';');
  for (uint32_t i = 0; i < input_fp16_nodes_vec.size(); ++i) {
    ge::NodePtr node = graph->FindNode(input_fp16_nodes_vec[i]);
    if (node == nullptr) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"},
                                                      {"input_fp16_nodes", input_fp16_nodes_vec[i]});
      GELOGE(PARAM_INVALID, "Input parameter[input_fp16_nodes]'s opname[%s] is not exist in model",
             input_fp16_nodes_vec[i].c_str());
      return PARAM_INVALID;
    }
    auto op_desc = node->GetOpDesc();
    GE_CHECK_NOTNULL(op_desc);
    if (op_desc->GetType() != ge::parser::DATA) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10017", {"parameter", "opname"},
                                                      {"input_fp16_nodes", input_fp16_nodes_vec[i]});
      GELOGE(PARAM_INVALID, "Input parameter[input_fp16_nodes]'s opname[%s] is not a input opname",
             input_fp16_nodes_vec[i].c_str());
      return PARAM_INVALID;
    }
    AddAttrsForInputNodes(adjust_fp16_format_vec, input_fp16_nodes_vec[i], i, op_desc);
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseAclWeightCompressConf(const ComputeGraphPtr &graph,
                                                          const string &compress_weight_conf) {
  GE_CHECK_NOTNULL(graph);
  if (compress_weight_conf.empty()) {
    return SUCCESS;
  }
  std::string real_path = ge::parser::RealPath(compress_weight_conf.c_str());
  if (real_path.empty()) {
    ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"},
                                                    {"compress_weight_conf", compress_weight_conf});
    GELOGE(PARAM_INVALID, "Can not get real path for %s.", compress_weight_conf.c_str());
    return PARAM_INVALID;
  }
  std::ifstream ifs(real_path);
  if (!ifs.is_open()) {
    ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"},
                                                    {"compress_weight_conf", compress_weight_conf});
    GELOGE(FAILED, "Open file %s failed", compress_weight_conf.c_str());
    return FAILED;
  }

  std::string compress_nodes;
  ifs >> compress_nodes;
  ifs.close();
  if (compress_nodes.empty()) {
    GELOGW("Compress weight of nodes info is empty");
    return SUCCESS;
  }
  GELOGI("Compress weight of nodes: %s", compress_nodes.c_str());

  vector<string> compress_node_vec = StringUtils::Split(compress_nodes, ';');
  for (size_t i = 0; i < compress_node_vec.size(); ++i) {
    ge::NodePtr node = graph->FindNode(compress_node_vec[i]);
    if (node == nullptr) {
      GELOGW("Node %s is not in graph", compress_node_vec[i].c_str());
      continue;
    }
    auto op_desc = node->GetOpDesc();
    GE_CHECK_NOTNULL(op_desc);
    if (!ge::AttrUtils::SetBool(op_desc, ge::ATTR_NAME_COMPRESS_WEIGHT, true)) {
      GELOGE(domi::FAILED, "Node %s SetBool failed.", compress_node_vec[i].c_str());
      return domi::FAILED;
    }
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseAclOutputType(const std::string &output_type,
                                                  std::map<std::string, vector<std::string>> &output_node_dt_map) {
  if (output_type.find(':') == std::string::npos) {
    GELOGI("output_type is not multiple nodes, means all out nodes");
    return CheckOutPutDataTypeSupport(output_type);
  }
  std::vector<std::string> out_type_vec;
  vector<string> nodes_v = StringUtils::Split(output_type, ';');
  for (const string &node : nodes_v) {
    vector<string> node_index_type_v = StringUtils::Split(node, ':');
    if (node_index_type_v.size() != 3) { // The size must be 3.
      ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                      {"output_type", node, kOutputTypeSample});
      GELOGE(PARAM_INVALID, "Invalid value for output_type[%s], %s.", node.c_str(), kOutputTypeSample);
      return domi::FAILED;
    }
    ge::DataType tmp_dt;
    std::string node_name = StringUtils::Trim(node_index_type_v[kOutputTypeNode]);
    std::string index_str = StringUtils::Trim(node_index_type_v[kOutputTypeIndex]);
    int32_t index;
    if (StringToInt(index_str, index) != SUCCESS) {
      GELOGE(PARAM_INVALID, "This str must be digit string, while the actual input is %s.", index_str.c_str());
      return domi::FAILED;
    }
    std::string dt_value = StringUtils::Trim(node_index_type_v[kOutputTypeDataType]);
    auto it = kOutputTypeSupportDatatype.find(dt_value);
    if (it == kOutputTypeSupportDatatype.end()) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                      {"output_type", dt_value, kOutputTypeSupport});
      GELOGE(ge::PARAM_INVALID, "Invalid value for output_type[%s], %s.", dt_value.c_str(), kOutputTypeSupport);
      return domi::FAILED;
    } else {
      tmp_dt = it->second;
    }
    out_type_vec.push_back(node_name + ":" + index_str);
    std::string index_dt_str = index_str + ":" + TypeUtils::DataTypeToSerialString(tmp_dt);
    auto it1 = output_node_dt_map.find(node_name);
    if (it1 == output_node_dt_map.end()) {
      vector<string> tmp_vec;
      tmp_vec.push_back(index_dt_str);
      output_node_dt_map.emplace(node_name, tmp_vec);
    } else {
      it1->second.push_back(index_dt_str);
    }
  }
  return VerifyOutputTypeAndOutNodes(out_type_vec);
}

void AclGrphParseUtil::GetOutputNodesNameAndIndex(std::vector<std::pair<ge::NodePtr, int32_t>> &output_nodes_info,
                                                  std::vector<std::string> &output_nodes_name) {
  output_nodes_name.clear();
  if (ge::GetParserContext().out_top_names.empty()) {
    // tf process, no top name.
    for (const auto output_node_info : output_nodes_info) {
      std::string node_name = output_node_info.first->GetName();
      int32_t index = output_node_info.second;
      output_nodes_name.push_back(node_name + ":" + std::to_string(index));
    }
    return;
  }
  // caffe process, need add top name after node_name:index
  for (size_t i = 0; i < output_nodes_info.size(); ++i) {
    std::string node_name = output_nodes_info[i].first->GetName();
    int32_t index = output_nodes_info[i].second;
    if (i < ge::GetParserContext().out_top_names.size()) {
      output_nodes_name.push_back(node_name + ":" + std::to_string(index) + ":" +
                                  ge::GetParserContext().out_top_names[i]);
    } else {
      GELOGW("Get top name of node [%s] fail.", node_name.c_str());
      output_nodes_name.push_back(node_name + ":" + std::to_string(index));
    }
  }
}

domi::Status AclGrphParseUtil::GetOutputLeaf(NodePtr node,
                                             std::vector<std::pair<ge::NodePtr, int32_t>> &output_nodes_info) {
  ge::OpDescPtr tmpDescPtr = node->GetOpDesc();
  if (tmpDescPtr == nullptr) {
    GELOGE(domi::FAILED, "Get outnode op desc fail.");
    return domi::FAILED;
  }
  size_t size = tmpDescPtr->GetOutputsSize();
  if (node->GetType() != ge::parser::NETOUTPUT) {
    for (size_t index = 0; index < size; ++index) {
      output_nodes_info.push_back(std::make_pair(node, index));
      GELOGD("Get output leaf node:%s.", node->GetName().c_str());
    }
  } else {
    const auto in_anchors = node->GetAllInDataAnchors();
    for (auto in_anchor : in_anchors) {
      auto out_anchor = in_anchor->GetPeerOutAnchor();
      if (out_anchor == nullptr) {
        GELOGE(domi::FAILED, "Get leaf node op desc fail.");
        return domi::FAILED;
      }
      auto out_node = out_anchor->GetOwnerNode();
      output_nodes_info.push_back(std::make_pair(out_node, out_anchor->GetIdx()));
    }
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::GetDefaultOutInfo(ge::ComputeGraphPtr &compute_graph,
                                                 std::vector<std::pair<ge::NodePtr, int32_t>> &output_nodes_info) {
  std::vector<std::pair<std::string, int32_t>> default_out_nodes = ge::GetParserContext().default_out_nodes;
  if (ge::GetParserContext().type == domi::CAFFE && !default_out_nodes.empty()) {
    for (uint32_t i = 0; i < default_out_nodes.size(); ++i) {
      ge::NodePtr out_node = compute_graph->FindNode(default_out_nodes[i].first);
      if (out_node == nullptr) {
        ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"},
                                                        {"out_nodes", default_out_nodes[i].first});
        GELOGE(domi::FAILED, "Can not find src node (%s) in graph.", default_out_nodes[i].first.c_str());
        return domi::FAILED;
      }
      output_nodes_info.push_back(std::make_pair(out_node, default_out_nodes[i].second));
      GELOGD("Get default output node:%s.", out_node->GetName().c_str());
    }
    return domi::SUCCESS;
  }

  for (ge::NodePtr node : compute_graph->GetDirectNode()) {
    if (!node->GetInAllNodes().empty() && node->GetOutAllNodes().empty()) {
      Status ret = GetOutputLeaf(node, output_nodes_info);
      GE_CHK_BOOL_RET_STATUS(ret == SUCCESS, ret, "Find leaf fail.");
    }
  }
  return domi::SUCCESS;
}

domi::Status AclGrphParseUtil::SetOutputNodeInfo(ge::Graph &graph,
                                                 const std::map<AscendString, AscendString> &parser_params) {
  ge::ComputeGraphPtr compute_graph = ge::GraphUtils::GetComputeGraph(graph);
  GE_CHECK_NOTNULL(compute_graph);

  string output_type;
  GetAclParams(parser_params, ge::ir_option::OUTPUT_TYPE, output_type);

  std::vector<std::pair<std::string, int32_t>> user_out_nodes = ge::GetParserContext().user_out_nodes;
  std::vector<domiTensorFormat_t> output_formats = ge::GetParserContext().output_formats;
  std::vector<std::pair<ge::NodePtr, int32_t>> output_nodes_info;
  std::vector<std::string> output_nodes_name;
  std::map<std::string, vector<std::string>> output_node_dt_map;
  if (!output_type.empty()) {
    if (ParseAclOutputType(output_type, output_node_dt_map) != SUCCESS) {
      GELOGE(domi::FAILED, "Parse output_type failed.");
      return domi::FAILED;
    }
  }

  // User declared outputs
  for (uint32_t i = 0; i < user_out_nodes.size(); ++i) {
    ge::NodePtr out_node = compute_graph->FindNode(user_out_nodes[i].first);
    if (out_node == nullptr) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"},
                                                      {"out_nodes", user_out_nodes[i].first});
      GELOGE(domi::FAILED, "Can not find src node (%s) in graph.", user_out_nodes[i].first.c_str());
      return domi::FAILED;
    }
    auto op_desc = out_node->GetOpDesc();
    GE_CHECK_NOTNULL(op_desc);
    if (CheckOutNode(op_desc, user_out_nodes[i].second) != SUCCESS) {
      GELOGE(domi::FAILED, "Check out node (%s) fail.", user_out_nodes[i].first.c_str());
      return domi::FAILED;
    }

    // add user_define_output_nodes attr.
    (void)ge::AttrUtils::SetStr(op_desc, ATTR_ATC_USER_DEFINE_OUTPUT_NODES, "true");

    if (i < output_formats.size()) {
      if (output_formats[i] == domi::DOMI_TENSOR_NC1HWC0) {
        GELOGI("The output node [%s] should be set NC1HWC0", user_out_nodes[i].first.c_str());
        vector<string> output_fp16_5hd_vec;
        (void)ge::AttrUtils::GetListStr(op_desc, "_user_defined_output_fp16_5hd", output_fp16_5hd_vec);
        output_fp16_5hd_vec.push_back(std::to_string(user_out_nodes[i].second) + ":" + "NC1HWC0");
        (void)ge::AttrUtils::SetListStr(op_desc, "_user_defined_output_fp16_5hd", output_fp16_5hd_vec);
      }
    }
    auto it = output_node_dt_map.find(user_out_nodes[i].first);
    if (it != output_node_dt_map.end()) {
      GELOGI("The output node [%s] need to be set output_type", user_out_nodes[i].first.c_str());
      (void)ge::AttrUtils::SetListStr(op_desc, "_user_defined_output_data_type", it->second);
    }
    output_nodes_info.push_back(std::make_pair(out_node, user_out_nodes[i].second));
  }
  // default output node (leaf)
  if (user_out_nodes.empty()) {
    if (GetDefaultOutInfo(compute_graph, output_nodes_info) != SUCCESS) {
      GELOGE(domi::FAILED, "Get default output info failed.");
      return domi::FAILED;
    }
  }
  GetOutputNodesNameAndIndex(output_nodes_info, output_nodes_name);
  compute_graph->SetGraphOutNodesInfo(output_nodes_info);
  ge::GetParserContext().net_out_nodes = output_nodes_name;
  GELOGI("Set graph %s output node success.", graph.GetName().c_str());
  return domi::SUCCESS;
}

domi::Status AclGrphParseUtil::ParseAclLogLevel(const std::string &log) {
  if (log.empty()) {
    return SUCCESS;
  }
  int ret = -1;
  if (log == "default") {
    ret = 0;
  } else if (log == "null") {
    ret = dlog_setlevel(-1, DLOG_NULL, 0);
  } else if (log == "debug") {
    ret = dlog_setlevel(-1, DLOG_DEBUG, 1);
  } else if (log == "info") {
    ret = dlog_setlevel(-1, DLOG_INFO, 1);
  } else if (log == "warning") {
    ret = dlog_setlevel(-1, DLOG_WARN, 1);
  } else if (log == "error") {
    ret = dlog_setlevel(-1, DLOG_ERROR, 1);
  } else {
    ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"}, {"log", log});
    GELOGE(PARAM_INVALID, "Invalid value for log:%s, only support debug, info, warning, error, null", log.c_str());
    return PARAM_INVALID;
  }
  if (ret != 0) {
    GELOGE(PARAM_INVALID, "Log setlevel fail !");
  }
  return domi::SUCCESS;
}

domi::Status AclGrphParseUtil::CheckOptions(const std::map<AscendString, AscendString> &parser_params) {
  for (auto &ele : parser_params) {
    const char *key_ascend = ele.first.GetString();
    if (key_ascend == nullptr) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"},
                                                      {"parser_params", "null AscendString"});
      GELOGE(PARAM_INVALID, "Input options key is null, Please check!");
      return PARAM_INVALID;
    }

    string key_str = key_ascend;
    auto it = ge::ir_option::ir_parser_suppported_options.find(key_str);
    if (it == ge::ir_option::ir_parser_suppported_options.end()) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"}, {"parser_params", key_str});
      GELOGE(PARAM_INVALID, "Input options include unsupported option(%s).Please check!", key_ascend);
      return PARAM_INVALID;
    }
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::CheckAclInputShapeNode(const ComputeGraphPtr &graph, const bool is_dynamic_input) {
  if (!is_dynamic_input) {
    for (auto node : graph->GetDirectNode()) {
      if (node->GetType() == ge::parser::DATA) {
        auto data_op_desc = node->GetOpDesc();
        GE_CHECK_NOTNULL(data_op_desc);
        auto tensor_desc = data_op_desc->MutableInputDesc(0);
        GE_CHECK_NOTNULL(tensor_desc);
        for (auto dim : tensor_desc->GetShape().GetDims()) {
          if (dim < 0) {
            GELOGE(PARAM_INVALID,
                   "Input op [%s] shape %ld is negative, maybe you should set input_shape to specify its shape",
                   node->GetName().c_str(), dim);
            const string reason = "maybe you should set input_shape to specify its shape";
            ErrorManager::GetInstance().ATCReportErrMessage("E10001", {"parameter", "value", "reason"},
                                                            {node->GetName(), to_string(dim), reason});
            return PARAM_INVALID;
          }
        }
      }
    }
  }
  for (auto it : ge::GetParserContext().user_input_dims) {
    std::string node_name = it.first;
    ge::NodePtr node = graph->FindNode(node_name);
    if (node == nullptr) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10016", {"parameter", "opname"}, {"input_shape", node_name});
      GELOGE(PARAM_INVALID, "Input parameter[input_shape]'s opname[%s] is not exist in model", node_name.c_str());
      return PARAM_INVALID;
    }
    if (node->GetType() != ge::parser::DATA) {
      ErrorManager::GetInstance().ATCReportErrMessage("E10017", {"parameter", "opname"}, {"input_shape", node_name});
      GELOGE(PARAM_INVALID, "Input parameter[input_shape]'s opname[%s] is not a input opname", node_name.c_str());
      return PARAM_INVALID;
    }
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::CheckAclOpNameMap(const ComputeGraphPtr &graph, const std::string &op_conf) {
  GE_CHECK_NOTNULL(graph);
  unordered_map<string, string> graphNodeTypes;
  for (const NodePtr &node : graph->GetAllNodes()) {
    auto op_desc = node->GetOpDesc();
    if (op_desc == nullptr) {
      GELOGE(PARAM_INVALID, "Invalid parameter for opDesc.");
      return PARAM_INVALID;
    }
    graphNodeTypes[op_desc->GetType()] = "";
  }
  std::map<std::string, std::string> &propertiesMap = ge::GetParserContext().op_conf_map;
  if (propertiesMap.empty()) {
    ErrorManager::GetInstance().ATCReportErrMessage("E10003", {"parameter", "value", "reason"},
                                                    {"op_name_map", op_conf, "the file content is empty"});
    GELOGE(PARAM_INVALID, "op_name_map file content is empty, please check file!");
    return PARAM_INVALID;
  }
  for (auto iter = propertiesMap.begin(); iter != propertiesMap.end(); iter++) {
    GE_IF_BOOL_EXEC(graphNodeTypes.find(iter->second) == graphNodeTypes.end(),
                    ErrorManager::GetInstance().ATCReportErrMessage(
                        "E10003", {"parameter", "value", "reason"},
                        {"op_name_map", op_conf, "type[" + iter->second + "] is not found in model"});
                    GELOGE(PARAM_INVALID, "Invalid parameter for op_name_map."); return PARAM_INVALID;);
  }
  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseParamsBeforeGraph(const std::map<AscendString, AscendString> &parser_params,
                                                      string &graph_name) {
  GELOGI("Parse graph user options start.");
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(CheckOptions(parser_params) != SUCCESS, return PARAM_INVALID,
                                 "Parse paragrams invalid.");
  // support paragrams: log, input_format, is_dynamic_input, input_shape, out_nodes
  //                    is_output_adjust_hw_layout, output, op_name_map, enable_scope_fusion_passes
  string log_level;
  GetAclParams(parser_params, ge::ir_option::LOG_LEVEL, log_level);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclLogLevel(log_level) != SUCCESS, return PARAM_INVALID,
                                 "Parse log_level failed");

  string input_format;
  GetAclParams(parser_params, ge::ir_option::INPUT_FORMAT, input_format);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclFormat(input_format) != SUCCESS, return PARAM_INVALID,
                                 "Parse input_format failed");

  string dynamic_input_str;
  GetAclParams(parser_params, ge::ir_option::IS_DYNAMIC_INPUT, dynamic_input_str);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(
      !dynamic_input_str.empty() && !CheckInputTrueOrFalse(dynamic_input_str, "is_dynamic_input"), return PARAM_INVALID,
      "Parse is_dynamic_input failed");
  bool is_dynamic_input = dynamic_input_str == "true" ? true : false;

  string input_shape;
  GetAclParams(parser_params, ge::ir_option::INPUT_SHAPE, input_shape);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclShape(input_shape, is_dynamic_input) != SUCCESS, return PARAM_INVALID,
                                 "Parse input_shape failed");

  string out_nodes;
  GetAclParams(parser_params, ge::ir_option::OUT_NODES, out_nodes);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclOutputNodes(out_nodes) != SUCCESS, return PARAM_INVALID,
                                 "Parse out_nodes failed");

  string is_output_adjust_hw_layout;
  GetAclParams(parser_params, ge::ir_option::IS_OUTPUT_ADJUST_HW_LAYOUT, is_output_adjust_hw_layout);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclOutputFp16NodesFormat(is_output_adjust_hw_layout) != SUCCESS,
                                 return PARAM_INVALID, "Parse is_output_adjust_hw_layout failed");

  string op_conf_str;
  GetAclParams(parser_params, ge::ir_option::OP_NAME_MAP, op_conf_str);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclOpConf(op_conf_str) != SUCCESS, return PARAM_INVALID,
                                 "Parse op_name_map failed");

  string tmp_name;
  GetAclParams(parser_params, ge::ir_option::OUTPUT, tmp_name);
  graph_name = tmp_name.empty() ? (kGraphDefaultName + "_" + ge::parser::CurrentTimeInStr()) : tmp_name;

  string enable_scope_fusion_passes;
  GetAclParams(parser_params, ge::ir_option::ENABLE_SCOPE_FUSION_PASSES, enable_scope_fusion_passes);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclEnableScope(enable_scope_fusion_passes) != SUCCESS, return PARAM_INVALID,
                                 "Parse enable_scope_fusion_passes failed");

  return SUCCESS;
}

domi::Status AclGrphParseUtil::ParseParamsAfterGraph(ge::Graph &graph,
                                                     const std::map<AscendString, AscendString> &parser_params) {
  // support paragrams: input_fp16_nodes, is_input_adjust_hw_layout, compress_weight_conf,
  ComputeGraphPtr compute_graph = GraphUtils::GetComputeGraph(graph);

  string input_fp16_nodes;
  GetAclParams(parser_params, ge::ir_option::INPUT_FP16_NODES, input_fp16_nodes);

  string is_input_adjust_hw_layout;
  GetAclParams(parser_params, ge::ir_option::IS_INPUT_ADJUST_HW_LAYOUT, is_input_adjust_hw_layout);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(
      ParseAclInputFp16Nodes(compute_graph, input_fp16_nodes, is_input_adjust_hw_layout) != SUCCESS,
      return PARAM_INVALID, "Parse input_fp16_nodes failed");

  bool is_dynamic_input = ge::GetParserContext().is_dynamic_input;
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(CheckAclInputShapeNode(compute_graph, is_dynamic_input) != SUCCESS,
                                 return PARAM_INVALID, "Check nodes input_shape info failed");

  string compress_weight_conf;
  GetAclParams(parser_params, ge::ir_option::COMPRESS_WEIGHT_CONF, compress_weight_conf);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(ParseAclWeightCompressConf(compute_graph, compress_weight_conf) != SUCCESS,
                                 return PARAM_INVALID, "Parse compress_weight_conf failed");
  string op_conf_str;
  GetAclParams(parser_params, ge::ir_option::OP_NAME_MAP, op_conf_str);
  if (!op_conf_str.empty()) {
    GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(CheckAclOpNameMap(compute_graph, op_conf_str) != SUCCESS, return PARAM_INVALID,
                                   "Check op_name_map info failed");
  }

  return SUCCESS;
}

namespace parser {
FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY std::string RealPath(const char *path) {
  if (path == nullptr) {
    GELOGE(ge::FAILED, "path pointer is NULL.");
    return "";
  }
  if (strlen(path) >= PATH_MAX) {
    ErrorManager::GetInstance().ATCReportErrMessage("E19002", {"filepath", "size"}, {path, std::to_string(PATH_MAX)});
    GELOGE(ge::FAILED, "Path[%s] len is too long, it must be less than %d", path, PATH_MAX);
    return "";
  }
  // Nullptr is returned when the path does not exist or there is no permission
  // Return absolute path when path is accessible
  std::string res;
  char resolved_path[PATH_MAX] = {0};
  if (realpath(path, resolved_path) != nullptr) {
    res = resolved_path;
  }

  return res;
}

// Get file length
FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY long GetFileLength(const std::string &input_file) {
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(input_file.empty(), return -1, "input_file path is null.");

  std::string real_path = RealPath(input_file.c_str());

  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(real_path.empty(), return -1, "input_file path '%s' not valid", input_file.c_str());
  unsigned long long file_length = 0;
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(mmGetFileSize(input_file.c_str(), &file_length) != EN_OK,
                                 ErrorManager::GetInstance().ATCReportErrMessage("E19001", {"file", "errmsg"},
                                                                                 {input_file, strerror(errno)});
                                         return -1, "Open file[%s] failed. %s", input_file.c_str(), strerror(errno));

  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((file_length == 0),
                                 ErrorManager::GetInstance().ATCReportErrMessage("E19015", {"filepath"}, {input_file});
                                         return -1, "File[%s] size is 0, not valid.", input_file.c_str());

  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(file_length > kMaxFileSizeLimit,
                                 ErrorManager::GetInstance().ATCReportErrMessage(
                                         "E19016", {"filepath", "filesize", "maxlen"},
                                         {input_file, std::to_string(file_length), std::to_string(kMaxFileSizeLimit)});
                                         return -1, "File[%s] size %lld is out of limit: %d.",
                                 input_file.c_str(), file_length, kMaxFileSizeLimit);
  return static_cast<long>(file_length);
}

FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY uint64_t GetCurrentTimestamp() {
  struct timeval tv{};
  int ret = gettimeofday(&tv, nullptr);
  GE_LOGE_IF(ret != 0, "Func gettimeofday may failed: ret=%d", ret);
  auto total_use_time = tv.tv_usec + tv.tv_sec * 1000000;  // 1000000: seconds to microseconds
  return static_cast<uint64_t>(total_use_time);
}

static bool ReadProtoFromCodedInputStream(CodedInputStream &coded_stream, Message *proto) {
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(proto == nullptr,
                                 return false, "incorrect parameter. nullptr == proto");

  coded_stream.SetTotalBytesLimit(kProtoReadBytesLimit, kWarningThreshold);
  return proto->ParseFromCodedStream(&coded_stream);
}

/** @ingroup domi_common
 *  @brief Read all data from binary file
 *  @param [in] file_name  File path
 *  @param [out] buffer  The address of the output memory, which needs to be released by the caller
 *  @param [out] length  Output memory size
 *  @return false fail
 *  @return true success
 */
FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY bool ReadBytesFromBinaryFile(const char *file_name, char **buffer,
                                                                              int &length) {
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((file_name == nullptr), return false, "incorrect parameter. file is nullptr");
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((buffer == nullptr), return false, "incorrect parameter. buffer is nullptr");

  std::string real_path = RealPath(file_name);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(real_path.empty(), return false, "file path '%s' not valid", file_name);

  std::ifstream file(real_path.c_str(), std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    GELOGE(ge::FAILED, "Read file %s failed.", file_name);
    return false;
  }

  length = static_cast<int>(file.tellg());

  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((length <= 0), file.close(); return false, "file length <= 0");

  file.seekg(0, std::ios::beg);

  *buffer = new(std::nothrow) char[length]();
  GE_CHK_BOOL_TRUE_EXEC_RET_STATUS(*buffer == nullptr, false, file.close(), "new an object failed.");

  file.read(*buffer, length);
  file.close();
  return true;
}

FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY bool ReadProtoFromBinaryFile(const char *file, Message *proto) {
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((file == nullptr || proto == nullptr),
                                 return false,
                                 "Input parameter file or proto is nullptr!");

  std::string real_path = RealPath(file);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(real_path.empty(),
                                 return false, "pb file path '%s' not valid", file);

  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(GetFileLength(real_path) == -1, return false, "file size not valid.");

  std::ifstream fs(real_path, std::ifstream::in | std::ifstream::binary);
  if (!fs.is_open()) {
    ErrorManager::GetInstance().ATCReportErrMessage("E19001", {"file", "errmsg"}, {file, "ifstream is_open failed"});
    GELOGE(ge::FAILED, "Open real path[%s] failed.", file);
    return false;
  }

  google::protobuf::io::IstreamInputStream istream(&fs);
  google::protobuf::io::CodedInputStream coded_stream(&istream);

  bool ret = ReadProtoFromCodedInputStream(coded_stream, proto);

  fs.close();

  if (!ret) {
    ErrorManager::GetInstance().ATCReportErrMessage("E19005", {"file"}, {file});
    GELOGE(ge::FAILED, "Parse file[%s] failed.", file);
    return ret;
  }

  return ret;
}

FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY bool ReadProtoFromArray(const void *data, int size, Message *proto) {
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((proto == nullptr || data == nullptr || size == 0), return false,
                                 "incorrect parameter. proto is nullptr || data is nullptr || size is 0");

  google::protobuf::io::CodedInputStream coded_stream(reinterpret_cast<uint8_t *>(const_cast<void *>(data)), size);
  return ReadProtoFromCodedInputStream(coded_stream, proto);
}

FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY bool ReadProtoFromText(const char *file,
                                                                        google::protobuf::Message *message) {
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((file == nullptr || message == nullptr), return false,
                                 "incorrect parameter. nullptr == file || nullptr == message");

  std::string real_path = RealPath(file);
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(real_path.empty(),
                                 ErrorManager::GetInstance().ATCReportErrMessage("E19000", {"path", "errmsg"},
                                                                                 {file, strerror(errno)});
                                         return false, "Path[%s]'s realpath is empty, errmsg[%s]", file,
                                 strerror(errno));

  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG(GetFileLength(real_path) == -1, return false, "file size not valid.");

  std::ifstream fs(real_path.c_str(), std::ifstream::in);

  if (!fs.is_open()) {
    ErrorManager::GetInstance().ATCReportErrMessage("E19017", {"realpth", "protofile"}, {real_path, file});
    GELOGE(ge::FAILED,
           "Fail to open proto file real path is '%s' when orginal file path is '%s'.", real_path.c_str(), file);
    return false;
  }

  google::protobuf::io::IstreamInputStream input(&fs);
  bool ret = google::protobuf::TextFormat::Parse(&input, message);
  GE_IF_BOOL_EXEC(!ret,
                  ErrorManager::GetInstance().ATCReportErrMessage("E19018", {"protofile"}, {file});
                          GELOGE(ret, "Parse file[%s] through [google::protobuf::TextFormat::Parse] failed, "
                                      "please check whether the file is a valid protobuf format file.", file));
  fs.close();

  return ret;
}

FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY bool ReadProtoFromMem(const char *data, int size,
                                                                       google::protobuf::Message *message) {
  GE_CHK_BOOL_TRUE_EXEC_WITH_LOG((data == nullptr || message == nullptr), return false,
                                 "incorrect parameter. data is nullptr || message is nullptr");
  std::string str(data, static_cast<size_t>(size));
  std::istringstream fs(str);

  google::protobuf::io::IstreamInputStream input(&fs);
  bool ret = google::protobuf::TextFormat::Parse(&input, message);
  GE_IF_BOOL_EXEC(
          !ret, GELOGE(ret, "Call [google::protobuf::TextFormat::Parse] func ret fail, please check your text file."));

  return ret;
}

///
/// @brief get the Original Type of FrameworkOp
/// @param [in] node
/// @param [out] type
/// @return Status
///
Status GetOriginalType(const ge::NodePtr &node, string &type) {
  GE_CHECK_NOTNULL(node);
  type = node->GetType();
  GE_IF_BOOL_EXEC(type != FRAMEWORKOP, return SUCCESS);
  GE_CHECK_NOTNULL(node->GetOpDesc());
  bool ret = ge::AttrUtils::GetStr(node->GetOpDesc(), ATTR_NAME_FRAMEWORK_ORIGINAL_TYPE, type);
  if (!ret) {
    GELOGE(INTERNAL_ERROR, "Get FrameWorkOp original type [%s]", type.c_str());
    return INTERNAL_ERROR;
  }
  GELOGD("Get FrameWorkOp original type [%s]", type.c_str());
  return SUCCESS;
}

FMK_FUNC_HOST_VISIBILITY bool ValidateStr(const std::string &str, const std::string &mode) {
  char ebuff[kMaxBuffSize];
  regex_t reg;
  int cflags = REG_EXTENDED | REG_NOSUB;
  int ret = regcomp(&reg, mode.c_str(), cflags);
  if (ret) {
    regerror(ret, &reg, ebuff, kMaxBuffSize);
    GELOGW("regcomp failed, reason: %s", ebuff);
    regfree(&reg);
    return true;
  }

  ret = regexec(&reg, str.c_str(), 0, nullptr, 0);
  if (ret) {
    regerror(ret, &reg, ebuff, kMaxBuffSize);
    GELOGE(ge::PARAM_INVALID, "regexec failed, reason: %s", ebuff);
    regfree(&reg);
    return false;
  }

  regfree(&reg);
  return true;
}

FMK_FUNC_HOST_VISIBILITY FMK_FUNC_DEV_VISIBILITY std::string CurrentTimeInStr() {
  std::time_t now = std::time(nullptr);
  std::tm *ptm = std::localtime(&now);
  if (ptm == nullptr) {
    GELOGE(ge::FAILED, "Localtime failed.");
    return "";
  }

  const int kTimeBufferLen = 32;
  char buffer[kTimeBufferLen + 1] = {0};
  // format: 20171122042550
  std::strftime(buffer, kTimeBufferLen, "%Y%m%d%H%M%S", ptm);
  return std::string(buffer);
}
}  // namespace parser
}  // namespace ge
