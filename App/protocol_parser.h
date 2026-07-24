#ifndef __PROTOCOL_PARSER_H
#define __PROTOCOL_PARSER_H

#include "main.h"

// 协议包结构体：最大负载 512 字节
typedef struct {
  uint8_t cmd;
  uint8_t len;
  uint8_t payload[512];
  uint8_t checksum;
} ProtocolPacket;

// 初始化解析状态机
void Protocol_Init(void);

// 喂入字节：返回 1 表示捕获到一个完整合规的包
uint8_t Protocol_FeedByte(uint8_t byte, ProtocolPacket *out_pkt);

// 核心分发器：在这里勾连不同的业务功能
void Protocol_Dispatcher(ProtocolPacket *pkt);

#endif