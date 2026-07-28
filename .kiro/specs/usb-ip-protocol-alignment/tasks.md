# Implementation Plan: USB/IP Protocol Alignment

## Overview

This plan refactors the `USBIPComponent` to achieve full USB/IP protocol compliance with the Linux kernel's `vhci-hcd` module. The implementation is broken into incremental steps: first adding synchronization primitives and state tracking, then fixing protocol-level correctness issues (error mapping, actual_length, unlink handling, isochronous rejection), then hardening the TCP reassembly and buffer validation, and finally wiring everything together with proper connection cleanup. Each step builds on the previous one and results in compilable, testable code.

## Tasks

- [x] 1. Add synchronization primitives and pending URB tracking infrastructure
  - [x] 1.1 Add mutexes, pending URB map, and XferCtx struct to usb_ip.h
    - Add `#include <unordered_map>` and FreeRTOS semaphore includes
    - Move `XferCtx` struct from usb_ip.cpp to usb_ip.h, adding `uint32_t seqnum` and `bool cancelled` fields
    - Add `SemaphoreHandle_t send_mutex_`, `SemaphoreHandle_t pending_mutex_`, and `std::unordered_map<uint32_t, XferCtx*> pending_urbs_` members to the class
    - Add `void cleanup_connection_()` and `static int32_t map_usb_status(usb_transfer_status_t)` declarations
    - Add `static uint32_t map_device_speed(usb_speed_t)` declaration
    - _Requirements: 8.1, 8.3, 4.1, 4.2_

  - [x] 1.2 Initialize mutexes in setup() and implement mutex-guarded send_response
    - Create `send_mutex_` and `pending_mutex_` with `xSemaphoreCreateMutex()` in `setup()`
    - Refactor `send_response()` to acquire `send_mutex_` before writing and release after all bytes sent (header + data as one atomic unit)
    - Ensure send_response checks `client_sock_ != -1` before sending
    - _Requirements: 8.3, 9.2_

- [x] 2. Implement error status mapping and speed reporting
  - [x] 2.1 Implement map_usb_status() function
    - Create `map_usb_status()` translating ESP-IDF `usb_transfer_status_t` to negative Linux errno values: COMPLETED→0, ERROR→-5, TIMED_OUT→-110, CANCELED→-104, STALL→-32, OVERFLOW→-75, NO_DEVICE→-19
    - Replace all `__bswap_32(-ETIME)` status assignments in ctrl_cb_ and ep_cb_ with `__bswap_32(map_usb_status(xfer->status))`
    - _Requirements: 3.6, 3.7_

  - [x] 2.2 Write property test for USB error status mapping
    - **Property 7: USB Error Status Mapping**
    - **Validates: Requirements 3.6, 3.8**

  - [x] 2.3 Implement map_device_speed() and update fill_devlist_/fill_import_
    - Create `map_device_speed()` mapping `usb_speed_t` enum to USB/IP speed constants (LOW→1, FULL→2, HIGH→3, default→2 with warning)
    - Replace hardcoded `__bswap_32(3)` speed in `fill_devlist_()` and `fill_import_()` with `__bswap_32(map_device_speed(dev_info_.speed))`
    - _Requirements: 11.1, 11.2, 11.3, 11.5, 11.6_

  - [x] 2.4 Write property test for device speed mapping
    - **Property 5: Device Speed Mapping**
    - **Validates: Requirements 2.4, 11.1, 11.2, 11.3, 11.5, 11.6**

- [x] 3. Fix control transfer actual_length and response formatting
  - [x] 3.1 Fix ctrl_cb_ to subtract 8-byte setup from actual_length
    - For control IN: set `actual_length = max(0, xfer->actual_num_bytes - 8)` and copy data starting at offset 8 in `xfer->data_buffer`
    - For control OUT: set `actual_length = 0`, no data copy
    - For failed transfers (status != COMPLETED): set `actual_length = 0`, no data copy
    - Set `start_frame=0`, `number_of_packets=0`, `error_count=0`, and zero bytes 40-47 (setup/padding field)
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 3.4, 3.5, 3.8_

  - [x] 3.2 Write property test for control transfer actual_length correction
    - **Property 8: Control Transfer Actual Length Correction**
    - **Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5**

  - [x] 3.3 Fix ep_cb_ response formatting
    - For bulk/interrupt IN with success: `actual_length = xfer->actual_num_bytes`
    - For OUT transfers: `actual_length = 0`
    - For failed transfers: `actual_length = 0`, no data copy
    - Ensure `devid=0`, `direction=0`, `ep=0`, `start_frame=0`, `number_of_packets=0`, `error_count=0`, bytes 40-47 zeroed
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.8_

  - [x] 3.4 Write property test for RET_SUBMIT response invariants
    - **Property 6: RET_SUBMIT Response Invariants**
    - **Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.8**

