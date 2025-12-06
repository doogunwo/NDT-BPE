#ifndef AGENT_H
#define AGENT_H

#include <iostream>
#include <queue>
#include <unordered_set>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <cstring>
#include <unistd.h>
#include "pool.h"

#define MSG_KEY 1234  // 메시지 큐 키

// 메시지 구조체
struct Message {
    long msg_type;
    char msg_text[16]; // 메시지 텍스트 (키 전달)
};

class Agent {
private:
    int msg_id;         // 메시지 큐 식별자
    Pool& pool;         // 공유 메모리 풀 객체 참조

    // 메시지 수신 함수 (0xD4 수신 확인)
    bool receive_spdk_command();

    // NVMe-oF Target에 공유 메모리 키 응답 전송
    void send_shm_key_response(int shm_key);

    // 키가 없을 때 메시지 전송
    void send_no_key_response();

public:
    // 생성자 & 소멸자
    Agent(Pool& shm_pool);
    ~Agent();

    // 메시지 처리 루프 실행
    void handle_requests();
};

#endif // AGENT_H
