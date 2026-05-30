# LoRa FUOTA ESP32

Prototype firmware update over LoRa for ESP32 TTGO LoRa32 boards.

This project is still in progress. The current goal is to make a simple end-to-end FUOTA flow work first, before handling production concerns such as duty cycle, time-on-air, dwell time, fleet scheduling, security hardening, or large-scale rollout behavior.

## Projects

- `lora-fuota-gateway`: downloads a firmware binary from an MQTT-provided URL, stores it on SD card, and broadcasts it over LoRa.
- `lora-fuota-node`: receives FUOTA frames over LoRa, tracks packet state, validates packet CRC, and reports receive progress on OLED/Serial.
- `shared`: shared FUOTA frame and metadata helpers used by both gateway and node.

## Current Status

Implemented prototype pieces:

- Binary FUOTA frame format for metadata, data, and end frames.
- Firmware metadata frame with file size, chunk size, total packet count, version, and image CRC32.
- Per-packet CRC32 validation.
- Receive bitmap for tracking which packet IDs arrived.
- Duplicate packet detection and missing packet preview.
- OLED progress animation on the node.

Not implemented yet:

- Writing received packets to SD card on the node.
- Sending NACK/retry requests back to the gateway.
- Gateway retransmission of specific missing packet IDs.
- Full staged image verification on the node.
- ESP32 OTA flashing from the received image.

## Roadmap

1. Store valid node packets into `/update.bin` on SD card using `packet_id * chunk_size` offsets.
2. Send missing packet IDs from node to gateway after the first broadcast pass.
3. Add gateway retry handling for requested packet IDs.
4. Verify the completed staged firmware image against metadata.
5. Flash the verified image using ESP32 OTA APIs.
6. Later, consider internal flash staging to reduce hardware cost when a node has no SD slot.

## Build

Each PlatformIO project is built separately:

```powershell
cd lora-fuota-gateway
pio run
```

```powershell
cd lora-fuota-node
pio run
```

## License

MIT License. See `LICENSE`.
