#include "../include/tokenizers_cpp.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include <sys/time.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/resource.h>
#include <chrono>
#include <atomic>

double bpe_time = 0; 
using json = nlohmann::json;
extern size_t count_raw_tokens(const std::string& text);




std::string model_path = "../model/byte_level_bpe_model.json";
std::string merges_path = "../model/merges.txt";

std::string LoadJSONFromFile(const std::string& path);
std::string LoadTXTFromFile(const std::string& path);

std::string merges_blob = LoadTXTFromFile(merges_path);
std::string json_blob = LoadJSONFromFile(model_path);
json j = json::parse(json_blob);
json model = j["model"];
std::string vocab_blob = model["vocab"].dump();


std::string added_token = R"({
    "[PAD]": 0,
    "[UNK]": 1,
    "[CLS]": 2,
    "[SEP]": 3,
    "[MASK]": 4
  })";

std::unique_ptr<tokenizers::Tokenizer> tokenizer =
      tokenizers::Tokenizer::FromBlobByteLevelBPE(vocab_blob, merges_blob, added_token);

static std::atomic<long long> g_tokenize_total_us{0};

std::string LoadJSONFromFile(const std::string& path) {
    std::ifstream fs(path);
    std::string data((std::istreambuf_iterator<char>(fs)), std::istreambuf_iterator<char>());
    return data;
}

std::string LoadTXTFromFile(const std::string& path) {
    std::ifstream fs(path);
    std::stringstream buffer;
    buffer << fs.rdbuf();
    return buffer.str();
}

std::vector<int32_t> token(const std::string text)
{


  // 시간 측정 시작
  auto t0 = std::chrono::steady_clock::now();

  // 실제 토큰화
  std::vector<int32_t> token_ids = tokenizer->Encode(text);

  // 시간 측정 종료
  auto t1 = std::chrono::steady_clock::now();
  long long dt_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  // 누적 업데이트 및 로그 출력
  long long total_us = g_tokenize_total_us.fetch_add(dt_us) + dt_us;
  double total_s = static_cast<double>(total_us) / 1e6;
  
  std::cout << "[LOG] tokenization_time: " << dt_us << " us, "
            << "cumulative: " << total_us << " us (≈ " << total_s << " s)"
            << std::endl;

  return token_ids;
}
