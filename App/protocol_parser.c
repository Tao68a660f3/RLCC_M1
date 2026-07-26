#include "protocol_parser.h"
#include "lyric_service.h"
#include <stdio.h>

// #define DEBUG_PRT

typedef enum {
  ST_IDLE = 0,
  ST_CMD,
  ST_LEN,
  ST_PAYLOAD,
  ST_CHECK,
  ST_ERROR
} ParseState;

static ParseState current_state = ST_IDLE;
static uint8_t calc_checksum = 0;
static uint8_t payload_cnt = 0;

void Protocol_Init(void) {
  current_state = ST_IDLE;
  calc_checksum = 0;
  payload_cnt = 0;
}

uint8_t Protocol_FeedByte(uint8_t byte, ProtocolPacket *out_pkt) {
  // 错误态：只有 0xAA 能重置，避免错位
  if (current_state == ST_ERROR) {
    if (byte == 0xAA) {
      current_state = ST_CMD;
      calc_checksum = 0;
    }
    return 0;
  }

  switch (current_state) {
  case ST_IDLE:
    if (byte == 0xAA) {
      current_state = ST_CMD;
      calc_checksum = 0;
    }
    break;

  case ST_CMD:
    out_pkt->cmd = byte;
    current_state = ST_LEN;
    break;

  case ST_LEN:
    // 长度超限 → 错误态，防止写爆 payload
    if (byte > MAX_PAYLOAD_SIZE) {
      current_state = ST_ERROR;
      return 0;
    }
    out_pkt->len = byte;
    payload_cnt = 0;
    current_state = (byte == 0) ? ST_CHECK : ST_PAYLOAD;
    break;

  case ST_PAYLOAD:
    out_pkt->payload[payload_cnt++] = byte;
    calc_checksum ^= byte;
    if (payload_cnt >= out_pkt->len) {
      current_state = ST_CHECK;
    }
    break;

  case ST_CHECK:
    current_state = ST_IDLE;
    if (byte == calc_checksum) {
      // 为字符串补齐结束符，确保安全
      out_pkt->payload[out_pkt->len] = '\0';
      return 1;
    } else {
      current_state = ST_ERROR;
    }
    break;
  }
  return 0;
}

/*
typedef enum {
    ST_IDLE,
    ST_CMD,
    ST_LEN,
    ST_PAYLOAD,
    ST_CHECK,
    ST_ERROR  // 新增错误状态
} State_t;

uint8_t Protocol_FeedByte(uint8_t byte, ProtocolPacket *out_pkt) {
    // 在错误状态下，唯一的出口是检测到合法的起始头 0xAA
    if (current_state == ST_ERROR) {
        if (byte == 0xAA) {
            current_state = ST_CMD;
            calc_checksum = 0;
            return 0;
        }
        return 0; // 继续等待
    }

    switch (current_state) {
        case ST_IDLE:
            if (byte == 0xAA) {
                current_state = ST_CMD;
                calc_checksum = 0;
            }
            break;

        case ST_CMD:
            out_pkt->cmd = byte;
            calc_checksum ^= byte; // 补齐校验
            current_state = ST_LEN;
            break;

        case ST_LEN:
            // 鲁棒性关键：检查长度是否超过 A1 的缓冲区限制
            if (byte > MAX_PAYLOAD_SIZE) {
                current_state = ST_ERROR;
                return 0;
            }
            out_pkt->len = byte;
            calc_checksum ^= byte; // 补齐校验
            payload_cnt = 0;
            current_state = (byte == 0) ? ST_CHECK : ST_PAYLOAD;
            break;

        case ST_PAYLOAD:
            out_pkt->payload[payload_cnt++] = byte;
            calc_checksum ^= byte;
            if (payload_cnt >= out_pkt->len) {
                current_state = ST_CHECK;
            }
            break;

        case ST_CHECK:
            current_state = ST_IDLE;
            if (byte == calc_checksum) {
                // 成功解析后，为字符串补齐结束符，确保滚动显示安全
                out_pkt->payload[out_pkt->len] = '\0';
                return 1;
            } else {
                current_state = ST_ERROR; // 校验失败进入错误状态
            }
            break;
    }
    return 0;
}
*/

// ---------------------------------------------------------
// 协议分发器：解析成功后，会根据命令码跳转到对应的处理函数
// ---------------------------------------------------------
void Protocol_Dispatcher(ProtocolPacket *pkt) {
  switch (pkt->cmd) {
  case 0x10:
    Lyric_OnMetadataReceived(pkt->payload, pkt->len);
    break;
  case 0x11:
    Lyric_OnSyncReceived(pkt->payload, pkt->len);
    break;
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
    Lyric_OnContentReceived(pkt->cmd, pkt->payload, pkt->len);
    break;
  case 0x20:
    RTC_OnTimeSyncReceived(pkt->payload, pkt->len);
    break;
  }

#ifdef DEBUG_PRT
  // debug
  for (uint8_t i = 0; i < LYRIC_POOL_SIZE; i++) {
    if (g_sys.sorted_lyrics[i]) {
      printf("Content: %s\r\n", g_sys.sorted_lyrics[i]->text);
      // printf("Start/Duration: %d/%d\r\n",
      // g_sys.sorted_lyrics[i]->start_time_ms,
      //        g_sys.sorted_lyrics[i]->duration);
      // if (g_sys.sorted_lyrics[i]->word_count > 0) {
      //   printf("Times: ");
      //   for (uint8_t j = 0; j < g_sys.sorted_lyrics[i]->word_count; j++) {
      //     printf("%d ", g_sys.sorted_lyrics[i]->time_offsets[j]);
      //   }
      //   printf("\r\n");
      // }
    }
    printf("\r\n");
  }
#endif
}
