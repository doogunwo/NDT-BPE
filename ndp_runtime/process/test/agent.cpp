#include "../include/agent.h"

// ✅ 생성자: 메시지 큐 초기화
Agent::Agent(Pool& shm_pool) : pool(shm_pool) {
    msg_id = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msg_id < 0) {
        perror("[ERROR] 메시지 큐 생성 실패");
        exit(1);
    }
}

// ✅ 소멸자: 메시지 큐 정리
Agent::~Agent() {
    msgctl(msg_id, IPC_RMID, nullptr); // 메시지 큐 삭제
}

// ✅ 메시지 큐에서 0xD4 명령을 수신하는 함수
bool Agent::receive_spdk_command() {
    Message request;
    if (msgrcv(msg_id, &request, sizeof(request.msg_text), 1, 0) > 0) {
        return (strcmp(request.msg_text, "\xD4") == 0);
    }
    return false;
}

// ✅ NVMe-oF Target에 공유 메모리 키를 응답하는 함수
void Agent::send_shm_key_response(int shm_key) {
    Message response;
    response.msg_type = 2;
    snprintf(response.msg_text, sizeof(response.msg_text), "%X", shm_key);

    if (msgsnd(msg_id, &response, sizeof(response.msg_text), 0) == -1) {
        perror("[ERROR] 메시지 큐 전송 실패");
    } else {
        std::cout << "[Agent] 공유 메모리 키 전달: " << shm_key << std::endl;
    }
}

// ✅ 할당 가능한 키가 없을 때 NVMe-oF Target에 메시지 전송
void Agent::send_no_key_response() {
    Message response;
    response.msg_type = 2;
    strcpy(response.msg_text, "NO_KEY");

    if (msgsnd(msg_id, &response, sizeof(response.msg_text), 0) == -1) {
        perror("[ERROR] 키 없음 메시지 전송 실패");
    } else {
        std::cout << "[Agent] 사용 가능한 공유 메모리 키 없음" << std::endl;
    }
}

// ✅ 메시지 큐 요청을 처리하는 루프
void Agent::handle_requests() {
    while (true) {
        if (receive_spdk_command()) {
            int allocated_key = pool.allocate_key();
            if (allocated_key == -1) {
                send_no_key_response();
            } else {
                send_shm_key_response(allocated_key);
            }
        }
        usleep(1000); // CPU 부하 방지
    }
}
