// MoeApk 扩散引擎 JNI 桥：local-dream 核心 JNI 化（去 httplib 进程壳）。
// 原项目：local-dream（CC BY-NC 4.0）https://github.com/xororz/local-dream
// MoeApk fork：https://github.com/Attect/local-dream-muka
//
// 设计：
// - nativeInit(optionsJson)：等价原 main() 的初始化段（tokenizer/pipeline/
//   embeddings/safety/QNN），options 字段同 ServerOptions。
// - nativeGenerate(requestJson, callback)：等价 POST /generate（请求 JSON 与
//   HTTP 版同构，parseGenerationRequest 复用）；进度与结果经 callback 回传。
// - 生成串行化（g_generation_mutex），nativeCancel 经 g_cancel 在下一步进度
//   回调处抛异常中断（与 HTTP 版"客户端断开即中止"同机制）。
// - 图像载荷全部 base64（与原 SSE 协议一致，Kotlin 层解码）。

#include <jni.h>

#include <android/dlext.h>

#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Config.hpp"
#include "EngineCore.hpp"
#include "MnnUtils.hpp"
#include "PAL/DynamicLoading.hpp"
#include "QnnRuntime.hpp"
#include "RequestParser.hpp"
#include "SDUtils.hpp"
#include "TextEncoder.hpp"

#include "json.hpp"

// QNN logging（SampleApp Logger.hpp 经 EngineCore->Pipeline.hpp 引入）
#include "Logger.hpp"

