/*
 * Copyright (c) Glow Contributors. See CONTRIBUTORS file.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "InferenceContext.h"
#include "DebugMacros.h"
#include "Importer.h"
#include "NNPI.h"
#include "NNPIDeviceManager.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <iomanip>
#include <sstream>

namespace glow {
namespace runtime {

InferenceContext::InferenceContext()
    : nnpiNetwork_(NNPI_INVALID_NNPIHANDLE), device_(NNPI_INVALID_NNPIHANDLE),
      inferCmd_(NNPI_INVALID_NNPIHANDLE), commandList_(NNPI_INVALID_NNPIHANDLE),
      deviceTracing_(nullptr), deviceOptions_(nullptr) {}

InferenceContext::~InferenceContext() {
  if (deviceOptions_ && deviceOptions_->inferOnDevice) {
    if (commandList_ != NNPI_INVALID_NNPIHANDLE) {
      LOG_NNPI_INF_IF_ERROR(nnpiCommandListDestroy(commandList_),
                            "Failed to destroy NNPI command list");
    }
    LOG_NNPI_INF_IF_ERROR(nnpiInferCommandDestroy(inferCmd_),
                          "Failed to destroy NNPI inference command");
  }
}

bool InferenceContext::init(
    const ResourceDescVec &inputs, const ResourceDescVec &outputs,
    // For ICE-Ref path.
    NNPINetwork network, NNPICompilationConfig config,
    // For ICE-T path.
    NNPIDeviceNetwork deviceNetwork, NNPIAdapter adapter,
    NNPIDeviceContext device,
    const std::unordered_set<const Placeholder *> &partialInputs,
    const std::unordered_set<const Placeholder *> &staticInputs,
    std::shared_ptr<NNPIDeviceTracing> deviceTracing,
    StaticPlaceholderMap *staticPlaceholderMap,
    std::shared_ptr<NNPIDeviceOptions> deviceOptions,
    const std::string &functionName, unsigned deviceId,
    PlaceholderUsageMap *phUsage) {
  deviceOptions_ = deviceOptions;
  deviceId_ = deviceId;
  nnpiNetwork_ = network;
  device_ = device;
  compilationConfig_ = config;
  partialInputs_ = &partialInputs;
  deviceTracing_ = deviceTracing;
  functionName_ = functionName;

  // Initialize trace context titles with device ID.
  std::stringstream deviceInfo;
  deviceInfo << "[Device #" << deviceId_ << "] ";
  traceBackendExecuteContextName_ = deviceInfo.str() + TRACING_BACKEND_EXECUTE;
  tracePreProcessContextName_ = deviceInfo.str() + TRACING_PRE_PROCESS;
  traceInferenceContextName_ = deviceInfo.str() + TRACING_INFERENCE;
  tracePostProcessContextName_ = deviceInfo.str() + TRACING_POST_PROCESS;

  LOG_AND_RETURN_IF(ERROR, staticPlaceholderMap == nullptr,
                    "InferenceContext Init was called with an invalid "
                    "staticPlaceholderMap",
                    false);

  /// Map from names to their Placeholders.
  std::unordered_map<std::string, const Placeholder *> staticPlaceholders;
  for (auto staticInput : staticInputs) {
    staticPlaceholders[staticInput->getName().str()] = staticInput;
    staticInputs_.insert(staticInput);
  }

  const size_t numInputs = inputs.size();
  const size_t numOutputs = outputs.size();

  if (!deviceOptions_->inferOnDevice) {
    // No P2P/DRT for ICE-Ref (everything is on the host).
    for (auto &in : inputs) {
      const auto *name = in.first.c_str();
      const auto &desc = in.second;
      const bool isStaticInput = staticPlaceholders.count(name) != 0;
      if (isStaticInput) {
        // Treat as a static input.
        auto PH = staticPlaceholders.at(name);
        if (staticPlaceholderMap->count(PH) &&
            staticPlaceholderMap->at(PH).lock()) {
          // Static placeholder already exists.
          inputResources_.push_back(staticPlaceholderMap->at(PH).lock());
        } else {
          // Create a new static placeholder.
          inputResources_.emplace_back(std::make_shared<NNPIResource>());
          LOG_AND_RETURN_IF(
              ERROR,
              !inputResources_.back()->init(
                  name, deviceOptions_, adapter, device_, &desc,
                  NNPIResource::ResourceUsage::StaticInputResource),
              "Failed to init static input resource", false);
          staticPlaceholderMap->insert({PH, inputResources_.back()});
        }
      } else {
        inputResources_.emplace_back(std::make_shared<NNPIResource>());
        LOG_AND_RETURN_IF(ERROR,
                          !inputResources_.back()->init(
                              name, deviceOptions_, adapter, device_, &desc,
                              NNPIResource::ResourceUsage::InputResource),
                          "Failed to init input resource", false);
      }
    }
    for (auto &out : outputs) {
      const auto *name = out.first.c_str();
      const auto &desc = out.second;
      LOG_AND_RETURN_IF(
          ERROR, !deviceOptions_->useIceT && staticPlaceholders.count(name),
          "ICE-Ref doesn't support static outputs", false);
      outputResources_.emplace_back(std::make_shared<NNPIResource>());
      LOG_AND_RETURN_IF(ERROR,
                        !outputResources_.back()->init(
                            name, deviceOptions_, adapter, device_, &desc,
                            NNPIResource::ResourceUsage::OutputResource),
                        "Failed to init input resource", false);
    }

    return true; // Nothing else to be done here for ice-ref.
  }

  // Create resources for inputs.
  for (auto &in : inputs) {
    const auto *name = in.first.c_str();
    const auto &desc = in.second;

    LOG_AND_RETURN_IF(ERROR,
                      phUsage &&
                          ((phUsage->count(name) == 0) ||
                           (phUsage->at(name).devices.count(device_) == 0)),
                      "Invalid placheholder usage for input resource", false);

    const auto isStaticInput = staticPlaceholders.count(name);
    if (isStaticInput) {
      // Treat as a static input.
      auto PH = staticPlaceholders.at(name);
      if (staticPlaceholderMap->count(PH) &&
          staticPlaceholderMap->at(PH).lock()) {
        // Static placeholder already exists.
        inputResources_.push_back(staticPlaceholderMap->at(PH).lock());
      } else {
        // Create a new static placeholder.
        inputResources_.emplace_back(std::make_shared<NNPIResource>());
        LOG_AND_RETURN_IF(ERROR,
                          !inputResources_.back()->init(
                              name, deviceOptions_, adapter, device_, &desc,
                              NNPIResource::ResourceUsage::StaticInputResource),
                          "Failed to init static input resource", false);
        staticPlaceholderMap->insert({PH, inputResources_.back()});
      }
    } else {
      // Dynamic input resource - create it here.
      NNPIResource::ResourceUsage usage = NNPIResource::ResourceUsage::None;
      if (!phUsage || (phUsage->at(name).numWriters == 0)) {
        usage = NNPIResource::ResourceUsage::InputResource; // Net input
      } else { // Some other context is writing to this placeholder --> P2P/DRT
        switch (phUsage->at(name).devices.size()) {
        case 1: // DRT
          usage = NNPIResource::ResourceUsage::DRTInput;
          break;
        case 2: // P2P
          usage = NNPIResource::ResourceUsage::P2PInput;
          break;
        default:
          LOG_AND_RETURN_IF(ERROR, true,
                            "Invalid number of devices accessing a resource",
                            false);
        }
      }
      inputResources_.emplace_back(std::make_shared<NNPIResource>());
      LOG_AND_RETURN_IF(ERROR,
                        !inputResources_.back()->init(name, deviceOptions_,
                                                      adapter, device_, &desc,
                                                      usage, phUsage),
                        "Failed to init input resource", false);
    }

    // Update placeholder usage.
    if (phUsage) {
      phUsage->at(name).readers.push_back(inputResources_.back());
    }
  }

  // Create resources for outputs.
  for (auto &out : outputs) {
    const auto *name = out.first.c_str();
    const auto &desc = out.second;
    LOG_AND_RETURN_IF(ERROR,
                      phUsage &&
                          ((phUsage->count(name) == 0) ||
                           (phUsage->at(name).devices.count(device_) == 0)),
                      "Invalid placheholder usage for output resource", false);
    NNPIResource::ResourceUsage usage = NNPIResource::ResourceUsage::None;
    if (!phUsage || (phUsage->at(name).numReaders == 0)) {
      usage = NNPIResource::ResourceUsage::OutputResource; // Net output
    } else { // Some other context is writing to this placeholder --> P2P/DRT
      switch (phUsage->at(name).devices.size()) {
      case 1: // DRT
        usage = NNPIResource::ResourceUsage::DRTOutput;
        break;
      case 2: // P2P
        usage = NNPIResource::ResourceUsage::P2POutput;
        break;
      default:
        LOG_AND_RETURN_IF(ERROR, true,
                          "Invalid number of devices accessing a resource",
                          false);
      }
    }
    outputResources_.emplace_back(std::make_shared<NNPIResource>());
    LOG_AND_RETURN_IF(ERROR,
                      !outputResources_.back()->init(name, deviceOptions_,
                                                     adapter, device_, &desc,
                                                     usage, phUsage),
                      "Failed to init output resource", false);

    // Update placeholder usage.
    if (phUsage) {
      phUsage->at(name).writers.push_back(outputResources_.back());
    }
  }
  DBG_MEM_USAGE("Created input and output host resources");

  // Create infer command.
  NNPIDeviceResource inputHandles[numInputs];
  NNPIDeviceResource outputHandles[numOutputs];
  for (uint32_t i = 0; i < numInputs; i++) {
    inputHandles[i] = inputResources_.at(i)->getDeviceResource();
  }
  for (uint32_t i = 0; i < numOutputs; i++) {
    outputHandles[i] = outputResources_.at(i)->getDeviceResource();
  }
  LOG_NNPI_INF_IF_ERROR_RETURN_FALSE(
      nnpiInferCommandCreate(deviceNetwork, inputHandles, numInputs,
                             outputHandles, numOutputs, &inferCmd_),
      "Failed to create NNPI inference command");

  if (deviceOptions_->enabledCommandLists > 0) {
    // Collect copy commands for the list (some resources may not need copying).
    std::vector<NNPICommandHandle> commands;
    std::vector<NNPICopyCommand> inputCopyCmds, outputCopyCmds;
    for (auto &res : inputResources_) {
      auto copyCmd = res->getCopyCommand();
      if (copyCmd) {
        res->setCmdListIdx(static_cast<uint32_t>(commands.size()));
        NNPICommandHandle cmd;
        cmd.type = NNPI_COMMAND_TYPE_COPY;
        cmd.copyCommand = copyCmd;
        commands.push_back(cmd);
      }
    }
    {
      NNPICommandHandle cmd;
      cmd.type = NNPI_COMMAND_TYPE_INFER;
      cmd.inferCommand = inferCmd_;
      commands.push_back(cmd);
    }
    for (auto &res : outputResources_) {
      auto copyCmd = res->getCopyCommand();
      if (copyCmd) {
        res->setCmdListIdx(static_cast<uint32_t>(commands.size()));
        NNPICommandHandle cmd;
        cmd.type = NNPI_COMMAND_TYPE_COPY;
        cmd.copyCommand = copyCmd;
        commands.push_back(cmd);
      }
    }

    // Create command list.
    LOG_NNPI_INF_IF_ERROR_RETURN_FALSE(
        nnpiCommandListCreate(&(commands[0]),
                              static_cast<uint32_t>(commands.size()), nullptr,
                              0, &commandList_),
        "Failed to create NNPI command list");

    // Preallocate enough configs to be used for partials later on.
    cmdConfigs_.resize(commands.size());
    // Preallocate enough errors to be used later durin inference.
    cmdListErrors_.resize(commands.size());
  }

  if (deviceOptions_->dumpRuntime) {
    dumpRuntime();
  }

  return true;
}

void InferenceContext::execute(RunIdentifierTy runId,
                               std::unique_ptr<ExecutionContext> ctx,
                               runtime::ResultCBTy resultCB) {
  TRACE_EVENT_SCOPE(ctx->getTraceContext(), TraceLevel::REQUEST,
                    traceBackendExecuteContextName_);
  if (ctx->getTraceContext()) {
    ctx->getTraceContext()->setThreadName(
        llvm::formatv("Inf ctx - device: {0}: {1}", deviceId_, functionName_)
            .str());
  }
  if (deviceTracing_) {
    deviceTracing_->start(ctx->getTraceContext(), device_);
  }

  // Pre inference input preparation.
  PlaceholderBindings &bindings = *ctx->getPlaceholderBindings();

  // Initialize placeholder lists in the same orders as inputResources_ and
  // outputResources_.
  if (netInputPlaceholders_.empty()) {
    for (const auto &in : inputResources_) {
      if (in->getUsage() == NNPIResource::ResourceUsage::StaticInputResource) {
        continue;
      }
      auto *placeholder = bindings.getPlaceholderByName(in->getName());
      if (!placeholder) {
        netInputPlaceholders_.clear();
        LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(ERROR, placeholder,
                                             "Can't find tensor for input",
                                             runId, ctx, resultCB);
      }

      netInputPlaceholders_.push_back(placeholder);
    }
  }
  if (netOutputPlaceholders_.empty()) {
    for (const auto &out : outputResources_) {
      auto *placeholder = bindings.getPlaceholderByName(out->getName());
      if (!placeholder) {
        netOutputPlaceholders_.clear();
        LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(ERROR, placeholder,
                                             "Can't find tensor for input",
                                             runId, ctx, resultCB);
      }
      netOutputPlaceholders_.push_back(placeholder);
    }
  }

  std::unordered_set<Tensor *> partialTensorInputs;
  for (auto &pht : bindings.pairs()) {
    if (partialInputs_->count(pht.first)) {
      partialTensorInputs.insert(pht.second);
    }
  }
  TRACE_EVENT_BEGIN(ctx->getTraceContext(), TraceLevel::COPY,
                    tracePreProcessContextName_);

  // Pre-inference
  std::vector<void *> rawInputs, rawOutputs;
  unsigned idx = 0;
  for (const auto &in : inputResources_) {
    if (in->getUsage() != NNPIResource::ResourceUsage::StaticInputResource) {
      auto *t = bindings.get(netInputPlaceholders_[idx++]);
      LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(
          ERROR, t, "Can't find tensor for input", runId, ctx, resultCB);
      LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(
          ERROR,
          in->preInference(t, partialTensorInputs.count(t)) ==
              NNPI_INF_NO_ERROR,
          "Failed pre-inference for input", runId, ctx, resultCB);
    }
    rawInputs.push_back(in->getHostPtr());
  }

  // Inference.
  if (deviceOptions_->inferOnDevice) {
    if (deviceOptions_->enabledCommandLists < 1) {
      // No command lists (schedule individual commands).
      TRACE_EVENT_END(ctx->getTraceContext(), TraceLevel::COPY,
                      tracePreProcessContextName_);
      TRACE_EVENT_BEGIN(ctx->getTraceContext(), TraceLevel::OPERATOR,
                        traceInferenceContextName_);
      // Queue inference.
      LOG_AND_CALLBACK_EXECUTE_NNPI_INF_IF_ERROR(
          nnpiInferCommandQueue(inferCmd_, 0), "Failed to queue infer command.",
          runId, ctx, resultCB);

      // Queue output copies
      for (auto &res : outputResources_) {
        auto cmd = res->getCopyCommand();
        if (cmd) {
          // todo: assert no partial output
          LOG_AND_CALLBACK_EXECUTE_NNPI_INF_IF_ERROR(
              nnpiCopyCommandQueue(cmd, nullptr),
              "Failed to queue output copy command.", runId, ctx, resultCB);
        }
      }
    } else { // Use commands lists.
      // Prepare updates for partial copies.
      uint32_t usedConfigs = 0;
      for (auto &res : inputResources_) {
        const auto partialSize = res->getPartialSize();
        if (partialSize > 0) {
          cmdConfigs_[usedConfigs].index = res->getCmdListIdx();
          cmdConfigs_[usedConfigs].type = NNPI_COMMAND_TYPE_COPY;
          cmdConfigs_[usedConfigs].copyConfig.size = partialSize;
          usedConfigs++;
        }
      }

      TRACE_EVENT_END(ctx->getTraceContext(), TraceLevel::COPY,
                      tracePreProcessContextName_);
      TRACE_EVENT_BEGIN(ctx->getTraceContext(), TraceLevel::OPERATOR,
                        traceInferenceContextName_);
      // Queue Command list
      LOG_AND_CALLBACK_EXECUTE_NNPI_INF_IF_ERROR(
          nnpiCommandListQueue(commandList_, &(cmdConfigs_.at(0)), usedConfigs),
          "Failed to queue command list.", runId, ctx, resultCB);

      // Wait on completion and error handling.
      uint32_t numErrors(0);
      // First wait for the command list to complete.
      NNPIInferenceErrorCode res =
          nnpiCommandListWait(commandList_, UINT32_MAX, NULL, 0, &numErrors);
      if (res != NNPI_INF_NO_ERROR) {
        LOG_NNPI_INF_IF_ERROR(res, "Failed to wait on command list");
      } else if (numErrors > 0) {
        LOG(ERROR) << "Errors returned from command list";

        // Errors were generate so we allocate error objects to hold the data.
        NNPICommandListError commandErrors[numErrors];
        LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(
            ERROR, commandErrors, "Failed to allocate command error array",
            runId, ctx, resultCB);
        memset(commandErrors, 0, sizeof(NNPICommandListError) * numErrors);

        // Then query all errors using another wait call (should return
        // immediately).
        NNPIInferenceErrorCode tmpRes = nnpiCommandListWait(
            commandList_, UINT32_MAX, commandErrors, numErrors, &numErrors);
        if (tmpRes != NNPI_INF_NO_ERROR) {
          LOG_NNPI_INF_IF_ERROR(tmpRes,
                                "Failed to wait on command list to get errors");
        } else {
          for (uint32_t i = 0; i < numErrors; i++) {
            LOG(ERROR) << NNPI_INF_ERROR_MSG(commandErrors[i].err,
                                             commandErrors[i].desc);
          }
        }
      }
      if (res != NNPI_INF_NO_ERROR || numErrors > 0) {
        LOG_AND_CALLBACK_EXECUTE_NNPI_INF_IF_ERROR(
            nnpiCommandListClearErrors(commandList_),
            "Failed to clear command list errors", runId, ctx, resultCB);
        LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(
            ERROR, false /* fail */, "Errors found in command list execution",
            runId, ctx, resultCB);
      }
    }
  } else if (!deviceOptions_->useIceT) {
    // Infer on ice-ref.

    for (auto &out : outputResources_) {
      // Collect output ptrs for ICE-Ref
      rawOutputs.push_back(out->getHostPtr());
    }

    TRACE_EVENT_END(ctx->getTraceContext(), TraceLevel::COPY,
                    TRACING_PRE_PROCESS);
    TRACE_EVENT_BEGIN(ctx->getTraceContext(), TraceLevel::OPERATOR,
                      TRACING_INFERENCE);
    LOG_AND_CALLBACK_EXECUTE_NNPI_IF_ERROR(
        nnpiNetworkInferOnHost(nnpiNetwork_, &(rawInputs[0]), rawInputs.size(),
                               &(rawOutputs[0]), rawOutputs.size(),
                               &compilationConfig_, NNPI_INVALID_NNPIHANDLE),
        "Failed NNPI infer (ICE-Ref)", runId, ctx, resultCB);
  } else //! UseInferenceAPI && UseIceT (compile without running).
  {
    // Nothing else to do here.
  }

  TRACE_EVENT_END(ctx->getTraceContext(), TraceLevel::OPERATOR,
                  traceInferenceContextName_);
  TRACE_EVENT_BEGIN(ctx->getTraceContext(), TraceLevel::COPY,
                    tracePostProcessContextName_);

  // Post inference output handling.
  for (unsigned i = 0, e = outputResources_.size(); i < e; ++i) {
    auto *t = bindings.get(netOutputPlaceholders_[i]);
    LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(
        ERROR, t, "Can't find tensor for output", runId, ctx, resultCB);
    LOG_AND_FAIL_EXECUTE_CALLBACK_IF_NOT(
        ERROR, outputResources_[i]->postInference(t) == NNPI_INF_NO_ERROR,
        "Failed in output postInference", runId, ctx, resultCB);
  }

  TRACE_EVENT_END(ctx->getTraceContext(), TraceLevel::COPY,
                  tracePostProcessContextName_);
  if (deviceTracing_) {
    deviceTracing_->stopAndUpdate(ctx->getTraceContext(), device_);
  }
  TRACE_EVENT_SCOPE_END(); // we move context in the line below

  // Invoke CB.
  resultCB(runId, Error::success(), std::move(ctx));
}

