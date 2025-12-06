// bpe_main.cpp - BPE 프로세스 (SPDK 슬롯 협상 기반)
#include <iostream>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <nlohmann/json.hpp>
#include <pthread.h>

#define MSG_KEY 1234
#define SHM_SLOT_SIZE 4096

using json = nlohmann::json;

extern std::string LoadJSONFromFile(const std::string& path);
extern std::string LoadTXTFromFile(const std::string& path);
extern std::vector<int32_t> token(const std::string vocab_blob, const std::string merges_blob, const std::string added_token, const std::string text);

struct msg_buffer {
    long msg_type;
    char msg_text[32];
};

void send_slot_response(int msgid, const std::string& message) {
    msg_buffer response;
    response.msg_type = 2;
    strncpy(response.msg_text, message.c_str(), sizeof(response.msg_text));
    msgsnd(msgid, &response, sizeof(response.msg_text), 0);
}
/*
int main() {
    int msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("msgget 실패");
        return 1;
    }

    pthread_spinlock_t spin;
    pthread_spin_init(&spin, 0);

    std::string vocab_blob, merges_blob;
    try {
        json j = json::parse(LoadJSONFromFile("../model/byte_level_bpe_model.json"));
        vocab_blob = j["model"]["vocab"].dump();
        merges_blob = LoadTXTFromFile("../model/merges.txt");
    } catch (...) {
        std::cerr << "[BPE] 모델 로딩 실패" << std::endl;
        return 1;
    }

    init_all_slots();

    while (true) {
        msg_buffer req;
        std::cout << "[BPE] 요청 대기 중..." << std::endl;
        msgrcv(msgid, &req, sizeof(req.msg_text), 1, 0);

        std::string request(req.msg_text);
        if (request == "REQ_SLOT") {
            int slot_id = find_empty_slot();
            if (slot_id == -1) {
                send_slot_response(msgid, "NO_SLOT");
            } else {
                reserve_slot(slot_id);
                send_slot_response(msgid, "SLOT_" + std::to_string(slot_id));
            }
        } else if (request.find("SLOT_") == 0 && request.find("_READY") != std::string::npos) {
            int slot_id = std::stoi(request.substr(5, request.find("_READY") - 5));
            char* ptr = get_slot_ptr(slot_id);
            std::string input(ptr, SHM_SLOT_SIZE);

            std::string added_token = R"({"[PAD]":0,"[UNK]":1,"[CLS]":2,"[SEP]":3,"[MASK]":4})";
            std::vector<int32_t> token_ids = token(vocab_blob, merges_blob, added_token, input);

            // 응답 데이터 저장
            memset(ptr, 0, SHM_SLOT_SIZE);
            size_t count = token_ids.size();
            memcpy(ptr, &count, sizeof(size_t));
            memcpy(ptr + sizeof(size_t), token_ids.data(), count * sizeof(int32_t));

            set_slot_status(slot_id, PROCESSED);
            send_slot_response(msgid, "SLOT_" + std::to_string(slot_id) + "_DONE");
        }
    }

    pthread_spin_destroy(&spin);
    return 0;
}
*/