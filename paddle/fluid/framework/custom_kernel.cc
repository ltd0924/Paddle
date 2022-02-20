/* Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#if defined _WIN32 || defined __APPLE__
#else
#define _LINUX
#endif

#include "paddle/fluid/framework/custom_kernel.h"
#include <dirent.h>
#include <algorithm>
#include <regex>
#include "paddle/fluid/framework/op_kernel_info_helper.h"
#include "paddle/fluid/framework/operator.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/phi/api/ext/op_kernel_info.h"
#include "paddle/phi/core/compat/convert_utils.h"
#include "paddle/phi/core/kernel_context.h"
#include "paddle/phi/core/kernel_registry.h"

namespace paddle {

namespace framework {

// set phi::Kernel args_def_ from op_kernel_info
// because we can not set directly to phi::Kernel without exposing
// phi::KernelArgsDef when parsing custom user function
static void ParseArgs(const OpKernelInfo& op_kernel_info,
                      phi::KernelArgsDef* args_def) {
  auto& input_defs = OpKernelInfoHelper::GetInputDefs(op_kernel_info);
  auto& output_defs = OpKernelInfoHelper::GetOutputDefs(op_kernel_info);
  auto& attribute_defs = OpKernelInfoHelper::GetAttributeDefs(op_kernel_info);

  for (auto& input : input_defs) {
    auto type_index =
        input.is_vector
            ? std::type_index(typeid(const std::vector<phi::DenseTensor>&))
            : std::type_index(typeid(const phi::DenseTensor&));
    args_def->AppendInput(input.backend, input.layout, input.dtype, type_index);
  }
  for (auto& output : output_defs) {
    auto type_index =
        output.is_vector
            ? std::type_index(typeid(const std::vector<phi::DenseTensor>&))
            : std::type_index(typeid(const phi::DenseTensor&));
    args_def->AppendOutput(output.backend, output.layout, output.dtype,
                           type_index);
  }
  for (auto& attr : attribute_defs) {
    args_def->AppendAttribute(attr.type_index);
  }
}

// custom pten kernel call function define
static void RunKernelFunc(phi::KernelContext* ctx,
                          const OpKernelInfo& op_kernel_info) {
  VLOG(3) << "[CUSTOM KERNEL] RunKernelFunc begin...";

  // input and output size is not params' num
  // but actual Tensors' size
  size_t input_size = ctx->InputsSize();
  size_t output_size = ctx->OutputsSize();
  size_t attr_size = ctx->AttrsSize();

  // parameters' num of unified user kernel function
  auto& input_defs = OpKernelInfoHelper::GetInputDefs(op_kernel_info);
  auto& output_defs = OpKernelInfoHelper::GetOutputDefs(op_kernel_info);
  auto& attribute_defs = OpKernelInfoHelper::GetAttributeDefs(op_kernel_info);

  PADDLE_ENFORCE_GE(input_size, input_defs.size(),
                    platform::errors::InvalidArgument(
                        "the size of ctx inputs size (%d) must be larger than "
                        "the size of kernel input_defs (%d).",
                        input_size, input_defs.size()));

  PADDLE_ENFORCE_GE(output_size, output_defs.size(),
                    platform::errors::InvalidArgument(
                        "the size of ctx outputs size (%d) must be larger than "
                        "the size of kernel output_defs (%d).",
                        output_size, output_defs.size()));

  PADDLE_ENFORCE_EQ(attr_size, attribute_defs.size(),
                    platform::errors::InvalidArgument(
                        "the size of ctx attribute size (%d) must be equal to "
                        "to the size of kernel attribute_defs (%d).",
                        attr_size, attribute_defs.size()));

  VLOG(3) << "[CUSTOM KERNEL] Input num: " << input_defs.size()
          << "[tensor size:" << input_size << "]"
          << " Attribute num: " << attribute_defs.size()
          << " Output num: " << output_defs.size()
          << "[tensor size:" << output_size << "].";

  // Inputs mapping
  std::vector<paddle::experimental::Tensor> custom_ins;
  std::vector<std::vector<paddle::experimental::Tensor>> custom_vec_ins;
  for (size_t in_idx = 0; in_idx < input_defs.size(); ++in_idx) {
    VLOG(3) << "Mapping Input[" << in_idx << "]";
    const std::pair<int, int> range = ctx->InputRangeAt(in_idx);

    // is_vector tells if this Input is Tensor or std::vector<Tensor>
    if (!input_defs.at(in_idx).is_vector) {
      paddle::experimental::Tensor custom_t;
      auto& ctx_tensor = ctx->InputAt<phi::DenseTensor>(range.first);
      custom_t.set_impl(std::make_shared<phi::DenseTensor>(ctx_tensor));
      custom_ins.emplace_back(custom_t);
    } else {
      std::vector<paddle::experimental::Tensor> custom_vec_in;
      auto ctx_tensor_vec =
          ctx->MoveInputsBetween<phi::DenseTensor>(range.first, range.second);
      for (auto& ctx_tensor : ctx_tensor_vec) {
        paddle::experimental::Tensor custom_t;
        custom_t.set_impl(std::make_shared<phi::DenseTensor>(ctx_tensor));
        custom_vec_in.emplace_back(custom_t);
      }
      custom_vec_ins.emplace_back(custom_vec_in);
    }
    VLOG(3) << "Mapped Input[" << in_idx << "] with range[" << range.first
            << "," << range.second << ").";
  }

  // Attributes mapping
  std::vector<paddle::any> custom_attrs;
  for (size_t attr_idx = 0; attr_idx < attribute_defs.size(); ++attr_idx) {
    VLOG(3) << "Mapping Attribute[" << attr_idx << "]";
    if (attribute_defs[attr_idx].type_index == std::type_index(typeid(bool))) {
      bool arg = ctx->AttrAt<bool>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(int))) {
      int arg = ctx->AttrAt<int>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(float))) {
      float arg = ctx->AttrAt<float>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(double))) {
      double arg = ctx->AttrAt<double>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(int64_t))) {
      int64_t arg = ctx->AttrAt<int64_t>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(phi::dtype::float16))) {
      phi::dtype::float16 arg = ctx->AttrAt<phi::dtype::float16>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(DataType))) {
      DataType arg = ctx->AttrAt<DataType>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(const Scalar&))) {
      const Scalar& arg = ctx->AttrAt<const Scalar&>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(const std::vector<int64_t>&))) {
      const std::vector<int64_t>& arg =
          ctx->AttrAt<const std::vector<int64_t>&>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(const ScalarArray&))) {
      const ScalarArray& arg = ctx->AttrAt<const ScalarArray&>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else if (attribute_defs[attr_idx].type_index ==
               std::type_index(typeid(const std::vector<int>&))) {
      const std::vector<int>& arg =
          ctx->AttrAt<const std::vector<int>&>(attr_idx);
      custom_attrs.emplace_back(arg);
    } else {
      PADDLE_THROW(platform::errors::Unimplemented(
          "Unsupported attribute attribute_defs[%d].type_index", attr_idx));
    }
    VLOG(3) << "Mapped Attribute[" << attr_idx << "]";
  }

  // Outputs mapping
  std::vector<paddle::experimental::Tensor*> custom_outs;
  std::vector<std::vector<paddle::experimental::Tensor*>> custom_vec_outs;
  std::vector<std::shared_ptr<phi::DenseTensor>> custom_outs_ptr;
  std::vector<std::vector<std::shared_ptr<phi::DenseTensor>>>
      custom_vec_outs_ptr;

  for (size_t out_idx = 0; out_idx < output_defs.size(); ++out_idx) {
    VLOG(3) << "Mapping Output[" << out_idx << "]";
    const std::pair<int, int> range = ctx->OutputRangeAt(out_idx);

    // is_vector tells if this Output is Tensor or std::vector<Tensor>
    if (!output_defs.at(out_idx).is_vector) {
      auto* ctx_tensor = ctx->MutableOutputAt<phi::DenseTensor>(range.first);
      auto* custom_t = new paddle::experimental::Tensor();
      auto custom_t_ptr = std::make_shared<phi::DenseTensor>(*ctx_tensor);
      custom_t->set_impl(custom_t_ptr);
      custom_outs.emplace_back(custom_t);
      custom_outs_ptr.emplace_back(custom_t_ptr);
    } else {
      std::vector<paddle::experimental::Tensor*> custom_vec_out;
      std::vector<std::shared_ptr<phi::DenseTensor>> custom_vec_out_ptr;
      auto ctx_tensor_vec = ctx->MutableOutputBetween<phi::DenseTensor>(
          range.first, range.second);
      for (auto ctx_tensor : ctx_tensor_vec) {
        auto* custom_t = new paddle::experimental::Tensor();
        auto custom_t_ptr = std::make_shared<phi::DenseTensor>(*ctx_tensor);
        custom_t->set_impl(custom_t_ptr);
        custom_vec_out.emplace_back(custom_t);
        custom_vec_out_ptr.emplace_back(custom_t_ptr);
      }
      custom_vec_outs.emplace_back(custom_vec_out);
      custom_vec_outs_ptr.emplace_back(custom_vec_out_ptr);
    }
    VLOG(3) << "Mapped Output[" << out_idx << "] with range[" << range.first
            << "," << range.second << ").";
  }

  // DeviceContext
  // In pten, the first paramter XXContext is decided when registering
  // through template param, but custom kernel function use unified
  // DeviceContext as first parameter of user_kernel_fn, we use backend
  // from OpKernelInfo to decide XXContext. In temporary simple
  // DeviceContext, we just set necessary info to dev_ctx(such as stream
  // in NPUContext), more related work should be done when
  // phi::DeviceContext is exposed to outer.
  DeviceContext dev_ctx;
  auto& backend = OpKernelInfoHelper::GetBackend(op_kernel_info);
  if (backend == phi::Backend::CPU) {
    // do nothing
  } else {
#ifdef PADDLE_WITH_CUSTOM_DEVICE
    size_t device_type_id_ = static_cast<size_t>(backend) -
                             static_cast<size_t>(phi::Backend::ALL_BACKEND);
    std::string device_type = phi::GetGlobalDeviceType(device_type_id_);
    if (!device_type.empty()) {
      auto custom_ctx =
          ctx->GetDeviceContext<paddle::platform::CustomDeviceContext>();
      dev_ctx.set_stream(custom_ctx.stream());
      return;
    }
#endif
    LOG(ERROR) << "[CUSTOM KERNEL] Unsupported kernel backend: " << backend
               << " with compiled Paddle.";
    return;
  }

  auto& user_kernel_fn = OpKernelInfoHelper::GetKernelFn(op_kernel_info);
  // call user function
  user_kernel_fn(dev_ctx, custom_ins, custom_vec_ins, custom_attrs,
                 &custom_outs, &custom_vec_outs);

  VLOG(3) << "[CUSTOM KERNEL] finished call user kernel function.";

  // NOTE: Map back the output tensors with stored shared_ptrs.
  for (int out_idx = output_defs.size() - 1; out_idx >= 0; --out_idx) {
    VLOG(3) << "Mapping Back Output[" << out_idx << "]";
    const std::pair<int, int> range = ctx->OutputRangeAt(out_idx);

    // is_vector tells if this Output is Tensor or std::vector<Tensor>
    if (!output_defs.at(out_idx).is_vector) {
      auto* ctx_tensor = ctx->MutableOutputAt<phi::DenseTensor>(range.first);
      *ctx_tensor = *(custom_outs_ptr.back().get());
      custom_outs_ptr.pop_back();
    } else {
      auto ctx_tensor_vec = ctx->MutableOutputBetween<phi::DenseTensor>(
          range.first, range.second);
      auto custom_vec_ptr_out = custom_vec_outs_ptr.back();
      for (int idx = ctx_tensor_vec.size() - 1; idx >= 0; --idx) {
        *(ctx_tensor_vec[idx]) = *(custom_vec_ptr_out.back().get());
        custom_vec_ptr_out.pop_back();
      }
      custom_vec_outs_ptr.pop_back();
    }
    VLOG(3) << "Mapped Output[" << out_idx << "] with range[" << range.first
            << "," << range.second << "].";
  }

  // delete newed paddle::Tensor for outputs while calling user kernel function
  for (size_t i = 0; i < custom_outs.size(); ++i) {
    delete custom_outs[i];
  }
  for (size_t i = 0; i < custom_vec_outs.size(); ++i) {
    for (size_t j = 0; j < custom_vec_outs[i].size(); ++j) {
      delete custom_vec_outs[i][j];
    }
  }
}

void RegisterKernelWithMetaInfo(
    const std::vector<OpKernelInfo>& op_kernel_infos) {
  for (size_t i = 0; i < op_kernel_infos.size(); ++i) {
    auto& kernel_info = op_kernel_infos[i];
    auto op_type = OpKernelInfoHelper::GetOpName(kernel_info);
    auto kernel_key = OpKernelInfoHelper::GetKernelKey(kernel_info);

    VLOG(3) << "[CUSTOM KERNEL] registering [" << op_type << "]" << kernel_key;

    // 1.Check whether this kernel is valid for a specific operator
    PADDLE_ENFORCE_EQ(
        phi::KernelFactory::Instance().HasCompatiblePtenKernel(op_type), true,
        platform::errors::InvalidArgument(
            "[CUSTOM KERNEL] %s is not ready for custom kernel registering.",
            op_type));

    // 2.Check whether kernel_key has been already registed
    PADDLE_ENFORCE_EQ(
        phi::KernelFactory::Instance().kernels()[op_type].find(kernel_key),
        phi::KernelFactory::Instance().kernels()[op_type].end(),
        platform::errors::InvalidArgument(
            "[CUSTOM KERNEL] The operator <%s>'s kernel: %s has been "
            "already existed in Paddle, please contribute PR if need "
            "to optimize the kernel code. Custom kernel do NOT support "
            "to replace existing kernel in Paddle.",
            op_type, kernel_key));

    // phi::KernelFn
    phi::KernelFn kernel_fn = [kernel_info](phi::KernelContext* ctx) {
      VLOG(3) << "[CUSTOM KERNEL] run custom PTEN kernel func in lambda.";
      RunKernelFunc(ctx, kernel_info);
    };
    // variadic_kernel_fn
    void* variadic_kernel_fn =
        OpKernelInfoHelper::GetVariadicKernelFn(kernel_info);
    phi::Kernel kernel(kernel_fn, variadic_kernel_fn);
    // args info
    ParseArgs(kernel_info, kernel.mutable_args_def());
    // register custom kernel to phi::KernelFactory
    phi::KernelFactory::Instance().kernels()[op_type][kernel_key] = kernel;
    VLOG(3) << "[CUSTOM KERNEL] Successed in registering operator <" << op_type
            << ">'s kernel " << kernel_key << " to Paddle. "
            << "It will be used like native ones.";
  }
}

void RegisterKernelWithMetaInfoMap(
    const paddle::OpKernelInfoMap& op_kernel_info_map) {
  auto& kernel_info_map = op_kernel_info_map.GetMap();
  VLOG(3) << "[CUSTOM KERNEL] size of op_kernel_info_map: "
          << kernel_info_map.size();

  // pair: {op_type, OpKernelInfo}
  for (auto& pair : kernel_info_map) {
    VLOG(3) << "[CUSTOM KERNEL] pair first -> op name: " << pair.first;
    RegisterKernelWithMetaInfo(pair.second);
  }
}

void LoadCustomKernelLib(const std::string& dso_lib_path, void* dso_handle) {
#ifdef _LINUX
  typedef OpKernelInfoMap& get_op_kernel_info_map_t();
  auto* func = reinterpret_cast<get_op_kernel_info_map_t*>(
      dlsym(dso_handle, "PD_GetOpKernelInfoMap"));

  if (func == nullptr) {
    LOG(WARNING) << "Skipped lib [" << dso_lib_path << "]: fail to find "
                 << "PD_GetOpKernelInfoMap symbol in this lib.";
    return;
  }
  auto& op_kernel_info_map = func();
  RegisterKernelWithMetaInfoMap(op_kernel_info_map);
  LOG(INFO) << "Successed in loading custom kernels in lib: " << dso_lib_path;
#else
  VLOG(3) << "Unsupported: Custom kernel is only implemented on Linux.";
#endif
  return;
}

}  // namespace framework
}  // namespace paddle
