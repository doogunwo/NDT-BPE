#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Runtime::BPE {

class BPETokenizer {
public:
    // Singleton 접근
    static BPETokenizer& Instance();

    // 복사/이동 금지
    BPETokenizer(const BPETokenizer&) = delete;
    BPETokenizer& operator=(const BPETokenizer&) = delete;

    // 토큰화 수행
    std::vector<std::int32_t> Tokenize(const std::string& text);
    std::vector<std::int32_t> Tokenize(std::string_view text);

    // 경로 초기화 (Instance 호출 전에만 유효)
    static void Init(std::string model_path, std::string merges_path);

private:
    BPETokenizer();
    ~BPETokenizer();

    struct Impl;                 // PImpl forward decl
    std::unique_ptr<Impl> impl_; // PImpl storage
};

} // namespace Runtime::BPE
