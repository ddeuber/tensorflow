/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_GPU_GPU_COMPILER_H_
#define XLA_SERVICE_GPU_GPU_COMPILER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "llvm/IR/Module.h"
#include "xla/autotune_results.pb.h"
#include "xla/hlo/ir/hlo_module.h"
#include "xla/hlo/ir/hlo_module_group.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/compiler.h"
#include "xla/service/executable.h"
#include "xla/service/gpu/autotuner_util.h"
#include "xla/service/gpu/buffer_sharing.h"
#include "xla/service/gpu/compile_module_to_llvm_ir.h"
#include "xla/service/gpu/executable.pb.h"
#include "xla/service/gpu/gpu_executable.h"
#include "xla/service/hlo.pb.h"
#include "xla/service/hlo_cost_analysis.h"
#include "xla/service/hlo_dataflow_analysis.h"
#include "xla/service/hlo_module_config.h"
#include "xla/service/hlo_pass_pipeline.h"
#include "xla/service/llvm_compiler.h"
#include "xla/status.h"
#include "xla/statusor.h"
#include "xla/stream_executor/device_description.h"
#include "xla/stream_executor/device_description.pb.h"
#include "xla/stream_executor/dnn.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/util.h"
#include "xla/xla.pb.h"
#include "tsl/platform/threadpool.h"

namespace xla {
namespace gpu {

// The GPU compiler generates efficient GPU executables.
class GpuCompiler : public LLVMCompiler {
 public:
  GpuCompiler(se::Platform::Id platform_id, const char* target_triple,
              const char* data_layout);

  using LLVMCompiler::Compile;

  // An attached device is passed in via stream_exec. We get GPU configuration
  // from the attached device OR from the `options` struct (in which case the
  // attached device is ignored during the compilation).
  // If you call this directly, follow it with RunBackend rather than Compile.
  StatusOr<std::unique_ptr<HloModule>> RunHloPasses(
      std::unique_ptr<HloModule> module, se::StreamExecutor* stream_exec,
      const CompileOptions& options) override;

  StatusOr<std::unique_ptr<BufferAssignment>> AssignBuffers(
      HloModule* hlo_module, const se::StreamExecutor* stream_exec) override;

  StatusOr<std::unique_ptr<Executable>> RunBackend(
      std::unique_ptr<HloModule> module, se::StreamExecutor* stream_exec,
      const CompileOptions& options) override;

  StatusOr<std::vector<std::unique_ptr<AotCompilationResult>>>
  CompileAheadOfTime(std::unique_ptr<HloModuleGroup> module_group,
                     AotCompilationOptions const& options) override;

  se::Platform::Id PlatformId() const override { return platform_id_; }

  HloCostAnalysis::ShapeSizeFunction ShapeSizeBytesFunction() const override;

  // Returns a (deserialized) AotCompilationResult from a serialized
  // AotCompilationResult.
  StatusOr<std::unique_ptr<AotCompilationResult>> LoadAotCompilationResult(
      const std::string& serialized_aot_result) override;

  // Stateless version of the same function.
  static StatusOr<std::unique_ptr<AotCompilationResult>>
  LoadAotCompilationResultStatic(const std::string& serialized_aot_result);

  StatusOr<std::unique_ptr<AotCompilationResult>> Export(
      Executable* executable) const override;

  Status RunPostSchedulingPipelines(
      HloModule* module, int64_t scheduler_mem_limit,
      const se::DeviceDescription& gpu_device_info) const;

  std::string target_triple() const { return target_triple_; }
  std::string data_layout() const { return data_layout_; }

 protected:
  struct BackendCompileResult {
    std::string asm_text;
    std::vector<uint8_t> binary;
  };

  // During compilation with device, stream_exec != null and autotune_results
  // == null. During deviceless AOT compilation, stream_exec == null and
  // autotune_results != null.
  // thread_pool is used to speed up compilation during autotuning.
  virtual Status OptimizeHloPostLayoutAssignment(
      HloModule* hlo_module, se::StreamExecutor* stream_exec,
      const CompileOptions& options, const TargetConfig& gpu_target_config,
      tsl::thread::ThreadPool* thread_pool);

