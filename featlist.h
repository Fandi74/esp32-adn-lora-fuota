#pragma once

/*
  LoRa FUOTA prototype feature list
  Target: make firmware update over LoRa work in a simple, testable way first.

  Current scope:
  - Ignore production constraints for now, including duty cycle, time-on-air,
    dwell time, regional compliance, fleet scheduling, and large-scale rollout.
  - Focus on reliable binary transfer, then ESP32 OTA handoff.

  Potential improvements:

  1. Fixed binary packet format
     - Replace text commands like START/END with typed binary frames.
     - Include magic, version, session_id, frame_type, packet_id, payload_len,
       payload_crc32, and payload.

  2. Firmware metadata frame
     - Send file_size, chunk_size, total_packets, firmware_version, and full
       image hash before data transfer starts.
     - Calculate total_packets from file_size instead of hardcoding capacity.

  3. Packet receive bitmap
     - Track received packet IDs with a bitmap.
     - For 4096 packets, this only needs 512 bytes.
     - Use it to find exactly which chunks are missing after the first pass.

  4. Per-packet corruption detection
     - Add CRC32 for every payload.
     - If CRC fails, discard the packet and keep that packet ID marked missing.

  5. Missing packet retry flow
     - After END, node sends a NACK list or NACK ranges.
     - Gateway retransmits only requested packet IDs.
     - Repeat until complete or retry limit is reached.

  6. Duplicate and out-of-order handling
     - Accept packets in any order.
     - Ignore duplicates that were already received and verified.
     - Keep counters for received, duplicate, bad_crc, and missing packets.

  7. Staging storage on node
     - Store valid chunks into a temporary firmware image before flashing.
     - For this stage, use an SD card as the first target because random writes,
       retry handling, and manual inspection are easiest.
     - Write packet_id * chunk_size offsets into /update.bin so missing packets
       can be filled later without restarting the whole transfer.
     - If the node does not have an SD slot, internal flash can be used later to
       save hardware cost, using LittleFS/SPIFFS or the inactive OTA partition.
     - Start with a staged file on SD because it is easier to debug than writing
       directly into OTA flash.

  8. Full image verification
     - After all chunks are present, calculate SHA-256 or CRC32 over the staged
       firmware image.
     - Only start OTA if the full image matches the metadata.

  9. ESP32 OTA handoff
     - Use the Arduino Update API or ESP-IDF OTA APIs to write the verified
       image to the next OTA partition.
     - Reboot only after Update.end() succeeds.

  10. Gateway retransmit index
      - Keep the downloaded firmware on SD.
      - Seek to packet_id * chunk_size when a node requests a retry.
      - Support retransmitting single IDs and compact ID ranges.

  11. Transfer session control
      - Use session_id so old packets from a previous transfer are ignored.
      - Add ABORT/TIMEOUT behavior so the node can recover cleanly.

  12. Simple progress reporting
      - Show progress on Serial and OLED: total packets, received packets,
        missing packets, bad CRC count, retry round, and verify result.

  13. Prototype configuration constants
      - Keep chunk size, retry limit, LoRa frequency, spreading factor,
        bandwidth, and coding rate as clear constants in one place.

  14. Small test firmware first
      - Validate the protocol with a small dummy binary or tiny firmware before
        trying a full 500 KB image.

  15. Prototype architecture modules
      - Boot manager: decide normal boot, update mode, rollback mode, or safe
        mode based on stored state.
      - Config manager: keep LoRa, MQTT, WiFi, chunk size, retry limit, and
        storage settings in one place.
      - LoRa packet protocol: own frame encoding, decoding, frame types,
        session IDs, packet IDs, payload length, and packet CRC.
      - FUOTA session manager: coordinate metadata, transfer start/end,
        timeout, retry rounds, and session cleanup.
      - Fragment bitmap: track received packet IDs and produce the missing
        packet list after each pass.
      - OTA writer: write verified firmware from staging storage into the ESP32
        OTA partition.
      - Hash verifier: verify packet CRC during receive and full image hash
        before OTA write.
      - Rollback/safe mode: recover if the updated firmware fails to boot or
        report healthy state.
      - Health report: expose update result, firmware version, packet stats,
        and boot status over Serial, OLED, or MQTT later.
      - Event log: keep simple update events such as start, download success,
        bad packet, retry request, verify result, OTA result, and reboot cause.
*/
