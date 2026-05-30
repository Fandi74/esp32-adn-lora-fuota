#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

static const uint8_t FUOTA_MAGIC_0 = 'F';
static const uint8_t FUOTA_MAGIC_1 = 'U';
static const uint8_t FUOTA_MAGIC_2 = 'O';
static const uint8_t FUOTA_MAGIC_3 = 'T';

static const uint8_t FUOTA_PROTOCOL_VERSION = 1;

static const uint8_t FUOTA_FRAME_METADATA = 1;
static const uint8_t FUOTA_FRAME_DATA = 2;
static const uint8_t FUOTA_FRAME_END = 3;

static const uint8_t FUOTA_CHUNK_SIZE = 125;
static const uint8_t FUOTA_HEADER_SIZE = 15;
static const uint8_t FUOTA_METADATA_PAYLOAD_SIZE = 16;
static const uint8_t FUOTA_MAX_PAYLOAD_SIZE = FUOTA_CHUNK_SIZE;
static const uint16_t FUOTA_MAX_FRAME_SIZE = FUOTA_HEADER_SIZE + FUOTA_MAX_PAYLOAD_SIZE;

struct FuotaFrameHeader {
  uint8_t type;
  uint16_t sessionId;
  uint16_t packetId;
  uint8_t payloadLen;
  uint32_t payloadCrc32;
};

struct FuotaMetadata {
  uint32_t fileSize;
  uint16_t chunkSize;
  uint16_t totalPackets;
  uint32_t firmwareVersion;
  uint32_t imageCrc32;
};

inline void fuotaWriteU16(uint8_t* buffer, uint16_t value) {
  buffer[0] = (uint8_t)(value & 0xFF);
  buffer[1] = (uint8_t)((value >> 8) & 0xFF);
}

inline void fuotaWriteU32(uint8_t* buffer, uint32_t value) {
  buffer[0] = (uint8_t)(value & 0xFF);
  buffer[1] = (uint8_t)((value >> 8) & 0xFF);
  buffer[2] = (uint8_t)((value >> 16) & 0xFF);
  buffer[3] = (uint8_t)((value >> 24) & 0xFF);
}

inline uint16_t fuotaReadU16(const uint8_t* buffer) {
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

inline uint32_t fuotaReadU32(const uint8_t* buffer) {
  return (uint32_t)buffer[0] |
         ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16) |
         ((uint32_t)buffer[3] << 24);
}

inline uint32_t fuotaCrc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320UL;
      } else {
        crc = crc >> 1;
      }
    }
  }
  return crc;
}

inline uint32_t fuotaCrc32(const uint8_t* data, size_t len) {
  return ~fuotaCrc32Update(0xFFFFFFFFUL, data, len);
}

inline bool fuotaHasValidMagic(const uint8_t* frame, size_t frameLen) {
  return frameLen >= FUOTA_HEADER_SIZE &&
         frame[0] == FUOTA_MAGIC_0 &&
         frame[1] == FUOTA_MAGIC_1 &&
         frame[2] == FUOTA_MAGIC_2 &&
         frame[3] == FUOTA_MAGIC_3;
}

inline size_t fuotaWriteFrame(uint8_t* frame, uint8_t type, uint16_t sessionId,
                              uint16_t packetId, const uint8_t* payload,
                              uint8_t payloadLen) {
  frame[0] = FUOTA_MAGIC_0;
  frame[1] = FUOTA_MAGIC_1;
  frame[2] = FUOTA_MAGIC_2;
  frame[3] = FUOTA_MAGIC_3;
  frame[4] = FUOTA_PROTOCOL_VERSION;
  frame[5] = type;
  fuotaWriteU16(&frame[6], sessionId);
  fuotaWriteU16(&frame[8], packetId);
  frame[10] = payloadLen;
  fuotaWriteU32(&frame[11], fuotaCrc32(payload, payloadLen));

  for (uint8_t i = 0; i < payloadLen; i++) {
    frame[FUOTA_HEADER_SIZE + i] = payload[i];
  }

  return FUOTA_HEADER_SIZE + payloadLen;
}

inline bool fuotaReadFrameHeader(const uint8_t* frame, size_t frameLen, FuotaFrameHeader& header) {
  if (!fuotaHasValidMagic(frame, frameLen) || frame[4] != FUOTA_PROTOCOL_VERSION) {
    return false;
  }

  header.type = frame[5];
  header.sessionId = fuotaReadU16(&frame[6]);
  header.packetId = fuotaReadU16(&frame[8]);
  header.payloadLen = frame[10];
  header.payloadCrc32 = fuotaReadU32(&frame[11]);

  return frameLen == (size_t)(FUOTA_HEADER_SIZE + header.payloadLen);
}

inline void fuotaWriteMetadata(uint8_t* payload, const FuotaMetadata& metadata) {
  fuotaWriteU32(&payload[0], metadata.fileSize);
  fuotaWriteU16(&payload[4], metadata.chunkSize);
  fuotaWriteU16(&payload[6], metadata.totalPackets);
  fuotaWriteU32(&payload[8], metadata.firmwareVersion);
  fuotaWriteU32(&payload[12], metadata.imageCrc32);
}

inline FuotaMetadata fuotaReadMetadata(const uint8_t* payload) {
  FuotaMetadata metadata;
  metadata.fileSize = fuotaReadU32(&payload[0]);
  metadata.chunkSize = fuotaReadU16(&payload[4]);
  metadata.totalPackets = fuotaReadU16(&payload[6]);
  metadata.firmwareVersion = fuotaReadU32(&payload[8]);
  metadata.imageCrc32 = fuotaReadU32(&payload[12]);
  return metadata;
}