  // CollectivesScheduleLinearizer enforces a total ordering between collectives
  // to work around divergence in executables introduced due to auto tuning,
  // specifically the use of extra scratch space for convolutions. This
  // function decided whether to apply this pass. If convolutions are present in
  // the code and we are using "online" autotuning (i.e., not AOT) we need to
  // use the pass, else we do not need to enable the pass.
  virtual bool RequiresCollectiveScheduleLinearizer(
      const HloModule* module, se::StreamExecutor* stream_exec) {
    return false;
  }

  // Add autotuning passes for convolution and gemm (except triton).
  virtual Status AddConvAndGemmAutotuningPasses(
      HloPassPipeline* pipeline, HloModule* hlo_module,
      AutotuneConfig& autotune_config, tsl::thread::ThreadPool* thread_pool) {
    return OkStatus();
  }

  // Add autotuning passes for triton gemm.
  virtual Status AddTritonGemmAutotuningPasses(
      HloPassPipeline* pipeline, HloModule* hlo_module,
      AutotuneConfig& autotune_config, tsl::thread::ThreadPool* thread_pool) {
    return OkStatus();
  }

  // Add passes that convert HLO operations to custom kernels.
  virtual Status AddCustomKernelReplacementPasses(
      HloPassPipeline* pipeline, const DebugOptions& debug_options) {
    return OkStatus();
  }

 private:
  struct CompileResultWithMetadata {
    BackendCompileResult backend_result;
    CompileModuleResults compile_module_results;
  };

  // Schedule and compile the module.
  StatusOr<CompileResultWithMetadata> CompileToBackendResult(
      HloModule* module, llvm::LLVMContext* llvm_context,
      se::StreamExecutor* executor, const CompileOptions& options,
      const se::DeviceDescription& gpu_device_info);

  StatusOr<BackendCompileResult> CompileToTargetBinary(
      const HloModuleConfig& module_config, llvm::Module* llvm_module,
      se::GpuComputeCapability gpu_version, se::StreamExecutor* stream_exec,
      const CompileOptions& options, const HloModule* debug_module);

  StatusOr<BackendCompileResult> CompileSingleModule(
      const HloModuleConfig& module_config,
      se::GpuComputeCapability gpu_version, const HloModule* debug_module,
      llvm::Module* llvm_module, bool relocatable,
      const CompileOptions& options, std::optional<int> shard_number);

  Status LoadAutotuneResultsFromFile(const DebugOptions& debug_options);
  Status SerializeAutotuneResultsToFile(const DebugOptions& debug_options);

  // During compilation with device, stream_exec != null and autotune_results
  // == null. During deviceless AOT compilation, stream_exec == null and
  // autotune_results != null.
  Status OptimizeHloModule(HloModule* hlo_module,
                           se::StreamExecutor* stream_exec,
                           const CompileOptions& options,
                           const TargetConfig& gpu_target_config);

  virtual Status OptimizeHloConvolutionCanonicalization(
      HloModule* hlo_module, se::GpuComputeCapability gpu_version,
      se::dnn::VersionInfo dnn_version,
      se::DeviceMemoryAllocator* device_allocator) = 0;

  virtual HloDataflowAnalysis::CanShareBuffer GetCanShareBuffer() const {
    return &FusionCanShareBufferHint;
  }

  // TODO(timshen): Replace `debug_module` with some portable debug information
  // that accommodates both HLO and MLIR.
  virtual StatusOr<BackendCompileResult> CompileTargetBinary(
      const HloModuleConfig& module_config, llvm::Module* llvm_module,
      se::GpuComputeCapability gpu_version, bool relocatable,
      const HloModule* debug_module, const CompileOptions& options) = 0;

  Status PrepareHloModuleForIrEmitting(HloModule* hlo_module);

  virtual StatusOr<bool> CanUseLinkModules(const HloModuleConfig& config) {
    return false;
  }

  virtual StatusOr<std::vector<uint8_t>> LinkModules(
      se::StreamExecutor* stream_exec,
      std::vector<std::vector<uint8_t>> modules,
      const DebugOptions& debug_options) {
    return Unimplemented("LinkModules is not implemented.");
  }

  se::Platform::Id platform_id_;

  // The triple that represents our target.
  const char* target_triple_;

  // The data layout of the emitted module.
  const char* data_layout_;

  // The size in bytes of a pointer. Used by ShapeSizeBytesFunction.
  const int64_t pointer_size_;

  GpuCompiler(const GpuCompiler&) = delete;
  GpuCompiler& operator=(const GpuCompiler&) = delete;
};

}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_GPU_COMPILER_H_