namespace {

JavaVM *g_vm = nullptr;
std::mutex g_generation_mutex;
std::unique_ptr<TextEncoder> g_text_encoder;
std::unique_ptr<Pipeline> g_pipeline;
MNN::Interpreter *g_safety_interpreter = nullptr;
MNN::Session *g_safety_session = nullptr;
std::atomic<bool> g_cancel{false};

// 从 options JSON 构造 ServerOptions（字段名对齐；type: sd15cpu/sd15npu/sdxl/anima）
ServerOptions parseOptions(const nlohmann::json &j) {
  ServerOptions opts;
  std::string type = j.value("type", "sd15cpu");
  if (type == "sd15cpu")
    opts.type = ServerOptions::ModelType::kSd15Cpu;
  else if (type == "sd15npu")
    opts.type = ServerOptions::ModelType::kSd15Npu;
  else if (type == "sdxl")
    opts.type = ServerOptions::ModelType::kSdxl;
  else if (type == "anima")
    opts.type = ServerOptions::ModelType::kAnima;
  else
    throw std::runtime_error("unknown type: " + type);
  opts.model_dir = j.value("model_dir", "");
  if (opts.model_dir.empty()) throw std::runtime_error("model_dir required");
  opts.lib_dir = j.value("lib_dir", "");
  opts.log_file = j.value("log_file", "");
  opts.host_dir = j.value("host_dir", "");
  opts.aux_dir = j.value("aux_dir", "");
  opts.patch_path = j.value("patch", "");
  opts.safety_checker_path = j.value("safety_checker", "");
  opts.nsfw_threshold = j.value("nsfw_threshold", 0.5f);
  opts.use_v_pred = j.value("use_v_pred", false);
  opts.no_img2img = j.value("no_img2img", false);
  opts.lowram = j.value("lowram", false);
  opts.anima_seq_dit = j.value("anima_seq_dit", false);
  return opts;
}

// 向 Java 回调进度。callback 接口方法：onProgress(IILjava/lang/String;)V
void callProgress(JNIEnv *env, jobject cb, jmethodID mid, int step, int total,
                  const std::string &img_b64) {
  jstring jimg = nullptr;
  if (!img_b64.empty()) jimg = env->NewStringUTF(img_b64.c_str());
  env->CallVoidMethod(cb, mid, step, total, jimg);
  if (jimg) env->DeleteLocalRef(jimg);
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
  g_vm = vm;
  return JNI_VERSION_1_6;
}

/** 初始化引擎。返回 "" 成功，否则错误消息。 */
JNIEXPORT jstring JNICALL
Java_com_moeapk_ai_engine_diffusion_DiffusionEngineImpl_nativeInit(
    JNIEnv *env, jclass, jstring optionsJson) {
  std::lock_guard<std::mutex> lock(g_generation_mutex);
  try {
    if (g_pipeline) {
      return env->NewStringUTF("");  // 已初始化：幂等
    }
    const char *optsStr = env->GetStringUTFChars(optionsJson, nullptr);
    ServerOptions opts = parseOptions(nlohmann::json::parse(optsStr));
    env->ReleaseStringUTFChars(optionsJson, optsStr);

    // App 进程 stdout/stderr 默认丢弃：QNN 初始化诊断依赖引擎的 stdout 日志，
    // 经 log_file 重定向到 App 可读的 cache 目录文件。
    if (!opts.log_file.empty()) {
      std::freopen(opts.log_file.c_str(), "a", stdout);
      std::freopen(opts.log_file.c_str(), "a", stderr);
      setvbuf(stdout, nullptr, _IOLBF, 0);
      setvbuf(stderr, nullptr, _IOLBF, 0);
    }

    if (!qnn::log::initializeLogging()) {
      // 已初始化过则返回 false，忽略
    }

    g_text_encoder =
        std::make_unique<TextEncoder>(opts.isSdxl(), opts.isAnima());
    const std::filesystem::path mdir(opts.model_dir);
    g_text_encoder->loadTokenizer((mdir / "tokenizer.json").string());
    if (opts.isAnima())
      g_text_encoder->loadT5Tokenizer((mdir / "tokenizer_t5.json").string());

    g_pipeline = createPipeline(opts, *g_text_encoder);
    g_text_encoder->loadEmbeddingTables(opts.model_dir);

    // Textual-inversion embeddings live two levels above the model dir.
    std::filesystem::path embeddingsPath =
        std::filesystem::path(opts.model_dir).parent_path().parent_path() /
        "embeddings";
    if (std::filesystem::exists(embeddingsPath)) {
      try {
        g_text_encoder->loadTextualInversions(embeddingsPath.string());
      } catch (const std::exception &e) {
        QNN_WARN("Failed to load embeddings: %s", e.what());
      }
    }

    if (!opts.safety_checker_path.empty()) {
      g_safety_interpreter =
          createMnnInterpreterMmap(opts.safety_checker_path.c_str());
      if (g_safety_interpreter) {
        MnnSessionOptions safety_opts;
        safety_opts.num_threads = 1;
        g_safety_session = createMnnSession(g_safety_interpreter, safety_opts);
        if (g_safety_session) {
          auto input =
              g_safety_interpreter->getSessionInput(g_safety_session, nullptr);
          g_safety_interpreter->resizeTensor(input, {1, 224, 224, 3});
          g_safety_interpreter->resizeSession(g_safety_session);
          g_safety_interpreter->releaseModel();
          g_pipeline->setSafetyChecker(g_safety_interpreter, g_safety_session,
                                       opts.nsfw_threshold);
        }
      }
    }

    if (!opts.isMnn()) {
      if (opts.lib_dir.empty())
        throw std::runtime_error("lib_dir required for QNN model types");
      // host 侧 QNN 链（libQnnHtp/Stub/libcdsprpc→libhidlbase→…）打进 App
      // jniLibs：App 命名空间的 default_library_paths 含 nativeLibraryDir，
      // NEEDED 链在其中闭环（bionic 全局 soinfo 表使命名空间内 NEEDED
      // ld-android.so 命中 linker 自身）。host_dir = nativeLibraryDir。
      // DSP 侧文件（*Skel.so / libQnnHtpVxx.so）经 DSP_LIBRARY_PATH 由
      // fastrpc 读取：lib_dir = 运行时下载的引擎目录。
      setenv("DSP_LIBRARY_PATH", opts.lib_dir.c_str(), 1);
      setenv("ADSP_LIBRARY_PATH", opts.lib_dir.c_str(), 1);
      // aux_dir：文件名非 lib 前缀的依赖（vendor.qti.hardware.dsp@1.0.so）
      // 无法经 APK jniLibs 解压（系统只落盘 lib*.so），App 侧解压到该目录，
      // 此处预载（绝对路径进命名空间后，NEEDED 按 soname 命中）。
      if (!opts.aux_dir.empty()) {
        std::error_code ec;
        for (const auto &e :
             std::filesystem::directory_iterator(opts.aux_dir, ec)) {
          if (e.path().extension() != ".so") continue;
          if (!dlopen(e.path().c_str(), RTLD_NOW | RTLD_GLOBAL))
            QNN_WARN("aux preload %s failed: %s",
                     e.path().filename().string().c_str(), dlerror());
        }
      }
      const std::string host_dir = opts.host_dir.empty() ? opts.lib_dir : opts.host_dir;
      if (!qnn_runtime::init(host_dir))
        throw std::runtime_error("Failed get QNN system func ptrs");
    }

    if (!g_pipeline->initialize()) {
      g_pipeline.reset();
      throw std::runtime_error("Pipeline initialization failed");
    }
    return env->NewStringUTF("");
  } catch (const std::exception &e) {
    g_pipeline.reset();
    g_text_encoder.reset();
    return env->NewStringUTF(e.what());
  }
}

/**
 * 生成（阻塞；在 IO 线程调用）。请求 JSON 同 HTTP /generate。
 * 进度经 callback.onProgress(step, total, previewBase64?)，
 * 结果经 callback.onComplete(resultJson)（image 字段为 base64）。
 * 返回 "" 成功，否则错误消息。
 */
JNIEXPORT jstring JNICALL
Java_com_moeapk_ai_engine_diffusion_DiffusionEngineImpl_nativeGenerate(
    JNIEnv *env, jclass, jstring requestJson, jobject callback) {
  if (!g_pipeline) return env->NewStringUTF("engine not initialized");
  std::lock_guard<std::mutex> lock(g_generation_mutex);
  g_cancel.store(false);
  try {
    const char *reqStr = env->GetStringUTFChars(requestJson, nullptr);
    auto json = nlohmann::json::parse(reqStr);
    env->ReleaseStringUTFChars(requestJson, reqStr);
    auto req = std::make_shared<GenerationRequest>(parseGenerationRequest(
        json, g_pipeline->isSdxl(), g_pipeline->isAnima(),
        g_pipeline->supportsImg2Img(), g_pipeline->supportsUltrafix()));

    jclass cbCls = env->GetObjectClass(callback);
    jmethodID midProgress =
        env->GetMethodID(cbCls, "onProgress", "(IILjava/lang/String;)V");
    jmethodID midComplete =
        env->GetMethodID(cbCls, "onComplete", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cbCls);
    if (!midProgress || !midComplete)
      return env->NewStringUTF("callback methods not found");

    auto result = g_pipeline->generate(
        *req, [&](int s, int t, const std::string &img) {
          if (g_cancel.load()) throw std::runtime_error("cancelled");
          callProgress(env, callback, midProgress, s, t, img);
        });

    std::string enc_img = encodeResultImage(result, req->output_format);
    nlohmann::json c = {
        {"type", "complete"},
        {"image", enc_img},
        {"format", req->output_format},
        {"seed", req->seed},
        {"width", result.width},
        {"height", result.height},
        {"channels", result.channels},
        {"generation_time_ms", result.generation_time_ms},
        {"first_step_time_ms", result.first_step_time_ms}};
    jstring jres = env->NewStringUTF(c.dump().c_str());
    env->CallVoidMethod(callback, midComplete, jres);
    env->DeleteLocalRef(jres);
    return env->NewStringUTF("");
  } catch (const std::exception &e) {
    return env->NewStringUTF(e.what());
  }
}

/** 请求取消当前生成（在下一步进度回调处生效）。 */
JNIEXPORT void JNICALL
Java_com_moeapk_ai_engine_diffusion_DiffusionEngineImpl_nativeCancel(
    JNIEnv *, jclass) {
  g_cancel.store(true);
}

/** 释放引擎（模型卸载）。 */
JNIEXPORT void JNICALL
Java_com_moeapk_ai_engine_diffusion_DiffusionEngineImpl_nativeRelease(
    JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(g_generation_mutex);
  g_pipeline.reset();
  g_text_encoder.reset();
  if (g_safety_session && g_safety_interpreter)
    g_safety_interpreter->releaseSession(g_safety_session);
  g_safety_session = nullptr;
  delete g_safety_interpreter;
  g_safety_interpreter = nullptr;
}

/** tokenize 统计（同 POST /tokenize）。返回 JSON 字符串。 */
JNIEXPORT jstring JNICALL
Java_com_moeapk_ai_engine_diffusion_DiffusionEngineImpl_nativeTokenize(
    JNIEnv *env, jclass, jstring text) {
  if (!g_text_encoder) return env->NewStringUTF("{\"error\":\"not initialized\"}");
  const char *textStr = env->GetStringUTFChars(text, nullptr);
  std::string prompt(textStr);
  env->ReleaseStringUTFChars(text, textStr);
  const int max_len = g_text_encoder->isAnima() ? anima_text_seq_len : 77;
  TokenizeInfo info = g_text_encoder->tokenizeInfo(prompt, max_len);
  nlohmann::json resp = {{"count", info.count},
                         {"max_length", max_len},
                         {"overflow_offset", info.overflow_offset}};
  return env->NewStringUTF(resp.dump().c_str());
}

}  // extern "C"
