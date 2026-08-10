#include "protocol_parser.h"
#include "lyric_service.h"
#include "ui_manager.h"
#include <stdio.h>

// #define DEBUG_PRT

typedef enum {
  ST_IDLE = 0,
  ST_CMD,
  ST_LENH,
  ST_LENL,
  ST_PAYLOAD,
  ST_CHECK,
  ST_ERROR
} ParseState;

static ParseState current_state = ST_IDLE;
static uint8_t calc_checksum = 0;
static uint16_t payload_cnt = 0;
static uint8_t len_h = 0; // 暂存 LenH，等 LenL 到达后合成 2 字节长度

void Protocol_Init(void) {
  current_state = ST_IDLE;
  calc_checksum = 0;
  payload_cnt = 0;
  len_h = 0;
}

uint8_t Protocol_FeedByte(uint8_t byte, ProtocolPacket *out_pkt) {
  // 错误态：只有 0xAA 能重置，避免错位
  if (current_state == ST_ERROR) {
    if (byte == 0xAA) {
      current_state = ST_CMD;
      calc_checksum = byte; // 帧头 0xAA 计入全帧异或
    }
    return 0;
  }

  switch (current_state) {
  case ST_IDLE:
    if (byte == 0xAA) {
      current_state = ST_CMD;
      calc_checksum = byte; // 帧头 0xAA 计入全帧异或
    }
    break;

  case ST_CMD:
    out_pkt->cmd = byte;
    calc_checksum ^= byte;
    current_state = ST_LENH;
    break;

  case ST_LENH:
    len_h = byte;
    calc_checksum ^= byte;
    current_state = ST_LENL;
    break;

  case ST_LENL:
    calc_checksum ^= byte;
    out_pkt->len = ((uint16_t)len_h << 8) | byte;
    // 长度超限 → 错误态，防止写爆 payload
    if (out_pkt->len > MAX_PAYLOAD_SIZE) {
      current_state = ST_ERROR;
      return 0;
    }
    payload_cnt = 0;
    current_state = (out_pkt->len == 0) ? ST_CHECK : ST_PAYLOAD;
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
      // 为字符串补齐结束符，确保安全（防止 len==MAX 时越界）
      if (out_pkt->len < MAX_PAYLOAD_SIZE)
        out_pkt->payload[out_pkt->len] = '\0';
      else
        out_pkt->payload[MAX_PAYLOAD_SIZE - 1] = '\0';
      return 1;
    } else {
      current_state = ST_ERROR;
    }
    break;
  }
  return 0;
}

// ---------------------------------------------------------
// 协议分发器：解析成功后，会根据命令码跳转到对应的处理函数
// ---------------------------------------------------------
void Protocol_Dispatcher(ProtocolPacket *pkt) {
  // 协议内文本子模式：任何非 0x16 指令到达 → 退出子模式，恢复歌词渲染。
  // 上位机需在发送纯文本段落时暂停其他指令包。
  if (pkt->cmd != 0x16) {
    UI_Manager_ExitProtocolTextMode();
  }

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
    Lyric_OnContentReceived(pkt->cmd, pkt->payload, pkt->len);
    break;
  case 0x16:
    // 纯文本：数据由协议层提供，行为与 SYS_MODE_TXT_MODE 完全一致，
    // 但不走 COM_Process_TextMode()
    UI_Manager_OnProtocolTextReceived(pkt->payload, pkt->len);
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