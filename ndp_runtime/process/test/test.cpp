#include "../include/shm_manager.hpp"
#include "../include/spinlock.hpp"
#include "../include/bpe.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <string>
#include <sys/stat.h>

#define PAGE_SIZE 4096

RE2 re(
    "('s|'t|'re|'ve|'m|'ll|'d| ?\\p{L}+| ?\\p{N}+| "
    "?[^\\s\\p{L}\\p{N}]+|\\s+\\(?!\\S\\)|\\s+)");

std::string read_file(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::in);
    if (!file) {
        std::cerr << "❌ Failed to open " << filepath << std::endl;
        exit(EXIT_FAILURE);
    }
    
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    std::cout << "✅ Successfully read file: " << filepath << " (Size: " << content.size() << " bytes)" << std::endl;
    return content;
}


void tokenize_from_shm(){

  std::string wiki_data = read_file("./wiki_corpus.txt"); 
  BPERanks bpe_ranks;
  std::fstream merges("../model/merges.txt", std::ios::in);
  load_merge_rules(merges, &bpe_ranks);

  std::unordered_map<std::string, int> t2i;
  std::unordered_map<int, std::string> i2t;

  std::fstream vocab("../model/vocab.txt", std::ios::in);
  load_vocab(vocab, &t2i, &i2t);

  std::unordered_map<uint8_t, wchar_t> b2u;
  std::unordered_map<wchar_t, uint8_t> u2b;
  bytes_to_unicode(&b2u, &u2b);

  std::vector<std::string> tokens;

  tokenize(wiki_data, re, bpe_ranks, b2u, &tokens);

  std::cout <<"[INFO] Tokenize : \n";
  for(const auto & token : tokens){
    std::cout << token <<" ";
  }
  std::cout<<std::endl;

}

int main() {
    std::cout << "[BPE Process] Initializing..." << std::endl;
  

    tokenize_from_shm();

    return 0;
}
