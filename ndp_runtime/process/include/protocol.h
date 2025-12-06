// message_protocol.h - 메시지 송수신 구조 정의
#ifndef MESSAGE_PROTOCOL_H
#define MESSAGE_PROTOCOL_H

#include <cstring>

// 메시지 타입 정의
#define MSG_TYPE_REQUEST 1
#define MSG_TYPE_RESPONSE 2

// 명령어 문자열
#define CMD_REQ_READ   "REQ_READ"   // 읽기 슬롯 요청
#define CMD_REQ_WRITE  "REQ_WRITE"  // 쓰기 슬롯 요청
#define CMD_RD_OK      "READ_OK"      // 읽기 슬롯 할당 완료
#define CMD_WR_OK      "WRITE_OK"      // 쓰기 슬롯 할당 완료
#define CMD_RD_READY   "READ_READY"   // 읽기 슬롯 데이터 준비됨
#define CMD_WR_READY   "WRITE_READY"   // 쓰기 슬롯 데이터 준비됨
#define CMD_RD_DONE    "READ_DONE"    // 읽기 슬롯 작업 완료
#define CMD_WR_DONE    "WRITE_DONE"    // 쓰기 슬롯 작업 완료
#define CMD_WAIT       "WAIT"       // 슬롯 없음, 대기 요청

// 메시지 패킷 구조체
struct MsgPacket {
    long msg_type;       // 메시지 타입 (1 = 요청, 2 = 응답)
    char command[12];    // 명령어 문자열 (REQ_READ 등)
    int slot_id;         // 슬롯 번호 (shm_id와 동일)

    MsgPacket() : msg_type(0), slot_id(-1) {
        std::memset(command, 0, sizeof(command));
    }
};

#endif // MESSAGE_PROTOCOL_H