- [x] 4. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 5. Implement pending URB tracking and unlink handling
  - [x] 5.1 Register URBs in pending map on submit
    - In `parse_request()` CMD_SUBMIT path: after allocating the new `usbip_submit_t`, create `XferCtx` with seqnum (host byte order), store in `pending_urbs_` under `pending_mutex_`
    - On submit failure (`req_ctrl_xfer_` or `req_ep_xfer_` returns error): remove from `pending_urbs_`, send error response with mapped status, free allocations
    - _Requirements: 8.1, 8.2_

  - [x] 5.2 Update callbacks to check pending map and cancelled flag
    - In `ctrl_cb_` and `ep_cb_`: lock `pending_mutex_`, check if seqnum still in `pending_urbs_`
    - If cancelled (not in map or `cancelled` flag set): free resources, do NOT send response
    - If not cancelled: remove from map, unlock, build and send response via mutex-guarded send
    - _Requirements: 4.1, 4.3, 8.2_

  - [x] 5.3 Implement proper unlink handler with pending URB lookup
    - On CMD_UNLINK: extract `unlink_seqnum` from payload, lock `pending_mutex_`
    - If found in `pending_urbs_`: set `cancelled=true`, remove from map, respond with status=-ECONNRESET (-104)
    - If not found: respond with status=0
    - Echo the unlink request's own seqnum (not the target seqnum) in the response header
    - _Requirements: 4.1, 4.2, 4.3, 4.4_

  - [x] 5.4 Write property test for unlink pending URB returns ECONNRESET
    - **Property 10: Unlink Pending URB Returns ECONNRESET**
    - **Validates: Requirements 4.1, 4.3**

  - [x] 5.5 Write property test for unlink completed URB returns zero
    - **Property 11: Unlink Completed URB Returns Zero**
    - **Validates: Requirements 4.2, 4.4**

- [x] 6. Implement buffer overflow protection and isochronous rejection
  - [x] 6.1 Add buffer overflow check for OUT transfers in handle_submit
    - Before allocating XferCtx: check if `transfer_buffer_length > 1024` for OUT direction
    - If exceeded: respond with RET_SUBMIT status=-ENOMEM (-12), actual_length=0, original seqnum echoed
    - For control IN with wLength > 1024: clamp wLength to 1024 in the setup packet before forwarding
    - _Requirements: 6.1, 6.3, 6.5_

  - [x] 6.2 Write property test for transfer buffer overflow protection
    - **Property 9: Transfer Buffer Overflow Protection**
    - **Validates: Requirements 6.1, 6.3, 6.5**

  - [x] 6.3 Implement isochronous transfer rejection
    - In CMD_SUBMIT path: check `number_of_packets > 0` (after byte-swap)
    - Calculate and drain the full payload from the TCP stream (transfer_buffer + iso descriptors)
    - Respond with RET_SUBMIT: status=-ENOSYS (-38), actual_length=0, number_of_packets=0
    - _Requirements: 7.1, 7.2_

  - [x] 6.4 Write property test for isochronous transfer rejection
    - **Property 12: Isochronous Transfer Rejection**
    - **Validates: Requirements 7.1, 7.2**

- [x] 7. Harden TCP reassembly state machine
  - [x] 7.1 Add isochronous payload size to TCP reassembly needed calculation
    - Update the `needed` size calculation in `tcp_task_` for CMD_SUBMIT with `number_of_packets > 0`: add `number_of_packets * 16` bytes (iso descriptor size) to `needed`
    - Add buffer overflow protection: if `buf_len` reaches 4096 without a complete message, close connection and reset buffer
    - _Requirements: 12.1, 12.3, 12.7_

  - [x] 7.2 Fix OP_REQ_IMPORT to require 40 bytes and validate message discrimination
    - Ensure OP_REQ_IMPORT requires exactly 40 bytes (8-byte header + 32-byte busid)
    - Fix message type discrimination: check 16-bit command at offset 2 for OP codes first, then fall through to 32-bit command at offset 0 for URB codes
    - Handle unrecognized command: log warning and close connection
    - _Requirements: 12.5, 12.6_

  - [x] 7.3 Write property test for TCP reassembly correctness
    - **Property 13: TCP Reassembly Correctness**
    - **Validates: Requirements 12.1, 12.2, 12.3, 12.5**

- [x] 8. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 9. Implement connection cleanup and device disconnect handling
  - [x] 9.1 Implement cleanup_connection_() with full resource teardown
    - Mark `client_sock_ = -1` to prevent callbacks from sending
    - Lock `pending_mutex_`, set `cancelled=true` on all pending XferCtx entries, clear map, unlock
    - Shutdown and close the socket
    - Reset `interfaces_claimed_ = false`
    - _Requirements: 9.1, 9.3, 9.7_

  - [x] 9.2 Integrate cleanup into tcp_task_ disconnect paths
    - Call `cleanup_connection_()` on recv returning 0 (FIN), recv error, or timeout
    - Ensure no new connection accepted until cleanup completes
    - Add device disconnect handling: on `on_device_disconnected()`, if client connected, send -ENODEV for pending URBs
    - _Requirements: 9.2, 9.3, 9.4, 9.5, 9.6_

  - [x] 9.3 Write unit tests for connection cleanup
    - Test that all XferCtx entries are freed on disconnect
    - Test that callbacks finding cancelled flag don't send responses
    - Test that server returns to accept loop after cleanup
    - _Requirements: 9.1, 9.3, 9.7_

