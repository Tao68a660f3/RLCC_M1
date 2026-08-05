#ifndef __PROTOCOL_PARSER_H
#define __PROTOCOL_PARSER_H

#include "main.h"

#define MAX_PAYLOAD_SIZE 1024

// 协议包结构体
typedef struct {
  uint8_t cmd;
  uint16_t len; // 新协议: 长度 2 字节 (LenH<<8 | LenL)
  uint8_t payload[MAX_PAYLOAD_SIZE];
  uint8_t checksum;
} ProtocolPacket;

// 初始化解析状态机
void Protocol_Init(void);

// 喂入字节：返回 1 表示捕获到一个完整合规的包
uint8_t Protocol_FeedByte(uint8_t byte, ProtocolPacket *out_pkt);

// 核心分发器：在这里勾连不同的业务功能
void Protocol_Dispatcher(ProtocolPacket *pkt);

#endif