void InferenceContext::dumpRuntime() const {
  for (auto &in : inputResources_) {
    std::string resourceType;
    unsigned color = 0;
    switch (in->getUsage()) {
    case NNPIResource::ResourceUsage::InputResource:
      resourceType = "Input";
      color = 2;

      // Add host resource node
      DotWriter::addNode(std::to_string(in->getHostResource()),
                         std::string("Host Resource\\lHandle: ") +
                             DotWriter::getHexStr(in->getHostResource()),
                         1, "Host");
      // Add copy command h2c
      DotWriter::addEdge(std::to_string(in->getHostResource()),
                         std::to_string(in->getDeviceResource()));
      break;
    case NNPIResource::ResourceUsage::StaticInputResource:
      resourceType = "Static Input";
      color = 3;
      break;
    case NNPIResource::ResourceUsage::P2PInput:
      resourceType = "P2P Input";
      color = 4;
      break;
    case NNPIResource::ResourceUsage::DRTInput:
      resourceType = "DRT In/Out";
      color = 5;
      break;
    default:; // do nothing
    }

    // add device resource node
    DotWriter::addNode(std::to_string(in->getDeviceResource()),
                       resourceType + std::string("\\lHandle: ") +
                           DotWriter::getHexStr(in->getDeviceResource()),
                       color, std::to_string(in->getDevice()));

    // connect to function
    DotWriter::addEdge(std::to_string(in->getDeviceResource()),
                       functionName_ + ":" + in->getName());
  }
  for (auto &out : outputResources_) {
    std::string resourceType;
    unsigned color = 0;
    switch (out->getUsage()) {
    case NNPIResource::ResourceUsage::OutputResource:
      resourceType = "Output";
      color = 2;

      // Add host resource node
      DotWriter::addNode(std::to_string(out->getHostResource()),
                         std::string("Host Resource\\lHandle: ") +
                             DotWriter::getHexStr(out->getHostResource()),
                         1, "Host");
      // Add copy command c2h
      DotWriter::addEdge(std::to_string(out->getDeviceResource()),
                         std::to_string(out->getHostResource()));
      break;
    case NNPIResource::ResourceUsage::P2POutput:
      resourceType = "P2P Output";
      color = 4;
      // Add copy command c2c
      DotWriter::addEdge(std::to_string(out->getDeviceResource()),
                         std::to_string(out->getP2PDeviceResource()));
      break;
    case NNPIResource::ResourceUsage::DRTOutput:
      resourceType = "DRT In/Out";
      color = 5;
      break;
    default:; // do nothing
    }

    // add device resource node
    DotWriter::addNode(std::to_string(out->getDeviceResource()),
                       resourceType + std::string("\\lHandle: ") +
                           DotWriter::getHexStr(out->getDeviceResource()),
                       color, std::to_string(out->getDevice()));

    // connect to function
    DotWriter::addEdge(functionName_ + ":" + out->getName(),
                       std::to_string(out->getDeviceResource()));
  }
}

} // namespace runtime
} // namespace glow
