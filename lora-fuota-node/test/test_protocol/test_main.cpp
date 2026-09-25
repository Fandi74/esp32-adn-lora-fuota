#include <Arduino.h>
#include <unity.h>
#include <fuota_protocol.h>

void test_crc32_known_vector() {
  const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926UL, fuotaCrc32(data, sizeof(data)));
}

void test_frame_crc_covers_header_and_payload() {
  uint8_t payload[] = {1, 2, 3};
  uint8_t frame[FUOTA_MAX_FRAME_SIZE];
  size_t frameLen = fuotaWriteFrame(frame, FUOTA_FRAME_DATA, 42, 7, payload, sizeof(payload));
  FuotaFrameHeader header;

  TEST_ASSERT_TRUE(fuotaReadFrameHeader(frame, frameLen, header));
  TEST_ASSERT_EQUAL_HEX32(header.frameCrc32,
                          fuotaFrameCrc32(frame, &frame[FUOTA_HEADER_SIZE], header.payloadLen));
  TEST_ASSERT_FALSE(fuotaReadFrameHeader(frame, frameLen - 1, header));

  frame[8] ^= 1;
  TEST_ASSERT_TRUE(fuotaReadFrameHeader(frame, frameLen, header));
  TEST_ASSERT_NOT_EQUAL(header.frameCrc32,
                        fuotaFrameCrc32(frame, &frame[FUOTA_HEADER_SIZE], header.payloadLen));
  frame[8] ^= 1;

  frame[FUOTA_HEADER_SIZE] ^= 1;
  TEST_ASSERT_NOT_EQUAL(header.frameCrc32,
                        fuotaFrameCrc32(frame, &frame[FUOTA_HEADER_SIZE], header.payloadLen));
}

void test_last_packet_length() {
  TEST_ASSERT_EQUAL_UINT16(125, fuotaExpectedPacketLength(251, 125, 0, 3));
  TEST_ASSERT_EQUAL_UINT16(125, fuotaExpectedPacketLength(251, 125, 1, 3));
  TEST_ASSERT_EQUAL_UINT16(1, fuotaExpectedPacketLength(251, 125, 2, 3));
  TEST_ASSERT_EQUAL_UINT16(125, fuotaExpectedPacketLength(250, 125, 1, 2));
}

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_known_vector);
  RUN_TEST(test_frame_crc_covers_header_and_payload);
  RUN_TEST(test_last_packet_length);
  UNITY_END();
}

void loop() {}
