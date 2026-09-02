#ifndef ENGINE_CORE_HPP
#define ENGINE_CORE_HPP

// 引擎核心：从 main.cpp 抽出的进程无关逻辑（ServerOptions/createPipeline/
// encodeResultImage），供 HTTP 壳（main.cpp）与 JNI 桥（src_jni/jni_bridge.cpp）
// 共用。所有函数出错抛 std::runtime_error，由调用方决定退出还是回传错误。

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "Pipeline.hpp"
#include "PipelineAnima.hpp"
#include "PipelineSd15Cpu.hpp"
#include "PipelineSd15Npu.hpp"
#include "PipelineSdxl.hpp"
#include "SDUtils.hpp"
#include "TextEncoder.hpp"

// The server runs exactly one of three fixed model formats, selected by
// --type. Each format implies the full file layout under --model_dir, the
// diffusion backend (MNN vs QNN), and the CLIP pipeline; nothing else is
// configurable per component.
//   sd15cpu: tokenizer.json clip_v2.mnn pos_emb.bin token_emb.bin
//            unet.mnn vae_encoder.mnn vae_decoder.mnn
//   sd15npu: tokenizer.json clip_v2.mnn pos_emb.bin token_emb.bin
//            unet.bin vae_encoder.bin vae_decoder.bin [+resolution patches]
//   sdxl:    tokenizer.json clip.mnn pos_emb.bin token_emb.bin clip_2.mnn
//            pos_emb_2.bin token_emb_2.bin unet.bin vae_encoder.bin
//            vae_decoder.bin
//   anima:   tokenizer.json tokenizer_t5.json token_emb.bin clip.bin
//            unet_part1.bin unet_part2.bin vae_decoder.bin
//            [vae_encoder.bin] (optional; enables img2img/inpaint)
// SD15/SDXL CLIP runs on MNN (CPU); Anima's CLIP (clip.bin) runs on QNN/HTP
// (the C++ side still does the qwen token_emb lookup -> input_embedding).
struct ServerOptions {
  enum class ModelType { kSd15Cpu, kSd15Npu, kSdxl, kAnima };

  int port = 8081;
  std::string listen_address = "127.0.0.1";
  ModelType type = ModelType::kSd15Npu;
  std::string model_dir;
  std::string lib_dir;
  std::string patch_path;
  std::string safety_checker_path;
  float nsfw_threshold = 0.5f;
  bool use_v_pred = false;
  bool no_img2img = false;  // skip the VAE encoder entirely
  bool lowram = false;
  bool anima_seq_dit = false;  // (anima+lowram) never co-resident DiT halves
  bool upscaler_mode = false;
  bool convert_mode = false;
  bool convert_clip_skip_2 = false;

  bool isSdxl() const { return type == ModelType::kSdxl; }
  bool isAnima() const { return type == ModelType::kAnima; }
  bool isMnn() const { return type == ModelType::kSd15Cpu; }
};

// Verifies the fixed per-type file layout under --model_dir and constructs
// the matching pipeline (not yet initialized).
inline std::unique_ptr<Pipeline> createPipeline(const ServerOptions &opts,
                                                TextEncoder &text_encoder) {
  const std::filesystem::path dir(opts.model_dir);
  const bool sdxl = opts.isSdxl();
  const bool anima = opts.isAnima();

  // Anima: Qwen "CLIP" (clip.bin, QNN) + split DiT (unet_part1/2.bin) + 16-ch
  // VAE. The Qwen text encoder uses RoPE internally, so there is no
  // pos_emb.bin.
  if (anima) {
    std::string clip_path = (dir / "clip.bin").string();
    std::string unet_part1_path = (dir / "unet_part1.bin").string();
    std::string unet_part2_path = (dir / "unet_part2.bin").string();
    std::string vae_decoder_path = (dir / "vae_decoder.bin").string();
    std::string vae_encoder_path =
        opts.no_img2img ? "" : (dir / "vae_encoder.bin").string();

    std::vector<std::string> required = {
        (dir / "tokenizer.json").string(),
        (dir / "tokenizer_t5.json").string(),
        clip_path,
        unet_part1_path,
        unet_part2_path,
        vae_decoder_path,
        (dir / "token_emb.bin").string(),
    };
    if (!vae_encoder_path.empty()) required.push_back(vae_encoder_path);
    for (const auto &p : required) {
      if (!std::filesystem::exists(p))
        throw std::runtime_error("File not found: " + p);
    }
    return std::make_unique<PipelineAnima>(
        text_encoder, opts.model_dir, clip_path, unet_part1_path,
        unet_part2_path, vae_decoder_path, vae_encoder_path, opts.lowram,
        opts.anima_seq_dit);
  }

  const std::string ext = opts.isMnn() ? ".mnn" : ".bin";
  std::string clip_path = (dir / (sdxl ? "clip.mnn" : "clip_v2.mnn")).string();
  std::string clip2_path = sdxl ? (dir / "clip_2.mnn").string() : "";
  std::string unet_path = (dir / ("unet" + ext)).string();
  std::string vae_decoder_path = (dir / ("vae_decoder" + ext)).string();
  std::string vae_encoder_path =
      opts.no_img2img ? "" : (dir / ("vae_encoder" + ext)).string();

  std::vector<std::string> required = {
      (dir / "tokenizer.json").string(),
      clip_path,
      unet_path,
      vae_decoder_path,
      (dir / "pos_emb.bin").string(),
      (dir / "token_emb.bin").string(),
  };
  if (!vae_encoder_path.empty()) required.push_back(vae_encoder_path);
  if (sdxl) {
    required.push_back(clip2_path);
    required.push_back((dir / "pos_emb_2.bin").string());
    required.push_back((dir / "token_emb_2.bin").string());
  }
  for (const auto &p : required) {
    if (!std::filesystem::exists(p))
      throw std::runtime_error("File not found: " + p);
  }

  switch (opts.type) {
    case ServerOptions::ModelType::kSd15Cpu:
      return std::make_unique<PipelineSd15Cpu>(
          text_encoder, opts.model_dir, clip_path, unet_path, vae_decoder_path,
          vae_encoder_path, opts.use_v_pred);
    case ServerOptions::ModelType::kSd15Npu:
      return std::make_unique<PipelineSd15Npu>(
          text_encoder, opts.model_dir, clip_path, unet_path, vae_decoder_path,
          vae_encoder_path, opts.patch_path, opts.use_v_pred);
    case ServerOptions::ModelType::kSdxl:
    default:
      return std::make_unique<PipelineSdxl>(
          text_encoder, opts.model_dir, clip_path, clip2_path, unet_path,
          vae_decoder_path, vae_encoder_path, opts.use_v_pred, opts.lowram);
  }
}

// Encodes the final image per the requested wire format and wraps it base64.
inline std::string encodeResultImage(const GenerationResult &result,
                                     const std::string &format) {
  if (format == "jpeg") {
    auto jpeg = encodeJPEG(result.image_data, result.width, result.height, 95);
    return base64_encode(std::string(jpeg.begin(), jpeg.end()));
  }
  if (format == "png") {
    auto png = encodePNG(result.image_data, result.width, result.height);
    return base64_encode(std::string(png.begin(), png.end()));
  }
  return base64_encode(
      std::string(result.image_data.begin(), result.image_data.end()));
}

#endif  // ENGINE_CORE_HPP
