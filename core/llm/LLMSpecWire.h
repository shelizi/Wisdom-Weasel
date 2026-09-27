#pragma once

// LLMLocalModelSpec 與 IPC 訊息欄位互轉（輸入法與推理行程共用）
#include "LLMProvider.h"
#include "../llm_ipc/protocol.h"

inline llm_ipc::ModelSpecFields ToWire(const LLMLocalModelSpec& spec) {
  llm_ipc::ModelSpecFields f;
  f.model_path = spec.model_path;
  f.instruct = spec.instruct;
  f.n_ctx = spec.n_ctx;
  f.n_gpu_layers = spec.n_gpu_layers;
  f.n_threads = spec.n_threads;
  f.disable_thinking = spec.disable_thinking;
  f.think_tokens = spec.think_tokens;
  return f;
}

inline LLMLocalModelSpec FromWire(const llm_ipc::ModelSpecFields& f) {
  LLMLocalModelSpec spec;
  spec.model_path = f.model_path;
  spec.instruct = f.instruct;
  spec.n_ctx = f.n_ctx;
  spec.n_gpu_layers = f.n_gpu_layers;
  spec.n_threads = f.n_threads;
  spec.disable_thinking = f.disable_thinking;
  spec.think_tokens = f.think_tokens;
  return spec;
}
