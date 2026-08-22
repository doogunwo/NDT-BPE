#include "bpe_tokenizer.h"

#include "tokenizers_cpp.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace Runtime::BPE {
namespace fs = std::filesystem;

// 내부 전역 변수
static std::atomic<long long> g_tokenize_total_us{0};
static std::string g_model_path  = "./model/bbpe_tokenizer.json";
static std::string g_merges_path = "./model/merges.txt";

static std::string ResolveArtifactPath(const char* env_name,
                                       const std::string& configured,
                                       std::initializer_list<const char*> fallbacks) {
    if (const char* env = std::getenv(env_name); env != nullptr && env[0] != '\0') {
        return env;
    }

    std::vector<fs::path> candidates;
    if (!configured.empty()) {
        candidates.emplace_back(configured);
    }
    for (const char* candidate : fallbacks) {
        candidates.emplace_back(candidate);
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            return fs::absolute(candidate, ec).string();
        }
    }
    return configured;
}

static bool TimingLoggingEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BPE_RUNTIME_LOG_TIMINGS");
        return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

// 파일 로드 헬퍼
static std::string LoadFileBinary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("File open failed: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Impl 정의
struct BPETokenizer::Impl {
    std::string tokenizer_json;

    Impl(const std::string& model_path, const std::string& merges_path) {
        const std::string resolved_model = ResolveArtifactPath(
            "BPE_MODEL_PATH",
            model_path,
            {
                "./model/bbpe_tokenizer.json",
                "../model/bbpe_tokenizer.json",
            });
        const std::string resolved_merges = ResolveArtifactPath(
            "BPE_MERGES_PATH",
            merges_path,
            {
                "./model/merges.txt",
                "../model/merges.txt",
            });

        if (!fs::exists(resolved_model) || !fs::exists(resolved_merges)) {
            throw std::runtime_error("Model files not found: " + resolved_model + " or " + resolved_merges);
        }

        // Load the exact Hugging Face tokenizer artifact. Reconstructing a
        // ByteLevelBPE tokenizer from only vocab/merges silently discards
        // normalizer, pre-tokenizer, decoder, added-token, and model options.
        // That made the storage result differ from Tokenizer.from_file().
        tokenizer_json = LoadFileBinary(resolved_model);

        auto tokenizer = CreateTokenizer();
        if (!tokenizer) throw std::runtime_error("Tokenizer init returned null");
    }

    std::unique_ptr<tokenizers::Tokenizer> CreateTokenizer() const {
        return tokenizers::Tokenizer::FromBlobJSON(tokenizer_json);
    }
};

void BPETokenizer::Init(std::string model_path, std::string merges_path) {
    g_model_path  = std::move(model_path);
    g_merges_path = std::move(merges_path);
}

BPETokenizer& BPETokenizer::Instance() {
    static BPETokenizer instance;
    return instance;
}

BPETokenizer::BPETokenizer()
    : impl_(std::make_unique<Impl>(g_model_path, g_merges_path)) {}

BPETokenizer::~BPETokenizer() = default;

std::vector<std::int32_t> BPETokenizer::Tokenize(const std::string& text) {
    thread_local const Impl* tls_owner = nullptr;
    thread_local std::unique_ptr<tokenizers::Tokenizer> tls_tokenizer;
    if (tls_owner != impl_.get() || !tls_tokenizer) {
        tls_tokenizer = impl_->CreateTokenizer();
        tls_owner = impl_.get();
    }

    if (!TimingLoggingEnabled()) {
        return tls_tokenizer->Encode(text);
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::int32_t> token_ids = tls_tokenizer->Encode(text);
    auto t1 = std::chrono::steady_clock::now();
    long long dt_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    long long total_us = g_tokenize_total_us.fetch_add(dt_us, std::memory_order_relaxed) + dt_us;
    std::fprintf(stderr, "[LOG] Time: %lld us, Total: %lld us\n", dt_us, total_us);
    return token_ids;
}

std::vector<std::int32_t> BPETokenizer::Tokenize(std::string_view text) {
    thread_local std::string tls_text;
    tls_text.assign(text.data(), text.size());
    return Tokenize(tls_text);
}

std::vector<std::vector<std::int32_t>> BPETokenizer::TokenizeBatch(const std::vector<std::string>& texts) {
    if (texts.empty()) {
        return {};
    }

    thread_local const Impl* tls_owner = nullptr;
    thread_local std::unique_ptr<tokenizers::Tokenizer> tls_tokenizer;
    if (tls_owner != impl_.get() || !tls_tokenizer) {
        tls_tokenizer = impl_->CreateTokenizer();
        tls_owner = impl_.get();
    }

    if (!TimingLoggingEnabled()) {
        return tls_tokenizer->EncodeBatch(texts);
    }

    auto t0 = std::chrono::steady_clock::now();
    auto batch_ids = tls_tokenizer->EncodeBatch(texts);
    auto t1 = std::chrono::steady_clock::now();
    long long dt_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    long long total_us = g_tokenize_total_us.fetch_add(dt_us, std::memory_order_relaxed) + dt_us;
    std::fprintf(stderr, "[LOG] BatchTime: %lld us, BatchSize: %zu, Total: %lld us\n",
                 dt_us, texts.size(), total_us);
    return batch_ids;
}

} // namespace Runtime::BPE