- [x] 10. Fix OP_REP_DEVLIST and OP_REP_IMPORT response details
  - [x] 10.1 Fix OP_REP_IMPORT response size and error handling
    - Ensure successful import response is exactly 320 bytes (8-byte header + 312-byte device struct)
    - Ensure failed import response is exactly 8 bytes (header only with non-zero status)
    - Validate busid matching as null-terminated string comparison within the 32-byte field
    - Echo the requested busid back in the response's device structure
    - _Requirements: 2.1, 2.2, 2.3, 2.6_

  - [x] 10.2 Write property test for OP_REP_IMPORT response format
    - **Property 4: OP_REP_IMPORT Response Format**
    - **Validates: Requirements 2.1, 2.2, 2.3, 2.6**

  - [x] 10.3 Fix OP_REP_DEVLIST response structure and size
    - Ensure total response size is 12 + 312 + (N × 4) when device present, 12 when absent
    - Verify all field offsets match the 312-byte usbip_usb_device structure exactly
    - Ensure `sizeof(usbip_devlist_t)` matches expected wire format or send fields individually
    - _Requirements: 1.1, 1.2, 1.3, 1.4, 1.5_

  - [x] 10.4 Write property test for OP_REP_DEVLIST structure integrity
    - **Property 3: OP_REP_DEVLIST Structure Integrity**
    - **Validates: Requirements 1.1, 1.2, 1.3, 1.4**

- [x] 11. Wire format endianness verification and interface claiming guard
  - [x] 11.1 Audit and fix all byte-order conversions in response builders
    - Verify all 32-bit URB header fields use `htonl()` / `__bswap_32()`
    - Verify all 16-bit OP fields use `htons()` / `__bswap_16()`
    - Verify setup packet (8 bytes) and transfer_buffer are NOT byte-swapped
    - Ensure signed status values (negative errno) are correctly encoded as big-endian two's complement
    - _Requirements: 13.1, 13.2, 13.3, 13.4, 13.5, 13.6_

  - [x] 11.2 Write property test for big-endian encoding correctness
    - **Property 1: Big-Endian Encoding Correctness**
    - **Validates: Requirements 1.5, 3.7, 13.1, 13.2, 13.3, 13.4**

  - [x] 11.3 Write property test for opaque field pass-through
    - **Property 2: Opaque Field Pass-Through**
    - **Validates: Requirements 13.5, 13.6**

  - [x] 11.4 Harden ensure_interfaces_claimed_() with error handling
    - If any `usb_host_interface_claim()` call fails: respond to the triggering URB with status=-ENODEV
    - If device handle or config descriptor not available: respond with -ENODEV
    - Ensure `interfaces_claimed_` is only set to true when all claims succeed
    - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5_

- [x] 12. Final integration and concurrent URB verification
  - [x] 12.1 Add device-not-ready error response for CMD_SUBMIT
    - When `device_ready_ == false` and CMD_SUBMIT received: send proper RET_SUBMIT with status=-ENODEV and original seqnum, instead of silently dropping
    - When heap allocation for XferCtx fails: send RET_SUBMIT with status=-ENOMEM
    - _Requirements: 8.6, 9.6_

  - [x] 12.2 Write property test for concurrent URB seqnum preservation
    - **Property 14: Concurrent URB Seqnum Preservation**
    - **Validates: Requirements 8.1, 8.2**

  - [x] 12.3 Write property test for socket write atomicity
    - **Property 15: Socket Write Atomicity**
    - **Validates: Requirements 8.3**

- [x] 13. Final checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate universal correctness properties from the design document
- Unit tests validate specific examples and edge cases
- The implementation language is C++ (ESP-IDF / ESP32-S3 platform)
- All property tests use Rapidcheck as specified in the design's testing strategy
- The two core files modified are `usb_ip.h` and `usb_ip.cpp` in `esphome_components/usb_ip/`
- Mock layers for USB Host API and sockets are needed for host-side testing

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2", "2.1", "2.3"] },
    { "id": 2, "tasks": ["2.2", "2.4", "3.1", "3.3"] },
    { "id": 3, "tasks": ["3.2", "3.4", "5.1"] },
    { "id": 4, "tasks": ["5.2", "5.3", "6.1", "6.3"] },
    { "id": 5, "tasks": ["5.4", "5.5", "6.2", "6.4", "7.1", "7.2"] },
    { "id": 6, "tasks": ["7.3", "9.1"] },
    { "id": 7, "tasks": ["9.2", "10.1", "10.3"] },
    { "id": 8, "tasks": ["9.3", "10.2", "10.4", "11.1", "11.4"] },
    { "id": 9, "tasks": ["11.2", "11.3", "12.1"] },
    { "id": 10, "tasks": ["12.2", "12.3"] }
  ]
}
```
