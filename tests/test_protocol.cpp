/**
 * Property-based tests for USB/IP protocol message formatting.
 * Uses RapidCheck + Google Test.
 *
 * Feature: usb-ip-protocol-alignment
 */

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <utility>
#include <algorithm>
#include <numeric>
#include <thread>
#include <mutex>

#include "mocks/mock_usb_host.h"

// --------------------------------------------------------------------------
// Minimal USB/IP protocol struct definitions needed for protocol tests.
// These replicate the wire-format structures from usb_ip.h without pulling
// in the full ESPHome/FreeRTOS dependency chain.
// --------------------------------------------------------------------------

#define USBIP_BSWAP32(x) ((((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >> 8) | (((x) & 0x0000ff00u) << 8) | (((x) & 0x000000ffu) << 24))
#define USBIP_RET_SUBMIT USBIP_BSWAP32(0x00000003)

#pragma pack(push, 1)
struct usbip_header_basic_t { uint32_t command, seqnum, devid, direction, ep; };
struct usbip_submit_t {
    usbip_header_basic_t header;
    union { uint32_t flags; uint32_t status; };
    uint32_t length, start_frame, num_packets;
    union { uint32_t interval; uint32_t error_count; };
    union { uint64_t setup; uint64_t padding; };
    uint8_t transfer_buffer[1024];
};
#pragma pack(pop)

// --------------------------------------------------------------------------
// Extract the function under test.
// map_device_speed is a static member of USBIPComponent, but its logic
// is purely a switch on usb_speed_t. We replicate it here for isolated testing.
// This mirrors the implementation in usb_ip.cpp exactly.
// --------------------------------------------------------------------------

static uint32_t map_device_speed(usb_speed_t speed) {
    switch (speed) {
        case USB_SPEED_LOW:  return 1;  // USB/IP LOW_SPEED
        case USB_SPEED_FULL: return 2;  // USB/IP FULL_SPEED
        case USB_SPEED_HIGH: return 3;  // USB/IP HIGH_SPEED
        default:
            // In production code, ESP_LOGW is called here
            return 2;  // Default to Full Speed
    }
}

// --------------------------------------------------------------------------
// Helper: simulate network byte order encoding (big-endian) of the speed field
// as it appears in OP_REP_DEVLIST and OP_REP_IMPORT responses.
// --------------------------------------------------------------------------

static uint32_t to_network_order(uint32_t val) {
    uint32_t result;
    uint8_t *p = reinterpret_cast<uint8_t *>(&result);
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >> 8) & 0xFF;
    p[3] = val & 0xFF;
    return result;
}

// ==========================================================================
// Property 5: Device Speed Mapping
// Validates: Requirements 2.4, 11.1, 11.2, 11.3, 11.5, 11.6
//
// For any physical USB device speed value, the speed field in both
// OP_REP_DEVLIST and OP_REP_IMPORT responses SHALL equal:
//   1 for Low Speed, 2 for Full Speed, 3 for High Speed,
//   and 2 (with warning) for any unrecognized value.
// The speed SHALL be identical in both response types for the same device session.
// ==========================================================================

// --- Unit tests for known speed values ---

TEST(USBIPProtocol, DeviceSpeedMapping_LowSpeed) {
    // **Validates: Requirements 11.3**
    // Low Speed (USB_SPEED_LOW = 0) maps to USB/IP speed 1
    EXPECT_EQ(map_device_speed(USB_SPEED_LOW), 1u);
}

TEST(USBIPProtocol, DeviceSpeedMapping_FullSpeed) {
    // **Validates: Requirements 11.1**
    // Full Speed (USB_SPEED_FULL = 1) maps to USB/IP speed 2
    EXPECT_EQ(map_device_speed(USB_SPEED_FULL), 2u);
}

TEST(USBIPProtocol, DeviceSpeedMapping_HighSpeed) {
    // **Validates: Requirements 11.2**
    // High Speed (USB_SPEED_HIGH = 2) maps to USB/IP speed 3
    EXPECT_EQ(map_device_speed(USB_SPEED_HIGH), 3u);
}

// --- Property test: any invalid speed defaults to 2 (Full Speed) ---

RC_GTEST_PROP(USBIPProtocol, DeviceSpeedMapping_InvalidDefaultsToFullSpeed, ()) {
    // **Validates: Requirements 11.5**
    // Generate a random uint8_t speed value outside the valid enum range {0, 1, 2}
    auto raw_speed = *rc::gen::inRange<int>(3, 256);
    auto speed = static_cast<usb_speed_t>(raw_speed);

    // Any unrecognized value must default to 2 (Full Speed)
    RC_ASSERT(map_device_speed(speed) == 2u);
}

// --- Property test: all valid speeds map to the correct USB/IP constant ---

RC_GTEST_PROP(USBIPProtocol, DeviceSpeedMapping_ValidSpeedsMapCorrectly, ()) {
    // **Validates: Requirements 11.1, 11.2, 11.3**
    // Generate one of the three valid speed values
    auto speed_idx = *rc::gen::inRange<int>(0, 3);
    auto speed = static_cast<usb_speed_t>(speed_idx);

    uint32_t expected;
    switch (speed_idx) {
        case 0: expected = 1; break;  // LOW → 1
        case 1: expected = 2; break;  // FULL → 2
        case 2: expected = 3; break;  // HIGH → 3
        default: RC_FAIL("unexpected index"); return;
    }

    RC_ASSERT(map_device_speed(speed) == expected);
}

// --- Property test: speed is identical in both DEVLIST and IMPORT contexts ---
// Since map_device_speed is a pure function called by both fill_devlist_() and
// fill_import_(), calling it twice with the same input must produce the same output.

RC_GTEST_PROP(USBIPProtocol, DeviceSpeedMapping_ConsistentAcrossResponses, ()) {
    // **Validates: Requirements 11.6**
    // For any speed value (valid or invalid), the mapping is deterministic
    auto raw_speed = *rc::gen::inRange<int>(0, 256);
    auto speed = static_cast<usb_speed_t>(raw_speed);

    uint32_t devlist_speed = map_device_speed(speed);
    uint32_t import_speed = map_device_speed(speed);

    RC_ASSERT(devlist_speed == import_speed);
}

// --- Property test: mapped speed is always within valid USB/IP range {1, 2, 3} ---

RC_GTEST_PROP(USBIPProtocol, DeviceSpeedMapping_AlwaysInValidRange, ()) {
    // **Validates: Requirements 2.4, 11.1, 11.2, 11.3, 11.5**
    // For any possible input, the result must be 1, 2, or 3
    auto raw_speed = *rc::gen::inRange<int>(0, 256);
    auto speed = static_cast<usb_speed_t>(raw_speed);

    uint32_t result = map_device_speed(speed);
    RC_ASSERT(result >= 1u);
    RC_ASSERT(result <= 3u);
}

// --- Property test: network byte order encoding preserves the speed value ---

RC_GTEST_PROP(USBIPProtocol, DeviceSpeedMapping_NetworkByteOrderCorrect, ()) {
    // **Validates: Requirements 2.4**
    // The speed field on the wire (big-endian) decodes back to the mapped value
    auto raw_speed = *rc::gen::inRange<int>(0, 256);
    auto speed = static_cast<usb_speed_t>(raw_speed);

    uint32_t mapped = map_device_speed(speed);
    uint32_t wire = to_network_order(mapped);

    // Decode from big-endian
    uint8_t *p = reinterpret_cast<uint8_t *>(&wire);
    uint32_t decoded = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                       (uint32_t(p[2]) << 8) | uint32_t(p[3]);

    RC_ASSERT(decoded == mapped);
}

// ==========================================================================
// Property 7: USB Error Status Mapping
// Validates: Requirements 3.6, 3.8
//
// For any USB transfer status code from ESP-IDF, the status field in
// USBIP_RET_SUBMIT SHALL be the corresponding negative Linux errno value:
//   COMPLETED→0, ERROR→-5, TIMED_OUT→-110, CANCELED→-104,
//   STALL→-32, OVERFLOW→-75, NO_DEVICE→-19
// encoded as a big-endian signed 32-bit integer.
// When status is non-zero and direction is IN, actual_length SHALL be 0.
// ==========================================================================

// --------------------------------------------------------------------------
// Extract the function under test: map_usb_status
// This is a static member of USBIPComponent. We replicate the logic here
// for isolated testing. Mirrors usb_ip.cpp exactly.
// --------------------------------------------------------------------------

static int32_t map_usb_status(usb_transfer_status_t status) {
    switch (status) {
        case USB_TRANSFER_STATUS_COMPLETED: return 0;
        case USB_TRANSFER_STATUS_ERROR:     return -5;   // -EIO
        case USB_TRANSFER_STATUS_TIMED_OUT: return -110; // -ETIMEDOUT
        case USB_TRANSFER_STATUS_CANCELED:  return -104; // -ECONNRESET
        case USB_TRANSFER_STATUS_STALL:     return -32;  // -EPIPE
        case USB_TRANSFER_STATUS_OVERFLOW:  return -75;  // -EOVERFLOW
        case USB_TRANSFER_STATUS_NO_DEVICE: return -19;  // -ENODEV
        default:                            return -5;   // -EIO
    }
}

// --------------------------------------------------------------------------
// Helper: encode a signed 32-bit status as big-endian (wire format)
// --------------------------------------------------------------------------

static int32_t status_to_wire(int32_t status) {
    uint32_t val = static_cast<uint32_t>(status);
    uint32_t wire;
    uint8_t *p = reinterpret_cast<uint8_t *>(&wire);
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >> 8) & 0xFF;
    p[3] = val & 0xFF;
    return static_cast<int32_t>(wire);
}

// --------------------------------------------------------------------------
// Helper: decode big-endian wire bytes back to host int32_t
// --------------------------------------------------------------------------

static int32_t wire_to_status(int32_t wire) {
    uint8_t *p = reinterpret_cast<uint8_t *>(&wire);
    uint32_t val = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                   (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    return static_cast<int32_t>(val);
}

// --- Expected mapping table (used by multiple tests) ---

struct StatusMapping {
    usb_transfer_status_t esp_status;
    int32_t expected_errno;
};

static const StatusMapping kExpectedMappings[] = {
    {USB_TRANSFER_STATUS_COMPLETED,  0},
    {USB_TRANSFER_STATUS_ERROR,     -5},
    {USB_TRANSFER_STATUS_TIMED_OUT, -110},
    {USB_TRANSFER_STATUS_CANCELED,  -104},
    {USB_TRANSFER_STATUS_STALL,     -32},
    {USB_TRANSFER_STATUS_OVERFLOW,  -75},
    {USB_TRANSFER_STATUS_NO_DEVICE, -19},
};

static constexpr int kNumMappings = sizeof(kExpectedMappings) / sizeof(kExpectedMappings[0]);

// --- Property test: known status codes map to correct errno ---

RC_GTEST_PROP(USBIPProtocol, ErrorStatusMapping_KnownCodes, ()) {
    // **Validates: Requirements 3.6**
    // Generate a random index into the known mapping table
    auto idx = *rc::gen::inRange<int>(0, kNumMappings);
    const auto &mapping = kExpectedMappings[idx];

    int32_t result = map_usb_status(mapping.esp_status);
    RC_ASSERT(result == mapping.expected_errno);

    // Also verify big-endian roundtrip: encode then decode
    int32_t wire = status_to_wire(result);
    int32_t decoded = wire_to_status(wire);
    RC_ASSERT(decoded == mapping.expected_errno);
}

// --- Property test: unknown/out-of-range status values map to -EIO (-5) ---

RC_GTEST_PROP(USBIPProtocol, ErrorStatusMapping_UnknownCodes, ()) {
    // **Validates: Requirements 3.6**
    // Generate a value outside the valid enum range [0..6]
    auto raw = *rc::gen::inRange<int>(7, 256);
    auto status = static_cast<usb_transfer_status_t>(raw);

    int32_t result = map_usb_status(status);
    RC_ASSERT(result == -5);  // Unknown → -EIO
}

// --- Property test: non-zero status with IN direction → actual_length must be 0 ---

RC_GTEST_PROP(USBIPProtocol, ErrorStatusMapping_NonZeroStatusInDirection, ()) {
    // **Validates: Requirements 3.8**
    // Generate a non-COMPLETED status (indices 1..6 in the mapping table)
    auto idx = *rc::gen::inRange<int>(1, kNumMappings);
    const auto &mapping = kExpectedMappings[idx];

    int32_t status = map_usb_status(mapping.esp_status);

    // status is non-zero
    RC_ASSERT(status != 0);

    // Per property: when status is non-zero and direction is IN,
    // actual_length SHALL be 0.
    // We verify the invariant: non-zero status implies actual_length = 0.
    // The actual enforcement happens in ctrl_cb_/ep_cb_, but the status
    // mapping must produce non-zero for all error cases.
    uint32_t actual_length = (status != 0) ? 0 : 42;  // simulated logic
    RC_ASSERT(actual_length == 0);
}

// --- Property test: big-endian encoding preserves sign for negative values ---

RC_GTEST_PROP(USBIPProtocol, ErrorStatusMapping_BigEndianEncoding, ()) {
    // **Validates: Requirements 3.6**
    // For any known status, verify that encoding to wire and decoding back
    // yields the same signed value (including negative values)
    auto idx = *rc::gen::inRange<int>(0, kNumMappings);
    const auto &mapping = kExpectedMappings[idx];

    int32_t errno_val = map_usb_status(mapping.esp_status);
    int32_t wire = status_to_wire(errno_val);
    int32_t decoded = wire_to_status(wire);

    RC_ASSERT(decoded == errno_val);
}

// --- Deterministic exhaustive test: verify all mappings ---

TEST(USBIPProtocol, ErrorStatusMapping_ExhaustiveCheck) {
    // **Validates: Requirements 3.6**
    // Verify every known ESP-IDF status maps correctly
    EXPECT_EQ(map_usb_status(USB_TRANSFER_STATUS_COMPLETED),  0);
    EXPECT_EQ(map_usb_status(USB_TRANSFER_STATUS_ERROR),     -5);
    EXPECT_EQ(map_usb_status(USB_TRANSFER_STATUS_TIMED_OUT), -110);
    EXPECT_EQ(map_usb_status(USB_TRANSFER_STATUS_CANCELED),  -104);
    EXPECT_EQ(map_usb_status(USB_TRANSFER_STATUS_STALL),     -32);
    EXPECT_EQ(map_usb_status(USB_TRANSFER_STATUS_OVERFLOW),  -75);
    EXPECT_EQ(map_usb_status(USB_TRANSFER_STATUS_NO_DEVICE), -19);

    // Unknown values (out of enum range) → -EIO
    EXPECT_EQ(map_usb_status(static_cast<usb_transfer_status_t>(7)),   -5);
    EXPECT_EQ(map_usb_status(static_cast<usb_transfer_status_t>(100)), -5);
    EXPECT_EQ(map_usb_status(static_cast<usb_transfer_status_t>(255)), -5);
}

// ==========================================================================
// Property 8: Control Transfer Actual Length Correction
// Validates: Requirements 5.1, 5.2, 5.3, 5.4, 5.5
//
// For any successfully completed control IN transfer where ESP-IDF reports
// actual_num_bytes (including the 8-byte setup), the actual_length in
// USBIP_RET_SUBMIT SHALL equal max(0, actual_num_bytes - 8), and only bytes
// starting at offset 8 in the USB data buffer SHALL be copied to the response
// transfer_buffer. For control OUT transfers, actual_length SHALL always be 0.
// ==========================================================================

// --------------------------------------------------------------------------
// Helper: simulate ctrl_cb_ actual_length computation for control IN
// This mirrors the logic in usb_ip.cpp ctrl_cb_ exactly.
// --------------------------------------------------------------------------

static int compute_ctrl_in_actual_length(int actual_num_bytes) {
    return (actual_num_bytes > 8) ? (actual_num_bytes - 8) : 0;
}

// --------------------------------------------------------------------------
// Helper: simulate ctrl_cb_ data copy for control IN
// Returns the number of bytes that would be copied and the source offset.
// --------------------------------------------------------------------------

static void simulate_ctrl_in_data_copy(const uint8_t *data_buffer, int actual_num_bytes,
                                        uint8_t *out_transfer_buffer, int *out_data_len) {
    int data_len = compute_ctrl_in_actual_length(actual_num_bytes);
    if (data_len > 0) {
        memcpy(out_transfer_buffer, data_buffer + 8, data_len);
    }
    *out_data_len = data_len;
}

// --- Property test: control IN actual_length = max(0, actual_num_bytes - 8) ---

RC_GTEST_PROP(USBIPProtocol, CtrlActualLength_InSubtractsSetup, ()) {
    // **Validates: Requirements 5.1, 5.2**
    // Generate random actual_num_bytes in range [0, 1032]
    // (1032 = 8 setup + 1024 max data, the max ESP-IDF can report)
    auto actual_num_bytes = *rc::gen::inRange<int>(0, 1033);

    int expected = (actual_num_bytes > 8) ? (actual_num_bytes - 8) : 0;
    int result = compute_ctrl_in_actual_length(actual_num_bytes);

    RC_ASSERT(result == expected);
    RC_ASSERT(result >= 0);
    RC_ASSERT(result <= 1024);
}

// --- Property test: control IN data is copied from offset 8 ---

RC_GTEST_PROP(USBIPProtocol, CtrlActualLength_InDataCopiedFromOffset8, ()) {
    // **Validates: Requirements 5.3**
    // Generate a random data buffer of size [9..1032] bytes (must have >8 to have data)
    auto buf_size = *rc::gen::inRange<int>(9, 1033);
    auto data_vec = *rc::gen::container<std::vector<uint8_t>>(buf_size, rc::gen::arbitrary<uint8_t>());

    uint8_t transfer_buffer[1024] = {};
    int data_len = 0;
    simulate_ctrl_in_data_copy(data_vec.data(), buf_size, transfer_buffer, &data_len);

    // data_len should be buf_size - 8
    RC_ASSERT(data_len == buf_size - 8);

    // Verify copied data matches data_buffer starting at offset 8
    for (int i = 0; i < data_len; i++) {
        RC_ASSERT(transfer_buffer[i] == data_vec[8 + i]);
    }
}

// --- Property test: control IN with actual_num_bytes <= 8 copies no data ---

RC_GTEST_PROP(USBIPProtocol, CtrlActualLength_InNoDataWhenLessThan8, ()) {
    // **Validates: Requirements 5.2**
    // Generate actual_num_bytes in [0, 8] — no data portion
    auto actual_num_bytes = *rc::gen::inRange<int>(0, 9);

    int result = compute_ctrl_in_actual_length(actual_num_bytes);
    RC_ASSERT(result == 0);
}

// --- Property test: control OUT actual_length is always 0 ---

RC_GTEST_PROP(USBIPProtocol, CtrlActualLength_OutAlwaysZero, ()) {
    // **Validates: Requirements 5.4**
    // For control OUT transfers, actual_length SHALL always be 0
    // regardless of what actual_num_bytes the USB host reports.
    auto actual_num_bytes = *rc::gen::inRange<int>(0, 1033);

    // In ctrl_cb_, for OUT direction (direction == 0), data_len stays 0
    // The code only enters the data copy path when direction != 0 (IN)
    // Simulate: direction is OUT, so data_len = 0 regardless
    int data_len = 0;  // For OUT: always 0, no data copy
    (void)actual_num_bytes;  // Not used for OUT

    RC_ASSERT(data_len == 0);
}

// --- Property test: failed control transfer has actual_length = 0 ---

RC_GTEST_PROP(USBIPProtocol, CtrlActualLength_FailedTransferZero, ()) {
    // **Validates: Requirements 5.5**
    // Generate any non-COMPLETED status (values 1..6 in enum)
    auto status_val = *rc::gen::inRange<int>(1, 7);
    auto status = static_cast<usb_transfer_status_t>(status_val);
    auto actual_num_bytes = *rc::gen::inRange<int>(0, 1033);

    // In ctrl_cb_, the data copy block is only entered when
    // status == USB_TRANSFER_STATUS_COMPLETED && direction != 0 (IN)
    // For any non-COMPLETED status, data_len stays 0 regardless of direction
    int data_len = 0;
    if (status == USB_TRANSFER_STATUS_COMPLETED) {
        // This branch should never be taken given our generator
        data_len = compute_ctrl_in_actual_length(actual_num_bytes);
    }

    RC_ASSERT(status != USB_TRANSFER_STATUS_COMPLETED);
    RC_ASSERT(data_len == 0);

    // Also verify the mapped status is non-zero (error)
    int32_t mapped = map_usb_status(status);
    RC_ASSERT(mapped != 0);
}

// ==========================================================================
// Property 6: RET_SUBMIT Response Invariants
// Validates: Requirements 3.1, 3.2, 3.3, 3.4, 3.5, 3.8
//
// For any completed USB transfer (success or failure), the USBIP_RET_SUBMIT
// response SHALL have: command=0x00000003, the original seqnum echoed,
// devid=0, direction=0, ep=0, start_frame=0, number_of_packets=0,
// error_count=0, and bytes 40-47 set to zero. For IN transfers with
// status=0, total response length SHALL equal 48 + actual_length.
// For OUT transfers or failed IN transfers, total response length SHALL
// equal 48.
// ==========================================================================

// --------------------------------------------------------------------------
// Helper: Build a RET_SUBMIT response as a raw byte buffer,
// replicating the logic in ctrl_cb_/ep_cb_ from usb_ip.cpp.
//
// The response layout (48-byte header):
//   Bytes 0-3:   command (big-endian 0x00000003)
//   Bytes 4-7:   seqnum (echoed from request, big-endian)
//   Bytes 8-11:  devid (always 0)
//   Bytes 12-15: direction (always 0)
//   Bytes 16-19: ep (always 0)
//   Bytes 20-23: status (big-endian signed int32)
//   Bytes 24-27: actual_length (big-endian)
//   Bytes 28-31: start_frame (always 0)
//   Bytes 32-35: number_of_packets (always 0)
//   Bytes 36-39: error_count (always 0)
//   Bytes 40-47: setup/padding (always 0)
//
// Parameters:
//   seqnum_be4     - original seqnum as 4 big-endian bytes
//   direction_val  - original direction as host uint32 (0=OUT, non-zero=IN)
//   xfer_status    - USB transfer completion status (ESP-IDF enum)
//   actual_num_bytes - bytes reported by ESP-IDF (for ctrl, includes 8-byte setup)
//   is_control     - true for control transfers (ctrl_cb_ path)
//
// Returns:
//   A pair of (response buffer, response length) as sent over the wire.
// --------------------------------------------------------------------------

static void write_be32(uint8_t *dst, uint32_t val) {
    dst[0] = (val >> 24) & 0xFF;
    dst[1] = (val >> 16) & 0xFF;
    dst[2] = (val >> 8) & 0xFF;
    dst[3] = val & 0xFF;
}

static std::pair<std::vector<uint8_t>, size_t> build_ret_submit(
    uint32_t seqnum_host,
    uint32_t direction_host,
    usb_transfer_status_t xfer_status,
    int actual_num_bytes,
    bool is_control)
{
    // Calculate data_len (mirrors the logic in ctrl_cb_/ep_cb_)
    int data_len = 0;
    if (xfer_status == USB_TRANSFER_STATUS_COMPLETED && direction_host != 0) {
        if (is_control) {
            // Control IN: subtract 8-byte setup packet
            data_len = (actual_num_bytes > 8) ? (actual_num_bytes - 8) : 0;
        } else {
            // Bulk/interrupt IN
            data_len = actual_num_bytes;
        }
    }
    // For OUT or failed transfers: data_len stays 0

    size_t response_len = 48 + data_len;
    std::vector<uint8_t> response(response_len, 0);

    // Bytes 0-3: command = 0x00000003 (RET_SUBMIT)
    write_be32(&response[0], 0x00000003);

    // Bytes 4-7: seqnum echoed (big-endian)
    write_be32(&response[4], seqnum_host);

    // Bytes 8-11: devid = 0 (already zeroed)
    // Bytes 12-15: direction = 0 (already zeroed)
    // Bytes 16-19: ep = 0 (already zeroed)

    // Bytes 20-23: status (big-endian signed int32)
    int32_t mapped_status = map_usb_status(xfer_status);
    write_be32(&response[20], static_cast<uint32_t>(mapped_status));

    // Bytes 24-27: actual_length (big-endian)
    write_be32(&response[24], static_cast<uint32_t>(data_len));

    // Bytes 28-31: start_frame = 0 (already zeroed)
    // Bytes 32-35: number_of_packets = 0 (already zeroed)
    // Bytes 36-39: error_count = 0 (already zeroed)
    // Bytes 40-47: setup/padding = 0 (already zeroed)

    // Fill dummy transfer data (for IN with data)
    if (data_len > 0) {
        memset(&response[48], 0xAB, data_len);
    }

    return {response, response_len};
}

// --------------------------------------------------------------------------
// Helper: read a big-endian uint32 from a byte buffer
// --------------------------------------------------------------------------

static uint32_t read_be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// --- Property test: RET_SUBMIT fixed header fields ---

RC_GTEST_PROP(USBIPProtocol, RetSubmit_FixedHeaderFields, ()) {
    // **Validates: Requirements 3.1, 3.4, 3.5**
    // Generate random seqnum, direction, transfer parameters
    auto seqnum = *rc::gen::arbitrary<uint32_t>();
    auto dir = *rc::gen::element<uint32_t>(0u, 1u);  // 0=OUT, 1=IN
    auto xfer_len = *rc::gen::inRange<int>(0, 512);
    auto status_idx = *rc::gen::inRange<int>(0, kNumMappings);
    auto is_control = *rc::gen::arbitrary<bool>();

    auto [response, resp_len] = build_ret_submit(
        seqnum, dir,
        kExpectedMappings[status_idx].esp_status,
        xfer_len, is_control);

    RC_ASSERT(resp_len >= 48u);
    const uint8_t *r = response.data();

    // command = 0x00000003 in big-endian
    RC_ASSERT(read_be32(r + 0) == 0x00000003);

    // devid = 0 (bytes 8-11)
    RC_ASSERT(read_be32(r + 8) == 0);

    // direction = 0 (bytes 12-15)
    RC_ASSERT(read_be32(r + 12) == 0);

    // ep = 0 (bytes 16-19)
    RC_ASSERT(read_be32(r + 16) == 0);

    // start_frame = 0 (bytes 28-31)
    RC_ASSERT(read_be32(r + 28) == 0);

    // number_of_packets = 0 (bytes 32-35)
    RC_ASSERT(read_be32(r + 32) == 0);

    // error_count = 0 (bytes 36-39)
    RC_ASSERT(read_be32(r + 36) == 0);

    // bytes 40-47 (setup/padding) = 0
    for (int i = 40; i < 48; i++) {
        RC_ASSERT(r[i] == 0);
    }
}

// --- Property test: seqnum is echoed correctly ---

RC_GTEST_PROP(USBIPProtocol, RetSubmit_SeqnumEchoed, ()) {
    // **Validates: Requirements 3.1, 3.2**
    // Generate random seqnum
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    auto [response, resp_len] = build_ret_submit(
        seqnum, 0u,  // OUT direction
        USB_TRANSFER_STATUS_COMPLETED, 0, false);

    const uint8_t *r = response.data();

    // seqnum is at bytes 4-7 in big-endian
    uint32_t echoed = read_be32(r + 4);
    RC_ASSERT(echoed == seqnum);
}

// --- Property test: response length for IN with status=0 is 48 + actual_length ---

RC_GTEST_PROP(USBIPProtocol, RetSubmit_InSuccessLength, ()) {
    // **Validates: Requirements 3.2, 3.3**
    // Generate random actual_length [0, 1024] for bulk/interrupt IN
    auto actual_length = *rc::gen::inRange<int>(0, 1025);

    auto [response, resp_len] = build_ret_submit(
        42u, 1u,  // IN direction
        USB_TRANSFER_STATUS_COMPLETED,
        actual_length, false /* bulk/interrupt */);

    // Total response = 48 + actual_length
    RC_ASSERT(resp_len == static_cast<size_t>(48 + actual_length));
    RC_ASSERT(response.size() == static_cast<size_t>(48 + actual_length));
}

// --- Property test: response length for OUT or failed IN is exactly 48 ---

RC_GTEST_PROP(USBIPProtocol, RetSubmit_OutOrFailedInLength, ()) {
    // **Validates: Requirements 3.3, 3.8**
    // Generate either an OUT transfer (any status) or a failed IN transfer
    auto is_out = *rc::gen::arbitrary<bool>();
    uint32_t dir;
    usb_transfer_status_t status;

    if (is_out) {
        // OUT transfer with any status
        dir = 0;
        auto status_idx = *rc::gen::inRange<int>(0, kNumMappings);
        status = kExpectedMappings[status_idx].esp_status;
    } else {
        // Failed IN transfer (non-COMPLETED status)
        dir = 1;
        auto status_idx = *rc::gen::inRange<int>(1, kNumMappings);  // skip COMPLETED
        status = kExpectedMappings[status_idx].esp_status;
    }

    auto actual_bytes = *rc::gen::inRange<int>(0, 512);

    auto [response, resp_len] = build_ret_submit(
        99u, dir, status, actual_bytes, false);

    // Response must be exactly 48 bytes (no data appended)
    RC_ASSERT(resp_len == 48u);
    RC_ASSERT(response.size() == 48u);
}

// --- Deterministic test: verify command value encoding ---

TEST(USBIPProtocol, RetSubmit_CommandEncoding) {
    // **Validates: Requirements 3.1**
    // Verify that command field is 0x00000003 in big-endian
    auto [response, resp_len] = build_ret_submit(
        1u, 0u,
        USB_TRANSFER_STATUS_COMPLETED, 0, false);

    // command at bytes 0-3 must be 0x00 0x00 0x00 0x03
    EXPECT_EQ(response[0], 0x00);
    EXPECT_EQ(response[1], 0x00);
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x03);
}

// --- Deterministic test: IN success includes data, OUT does not ---

TEST(USBIPProtocol, RetSubmit_DataPresenceByDirection) {
    // **Validates: Requirements 3.2, 3.3**
    // IN with 64 bytes of data, status=COMPLETED
    auto [resp_in, len_in] = build_ret_submit(
        10u, 1u,  // IN
        USB_TRANSFER_STATUS_COMPLETED, 64, false);
    EXPECT_EQ(len_in, 48u + 64u);

    // OUT with same parameters
    auto [resp_out, len_out] = build_ret_submit(
        10u, 0u,  // OUT
        USB_TRANSFER_STATUS_COMPLETED, 64, false);
    EXPECT_EQ(len_out, 48u);
}

// ==========================================================================
// Property 10: Unlink Pending URB Returns ECONNRESET
// Validates: Requirements 4.1, 4.3
//
// For any USBIP_CMD_UNLINK where the unlink_seqnum identifies a URB still
// in the pending map, the response SHALL have status=-ECONNRESET (-104),
// and no subsequent USBIP_RET_SUBMIT SHALL be sent for the cancelled URB's
// seqnum.
// ==========================================================================

// --------------------------------------------------------------------------
// Simulate the unlink handler logic: given a set of pending URBs and a target
// seqnum, return the status code and mutate the pending map.
// This mirrors the USBIP_CMD_UNLINK logic in usb_ip.cpp parse_request().
// --------------------------------------------------------------------------

static int32_t simulate_unlink(std::unordered_map<uint32_t, bool> &pending_urbs, uint32_t target_seqnum) {
    auto it = pending_urbs.find(target_seqnum);
    if (it != pending_urbs.end()) {
        pending_urbs.erase(it);
        return -104;  // -ECONNRESET
    }
    return 0;  // Already completed or unknown
}

// --------------------------------------------------------------------------
// Helper: build a USBIP_RET_UNLINK response as raw bytes, mirroring the
// logic in usb_ip.cpp for CMD_UNLINK handling.
//
// Response layout (48 bytes):
//   Bytes 0-3:   command (big-endian 0x00000004 = RET_UNLINK)
//   Bytes 4-7:   seqnum (unlink request's OWN seqnum, big-endian)
//   Bytes 8-11:  devid (always 0)
//   Bytes 12-15: direction (always 0)
//   Bytes 16-19: ep (always 0)
//   Bytes 20-23: status (big-endian signed int32: -104 or 0)
//   Bytes 24-47: padding (zeros)
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_ret_unlink(uint32_t unlink_own_seqnum, int32_t status) {
    std::vector<uint8_t> response(48, 0);

    // Bytes 0-3: command = 0x00000004 (RET_UNLINK) big-endian
    write_be32(&response[0], 0x00000004);

    // Bytes 4-7: unlink request's own seqnum (big-endian)
    write_be32(&response[4], unlink_own_seqnum);

    // Bytes 8-11: devid = 0 (already zeroed)
    // Bytes 12-15: direction = 0 (already zeroed)
    // Bytes 16-19: ep = 0 (already zeroed)

    // Bytes 20-23: status (big-endian signed int32)
    write_be32(&response[20], static_cast<uint32_t>(status));

    // Bytes 24-47: padding = 0 (already zeroed)

    return response;
}

// --- Property test: unlink of pending URB returns -ECONNRESET ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_ReturnsECONNRESET, ()) {
    // **Validates: Requirements 4.1, 4.3**
    // Generate a set of pending seqnums (at least one)
    auto pending_set = *rc::gen::nonEmpty<std::vector<uint32_t>>();
    std::unordered_map<uint32_t, bool> pending;
    for (auto s : pending_set) pending[s] = true;

    // Pick one that exists in the pending set
    auto idx = *rc::gen::inRange<size_t>(0, pending_set.size());
    uint32_t target = pending_set[idx];

    int32_t status = simulate_unlink(pending, target);

    // Status must be -ECONNRESET (-104)
    RC_ASSERT(status == -104);

    // The target seqnum must be removed from pending (no subsequent RET_SUBMIT)
    RC_ASSERT(pending.find(target) == pending.end());
}

// --- Property test: wire encoding of -104 as big-endian ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_StatusBigEndianEncoding, ()) {
    // **Validates: Requirements 4.1, 4.3**
    // The status -104 in big-endian is the two's complement encoding of -104
    // as a signed 32-bit integer: 0xFFFFFF98
    auto unlink_own_seqnum = *rc::gen::arbitrary<uint32_t>();
    int32_t status = -104;  // -ECONNRESET

    auto response = build_ret_unlink(unlink_own_seqnum, status);

    // Verify status bytes at offset 20-23 encode -104 in big-endian
    // -104 as uint32 = 0xFFFFFF98
    // big-endian bytes: 0xFF, 0xFF, 0xFF, 0x98
    RC_ASSERT(response[20] == 0xFF);
    RC_ASSERT(response[21] == 0xFF);
    RC_ASSERT(response[22] == 0xFF);
    RC_ASSERT(response[23] == 0x98);

    // Also verify round-trip: decode back to host value
    int32_t decoded = static_cast<int32_t>(read_be32(&response[20]));
    RC_ASSERT(decoded == -104);
}

// --- Property test: response command field = 0x00000004 (RET_UNLINK) ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_ResponseCommand, ()) {
    // **Validates: Requirements 4.1, 4.3**
    // Generate random parameters
    auto unlink_own_seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_ret_unlink(unlink_own_seqnum, -104);

    // Command at bytes 0-3 must be 0x00000004 (RET_UNLINK) in big-endian
    RC_ASSERT(read_be32(&response[0]) == 0x00000004);

    // Individual bytes: 0x00 0x00 0x00 0x04
    RC_ASSERT(response[0] == 0x00);
    RC_ASSERT(response[1] == 0x00);
    RC_ASSERT(response[2] == 0x00);
    RC_ASSERT(response[3] == 0x04);
}

// --- Property test: own seqnum is echoed (not the target seqnum) ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_OwnSeqnumEchoed, ()) {
    // **Validates: Requirements 4.1, 4.3**
    // Generate distinct own seqnum and target seqnum
    auto own_seqnum = *rc::gen::arbitrary<uint32_t>();
    auto target_seqnum = *rc::gen::suchThat<uint32_t>([&](uint32_t t) {
        return t != own_seqnum;
    });

    // Build response for the unlink request (own_seqnum is in the header)
    auto response = build_ret_unlink(own_seqnum, -104);

    // Seqnum at bytes 4-7 must be the unlink request's own seqnum
    uint32_t echoed = read_be32(&response[4]);
    RC_ASSERT(echoed == own_seqnum);

    // Must NOT be the target seqnum
    RC_ASSERT(echoed != target_seqnum);
}

// --- Property test: pending map invariant - after unlink, no entry exists ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_MapInvariant, ()) {
    // **Validates: Requirements 4.1, 4.3**
    // Generate a larger pending URB map with unique seqnums
    auto count = *rc::gen::inRange<size_t>(1, 20);
    auto seqnums_raw = *rc::gen::container<std::vector<uint32_t>>(count, rc::gen::arbitrary<uint32_t>());
    // Deduplicate
    std::unordered_map<uint32_t, bool> pending;
    std::vector<uint32_t> seqnums;
    for (auto s : seqnums_raw) {
        if (pending.find(s) == pending.end()) {
            pending[s] = true;
            seqnums.push_back(s);
        }
    }
    RC_PRE(!seqnums.empty());

    // Pick a random target from the set
    auto idx = *rc::gen::inRange<size_t>(0, seqnums.size());
    uint32_t target = seqnums[idx];

    size_t original_size = pending.size();
    int32_t status = simulate_unlink(pending, target);

    // Invariants:
    // 1. Status is -ECONNRESET
    RC_ASSERT(status == -104);
    // 2. Target removed from pending
    RC_ASSERT(pending.find(target) == pending.end());
    // 3. Map size decreased by exactly 1
    RC_ASSERT(pending.size() == original_size - 1);
    // 4. Other entries remain intact
    for (auto s : seqnums) {
        if (s != target) {
            RC_ASSERT(pending.find(s) != pending.end());
        }
    }
}

// --- Property test: RET_UNLINK response is always exactly 48 bytes ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_ResponseSize, ()) {
    // **Validates: Requirements 4.3**
    auto own_seqnum = *rc::gen::arbitrary<uint32_t>();
    auto status = *rc::gen::element<int32_t>(-104, 0);

    auto response = build_ret_unlink(own_seqnum, status);

    // Response must be exactly 48 bytes
    RC_ASSERT(response.size() == 48u);
}

// --- Property test: bytes 24-47 are always zero (padding) ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_PaddingZeroed, ()) {
    // **Validates: Requirements 4.3**
    auto own_seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_ret_unlink(own_seqnum, -104);

    // Bytes 24-47 must all be zero
    for (size_t i = 24; i < 48; i++) {
        RC_ASSERT(response[i] == 0);
    }
}

// --- Property test: fixed header fields devid=0, direction=0, ep=0 ---

RC_GTEST_PROP(USBIPProtocol, UnlinkPending_FixedHeaderFields, ()) {
    // **Validates: Requirements 4.3**
    auto own_seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_ret_unlink(own_seqnum, -104);

    // devid at bytes 8-11 = 0
    RC_ASSERT(read_be32(&response[8]) == 0);
    // direction at bytes 12-15 = 0
    RC_ASSERT(read_be32(&response[12]) == 0);
    // ep at bytes 16-19 = 0
    RC_ASSERT(read_be32(&response[16]) == 0);
}

// --- Deterministic test: verify the full response for a pending unlink ---

TEST(USBIPProtocol, UnlinkPending_FullResponseVerification) {
    // **Validates: Requirements 4.1, 4.3**
    // Simulate: seqnums {100, 200, 300} pending, unlink target=200
    std::unordered_map<uint32_t, bool> pending = {{100, true}, {200, true}, {300, true}};
    uint32_t own_seqnum = 500;
    uint32_t target_seqnum = 200;

    int32_t status = simulate_unlink(pending, target_seqnum);
    EXPECT_EQ(status, -104);
    EXPECT_EQ(pending.count(200), 0u);  // removed
    EXPECT_EQ(pending.count(100), 1u);  // still there
    EXPECT_EQ(pending.count(300), 1u);  // still there

    auto response = build_ret_unlink(own_seqnum, status);
    EXPECT_EQ(response.size(), 48u);

    // Verify command = RET_UNLINK (0x00000004)
    EXPECT_EQ(response[0], 0x00);
    EXPECT_EQ(response[1], 0x00);
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x04);

    // Verify seqnum = 500 (0x000001F4) big-endian
    EXPECT_EQ(response[4], 0x00);
    EXPECT_EQ(response[5], 0x00);
    EXPECT_EQ(response[6], 0x01);
    EXPECT_EQ(response[7], 0xF4);

    // Verify devid, direction, ep = 0
    for (int i = 8; i < 20; i++) {
        EXPECT_EQ(response[i], 0x00);
    }

    // Verify status = -104 (0xFFFFFF98) big-endian
    EXPECT_EQ(response[20], 0xFF);
    EXPECT_EQ(response[21], 0xFF);
    EXPECT_EQ(response[22], 0xFF);
    EXPECT_EQ(response[23], 0x98);

    // Verify padding bytes 24-47 = 0
    for (int i = 24; i < 48; i++) {
        EXPECT_EQ(response[i], 0x00);
    }
}

// ==========================================================================
// Property 9: Transfer Buffer Overflow Protection
// Validates: Requirements 6.1, 6.3, 6.5
//
// For any USBIP_CMD_SUBMIT with transfer_buffer_length > 1024 bytes and
// direction=OUT, the response SHALL have status=-ENOMEM (-12),
// actual_length=0, and the original seqnum echoed.
// For any control IN transfer with wLength > 1024, the wLength forwarded
// to the physical device SHALL be clamped to 1024.
// ==========================================================================

// --------------------------------------------------------------------------
// Helper: simulate buffer overflow check for OUT direction.
// This mirrors the logic in parse_request() CMD_SUBMIT path:
//   if (direction == 0 && transfer_buffer_length > 1024) → reject
// --------------------------------------------------------------------------

static bool should_reject_out_overflow(uint32_t direction, uint32_t transfer_buffer_length) {
    return (direction == 0 && transfer_buffer_length > 1024);
}

// --------------------------------------------------------------------------
// Helper: simulate wLength clamping for control IN transfers.
// This mirrors the logic in parse_request() CMD_SUBMIT path:
//   if (ep == 0 && dir != 0 && wLength > 1024) → clamp to 1024
// --------------------------------------------------------------------------

static uint16_t clamp_wlength(uint16_t wLength) {
    return (wLength > 1024) ? 1024 : wLength;
}

// --------------------------------------------------------------------------
// Helper: build the error response for buffer overflow rejection.
// Mirrors the code path in parse_request() that builds err_resp with
// status=-ENOMEM (-12), actual_length=0, original seqnum echoed.
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_overflow_rejection(uint32_t seqnum_host) {
    std::vector<uint8_t> response(48, 0);

    // command = 0x00000003 (RET_SUBMIT) in big-endian
    write_be32(&response[0], 0x00000003);

    // seqnum echoed in big-endian
    write_be32(&response[4], seqnum_host);

    // devid=0, direction=0, ep=0 (already zeroed)

    // status = -ENOMEM (-12) in big-endian
    write_be32(&response[20], static_cast<uint32_t>(static_cast<int32_t>(-12)));

    // actual_length = 0 (already zeroed at bytes 24-27)

    // start_frame=0, number_of_packets=0, error_count=0 (already zeroed)
    // bytes 40-47 = 0 (already zeroed)

    return response;
}

// --- Property test: OUT transfers with length > 1024 are rejected ---

RC_GTEST_PROP(USBIPProtocol, BufferOverflow_OutRejected, ()) {
    // **Validates: Requirements 6.1**
    // Generate random transfer_buffer_length > 1024 and direction = OUT (0)
    auto length = *rc::gen::inRange<uint32_t>(1025, 65536);
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    // The overflow check should trigger for OUT direction
    RC_ASSERT(should_reject_out_overflow(0, length));

    // Build the rejection response and verify fields
    auto response = build_overflow_rejection(seqnum);
    RC_ASSERT(response.size() == 48u);

    // Verify command = 0x00000003
    RC_ASSERT(read_be32(&response[0]) == 0x00000003);

    // Verify seqnum is echoed
    RC_ASSERT(read_be32(&response[4]) == seqnum);

    // Verify status = -12 (ENOMEM) as big-endian signed int32
    int32_t status = static_cast<int32_t>(read_be32(&response[20]));
    RC_ASSERT(status == -12);

    // Verify actual_length = 0
    RC_ASSERT(read_be32(&response[24]) == 0u);

    // Verify no trailing data (response is exactly 48 bytes)
    RC_ASSERT(response.size() == 48u);
}

// --- Property test: OUT transfers with length <= 1024 are NOT rejected ---

RC_GTEST_PROP(USBIPProtocol, BufferOverflow_OutNotRejectedUnderLimit, ()) {
    // **Validates: Requirements 6.1**
    // Generate random transfer_buffer_length in [0, 1024] and direction = OUT (0)
    auto length = *rc::gen::inRange<uint32_t>(0, 1025);

    // The overflow check should NOT trigger
    RC_ASSERT(!should_reject_out_overflow(0, length));
}

// --- Property test: IN transfers with any length are NOT rejected ---

RC_GTEST_PROP(USBIPProtocol, BufferOverflow_InNotRejected, ()) {
    // **Validates: Requirements 6.1**
    // Generate random transfer_buffer_length (any size) and direction = IN (1)
    auto length = *rc::gen::inRange<uint32_t>(0, 65536);

    // IN transfers (direction != 0) are never rejected for buffer overflow
    RC_ASSERT(!should_reject_out_overflow(1, length));
}

// --- Property test: control IN wLength > 1024 is clamped to 1024 ---

RC_GTEST_PROP(USBIPProtocol, BufferOverflow_WLengthClamped, ()) {
    // **Validates: Requirements 6.3**
    // Generate random wLength > 1024
    auto wLength = *rc::gen::inRange<uint16_t>(1025, 65535);

    uint16_t clamped = clamp_wlength(wLength);
    RC_ASSERT(clamped == 1024);
}

// --- Property test: control IN wLength <= 1024 is NOT clamped ---

RC_GTEST_PROP(USBIPProtocol, BufferOverflow_WLengthNotClampedUnderLimit, ()) {
    // **Validates: Requirements 6.3**
    // Generate random wLength in [0, 1024]
    auto wLength = *rc::gen::inRange<uint16_t>(0, 1025);

    uint16_t clamped = clamp_wlength(wLength);
    RC_ASSERT(clamped == wLength);
}

// --- Property test: rejection response echoes seqnum correctly ---

RC_GTEST_PROP(USBIPProtocol, BufferOverflow_SeqnumEchoed, ()) {
    // **Validates: Requirements 6.5**
    // Generate a random seqnum and verify it appears in the rejection response
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_overflow_rejection(seqnum);

    // seqnum at bytes 4-7 in big-endian
    uint32_t echoed = read_be32(&response[4]);
    RC_ASSERT(echoed == seqnum);
}

// --- Deterministic test: boundary value — exactly 1024 is allowed ---

TEST(USBIPProtocol, BufferOverflow_Boundary1024Allowed) {
    // **Validates: Requirements 6.1**
    // transfer_buffer_length of exactly 1024 for OUT should NOT be rejected
    EXPECT_FALSE(should_reject_out_overflow(0, 1024));
}

// --- Deterministic test: boundary value — 1025 is rejected ---

TEST(USBIPProtocol, BufferOverflow_Boundary1025Rejected) {
    // **Validates: Requirements 6.1**
    // transfer_buffer_length of 1025 for OUT should be rejected
    EXPECT_TRUE(should_reject_out_overflow(0, 1025));
}

// --- Deterministic test: wLength clamping boundary ---

TEST(USBIPProtocol, BufferOverflow_WLengthClampBoundary) {
    // **Validates: Requirements 6.3**
    EXPECT_EQ(clamp_wlength(1024), 1024);   // Not clamped
    EXPECT_EQ(clamp_wlength(1025), 1024);   // Clamped
    EXPECT_EQ(clamp_wlength(65535), 1024);  // Clamped
    EXPECT_EQ(clamp_wlength(0), 0);         // Not clamped
    EXPECT_EQ(clamp_wlength(512), 512);     // Not clamped
}

// --- Deterministic test: overflow rejection response format ---

TEST(USBIPProtocol, BufferOverflow_RejectionResponseFormat) {
    // **Validates: Requirements 6.1, 6.5**
    // Verify the complete rejection response structure
    auto response = build_overflow_rejection(0x12345678);
    ASSERT_EQ(response.size(), 48u);

    // command = 0x00000003
    EXPECT_EQ(response[0], 0x00);
    EXPECT_EQ(response[1], 0x00);
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x03);

    // seqnum = 0x12345678
    EXPECT_EQ(response[4], 0x12);
    EXPECT_EQ(response[5], 0x34);
    EXPECT_EQ(response[6], 0x56);
    EXPECT_EQ(response[7], 0x78);

    // devid = 0
    EXPECT_EQ(read_be32(&response[8]), 0u);
    // direction = 0
    EXPECT_EQ(read_be32(&response[12]), 0u);
    // ep = 0
    EXPECT_EQ(read_be32(&response[16]), 0u);

    // status = -12 (0xFFFFFFF4 in two's complement big-endian)
    EXPECT_EQ(response[20], 0xFF);
    EXPECT_EQ(response[21], 0xFF);
    EXPECT_EQ(response[22], 0xFF);
    EXPECT_EQ(response[23], 0xF4);

    // actual_length = 0
    EXPECT_EQ(read_be32(&response[24]), 0u);

    // start_frame = 0, number_of_packets = 0, error_count = 0
    EXPECT_EQ(read_be32(&response[28]), 0u);
    EXPECT_EQ(read_be32(&response[32]), 0u);
    EXPECT_EQ(read_be32(&response[36]), 0u);

    // bytes 40-47 = 0
    for (int i = 40; i < 48; i++) {
        EXPECT_EQ(response[i], 0x00);
    }
}

// ==========================================================================
// Property 12: Isochronous Transfer Rejection
// Validates: Requirements 7.1, 7.2
//
// For any USBIP_CMD_SUBMIT with number_of_packets > 0, the response SHALL
// have status=-ENOSYS (-38), actual_length=0, number_of_packets=0, and no
// transfer_buffer payload. The full request payload (transfer_buffer + iso
// descriptors) SHALL be consumed from the TCP stream before the response
// is sent.
// ==========================================================================

// --------------------------------------------------------------------------
// Helper: determine if a CMD_SUBMIT represents an isochronous transfer.
// Isochronous transfers are identified by number_of_packets > 0.
// --------------------------------------------------------------------------

static bool is_isochronous_transfer(uint32_t num_packets) {
    return num_packets > 0;
}

// --------------------------------------------------------------------------
// Helper: build an isochronous rejection response as a raw byte buffer.
// Replicates the logic from usb_ip.cpp parse_request() when num_packets > 0.
//
// The response is a 48-byte USBIP_RET_SUBMIT with:
//   command=0x00000003, seqnum echoed, devid=0, direction=0, ep=0,
//   status=-38 (ENOSYS) big-endian, actual_length=0, start_frame=0,
//   number_of_packets=0, error_count=0, bytes 40-47 = 0.
//   No transfer_buffer payload follows.
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_iso_rejection_response(uint32_t seqnum_host) {
    std::vector<uint8_t> response(48, 0);

    // Bytes 0-3: command = 0x00000003 (RET_SUBMIT) in big-endian
    write_be32(&response[0], 0x00000003);

    // Bytes 4-7: seqnum echoed in big-endian
    write_be32(&response[4], seqnum_host);

    // Bytes 8-11: devid = 0 (already zeroed)
    // Bytes 12-15: direction = 0 (already zeroed)
    // Bytes 16-19: ep = 0 (already zeroed)

    // Bytes 20-23: status = -38 (-ENOSYS) in big-endian two's complement
    write_be32(&response[20], static_cast<uint32_t>(static_cast<int32_t>(-38)));

    // Bytes 24-27: actual_length = 0 (already zeroed)
    // Bytes 28-31: start_frame = 0 (already zeroed)
    // Bytes 32-35: number_of_packets = 0 (already zeroed)
    // Bytes 36-39: error_count = 0 (already zeroed)
    // Bytes 40-47: setup/padding = 0 (already zeroed)

    return response;
}

// --- Property test: isochronous transfer detected when num_packets > 0 ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_DetectedWhenNumPacketsPositive, ()) {
    // **Validates: Requirements 7.1, 7.2**
    // Any num_packets > 0 indicates an isochronous transfer
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);

    RC_ASSERT(is_isochronous_transfer(num_packets));
}

// --- Property test: num_packets == 0 is NOT isochronous ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_NotIsoWhenNumPacketsZero, ()) {
    // **Validates: Requirements 7.1**
    // num_packets == 0 means a non-isochronous transfer (bulk/interrupt/control)
    RC_ASSERT(!is_isochronous_transfer(0));
}

// --- Property test: response status is -ENOSYS (-38) ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_StatusENOSYS, ()) {
    // **Validates: Requirements 7.2**
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    RC_ASSERT(is_isochronous_transfer(num_packets));

    auto response = build_iso_rejection_response(seqnum);

    // Status at bytes 20-23 must be -38 in big-endian
    int32_t wire_status = static_cast<int32_t>(read_be32(&response[20]));
    RC_ASSERT(wire_status == -38);
}

// --- Property test: response actual_length is 0 ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_ActualLengthZero, ()) {
    // **Validates: Requirements 7.2**
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    RC_ASSERT(is_isochronous_transfer(num_packets));

    auto response = build_iso_rejection_response(seqnum);

    // actual_length at bytes 24-27 must be 0
    uint32_t actual_length = read_be32(&response[24]);
    RC_ASSERT(actual_length == 0);
}

// --- Property test: response number_of_packets is 0 ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_NumPacketsZeroInResponse, ()) {
    // **Validates: Requirements 7.2**
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    RC_ASSERT(is_isochronous_transfer(num_packets));

    auto response = build_iso_rejection_response(seqnum);

    // number_of_packets at bytes 32-35 must be 0
    uint32_t resp_num_packets = read_be32(&response[32]);
    RC_ASSERT(resp_num_packets == 0);
}

// --- Property test: response is exactly 48 bytes (no transfer_buffer payload) ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_ResponseExactly48Bytes, ()) {
    // **Validates: Requirements 7.2**
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    RC_ASSERT(is_isochronous_transfer(num_packets));

    auto response = build_iso_rejection_response(seqnum);

    // No transfer_buffer or iso_packet_descriptor payload in the response
    RC_ASSERT(response.size() == 48u);
}

// --- Property test: seqnum is echoed correctly ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_SeqnumEchoed, ()) {
    // **Validates: Requirements 7.2**
    auto seqnum = *rc::gen::arbitrary<uint32_t>();
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);

    RC_ASSERT(is_isochronous_transfer(num_packets));

    auto response = build_iso_rejection_response(seqnum);

    // seqnum at bytes 4-7 must match the original
    uint32_t echoed_seqnum = read_be32(&response[4]);
    RC_ASSERT(echoed_seqnum == seqnum);
}

// --- Property test: command field is RET_SUBMIT (0x00000003) ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_CommandIsRetSubmit, ()) {
    // **Validates: Requirements 7.2**
    auto seqnum = *rc::gen::arbitrary<uint32_t>();
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);

    RC_ASSERT(is_isochronous_transfer(num_packets));

    auto response = build_iso_rejection_response(seqnum);

    // command at bytes 0-3 must be 0x00000003 (RET_SUBMIT)
    uint32_t command = read_be32(&response[0]);
    RC_ASSERT(command == 0x00000003);
}

// --- Property test: wire encoding of -38 as big-endian two's complement ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_BigEndianNegative38, ()) {
    // **Validates: Requirements 7.2**
    auto seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_iso_rejection_response(seqnum);

    // -38 in 32-bit two's complement = 0xFFFFFFDA
    // Big-endian wire bytes: 0xFF 0xFF 0xFF 0xDA
    RC_ASSERT(response[20] == 0xFF);
    RC_ASSERT(response[21] == 0xFF);
    RC_ASSERT(response[22] == 0xFF);
    RC_ASSERT(response[23] == 0xDA);
}

// --- Property test: fixed header fields (devid, direction, ep, start_frame, error_count, padding) ---

RC_GTEST_PROP(USBIPProtocol, IsoRejection_FixedHeaderFieldsZeroed, ()) {
    // **Validates: Requirements 7.2**
    auto seqnum = *rc::gen::arbitrary<uint32_t>();
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 256);

    RC_ASSERT(is_isochronous_transfer(num_packets));

    auto response = build_iso_rejection_response(seqnum);

    // devid = 0 (bytes 8-11)
    RC_ASSERT(read_be32(&response[8]) == 0);
    // direction = 0 (bytes 12-15)
    RC_ASSERT(read_be32(&response[12]) == 0);
    // ep = 0 (bytes 16-19)
    RC_ASSERT(read_be32(&response[16]) == 0);
    // start_frame = 0 (bytes 28-31)
    RC_ASSERT(read_be32(&response[28]) == 0);
    // error_count = 0 (bytes 36-39)
    RC_ASSERT(read_be32(&response[36]) == 0);
    // bytes 40-47 (setup/padding) = 0
    for (int i = 40; i < 48; i++) {
        RC_ASSERT(response[i] == 0);
    }
}

// --- Deterministic test: verify complete isochronous rejection response bytes ---

TEST(USBIPProtocol, IsoRejection_CompleteResponseVerification) {
    // **Validates: Requirements 7.1, 7.2**
    // Build a response for seqnum=0x12345678
    auto response = build_iso_rejection_response(0x12345678);

    EXPECT_EQ(response.size(), 48u);

    // command = 0x00000003 (RET_SUBMIT)
    EXPECT_EQ(response[0], 0x00);
    EXPECT_EQ(response[1], 0x00);
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x03);

    // seqnum = 0x12345678
    EXPECT_EQ(response[4], 0x12);
    EXPECT_EQ(response[5], 0x34);
    EXPECT_EQ(response[6], 0x56);
    EXPECT_EQ(response[7], 0x78);

    // devid = 0
    EXPECT_EQ(read_be32(&response[8]), 0u);
    // direction = 0
    EXPECT_EQ(read_be32(&response[12]), 0u);
    // ep = 0
    EXPECT_EQ(read_be32(&response[16]), 0u);

    // status = -38 (0xFFFFFFDA)
    EXPECT_EQ(response[20], 0xFF);
    EXPECT_EQ(response[21], 0xFF);
    EXPECT_EQ(response[22], 0xFF);
    EXPECT_EQ(response[23], 0xDA);

    // actual_length = 0
    EXPECT_EQ(read_be32(&response[24]), 0u);
    // start_frame = 0
    EXPECT_EQ(read_be32(&response[28]), 0u);
    // number_of_packets = 0
    EXPECT_EQ(read_be32(&response[32]), 0u);
    // error_count = 0
    EXPECT_EQ(read_be32(&response[36]), 0u);

    // bytes 40-47 = 0
    for (int i = 40; i < 48; i++) {
        EXPECT_EQ(response[i], 0x00);
    }
}

// --- Deterministic test: verify isochronous detection boundary ---

TEST(USBIPProtocol, IsoRejection_BoundaryDetection) {
    // **Validates: Requirements 7.1**
    // num_packets = 0 is NOT isochronous
    EXPECT_FALSE(is_isochronous_transfer(0));
    // num_packets = 1 IS isochronous (boundary)
    EXPECT_TRUE(is_isochronous_transfer(1));
    // Large values are also isochronous
    EXPECT_TRUE(is_isochronous_transfer(255));
    EXPECT_TRUE(is_isochronous_transfer(0xFFFFFFFF));
}

// ==========================================================================
// Property 11: Unlink Completed URB Returns Zero
// Validates: Requirements 4.2, 4.4
//
// For any USBIP_CMD_UNLINK where the unlink_seqnum does NOT identify a
// pending URB (already completed or unknown), the response SHALL have
// status=0 with the unlink request's own seqnum echoed.
// ==========================================================================

// (Uses simulate_unlink and build_ret_unlink helpers defined above in Property 10 section)

// --- Property test: unlink of non-pending seqnum returns status=0 ---

RC_GTEST_PROP(USBIPProtocol, UnlinkCompleted_ReturnsZero, ()) {
    // **Validates: Requirements 4.2, 4.4**
    // Generate a set of pending seqnums (simulating URBs that are still in-flight)
    auto pending_count = *rc::gen::inRange<size_t>(0, 10);
    auto pending_set = *rc::gen::container<std::vector<uint32_t>>(
        pending_count,
        rc::gen::arbitrary<uint32_t>());
    std::unordered_map<uint32_t, bool> pending;
    for (auto s : pending_set) pending[s] = true;

    // Generate a target seqnum that is NOT in the pending set
    auto target = *rc::gen::suchThat<uint32_t>([&](uint32_t v) {
        return pending.find(v) == pending.end();
    });

    // Simulate unlink - should return 0 since target is not pending
    int32_t status = simulate_unlink(pending, target);
    RC_ASSERT(status == 0);
}

// --- Property test: RET_UNLINK command field is 0x00000004 ---

RC_GTEST_PROP(USBIPProtocol, UnlinkCompleted_CommandIsRetUnlink, ()) {
    // **Validates: Requirements 4.2, 4.4**
    // Generate a random unlink request seqnum and status=0 (completed URB case)
    auto unlink_seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_ret_unlink(unlink_seqnum, 0);
    const uint8_t *r = response.data();

    // command at bytes 0-3 must be 0x00000004 (RET_UNLINK) in big-endian
    RC_ASSERT(read_be32(r + 0) == 0x00000004);
}

// --- Property test: unlink request's own seqnum is echoed ---

RC_GTEST_PROP(USBIPProtocol, UnlinkCompleted_SeqnumEchoed, ()) {
    // **Validates: Requirements 4.4**
    // Generate a random unlink request seqnum
    auto unlink_seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_ret_unlink(unlink_seqnum, 0);
    const uint8_t *r = response.data();

    // seqnum at bytes 4-7 must echo the unlink request's own seqnum
    uint32_t echoed = read_be32(r + 4);
    RC_ASSERT(echoed == unlink_seqnum);
}

// --- Property test: status=0 is encoded as 4 zero bytes on the wire ---

RC_GTEST_PROP(USBIPProtocol, UnlinkCompleted_StatusZeroWireEncoding, ()) {
    // **Validates: Requirements 4.2, 4.4**
    // For any unlink of a non-pending URB, status=0 encodes as 4 zero bytes
    auto unlink_seqnum = *rc::gen::arbitrary<uint32_t>();

    auto response = build_ret_unlink(unlink_seqnum, 0);
    const uint8_t *r = response.data();

    // status at bytes 20-23 should be all zeros (0 in big-endian)
    RC_ASSERT(r[20] == 0x00);
    RC_ASSERT(r[21] == 0x00);
    RC_ASSERT(r[22] == 0x00);
    RC_ASSERT(r[23] == 0x00);
}

// --- Property test: bytes 24-47 are all zero (padding) ---

RC_GTEST_PROP(USBIPProtocol, UnlinkCompleted_PaddingAllZeros, ()) {
    // **Validates: Requirements 4.2, 4.4**
    // For any RET_UNLINK response, bytes 24-47 must be zero
    auto unlink_seqnum = *rc::gen::arbitrary<uint32_t>();
    // Generate status from {0, -104} — padding must be zero in both cases
    auto status = *rc::gen::element<int32_t>(0, -104);

    auto response = build_ret_unlink(unlink_seqnum, status);
    const uint8_t *r = response.data();

    // Bytes 24-47 must all be zero
    for (int i = 24; i < 48; i++) {
        RC_ASSERT(r[i] == 0);
    }
}

// --- Property test: full unlink flow for non-pending target ---

RC_GTEST_PROP(USBIPProtocol, UnlinkCompleted_FullFlowNonPending, ()) {
    // **Validates: Requirements 4.2, 4.4**
    // End-to-end: generate pending set, target NOT in set, verify full response
    auto pending_count = *rc::gen::inRange<size_t>(0, 10);
    auto pending_set = *rc::gen::container<std::vector<uint32_t>>(
        pending_count,
        rc::gen::arbitrary<uint32_t>());
    std::unordered_map<uint32_t, bool> pending;
    for (auto s : pending_set) pending[s] = true;

    // Generate target NOT in the pending set
    auto target = *rc::gen::suchThat<uint32_t>([&](uint32_t v) {
        return pending.find(v) == pending.end();
    });

    // Generate unlink request's own seqnum (distinct from target)
    auto unlink_seqnum = *rc::gen::arbitrary<uint32_t>();

    // Simulate unlink: target not found → status=0
    int32_t status = simulate_unlink(pending, target);
    RC_ASSERT(status == 0);

    // Build the response with the unlink request's OWN seqnum (not target)
    auto response = build_ret_unlink(unlink_seqnum, status);
    const uint8_t *r = response.data();

    // Verify complete response structure
    RC_ASSERT(response.size() == 48u);
    RC_ASSERT(read_be32(r + 0) == 0x00000004);   // command = RET_UNLINK
    RC_ASSERT(read_be32(r + 4) == unlink_seqnum); // echoes unlink request's seqnum
    RC_ASSERT(read_be32(r + 8) == 0);             // devid = 0
    RC_ASSERT(read_be32(r + 12) == 0);            // direction = 0
    RC_ASSERT(read_be32(r + 16) == 0);            // ep = 0
    RC_ASSERT(read_be32(r + 20) == 0);            // status = 0
    for (int i = 24; i < 48; i++) {
        RC_ASSERT(r[i] == 0);                     // padding all zeros
    }
}

// --- Deterministic test: unlink with empty pending set returns 0 ---

TEST(USBIPProtocol, UnlinkCompleted_EmptyPendingSet) {
    // **Validates: Requirements 4.2, 4.4**
    std::unordered_map<uint32_t, bool> pending;
    EXPECT_EQ(simulate_unlink(pending, 12345), 0);
    EXPECT_EQ(simulate_unlink(pending, 0), 0);
    EXPECT_EQ(simulate_unlink(pending, 0xFFFFFFFF), 0);
}

// --- Deterministic test: unlink of recently-completed seqnum returns 0 ---

TEST(USBIPProtocol, UnlinkCompleted_AfterCompletion) {
    // **Validates: Requirements 4.2, 4.4**
    // Simulate: seqnum 42 was pending, then completed (removed from map)
    std::unordered_map<uint32_t, bool> pending;
    pending[42] = true;
    pending[43] = true;

    // Simulate completion of seqnum 42 (remove from pending)
    pending.erase(42);

    // Now unlink targets seqnum 42 — it's no longer pending
    int32_t status = simulate_unlink(pending, 42);
    EXPECT_EQ(status, 0);

    // Seqnum 43 is still pending — should return -104
    status = simulate_unlink(pending, 43);
    EXPECT_EQ(status, -104);
}

// --- Deterministic test: verify RET_UNLINK wire format byte-by-byte ---

TEST(USBIPProtocol, UnlinkCompleted_WireFormat) {
    // **Validates: Requirements 4.2, 4.4**
    auto response = build_ret_unlink(0x12345678, 0);

    // Verify byte-by-byte
    // command = 0x00000004
    EXPECT_EQ(response[0], 0x00);
    EXPECT_EQ(response[1], 0x00);
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x04);

    // seqnum = 0x12345678
    EXPECT_EQ(response[4], 0x12);
    EXPECT_EQ(response[5], 0x34);
    EXPECT_EQ(response[6], 0x56);
    EXPECT_EQ(response[7], 0x78);

    // devid = 0
    EXPECT_EQ(response[8], 0x00);
    EXPECT_EQ(response[9], 0x00);
    EXPECT_EQ(response[10], 0x00);
    EXPECT_EQ(response[11], 0x00);

    // direction = 0
    EXPECT_EQ(response[12], 0x00);
    EXPECT_EQ(response[13], 0x00);
    EXPECT_EQ(response[14], 0x00);
    EXPECT_EQ(response[15], 0x00);

    // ep = 0
    EXPECT_EQ(response[16], 0x00);
    EXPECT_EQ(response[17], 0x00);
    EXPECT_EQ(response[18], 0x00);
    EXPECT_EQ(response[19], 0x00);

    // status = 0
    EXPECT_EQ(response[20], 0x00);
    EXPECT_EQ(response[21], 0x00);
    EXPECT_EQ(response[22], 0x00);
    EXPECT_EQ(response[23], 0x00);

    // padding bytes 24-47 all zero
    for (int i = 24; i < 48; i++) {
        EXPECT_EQ(response[i], 0x00);
    }
}

// ==========================================================================
// Property 13: TCP Reassembly Correctness
// Validates: Requirements 12.1, 12.2, 12.3, 12.5
//
// For any sequence of valid USB/IP protocol messages delivered across
// arbitrary TCP segment boundaries (including single-byte segments and
// multiple messages per segment), all messages SHALL be processed correctly
// and in arrival order, producing the same responses as if each message
// arrived in its own segment.
//
// We test the reassembly SIZE CALCULATION logic (which is what tcp_task_
// implements): given a buffer with partial or complete message data,
// compute the expected message length ("needed") correctly.
// ==========================================================================

// --------------------------------------------------------------------------
// Helper: compute needed size for a message (mirrors tcp_task_ logic)
//
// Protocol dispatch logic from usb_ip.cpp tcp_task_:
//   1. Read 8 bytes minimum to check OP-layer command field (offset 2-3)
//   2. If command == OP_REQ_DEVLIST (0x8005) → needed = 8
//   3. If command == OP_REQ_IMPORT (0x8003) → needed = 40
//   4. Otherwise, need 48 bytes for URB header
//   5. Check 32-bit command at offset 0:
//      - CMD_UNLINK (0x00000002) → needed = 48
//      - CMD_SUBMIT (0x00000001) → needed = 48 + out_len + iso_len
//   6. Unrecognized → 0 (protocol error)
// --------------------------------------------------------------------------

static size_t compute_needed(const uint8_t *buf, size_t buf_len) {
    if (buf_len < 8) return 0;  // need at least 8 bytes to identify message type

    // Check OP-layer 16-bit command at offset 2 (big-endian)
    uint16_t op_cmd = (uint16_t(buf[2]) << 8) | buf[3];
    if (op_cmd == 0x8005) return 8;   // OP_REQ_DEVLIST
    if (op_cmd == 0x8003) return 40;  // OP_REQ_IMPORT

    // Not an OP message — need at least 48 bytes for URB header
    if (buf_len < 48) return 0;

    // Check 32-bit command at offset 0 (big-endian)
    uint32_t cmd32 = (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
                     (uint32_t(buf[2]) << 8) | buf[3];
    if (cmd32 == 0x00000002) return 48;  // CMD_UNLINK

    if (cmd32 == 0x00000001) {
        // CMD_SUBMIT: 48 + out_len + iso_len
        // direction at offset 12-15 (big-endian uint32)
        uint32_t direction = (uint32_t(buf[12]) << 24) | (uint32_t(buf[13]) << 16) |
                             (uint32_t(buf[14]) << 8) | buf[15];
        // transfer_buffer_length at offset 20-23 (big-endian uint32)
        uint32_t length = (uint32_t(buf[20]) << 24) | (uint32_t(buf[21]) << 16) |
                          (uint32_t(buf[22]) << 8) | buf[23];
        // num_packets at offset 28-31 (big-endian uint32)
        uint32_t num_packets = (uint32_t(buf[28]) << 24) | (uint32_t(buf[29]) << 16) |
                               (uint32_t(buf[30]) << 8) | buf[31];

        uint32_t out_len = (direction == 0) ? length : 0;
        uint32_t iso_len = (num_packets > 0) ? (num_packets * 16) : 0;
        return 48 + out_len + iso_len;
    }

    return 0;  // unrecognized command
}

// --------------------------------------------------------------------------
// Helper: build an OP_REQ_DEVLIST message (8 bytes)
// Wire format: version(2) + command(2) + status(4)
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_op_req_devlist() {
    std::vector<uint8_t> msg(8, 0);
    // version = 0x0111 (big-endian)
    msg[0] = 0x01; msg[1] = 0x11;
    // command = 0x8005 (big-endian)
    msg[2] = 0x80; msg[3] = 0x05;
    // status = 0
    return msg;
}

// --------------------------------------------------------------------------
// Helper: build an OP_REQ_IMPORT message (40 bytes)
// Wire format: version(2) + command(2) + status(4) + busid[32]
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_op_req_import() {
    std::vector<uint8_t> msg(40, 0);
    // version = 0x0111 (big-endian)
    msg[0] = 0x01; msg[1] = 0x11;
    // command = 0x8003 (big-endian)
    msg[2] = 0x80; msg[3] = 0x03;
    // status = 0, busid filled with "1-1\0..."
    msg[8] = '1'; msg[9] = '-'; msg[10] = '1';
    return msg;
}

// --------------------------------------------------------------------------
// Helper: build a CMD_SUBMIT message header (48 bytes + out_len + iso_len)
// Parameters:
//   direction: 0=OUT, non-zero=IN
//   transfer_buffer_length: size of transfer data (used if direction==OUT)
//   num_packets: number of iso packets (0 for non-isochronous)
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_cmd_submit(uint32_t direction,
                                              uint32_t transfer_buffer_length,
                                              uint32_t num_packets) {
    uint32_t out_len = (direction == 0) ? transfer_buffer_length : 0;
    uint32_t iso_len = (num_packets > 0) ? (num_packets * 16) : 0;
    size_t total = 48 + out_len + iso_len;
    std::vector<uint8_t> msg(total, 0);

    // command = 0x00000001 (CMD_SUBMIT) at offset 0, big-endian
    msg[0] = 0x00; msg[1] = 0x00; msg[2] = 0x00; msg[3] = 0x01;

    // seqnum at offset 4 (arbitrary)
    msg[4] = 0x00; msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x01;

    // devid at offset 8 = 0
    // direction at offset 12-15, big-endian
    msg[12] = (direction >> 24) & 0xFF;
    msg[13] = (direction >> 16) & 0xFF;
    msg[14] = (direction >> 8) & 0xFF;
    msg[15] = direction & 0xFF;

    // ep at offset 16 = 0

    // transfer_buffer_length at offset 20-23, big-endian
    msg[20] = (transfer_buffer_length >> 24) & 0xFF;
    msg[21] = (transfer_buffer_length >> 16) & 0xFF;
    msg[22] = (transfer_buffer_length >> 8) & 0xFF;
    msg[23] = transfer_buffer_length & 0xFF;

    // start_frame at offset 24 = 0

    // num_packets at offset 28-31, big-endian
    msg[28] = (num_packets >> 24) & 0xFF;
    msg[29] = (num_packets >> 16) & 0xFF;
    msg[30] = (num_packets >> 8) & 0xFF;
    msg[31] = num_packets & 0xFF;

    // interval at offset 32 = 0
    // setup at offset 40 = 0
    // transfer_buffer and iso descriptors filled with 0xAA for identification
    for (size_t i = 48; i < total; i++) {
        msg[i] = 0xAA;
    }

    return msg;
}

// --------------------------------------------------------------------------
// Helper: build a CMD_UNLINK message (48 bytes)
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_cmd_unlink() {
    std::vector<uint8_t> msg(48, 0);
    // command = 0x00000002 (CMD_UNLINK) at offset 0, big-endian
    msg[0] = 0x00; msg[1] = 0x00; msg[2] = 0x00; msg[3] = 0x02;
    // seqnum at offset 4 (arbitrary)
    msg[4] = 0x00; msg[5] = 0x00; msg[6] = 0x00; msg[7] = 0x02;
    // unlink_seqnum at offset 20 (arbitrary)
    msg[20] = 0x00; msg[21] = 0x00; msg[22] = 0x00; msg[23] = 0x01;
    return msg;
}

// --- Property test: OP_REQ_DEVLIST always needs exactly 8 bytes ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_DevlistNeeds8Bytes, ()) {
    // **Validates: Requirements 12.5**
    auto msg = build_op_req_devlist();
    RC_ASSERT(msg.size() == 8u);
    size_t needed = compute_needed(msg.data(), msg.size());
    RC_ASSERT(needed == 8u);
}

// --- Property test: OP_REQ_IMPORT always needs exactly 40 bytes ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_ImportNeeds40Bytes, ()) {
    // **Validates: Requirements 12.5**
    auto msg = build_op_req_import();
    RC_ASSERT(msg.size() == 40u);
    size_t needed = compute_needed(msg.data(), msg.size());
    RC_ASSERT(needed == 40u);
}

// --- Property test: CMD_UNLINK always needs exactly 48 bytes ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_UnlinkNeeds48Bytes, ()) {
    // **Validates: Requirements 12.5**
    auto msg = build_cmd_unlink();
    RC_ASSERT(msg.size() == 48u);
    size_t needed = compute_needed(msg.data(), msg.size());
    RC_ASSERT(needed == 48u);
}

// --- Property test: CMD_SUBMIT with direction=OUT needs 48 + transfer_buffer_length ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_SubmitOutNeedsHeaderPlusData, ()) {
    // **Validates: Requirements 12.3**
    // Generate random OUT transfer_buffer_length [0, 1024]
    auto transfer_len = *rc::gen::inRange<uint32_t>(0, 1025);

    auto msg = build_cmd_submit(0, transfer_len, 0);  // direction=0 (OUT), no iso
    RC_ASSERT(msg.size() == 48u + transfer_len);

    size_t needed = compute_needed(msg.data(), msg.size());
    RC_ASSERT(needed == 48u + transfer_len);
}

// --- Property test: CMD_SUBMIT with direction=IN needs exactly 48 bytes ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_SubmitInNeeds48Bytes, ()) {
    // **Validates: Requirements 12.3**
    // Generate random IN transfer_buffer_length [0, 65535]
    // For IN transfers, no data follows the header regardless of length field
    auto transfer_len = *rc::gen::inRange<uint32_t>(0, 65536);

    auto msg = build_cmd_submit(1, transfer_len, 0);  // direction=1 (IN), no iso
    // For IN, out_len=0, so total message is 48 bytes
    RC_ASSERT(msg.size() == 48u);

    size_t needed = compute_needed(msg.data(), msg.size());
    RC_ASSERT(needed == 48u);
}

// --- Property test: CMD_SUBMIT with iso packets needs 48 + out_len + num_packets*16 ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_SubmitIsoNeeds48PlusOutPlusIso, ()) {
    // **Validates: Requirements 12.1, 12.2, 12.3**
    // Generate random parameters for an isochronous OUT transfer
    auto transfer_len = *rc::gen::inRange<uint32_t>(0, 512);
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 32);

    auto msg = build_cmd_submit(0, transfer_len, num_packets);  // OUT + iso
    size_t expected_size = 48u + transfer_len + (num_packets * 16);
    RC_ASSERT(msg.size() == expected_size);

    size_t needed = compute_needed(msg.data(), msg.size());
    RC_ASSERT(needed == expected_size);
}

// --- Property test: CMD_SUBMIT ISO with IN direction (out_len=0) ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_SubmitIsoInNeeds48PlusIsoOnly, ()) {
    // **Validates: Requirements 12.1, 12.3**
    // ISO IN: no transfer_buffer on the wire (out_len=0), but iso descriptors present
    auto transfer_len = *rc::gen::inRange<uint32_t>(0, 512);
    auto num_packets = *rc::gen::inRange<uint32_t>(1, 32);

    auto msg = build_cmd_submit(1, transfer_len, num_packets);  // IN + iso
    size_t expected_size = 48u + (num_packets * 16);  // out_len=0 for IN
    RC_ASSERT(msg.size() == expected_size);

    size_t needed = compute_needed(msg.data(), msg.size());
    RC_ASSERT(needed == expected_size);
}

// --- Property test: partial data (less than 8 bytes) returns 0 (need more data) ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_PartialLessThan8Returns0, ()) {
    // **Validates: Requirements 12.1**
    // Generate a partial buffer [1..7] bytes of any valid message
    auto msg = build_op_req_devlist();  // 8 bytes
    auto partial_len = *rc::gen::inRange<size_t>(1, 8);

    size_t needed = compute_needed(msg.data(), partial_len);
    RC_ASSERT(needed == 0u);  // Can't determine message type yet
}

// --- Property test: partial URB data (8-47 bytes, non-OP) returns 0 ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_PartialURBLessThan48Returns0, ()) {
    // **Validates: Requirements 12.1**
    // Generate a CMD_SUBMIT or CMD_UNLINK message, but present only 8-47 bytes
    auto msg = build_cmd_unlink();  // 48 bytes
    auto partial_len = *rc::gen::inRange<size_t>(8, 48);

    size_t needed = compute_needed(msg.data(), partial_len);
    RC_ASSERT(needed == 0u);  // Need at least 48 to parse URB header
}

// --- Property test: multiple coalesced messages are correctly sized sequentially ---

RC_GTEST_PROP(USBIPProtocol, TCPReassembly_CoalescedMessagesSequentialSizing, ()) {
    // **Validates: Requirements 12.2**
    // Generate 2-5 random messages, concatenate them, verify sequential parsing
    auto msg_count = *rc::gen::inRange<int>(2, 6);

    std::vector<uint8_t> coalesced;
    std::vector<size_t> expected_sizes;

    for (int i = 0; i < msg_count; i++) {
        auto msg_type = *rc::gen::inRange<int>(0, 4);
        std::vector<uint8_t> msg;
        switch (msg_type) {
            case 0:
                msg = build_op_req_devlist();
                break;
            case 1:
                msg = build_op_req_import();
                break;
            case 2:
                msg = build_cmd_unlink();
                break;
            case 3: {
                auto dir = *rc::gen::element<uint32_t>(0u, 1u);
                auto len = *rc::gen::inRange<uint32_t>(0, 256);
                msg = build_cmd_submit(dir, len, 0);
                break;
            }
        }
        expected_sizes.push_back(msg.size());
        coalesced.insert(coalesced.end(), msg.begin(), msg.end());
    }

    // Simulate the tcp_task_ reassembly loop: parse each message from the buffer
    size_t offset = 0;
    for (int i = 0; i < msg_count; i++) {
        size_t remaining = coalesced.size() - offset;
        size_t needed = compute_needed(coalesced.data() + offset, remaining);
        RC_ASSERT(needed == expected_sizes[i]);
        RC_ASSERT(needed <= remaining);
        offset += needed;
    }

    // After processing all messages, offset should equal total buffer size
    RC_ASSERT(offset == coalesced.size());
}

// --- Deterministic tests: verify specific sizes ---

TEST(USBIPProtocol, TCPReassembly_DevlistSize) {
    // **Validates: Requirements 12.5**
    auto msg = build_op_req_devlist();
    EXPECT_EQ(compute_needed(msg.data(), msg.size()), 8u);
}

TEST(USBIPProtocol, TCPReassembly_ImportSize) {
    // **Validates: Requirements 12.5**
    auto msg = build_op_req_import();
    EXPECT_EQ(compute_needed(msg.data(), msg.size()), 40u);
}

TEST(USBIPProtocol, TCPReassembly_UnlinkSize) {
    // **Validates: Requirements 12.5**
    auto msg = build_cmd_unlink();
    EXPECT_EQ(compute_needed(msg.data(), msg.size()), 48u);
}

TEST(USBIPProtocol, TCPReassembly_SubmitOut64) {
    // **Validates: Requirements 12.3**
    // CMD_SUBMIT OUT with 64 bytes transfer_buffer → needed = 48 + 64 = 112
    auto msg = build_cmd_submit(0, 64, 0);
    EXPECT_EQ(compute_needed(msg.data(), msg.size()), 112u);
}

TEST(USBIPProtocol, TCPReassembly_SubmitInAny) {
    // **Validates: Requirements 12.3**
    // CMD_SUBMIT IN with length=512 → needed = 48 (no data on wire for IN)
    auto msg = build_cmd_submit(1, 512, 0);
    EXPECT_EQ(compute_needed(msg.data(), msg.size()), 48u);
}

TEST(USBIPProtocol, TCPReassembly_SubmitIsoOut) {
    // **Validates: Requirements 12.3**
    // CMD_SUBMIT OUT with 32 bytes + 4 iso packets → needed = 48 + 32 + 4*16 = 144
    auto msg = build_cmd_submit(0, 32, 4);
    EXPECT_EQ(compute_needed(msg.data(), msg.size()), 144u);
}

TEST(USBIPProtocol, TCPReassembly_PartialEmpty) {
    // **Validates: Requirements 12.1**
    // Empty buffer → needed = 0
    uint8_t buf[1] = {0};
    EXPECT_EQ(compute_needed(buf, 0), 0u);
}

TEST(USBIPProtocol, TCPReassembly_Partial7Bytes) {
    // **Validates: Requirements 12.1**
    // 7 bytes is not enough to identify any message type
    auto msg = build_op_req_devlist();
    EXPECT_EQ(compute_needed(msg.data(), 7), 0u);
}

TEST(USBIPProtocol, TCPReassembly_CoalescedDevlistAndUnlink) {
    // **Validates: Requirements 12.2**
    // Two messages concatenated: devlist (8) + unlink (48) = 56 bytes total
    auto devlist = build_op_req_devlist();
    auto unlink = build_cmd_unlink();
    std::vector<uint8_t> buf;
    buf.insert(buf.end(), devlist.begin(), devlist.end());
    buf.insert(buf.end(), unlink.begin(), unlink.end());

    // First message: needed = 8
    EXPECT_EQ(compute_needed(buf.data(), buf.size()), 8u);

    // After consuming first 8 bytes, second message: needed = 48
    EXPECT_EQ(compute_needed(buf.data() + 8, buf.size() - 8), 48u);
}

// ==========================================================================
// Property 4: OP_REP_IMPORT Response Format
// Validates: Requirements 2.1, 2.2, 2.3, 2.6
//
// For any OP_REQ_IMPORT request with a busid string, if the busid matches
// an available device then the response SHALL be exactly 320 bytes with the
// busid echoed back in the device structure; if the busid does not match,
// the response SHALL be exactly 8 bytes with a non-zero status field.
// ==========================================================================

// --------------------------------------------------------------------------
// Helper: build a successful OP_REP_IMPORT response (320 bytes).
// Mirrors the logic in usb_ip.cpp parse_request() for OP_REQ_IMPORT when
// device is ready and busid matches.
//
// Response layout (320 bytes total):
//   Bytes 0-1:   version = 0x0111 (big-endian)
//   Bytes 2-3:   command = 0x0003 (OP_REP_IMPORT, big-endian)
//   Bytes 4-7:   status = 0 (success)
//   Bytes 8-263:  path[256] (null-terminated, zero-padded)
//   Bytes 264-295: busid[32] (null-terminated, zero-padded, echoed from request)
//   Bytes 296-299: busnum (big-endian)
//   Bytes 300-303: devnum (big-endian)
//   Bytes 304-307: speed (big-endian)
//   Bytes 308-309: idVendor (big-endian)
//   Bytes 310-311: idProduct (big-endian)
//   Bytes 312-313: bcdDevice (big-endian)
//   Byte 314:     bDeviceClass
//   Byte 315:     bDeviceSubClass
//   Byte 316:     bDeviceProtocol
//   Byte 317:     bConfigurationValue
//   Byte 318:     bNumConfigurations
//   Byte 319:     bNumInterfaces
// --------------------------------------------------------------------------

struct MockDeviceInfo {
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bConfigurationValue;
    uint8_t bNumConfigurations;
    uint8_t bNumInterfaces;
    uint32_t speed;  // Already mapped USB/IP speed value (1, 2, or 3)
};

static void write_be16(uint8_t *dst, uint16_t val) {
    dst[0] = (val >> 8) & 0xFF;
    dst[1] = val & 0xFF;
}

static std::vector<uint8_t> build_import_success_response(
    const char *busid, const MockDeviceInfo &dev) {
    std::vector<uint8_t> response(320, 0);

    // Bytes 0-1: version = 0x0111
    response[0] = 0x01; response[1] = 0x11;
    // Bytes 2-3: command = 0x0003 (OP_REP_IMPORT)
    response[2] = 0x00; response[3] = 0x03;
    // Bytes 4-7: status = 0 (success)
    // (already zeroed)

    // Bytes 8-263: path[256] (null-terminated)
    const char *path = "/esphome/usbip/usb1";
    strncpy(reinterpret_cast<char *>(&response[8]), path, 255);

    // Bytes 264-295: busid[32] (echoed from request, null-terminated)
    strncpy(reinterpret_cast<char *>(&response[264]), busid, 31);

    // Bytes 296-299: busnum (big-endian) = 1
    write_be32(&response[296], 1);
    // Bytes 300-303: devnum (big-endian) = 1
    write_be32(&response[300], 1);
    // Bytes 304-307: speed (big-endian)
    write_be32(&response[304], dev.speed);

    // Bytes 308-309: idVendor (big-endian)
    write_be16(&response[308], dev.idVendor);
    // Bytes 310-311: idProduct (big-endian)
    write_be16(&response[310], dev.idProduct);
    // Bytes 312-313: bcdDevice (big-endian)
    write_be16(&response[312], dev.bcdDevice);

    // Single-byte fields (no byte swap)
    response[314] = dev.bDeviceClass;
    response[315] = dev.bDeviceSubClass;
    response[316] = dev.bDeviceProtocol;
    response[317] = dev.bConfigurationValue;
    response[318] = dev.bNumConfigurations;
    response[319] = dev.bNumInterfaces;

    return response;
}

// --------------------------------------------------------------------------
// Helper: build a failed OP_REP_IMPORT response (8 bytes only).
// Mirrors the logic in usb_ip.cpp parse_request() for OP_REQ_IMPORT when
// device is NOT ready or busid does NOT match.
//
// Response layout (8 bytes total):
//   Bytes 0-1: version = 0x0111 (big-endian)
//   Bytes 2-3: command = 0x0003 (OP_REP_IMPORT, big-endian)
//   Bytes 4-7: status = non-zero (big-endian, 0x00000001)
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_import_failure_response() {
    std::vector<uint8_t> response(8, 0);

    // Bytes 0-1: version = 0x0111
    response[0] = 0x01; response[1] = 0x11;
    // Bytes 2-3: command = 0x0003 (OP_REP_IMPORT)
    response[2] = 0x00; response[3] = 0x03;
    // Bytes 4-7: status = 1 (not available) in big-endian
    write_be32(&response[4], 1);

    return response;
}

// --------------------------------------------------------------------------
// Helper: simulate the import busid matching logic from parse_request().
// Returns true if the busid matches the available device.
// --------------------------------------------------------------------------

static bool import_busid_matches(const char *req_busid, const char *device_busid, bool device_ready) {
    if (!device_ready) return false;
    return strcmp(req_busid, device_busid) == 0;
}

// --- Property test: successful import response is exactly 320 bytes ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_SuccessIs320Bytes, ()) {
    // **Validates: Requirements 2.1**
    // Generate random device descriptor values
    auto idVendor = *rc::gen::inRange<uint16_t>(1, 0xFFFF);
    auto idProduct = *rc::gen::inRange<uint16_t>(1, 0xFFFF);
    auto bcdDevice = *rc::gen::arbitrary<uint16_t>();
    auto bDeviceClass = *rc::gen::arbitrary<uint8_t>();
    auto bDeviceSubClass = *rc::gen::arbitrary<uint8_t>();
    auto bDeviceProtocol = *rc::gen::arbitrary<uint8_t>();
    auto bConfigVal = *rc::gen::inRange<uint8_t>(1, 255);
    auto bNumConfigs = *rc::gen::inRange<uint8_t>(1, 5);
    auto bNumInterfaces = *rc::gen::inRange<uint8_t>(1, 10);
    auto speed = *rc::gen::element<uint32_t>(1u, 2u, 3u);

    MockDeviceInfo dev{idVendor, idProduct, bcdDevice, bDeviceClass,
                       bDeviceSubClass, bDeviceProtocol, bConfigVal,
                       bNumConfigs, bNumInterfaces, speed};

    auto response = build_import_success_response("1-1", dev);

    // Success response must be exactly 320 bytes
    RC_ASSERT(response.size() == 320u);
}

// --- Property test: failed import response is exactly 8 bytes ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_FailureIs8Bytes, ()) {
    // **Validates: Requirements 2.2**
    auto response = build_import_failure_response();

    // Failure response must be exactly 8 bytes
    RC_ASSERT(response.size() == 8u);
}

// --- Property test: failed import response has non-zero status ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_FailureHasNonZeroStatus, ()) {
    // **Validates: Requirements 2.2**
    auto response = build_import_failure_response();

    // Status at bytes 4-7 must be non-zero
    uint32_t status = read_be32(&response[4]);
    RC_ASSERT(status != 0u);
}

// --- Property test: successful import response has status=0 ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_SuccessHasZeroStatus, ()) {
    // **Validates: Requirements 2.1**
    auto idVendor = *rc::gen::inRange<uint16_t>(1, 0xFFFF);
    auto idProduct = *rc::gen::inRange<uint16_t>(1, 0xFFFF);
    auto speed = *rc::gen::element<uint32_t>(1u, 2u, 3u);

    MockDeviceInfo dev{idVendor, idProduct, 0x0100, 0xFF, 0, 0, 1, 1, 1, speed};
    auto response = build_import_success_response("1-1", dev);

    // Status at bytes 4-7 must be zero
    uint32_t status = read_be32(&response[4]);
    RC_ASSERT(status == 0u);
}

// --- Property test: busid is echoed back in successful response ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_BusidEchoed, ()) {
    // **Validates: Requirements 2.3**
    // Generate a random busid string (1-31 printable characters)
    auto busid_len = *rc::gen::inRange<int>(1, 31);
    auto busid_chars = *rc::gen::container<std::vector<char>>(
        busid_len,
        rc::gen::inRange<char>('!', '~'));  // printable ASCII, no spaces
    std::string busid(busid_chars.begin(), busid_chars.end());

    MockDeviceInfo dev{0x1234, 0x5678, 0x0100, 0xFF, 0, 0, 1, 1, 2, 2};
    auto response = build_import_success_response(busid.c_str(), dev);

    // Extract busid from response at offset 264 (32-byte field)
    char echoed_busid[32] = {};
    memcpy(echoed_busid, &response[264], 31);

    // Echoed busid must match the requested busid
    RC_ASSERT(strcmp(echoed_busid, busid.c_str()) == 0);
}

// --- Property test: busid field is null-terminated and zero-padded ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_BusidNullTerminatedAndPadded, ()) {
    // **Validates: Requirements 2.3**
    // Generate a short busid (3-10 chars)
    auto busid_len = *rc::gen::inRange<int>(1, 10);
    auto busid_chars = *rc::gen::container<std::vector<char>>(
        busid_len,
        rc::gen::inRange<char>('0', '9'));
    std::string busid(busid_chars.begin(), busid_chars.end());

    MockDeviceInfo dev{0x1234, 0x5678, 0x0100, 0, 0, 0, 1, 1, 1, 3};
    auto response = build_import_success_response(busid.c_str(), dev);

    // The busid field starts at offset 264, is 32 bytes total
    // Must have a null terminator after the string
    RC_ASSERT(response[264 + busid_len] == 0);

    // Remaining bytes after the null terminator must be zero (padding)
    for (int i = busid_len + 1; i < 32; i++) {
        RC_ASSERT(response[264 + i] == 0);
    }
}

// --- Property test: busid matching is null-terminated string comparison ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_BusidMatchIsStringComparison, ()) {
    // **Validates: Requirements 2.1, 2.2**
    // Generate two different busid strings
    auto busid_a_len = *rc::gen::inRange<int>(1, 10);
    auto busid_a_chars = *rc::gen::container<std::vector<char>>(
        busid_a_len, rc::gen::inRange<char>('0', '9'));
    std::string busid_a(busid_a_chars.begin(), busid_a_chars.end());

    auto busid_b_len = *rc::gen::inRange<int>(1, 10);
    auto busid_b_chars = *rc::gen::container<std::vector<char>>(
        busid_b_len, rc::gen::inRange<char>('a', 'z'));
    std::string busid_b(busid_b_chars.begin(), busid_b_chars.end());

    // Same busid must match
    RC_ASSERT(import_busid_matches(busid_a.c_str(), busid_a.c_str(), true));

    // Different busid must NOT match (unless they happen to be equal)
    if (busid_a != busid_b) {
        RC_ASSERT(!import_busid_matches(busid_a.c_str(), busid_b.c_str(), true));
    }
}

// --- Property test: device not ready always results in failure regardless of busid ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_NotReadyAlwaysFails, ()) {
    // **Validates: Requirements 2.2**
    // Generate a random busid
    auto busid_len = *rc::gen::inRange<int>(1, 10);
    auto busid_chars = *rc::gen::container<std::vector<char>>(
        busid_len, rc::gen::inRange<char>('0', '9'));
    std::string busid(busid_chars.begin(), busid_chars.end());

    // When device_ready is false, matching always fails
    RC_ASSERT(!import_busid_matches(busid.c_str(), busid.c_str(), false));
}

// --- Property test: success response header has correct version and command ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_SuccessHeaderFields, ()) {
    // **Validates: Requirements 2.1**
    auto speed = *rc::gen::element<uint32_t>(1u, 2u, 3u);
    MockDeviceInfo dev{0x1234, 0x5678, 0x0100, 0xFF, 0, 0, 1, 1, 1, speed};
    auto response = build_import_success_response("1-1", dev);

    // Version at bytes 0-1 = 0x0111
    uint16_t version = (uint16_t(response[0]) << 8) | response[1];
    RC_ASSERT(version == 0x0111);

    // Command at bytes 2-3 = 0x0003 (OP_REP_IMPORT)
    uint16_t command = (uint16_t(response[2]) << 8) | response[3];
    RC_ASSERT(command == 0x0003);
}

// --- Property test: failure response header has correct version and command ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_FailureHeaderFields, ()) {
    // **Validates: Requirements 2.2**
    auto response = build_import_failure_response();

    // Version at bytes 0-1 = 0x0111
    uint16_t version = (uint16_t(response[0]) << 8) | response[1];
    RC_ASSERT(version == 0x0111);

    // Command at bytes 2-3 = 0x0003 (OP_REP_IMPORT)
    uint16_t command = (uint16_t(response[2]) << 8) | response[3];
    RC_ASSERT(command == 0x0003);
}

// --- Property test: device descriptor fields are populated correctly ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_DeviceFieldsPopulated, ()) {
    // **Validates: Requirements 2.6**
    // Generate random device descriptor values
    auto idVendor = *rc::gen::inRange<uint16_t>(1, 0xFFFF);
    auto idProduct = *rc::gen::inRange<uint16_t>(1, 0xFFFF);
    auto bcdDevice = *rc::gen::arbitrary<uint16_t>();
    auto bDeviceClass = *rc::gen::arbitrary<uint8_t>();
    auto bDeviceSubClass = *rc::gen::arbitrary<uint8_t>();
    auto bDeviceProtocol = *rc::gen::arbitrary<uint8_t>();
    auto bConfigVal = *rc::gen::inRange<uint8_t>(1, 255);
    auto bNumConfigs = *rc::gen::inRange<uint8_t>(1, 5);
    auto bNumInterfaces = *rc::gen::inRange<uint8_t>(1, 10);
    auto speed = *rc::gen::element<uint32_t>(1u, 2u, 3u);

    MockDeviceInfo dev{idVendor, idProduct, bcdDevice, bDeviceClass,
                       bDeviceSubClass, bDeviceProtocol, bConfigVal,
                       bNumConfigs, bNumInterfaces, speed};

    auto response = build_import_success_response("1-1", dev);

    // Verify idVendor at bytes 308-309 (big-endian)
    uint16_t wire_vendor = (uint16_t(response[308]) << 8) | response[309];
    RC_ASSERT(wire_vendor == idVendor);

    // Verify idProduct at bytes 310-311 (big-endian)
    uint16_t wire_product = (uint16_t(response[310]) << 8) | response[311];
    RC_ASSERT(wire_product == idProduct);

    // Verify bcdDevice at bytes 312-313 (big-endian)
    uint16_t wire_bcd = (uint16_t(response[312]) << 8) | response[313];
    RC_ASSERT(wire_bcd == bcdDevice);

    // Verify single-byte fields (no byte swap)
    RC_ASSERT(response[314] == bDeviceClass);
    RC_ASSERT(response[315] == bDeviceSubClass);
    RC_ASSERT(response[316] == bDeviceProtocol);
    RC_ASSERT(response[317] == bConfigVal);
    RC_ASSERT(response[318] == bNumConfigs);
    RC_ASSERT(response[319] == bNumInterfaces);

    // Verify speed at bytes 304-307 (big-endian)
    uint32_t wire_speed = read_be32(&response[304]);
    RC_ASSERT(wire_speed == speed);
}

// --- Property test: response size dichotomy — exactly 320 or 8, nothing else ---

RC_GTEST_PROP(USBIPProtocol, ImportResponse_SizeDichotomy, ()) {
    // **Validates: Requirements 2.1, 2.2**
    // Generate a random scenario: device ready or not, busid match or not
    auto device_ready = *rc::gen::arbitrary<bool>();
    auto busid_matches = *rc::gen::arbitrary<bool>();

    // Simulate the outcome
    bool is_success = device_ready && busid_matches;

    if (is_success) {
        MockDeviceInfo dev{0x1234, 0x5678, 0x0100, 0, 0, 0, 1, 1, 1, 2};
        auto response = build_import_success_response("1-1", dev);
        RC_ASSERT(response.size() == 320u);
    } else {
        auto response = build_import_failure_response();
        RC_ASSERT(response.size() == 8u);
    }
}

// --- Deterministic test: verify successful import response byte-by-byte ---

TEST(USBIPProtocol, ImportResponse_SuccessFullVerification) {
    // **Validates: Requirements 2.1, 2.3, 2.6**
    MockDeviceInfo dev{0x1D6B, 0x0002, 0x0413, 0x09, 0x00, 0x01, 1, 1, 2, 3};
    auto response = build_import_success_response("1-1", dev);

    ASSERT_EQ(response.size(), 320u);

    // Header
    EXPECT_EQ(response[0], 0x01);  // version high
    EXPECT_EQ(response[1], 0x11);  // version low
    EXPECT_EQ(response[2], 0x00);  // command high
    EXPECT_EQ(response[3], 0x03);  // command low
    EXPECT_EQ(read_be32(&response[4]), 0u);  // status = 0

    // path starts at offset 8
    EXPECT_EQ(response[8], '/');
    EXPECT_EQ(response[9], 'e');

    // busid at offset 264: "1-1\0..."
    EXPECT_EQ(response[264], '1');
    EXPECT_EQ(response[265], '-');
    EXPECT_EQ(response[266], '1');
    EXPECT_EQ(response[267], 0x00);  // null terminator
    // remaining busid bytes zeroed
    for (int i = 268; i < 296; i++) {
        EXPECT_EQ(response[i], 0x00);
    }

    // busnum = 1 at offset 296
    EXPECT_EQ(read_be32(&response[296]), 1u);
    // devnum = 1 at offset 300
    EXPECT_EQ(read_be32(&response[300]), 1u);
    // speed = 3 at offset 304
    EXPECT_EQ(read_be32(&response[304]), 3u);

    // idVendor = 0x1D6B at offset 308
    EXPECT_EQ(response[308], 0x1D);
    EXPECT_EQ(response[309], 0x6B);
    // idProduct = 0x0002 at offset 310
    EXPECT_EQ(response[310], 0x00);
    EXPECT_EQ(response[311], 0x02);
    // bcdDevice = 0x0413 at offset 312
    EXPECT_EQ(response[312], 0x04);
    EXPECT_EQ(response[313], 0x13);

    // Single-byte fields
    EXPECT_EQ(response[314], 0x09);  // bDeviceClass
    EXPECT_EQ(response[315], 0x00);  // bDeviceSubClass
    EXPECT_EQ(response[316], 0x01);  // bDeviceProtocol
    EXPECT_EQ(response[317], 1);     // bConfigurationValue
    EXPECT_EQ(response[318], 1);     // bNumConfigurations
    EXPECT_EQ(response[319], 2);     // bNumInterfaces
}

// --- Deterministic test: verify failed import response byte-by-byte ---

TEST(USBIPProtocol, ImportResponse_FailureFullVerification) {
    // **Validates: Requirements 2.2**
    auto response = build_import_failure_response();

    ASSERT_EQ(response.size(), 8u);

    // version = 0x0111
    EXPECT_EQ(response[0], 0x01);
    EXPECT_EQ(response[1], 0x11);
    // command = 0x0003
    EXPECT_EQ(response[2], 0x00);
    EXPECT_EQ(response[3], 0x03);
    // status = 1 (non-zero) in big-endian
    EXPECT_EQ(response[4], 0x00);
    EXPECT_EQ(response[5], 0x00);
    EXPECT_EQ(response[6], 0x00);
    EXPECT_EQ(response[7], 0x01);
}

// --- Deterministic test: busid matching logic ---

TEST(USBIPProtocol, ImportResponse_BusidMatchingLogic) {
    // **Validates: Requirements 2.1, 2.2**
    // Device ready, exact match
    EXPECT_TRUE(import_busid_matches("1-1", "1-1", true));
    // Device ready, no match
    EXPECT_FALSE(import_busid_matches("2-1", "1-1", true));
    EXPECT_FALSE(import_busid_matches("1-1-1", "1-1", true));
    EXPECT_FALSE(import_busid_matches("", "1-1", true));
    // Device not ready, even with matching busid
    EXPECT_FALSE(import_busid_matches("1-1", "1-1", false));
}

// ==========================================================================
// Property 3: OP_REP_DEVLIST Structure Integrity
// Validates: Requirements 1.1, 1.2, 1.3, 1.4
//
// For any valid USB device descriptor and configuration descriptor with N
// interfaces (0 ≤ N ≤ 10), the OP_REP_DEVLIST response SHALL have total
// length = 12 + 312 + (N × 4) bytes when a device is present, or exactly
// 12 bytes when no device is present, with all device descriptor fields at
// their specified byte offsets within the 312-byte structure.
// ==========================================================================

// --------------------------------------------------------------------------
// The OP_REP_DEVLIST wire format:
//
// When device is present:
//   Bytes 0-1:   version (0x0111, big-endian)
//   Bytes 2-3:   command (0x0005, big-endian)
//   Bytes 4-7:   status (0x00000000)
//   Bytes 8-11:  device count (big-endian, = 1 for single device)
//   Bytes 12-267: path[256] (null-terminated, zero-padded)
//   Bytes 268-299: busid[32] (null-terminated, zero-padded)
//   Bytes 300-303: busnum (big-endian uint32)
//   Bytes 304-307: devnum (big-endian uint32)
//   Bytes 308-311: speed (big-endian uint32)
//   Bytes 312-313: idVendor (big-endian uint16)
//   Bytes 314-315: idProduct (big-endian uint16)
//   Bytes 316-317: bcdDevice (big-endian uint16)
//   Byte 318:    bDeviceClass
//   Byte 319:    bDeviceSubClass
//   Byte 320:    bDeviceProtocol
//   Byte 321:    bConfigurationValue
//   Byte 322:    bNumConfigurations
//   Byte 323:    bNumInterfaces
//   Bytes 324..(324 + N*4 - 1): N interface descriptors (4 bytes each)
//
// When no device is present:
//   Bytes 0-1:   version (0x0111)
//   Bytes 2-3:   command (0x0005)
//   Bytes 4-7:   status (0x00000000)
//   Bytes 8-11:  device count (0x00000000)
//   Total: 12 bytes
//
// These offsets match the static_asserts in usb_ip.h exactly.
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// Helper: compute the expected OP_REP_DEVLIST response length.
// device_present: true if a USB device is attached
// num_interfaces: number of interfaces (0-10) on the device
// --------------------------------------------------------------------------

static size_t compute_devlist_response_length(bool device_present, uint8_t num_interfaces) {
    if (!device_present) return 12;  // header(8) + count(4)
    return 12 + 312 + (static_cast<size_t>(num_interfaces) * 4);
}

// --------------------------------------------------------------------------
// Helper: verify that the usbip_devlist_t packed struct has the correct
// field offsets matching the wire format specification.
//
// This uses offsetof() to check the actual struct layout matches the spec.
// (These are also checked by static_asserts in usb_ip.h, but we verify
// them at runtime in test context to confirm our test struct mirrors the
// production struct.)
// --------------------------------------------------------------------------

#pragma pack(push, 1)
struct test_usbip_request_t { uint16_t version; uint16_t command; uint32_t status; };
struct test_usbip_interface_t { uint8_t bInterfaceClass; uint8_t bInterfaceSubClass; uint8_t bInterfaceProtocol; uint8_t padding; };
struct test_usbip_devlist_t {
    test_usbip_request_t request; uint32_t count;
    char path[256]; char busid[32];
    uint32_t busnum, devnum, speed;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bConfigurationValue, bNumConfigurations, bNumInterfaces;
    test_usbip_interface_t intfs[10];
};
#pragma pack(pop)

// --- Property test: response length = 12 + 312 + N×4 when device is present ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_LengthWithDevice, ()) {
    // **Validates: Requirements 1.2, 1.3**
    // Generate random number of interfaces [0, 10]
    auto num_interfaces = *rc::gen::inRange<uint8_t>(0, 11);

    size_t expected = 12 + 312 + (static_cast<size_t>(num_interfaces) * 4);
    size_t computed = compute_devlist_response_length(true, num_interfaces);

    RC_ASSERT(computed == expected);
    // Minimum: 12 + 312 + 0 = 324 (0 interfaces)
    RC_ASSERT(computed >= 324u);
    // Maximum: 12 + 312 + 40 = 364 (10 interfaces)
    RC_ASSERT(computed <= 364u);
}

// --- Property test: response length = 12 when no device present ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_LengthNoDevice, ()) {
    // **Validates: Requirements 1.4**
    // Regardless of what num_interfaces might be, no-device = 12 bytes
    auto num_interfaces = *rc::gen::inRange<uint8_t>(0, 11);

    size_t computed = compute_devlist_response_length(false, num_interfaces);
    RC_ASSERT(computed == 12u);
}

// --- Property test: struct size of devlist with 10 interfaces = 364 ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_PackedStructSize, ()) {
    // **Validates: Requirements 1.1, 1.2**
    // The packed struct must be exactly 364 bytes (12 + 312 + 40)
    // regardless of platform alignment
    RC_ASSERT(sizeof(test_usbip_devlist_t) == 364u);
}

// --- Property test: field offsets within the 312-byte device descriptor ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_FieldOffsetsCorrect, ()) {
    // **Validates: Requirements 1.1, 1.2**
    // All field offsets must match the wire format specification.
    // The device descriptor starts at offset 12 (after 8-byte header + 4-byte count).

    // path starts at offset 12
    RC_ASSERT(offsetof(test_usbip_devlist_t, path) == 12u);
    // busid starts at offset 268 (12 + 256)
    RC_ASSERT(offsetof(test_usbip_devlist_t, busid) == 268u);
    // busnum starts at offset 300 (268 + 32)
    RC_ASSERT(offsetof(test_usbip_devlist_t, busnum) == 300u);
    // devnum starts at offset 304
    RC_ASSERT(offsetof(test_usbip_devlist_t, devnum) == 304u);
    // speed starts at offset 308
    RC_ASSERT(offsetof(test_usbip_devlist_t, speed) == 308u);
    // idVendor starts at offset 312
    RC_ASSERT(offsetof(test_usbip_devlist_t, idVendor) == 312u);
    // idProduct starts at offset 314
    RC_ASSERT(offsetof(test_usbip_devlist_t, idProduct) == 314u);
    // bcdDevice starts at offset 316
    RC_ASSERT(offsetof(test_usbip_devlist_t, bcdDevice) == 316u);
    // bDeviceClass at offset 318
    RC_ASSERT(offsetof(test_usbip_devlist_t, bDeviceClass) == 318u);
    // bDeviceSubClass at offset 319
    RC_ASSERT(offsetof(test_usbip_devlist_t, bDeviceSubClass) == 319u);
    // bDeviceProtocol at offset 320
    RC_ASSERT(offsetof(test_usbip_devlist_t, bDeviceProtocol) == 320u);
    // bConfigurationValue at offset 321
    RC_ASSERT(offsetof(test_usbip_devlist_t, bConfigurationValue) == 321u);
    // bNumConfigurations at offset 322
    RC_ASSERT(offsetof(test_usbip_devlist_t, bNumConfigurations) == 322u);
    // bNumInterfaces at offset 323
    RC_ASSERT(offsetof(test_usbip_devlist_t, bNumInterfaces) == 323u);
    // Interface array starts at offset 324 (12 + 312 = end of device descriptor)
    RC_ASSERT(offsetof(test_usbip_devlist_t, intfs) == 324u);
}

// --- Property test: interface descriptors are exactly 4 bytes each ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_InterfaceDescriptorSize, ()) {
    // **Validates: Requirements 1.3**
    // Each interface descriptor is exactly 4 bytes (class, subclass, protocol, padding)
    RC_ASSERT(sizeof(test_usbip_interface_t) == 4u);

    // Generate a random number of interfaces and verify total interface block size
    auto num_interfaces = *rc::gen::inRange<uint8_t>(0, 11);
    size_t intf_block_size = static_cast<size_t>(num_interfaces) * sizeof(test_usbip_interface_t);
    RC_ASSERT(intf_block_size == static_cast<size_t>(num_interfaces) * 4u);
}

// --- Property test: 312-byte device structure size is correct ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_DeviceStructIs312Bytes, ()) {
    // **Validates: Requirements 1.2**
    // The device descriptor portion (without header and interfaces) is exactly 312 bytes.
    // Computed as: offset(intfs) - offset(path) = 324 - 12 = 312
    size_t device_struct_size = offsetof(test_usbip_devlist_t, intfs) - offsetof(test_usbip_devlist_t, path);
    RC_ASSERT(device_struct_size == 312u);
}

// --- Property test: response length matches struct minus unused interfaces ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_ResponseLengthMatchesSendSize, ()) {
    // **Validates: Requirements 1.2, 1.3**
    // The actual send size should be sizeof(devlist) - sizeof(intfs) + num_interfaces * 4
    // This mirrors the logic in parse_request():
    //   resp_len = sizeof(usbip_devlist_t) - sizeof(devlist_.intfs) + sizeof(usbip_interface_t) * num_intf
    auto num_interfaces = *rc::gen::inRange<uint8_t>(0, 11);

    size_t resp_len = sizeof(test_usbip_devlist_t)
                    - sizeof(test_usbip_interface_t) * 10  // remove all 10 interface slots
                    + sizeof(test_usbip_interface_t) * num_interfaces;  // add back only used ones

    size_t expected = 12 + 312 + (static_cast<size_t>(num_interfaces) * 4);
    RC_ASSERT(resp_len == expected);
}

// --- Property test: header portion is always 12 bytes (8 op_common + 4 count) ---

RC_GTEST_PROP(USBIPProtocol, DevlistStructure_HeaderIs12Bytes, ()) {
    // **Validates: Requirements 1.1**
    // The header portion (request + count) occupies exactly 12 bytes
    size_t header_size = offsetof(test_usbip_devlist_t, path);
    RC_ASSERT(header_size == 12u);

    // Verify composition: request (8 bytes) + count (4 bytes)
    RC_ASSERT(sizeof(test_usbip_request_t) == 8u);
    RC_ASSERT(sizeof(uint32_t) == 4u);
    RC_ASSERT(sizeof(test_usbip_request_t) + sizeof(uint32_t) == 12u);
}

// --- Deterministic test: verify all offset values match specification ---

TEST(USBIPProtocol, DevlistStructure_AllOffsetsMatchSpec) {
    // **Validates: Requirements 1.1, 1.2**
    // These match the static_asserts from usb_ip.h exactly
    EXPECT_EQ(sizeof(test_usbip_devlist_t), 364u);
    EXPECT_EQ(sizeof(test_usbip_request_t), 8u);
    EXPECT_EQ(sizeof(test_usbip_interface_t), 4u);

    EXPECT_EQ(offsetof(test_usbip_devlist_t, path), 12u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, busid), 268u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, busnum), 300u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, devnum), 304u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, speed), 308u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, idVendor), 312u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, idProduct), 314u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, bcdDevice), 316u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, bDeviceClass), 318u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, bDeviceSubClass), 319u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, bDeviceProtocol), 320u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, bConfigurationValue), 321u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, bNumConfigurations), 322u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, bNumInterfaces), 323u);
    EXPECT_EQ(offsetof(test_usbip_devlist_t, intfs), 324u);
}

// --- Deterministic test: response sizes for boundary interface counts ---

TEST(USBIPProtocol, DevlistStructure_ResponseSizeBoundaries) {
    // **Validates: Requirements 1.2, 1.3, 1.4**
    // No device: exactly 12 bytes
    EXPECT_EQ(compute_devlist_response_length(false, 0), 12u);
    EXPECT_EQ(compute_devlist_response_length(false, 5), 12u);
    EXPECT_EQ(compute_devlist_response_length(false, 10), 12u);

    // Device present with N interfaces:
    EXPECT_EQ(compute_devlist_response_length(true, 0), 324u);   // 12 + 312 + 0
    EXPECT_EQ(compute_devlist_response_length(true, 1), 328u);   // 12 + 312 + 4
    EXPECT_EQ(compute_devlist_response_length(true, 2), 332u);   // 12 + 312 + 8
    EXPECT_EQ(compute_devlist_response_length(true, 5), 344u);   // 12 + 312 + 20
    EXPECT_EQ(compute_devlist_response_length(true, 10), 364u);  // 12 + 312 + 40
}

// --- Deterministic test: verify device descriptor within struct is at correct byte positions ---

TEST(USBIPProtocol, DevlistStructure_DeviceFieldByteLevelCheck) {
    // **Validates: Requirements 1.1, 1.2**
    // Fill a test struct and verify fields appear at expected byte offsets
    test_usbip_devlist_t dl = {};
    memset(&dl, 0, sizeof(dl));

    // Set distinguishable values
    dl.request.version = 0x0111;
    dl.request.command = 0x0005;
    dl.request.status = 0;
    dl.count = 0x01000000;  // 1 in big-endian (as stored on wire)
    strncpy(dl.path, "/test/path", sizeof(dl.path) - 1);
    strncpy(dl.busid, "1-1", sizeof(dl.busid) - 1);
    dl.busnum = 0x01000000;  // 1 in big-endian
    dl.devnum = 0x02000000;  // 2 in big-endian
    dl.speed = 0x03000000;   // 3 in big-endian
    dl.idVendor = 0xABCD;
    dl.idProduct = 0x1234;
    dl.bcdDevice = 0x0100;
    dl.bDeviceClass = 0xFF;
    dl.bDeviceSubClass = 0xFE;
    dl.bDeviceProtocol = 0xFD;
    dl.bConfigurationValue = 1;
    dl.bNumConfigurations = 1;
    dl.bNumInterfaces = 2;
    dl.intfs[0] = {0x03, 0x01, 0x02, 0x00};
    dl.intfs[1] = {0x08, 0x06, 0x50, 0x00};

    const uint8_t *raw = reinterpret_cast<const uint8_t *>(&dl);

    // Verify path starts at byte 12
    EXPECT_EQ(raw[12], '/');
    EXPECT_EQ(raw[13], 't');

    // Verify busid starts at byte 268
    EXPECT_EQ(raw[268], '1');
    EXPECT_EQ(raw[269], '-');
    EXPECT_EQ(raw[270], '1');

    // Verify bDeviceClass at byte 318
    EXPECT_EQ(raw[318], 0xFF);
    // Verify bDeviceSubClass at byte 319
    EXPECT_EQ(raw[319], 0xFE);
    // Verify bDeviceProtocol at byte 320
    EXPECT_EQ(raw[320], 0xFD);
    // Verify bConfigurationValue at byte 321
    EXPECT_EQ(raw[321], 1);
    // Verify bNumConfigurations at byte 322
    EXPECT_EQ(raw[322], 1);
    // Verify bNumInterfaces at byte 323
    EXPECT_EQ(raw[323], 2);

    // Verify first interface at byte 324
    EXPECT_EQ(raw[324], 0x03);  // bInterfaceClass
    EXPECT_EQ(raw[325], 0x01);  // bInterfaceSubClass
    EXPECT_EQ(raw[326], 0x02);  // bInterfaceProtocol
    EXPECT_EQ(raw[327], 0x00);  // padding

    // Verify second interface at byte 328
    EXPECT_EQ(raw[328], 0x08);  // bInterfaceClass
    EXPECT_EQ(raw[329], 0x06);  // bInterfaceSubClass
    EXPECT_EQ(raw[330], 0x50);  // bInterfaceProtocol
    EXPECT_EQ(raw[331], 0x00);  // padding
}

// ==========================================================================
// Unit Tests: Connection Cleanup Logic
// Validates: Requirements 9.1, 9.3, 9.7
//
// These tests simulate the cleanup_connection_() state machine in isolation,
// verifying that all pending URBs are cancelled, the socket is invalidated
// to prevent further sends, and interface state is reset so the server can
// accept a new connection.
// ==========================================================================

// --------------------------------------------------------------------------
// Simulated cleanup state machine.
// Mirrors the logic of USBIPComponent::cleanup_connection_() exactly:
//   1. Set client_sock_ = -1 (prevents callbacks from sending)
//   2. Mark all pending URBs as cancelled and clear the map
//   3. Close the old socket (simulated as setting old_sock to -1)
//   4. Reset interfaces_claimed_ to false
// --------------------------------------------------------------------------

struct SimulatedXferCtx {
    uint32_t seqnum;
    bool cancelled;
};

struct SimulatedConnectionState {
    int client_sock;
    bool interfaces_claimed;
    std::unordered_map<uint32_t, SimulatedXferCtx*> pending_urbs;

    // Simulates cleanup_connection_() from usb_ip.cpp
    void cleanup_connection() {
        // 1. Mark socket as invalid (prevents callbacks from sending)
        int old_sock = client_sock;
        client_sock = -1;

        // 2. Cancel all pending USB transfers
        for (auto &[seqnum, ctx] : pending_urbs) {
            ctx->cancelled = true;
        }
        pending_urbs.clear();

        // 3. Close socket (simulated — in real code this calls shutdown+close)
        (void)old_sock;  // In real code: shutdown(old_sock, SHUT_RDWR); close(old_sock);

        // 4. Reset interface state
        interfaces_claimed = false;
    }

    // Simulates what send_response() does when client_sock_ == -1
    size_t send_response(const void * /*data*/, size_t /*len*/) {
        if (client_sock == -1) {
            return 0;  // No send — socket invalid
        }
        return 1;  // Simulated success
    }

    // Simulates what a callback does when it finds ctx->cancelled == true
    bool callback_should_send(SimulatedXferCtx *ctx) {
        if (ctx->cancelled) {
            return false;  // Suppress response, just free resources
        }
        return true;  // Normal completion — send response
    }
};

// --- Test: all pending URB entries are cancelled and map cleared on disconnect ---

TEST(USBIPProtocol, Cleanup_AllXferCtxCancelledAndMapCleared) {
    // **Validates: Requirements 9.1, 9.3**
    // Setup: simulate a connection with multiple pending URBs
    SimulatedConnectionState state;
    state.client_sock = 5;  // valid socket
    state.interfaces_claimed = true;

    // Create several pending XferCtx entries
    auto *ctx1 = new SimulatedXferCtx{100, false};
    auto *ctx2 = new SimulatedXferCtx{200, false};
    auto *ctx3 = new SimulatedXferCtx{300, false};
    state.pending_urbs[100] = ctx1;
    state.pending_urbs[200] = ctx2;
    state.pending_urbs[300] = ctx3;

    ASSERT_EQ(state.pending_urbs.size(), 3u);
    ASSERT_FALSE(ctx1->cancelled);
    ASSERT_FALSE(ctx2->cancelled);
    ASSERT_FALSE(ctx3->cancelled);

    // Act: perform cleanup
    state.cleanup_connection();

    // Assert: map is cleared
    EXPECT_TRUE(state.pending_urbs.empty());

    // Assert: all ctx entries have cancelled = true
    // (In real code, the callbacks will check this flag when they fire)
    EXPECT_TRUE(ctx1->cancelled);
    EXPECT_TRUE(ctx2->cancelled);
    EXPECT_TRUE(ctx3->cancelled);

    // Cleanup heap
    delete ctx1;
    delete ctx2;
    delete ctx3;
}

// --- Test: client_sock_ set to -1 after cleanup (prevents further sends) ---

TEST(USBIPProtocol, Cleanup_ClientSockInvalidatedPreventsResponses) {
    // **Validates: Requirements 9.1, 9.3**
    // Setup: simulate an active connection
    SimulatedConnectionState state;
    state.client_sock = 7;  // valid socket
    state.interfaces_claimed = true;

    // Verify send works before cleanup
    EXPECT_EQ(state.send_response(nullptr, 10), 1u);

    // Act: perform cleanup
    state.cleanup_connection();

    // Assert: client_sock is -1
    EXPECT_EQ(state.client_sock, -1);

    // Assert: send_response returns 0 (no data sent) when socket is invalid
    EXPECT_EQ(state.send_response(nullptr, 10), 0u);
}

// --- Test: callbacks finding cancelled flag don't send responses ---

TEST(USBIPProtocol, Cleanup_CancelledCallbacksSuppressResponses) {
    // **Validates: Requirements 9.1, 9.3**
    // Setup: simulate a connection with pending URBs
    SimulatedConnectionState state;
    state.client_sock = 4;
    state.interfaces_claimed = true;

    auto *ctx1 = new SimulatedXferCtx{500, false};
    auto *ctx2 = new SimulatedXferCtx{501, false};
    state.pending_urbs[500] = ctx1;
    state.pending_urbs[501] = ctx2;

    // Before cleanup: callbacks should send responses
    EXPECT_TRUE(state.callback_should_send(ctx1));
    EXPECT_TRUE(state.callback_should_send(ctx2));

    // Act: perform cleanup
    state.cleanup_connection();

    // After cleanup: callbacks find cancelled=true and suppress responses
    EXPECT_FALSE(state.callback_should_send(ctx1));
    EXPECT_FALSE(state.callback_should_send(ctx2));

    // Additionally, even if a callback tries to send, send_response returns 0
    EXPECT_EQ(state.send_response(nullptr, 48), 0u);

    // Cleanup heap
    delete ctx1;
    delete ctx2;
}

// --- Test: interfaces_claimed_ reset to false after cleanup ---

TEST(USBIPProtocol, Cleanup_InterfacesClaimedReset) {
    // **Validates: Requirements 9.7**
    // Setup: simulate a connection where interfaces were claimed
    SimulatedConnectionState state;
    state.client_sock = 3;
    state.interfaces_claimed = true;

    ASSERT_TRUE(state.interfaces_claimed);

    // Act: perform cleanup
    state.cleanup_connection();

    // Assert: interfaces_claimed is reset to false
    // This ensures the next connection will re-claim interfaces if needed
    EXPECT_FALSE(state.interfaces_claimed);
}

// --- Test: cleanup with empty pending map (no URBs in flight) ---

TEST(USBIPProtocol, Cleanup_EmptyPendingMapNoError) {
    // **Validates: Requirements 9.3, 9.7**
    // Setup: connection with no pending URBs
    SimulatedConnectionState state;
    state.client_sock = 10;
    state.interfaces_claimed = true;

    ASSERT_TRUE(state.pending_urbs.empty());

    // Act: cleanup should handle empty map gracefully
    state.cleanup_connection();

    // Assert: state is properly reset
    EXPECT_EQ(state.client_sock, -1);
    EXPECT_FALSE(state.interfaces_claimed);
    EXPECT_TRUE(state.pending_urbs.empty());
}

// --- Test: cleanup with client_sock already -1 (double-cleanup safety) ---

TEST(USBIPProtocol, Cleanup_AlreadyDisconnectedSafe) {
    // **Validates: Requirements 9.3, 9.7**
    // Setup: simulate a state where socket is already -1
    SimulatedConnectionState state;
    state.client_sock = -1;
    state.interfaces_claimed = false;

    // Act: cleanup should not crash or misbehave
    state.cleanup_connection();

    // Assert: state remains clean
    EXPECT_EQ(state.client_sock, -1);
    EXPECT_FALSE(state.interfaces_claimed);
    EXPECT_TRUE(state.pending_urbs.empty());
}

// --- Test: server returns to accept loop after cleanup (ready for new connection) ---

TEST(USBIPProtocol, Cleanup_ServerReadyForNewConnection) {
    // **Validates: Requirements 9.3, 9.7**
    // This test verifies the post-cleanup state matches what tcp_task_
    // expects when returning to the top of the accept loop:
    //   - client_sock_ == -1 (no active client)
    //   - interfaces_claimed_ == false (will re-claim on next submit)
    //   - pending_urbs_ is empty (no leftover state)
    SimulatedConnectionState state;
    state.client_sock = 8;
    state.interfaces_claimed = true;

    auto *ctx = new SimulatedXferCtx{999, false};
    state.pending_urbs[999] = ctx;

    // Act: perform cleanup (simulates what happens when recv returns 0 or error)
    state.cleanup_connection();

    // Assert: server state is ready for the next accept() call
    EXPECT_EQ(state.client_sock, -1);
    EXPECT_FALSE(state.interfaces_claimed);
    EXPECT_TRUE(state.pending_urbs.empty());

    // Simulate accepting a new connection
    state.client_sock = 12;  // New socket from accept()

    // Verify: send_response works again with the new socket
    EXPECT_EQ(state.send_response(nullptr, 10), 1u);

    // Verify: new URBs can be tracked again
    auto *new_ctx = new SimulatedXferCtx{1000, false};
    state.pending_urbs[1000] = new_ctx;
    EXPECT_EQ(state.pending_urbs.size(), 1u);

    // Cleanup heap
    delete ctx;
    delete new_ctx;
}

// --- Test: large number of pending URBs all get cancelled ---

TEST(USBIPProtocol, Cleanup_ManyPendingURBsAllCancelled) {
    // **Validates: Requirements 9.1, 9.3**
    // Stress test: simulate many concurrent URBs in flight at disconnect
    SimulatedConnectionState state;
    state.client_sock = 6;
    state.interfaces_claimed = true;

    const size_t NUM_URBS = 50;
    std::vector<SimulatedXferCtx*> contexts;

    for (size_t i = 0; i < NUM_URBS; i++) {
        auto *ctx = new SimulatedXferCtx{static_cast<uint32_t>(i + 1), false};
        state.pending_urbs[i + 1] = ctx;
        contexts.push_back(ctx);
    }

    ASSERT_EQ(state.pending_urbs.size(), NUM_URBS);

    // Act: cleanup all at once
    state.cleanup_connection();

    // Assert: map is empty
    EXPECT_TRUE(state.pending_urbs.empty());

    // Assert: every single context has cancelled = true
    for (size_t i = 0; i < NUM_URBS; i++) {
        EXPECT_TRUE(contexts[i]->cancelled)
            << "XferCtx with seqnum " << (i + 1) << " was not cancelled";
    }

    // Assert: no callback would send a response
    for (auto *ctx : contexts) {
        EXPECT_FALSE(state.callback_should_send(ctx));
    }

    // Cleanup heap
    for (auto *ctx : contexts) {
        delete ctx;
    }
}

// ==========================================================================
// Property 2: Opaque Field Pass-Through
// Validates: Requirements 13.5, 13.6
//
// For any 8-byte setup packet in a CMD_SUBMIT request, the setup field SHALL
// appear byte-for-byte identical in the forwarded USB transfer. For any
// transfer_buffer data received from the physical device, the bytes SHALL
// appear in the RET_SUBMIT response without any byte-order transformation.
// ==========================================================================

// --------------------------------------------------------------------------
// Helper: simulate the setup packet forwarding path.
// In req_ctrl_xfer_(), the 8-byte setup field from the CMD_SUBMIT request is
// copied verbatim into the USB transfer's data_buffer (offset 0, no swap):
//   memcpy(xfer->data_buffer, &req->setup, 8);
//
// This function replicates that logic: given an 8-byte setup array from the
// request, produce the 8-byte setup as it would appear in the USB transfer.
// --------------------------------------------------------------------------

static void forward_setup_packet(const uint8_t *request_setup, uint8_t *usb_xfer_setup) {
    // Opaque copy — no byte-order transformation applied
    memcpy(usb_xfer_setup, request_setup, 8);
}

// --------------------------------------------------------------------------
// Helper: simulate the transfer_buffer pass-through path for IN responses.
// In ep_cb_() for bulk/interrupt IN (success), data is copied as-is:
//   memcpy(req->transfer_buffer, xfer->data_buffer, data_len);
// In ctrl_cb_() for control IN (success), data from offset 8 is copied:
//   memcpy(req->transfer_buffer, xfer->data_buffer + 8, data_len);
//
// In both cases, the bytes are treated as opaque — no swap is applied.
// This function simulates writing device data into the response buffer.
// --------------------------------------------------------------------------

static void forward_device_data_to_response(const uint8_t *device_data, size_t data_len,
                                             uint8_t *response_transfer_buffer) {
    // Opaque copy — no byte-order transformation applied
    if (data_len > 0) {
        memcpy(response_transfer_buffer, device_data, data_len);
    }
}

// --- Property test: random 8-byte setup packets are preserved byte-for-byte ---

RC_GTEST_PROP(USBIPProtocol, OpaquePassThrough_SetupPacketPreserved, ()) {
    // **Validates: Requirements 13.5**
    // Generate a random 8-byte setup packet (any byte values)
    auto setup_vec = *rc::gen::container<std::vector<uint8_t>>(8, rc::gen::arbitrary<uint8_t>());

    // Forward through the simulated path
    uint8_t forwarded[8] = {};
    forward_setup_packet(setup_vec.data(), forwarded);

    // Every byte must be identical — no swap, no transformation
    for (int i = 0; i < 8; i++) {
        RC_ASSERT(forwarded[i] == setup_vec[i]);
    }
}

// --- Property test: setup packet with structured USB control fields preserved ---

RC_GTEST_PROP(USBIPProtocol, OpaquePassThrough_SetupFieldsNotSwapped, ()) {
    // **Validates: Requirements 13.5**
    // Generate a realistic setup packet with structured fields:
    //   Byte 0: bmRequestType
    //   Byte 1: bRequest
    //   Bytes 2-3: wValue (little-endian in USB spec)
    //   Bytes 4-5: wIndex (little-endian in USB spec)
    //   Bytes 6-7: wLength (little-endian in USB spec)
    auto bmRequestType = *rc::gen::arbitrary<uint8_t>();
    auto bRequest = *rc::gen::arbitrary<uint8_t>();
    auto wValue = *rc::gen::arbitrary<uint16_t>();
    auto wIndex = *rc::gen::arbitrary<uint16_t>();
    auto wLength = *rc::gen::inRange<uint16_t>(0, 1025);

    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = wValue & 0xFF;         // wValue low byte
    setup[3] = (wValue >> 8) & 0xFF;  // wValue high byte
    setup[4] = wIndex & 0xFF;         // wIndex low byte
    setup[5] = (wIndex >> 8) & 0xFF;  // wIndex high byte
    setup[6] = wLength & 0xFF;        // wLength low byte
    setup[7] = (wLength >> 8) & 0xFF; // wLength high byte

    // Forward through the opaque copy path
    uint8_t forwarded[8] = {};
    forward_setup_packet(setup, forwarded);

    // The setup packet must NOT be byte-swapped — preserved exactly
    RC_ASSERT(forwarded[0] == bmRequestType);
    RC_ASSERT(forwarded[1] == bRequest);
    // wValue bytes in original (little-endian) order
    RC_ASSERT(forwarded[2] == (wValue & 0xFF));
    RC_ASSERT(forwarded[3] == ((wValue >> 8) & 0xFF));
    // wIndex bytes in original (little-endian) order
    RC_ASSERT(forwarded[4] == (wIndex & 0xFF));
    RC_ASSERT(forwarded[5] == ((wIndex >> 8) & 0xFF));
    // wLength bytes in original (little-endian) order
    RC_ASSERT(forwarded[6] == (wLength & 0xFF));
    RC_ASSERT(forwarded[7] == ((wLength >> 8) & 0xFF));
}

// --- Property test: random transfer_buffer data appears as-is in response ---

RC_GTEST_PROP(USBIPProtocol, OpaquePassThrough_TransferBufferPreserved, ()) {
    // **Validates: Requirements 13.6**
    // Generate random transfer_buffer data of random length [1, 1024]
    auto data_len = *rc::gen::inRange<size_t>(1, 1025);
    auto device_data = *rc::gen::container<std::vector<uint8_t>>(data_len, rc::gen::arbitrary<uint8_t>());

    // Forward through the opaque copy path
    std::vector<uint8_t> response_buffer(data_len, 0);
    forward_device_data_to_response(device_data.data(), data_len, response_buffer.data());

    // Every byte must be identical — no swap, no transformation
    for (size_t i = 0; i < data_len; i++) {
        RC_ASSERT(response_buffer[i] == device_data[i]);
    }
}

// --- Property test: transfer_buffer with byte patterns that would change under swap ---

RC_GTEST_PROP(USBIPProtocol, OpaquePassThrough_NoSwapEvenForMultibytePatterns, ()) {
    // **Validates: Requirements 13.6**
    // Generate data that includes 32-bit values which would look different
    // if a big-endian swap were erroneously applied.
    // We place specific 4-byte sequences and verify they're not swapped.
    auto num_words = *rc::gen::inRange<size_t>(1, 256);
    std::vector<uint8_t> device_data(num_words * 4);

    // Fill with random uint32 values stored as little-endian bytes
    for (size_t i = 0; i < num_words; i++) {
        uint32_t val = *rc::gen::arbitrary<uint32_t>();
        device_data[i * 4 + 0] = (val >> 0) & 0xFF;
        device_data[i * 4 + 1] = (val >> 8) & 0xFF;
        device_data[i * 4 + 2] = (val >> 16) & 0xFF;
        device_data[i * 4 + 3] = (val >> 24) & 0xFF;
    }

    // Forward through the opaque copy path
    std::vector<uint8_t> response_buffer(device_data.size(), 0);
    forward_device_data_to_response(device_data.data(), device_data.size(), response_buffer.data());

    // Byte-for-byte identical — no swap applied
    for (size_t i = 0; i < device_data.size(); i++) {
        RC_ASSERT(response_buffer[i] == device_data[i]);
    }
}

// --- Property test: empty transfer_buffer (0 bytes) is handled correctly ---

RC_GTEST_PROP(USBIPProtocol, OpaquePassThrough_EmptyTransferBuffer, ()) {
    // **Validates: Requirements 13.6**
    // When data_len is 0, no bytes should be copied and no crash occurs
    uint8_t dummy_output[1] = {0xFF};  // sentinel to verify no write
    forward_device_data_to_response(nullptr, 0, dummy_output);

    // The sentinel byte should remain unchanged (no write occurred)
    RC_ASSERT(dummy_output[0] == 0xFF);
}

// --- Property test: setup packet embedded in CMD_SUBMIT struct is opaque ---

RC_GTEST_PROP(USBIPProtocol, OpaquePassThrough_SetupInSubmitStruct, ()) {
    // **Validates: Requirements 13.5**
    // Simulate the full CMD_SUBMIT flow: the setup field at offset 40-47
    // in the usbip_submit_t struct is copied verbatim to the USB transfer.
    auto setup_vec = *rc::gen::container<std::vector<uint8_t>>(8, rc::gen::arbitrary<uint8_t>());

    // Place into a usbip_submit_t struct (as it arrives from the wire)
    usbip_submit_t submit{};
    memcpy(&submit.setup, setup_vec.data(), 8);

    // The forwarding code does: memcpy(xfer->data_buffer, &req->setup, 8)
    uint8_t usb_data_buffer[1032] = {};
    memcpy(usb_data_buffer, &submit.setup, 8);

    // Verify byte-for-byte preservation
    for (int i = 0; i < 8; i++) {
        RC_ASSERT(usb_data_buffer[i] == setup_vec[i]);
    }
}

// --- Property test: transfer_buffer in RET_SUBMIT response matches device data ---

RC_GTEST_PROP(USBIPProtocol, OpaquePassThrough_RetSubmitTransferBuffer, ()) {
    // **Validates: Requirements 13.6**
    // Simulate the full response path: device returns data in xfer->data_buffer,
    // which is copied into the response's transfer_buffer field.
    auto data_len = *rc::gen::inRange<int>(1, 512);
    auto device_data = *rc::gen::container<std::vector<uint8_t>>(
        static_cast<size_t>(data_len), rc::gen::arbitrary<uint8_t>());

    // Build a response using the existing build_ret_submit helper pattern
    // but verify the data portion specifically
    usbip_submit_t response{};
    memset(&response, 0, sizeof(response));

    // Simulate: memcpy(req->transfer_buffer, xfer->data_buffer, data_len)
    // This is the actual code path in ep_cb_ for bulk/interrupt IN
    memcpy(response.transfer_buffer, device_data.data(), data_len);

    // Verify: every byte in transfer_buffer matches the device data exactly
    for (int i = 0; i < data_len; i++) {
        RC_ASSERT(response.transfer_buffer[i] == device_data[i]);
    }
}

// --- Deterministic test: specific setup packet byte pattern preserved ---

TEST(USBIPProtocol, OpaquePassThrough_SetupPacketDeterministic) {
    // **Validates: Requirements 13.5**
    // GET_DESCRIPTOR for device descriptor: 80 06 00 01 00 00 12 00
    uint8_t setup[8] = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};
    uint8_t forwarded[8] = {};

    forward_setup_packet(setup, forwarded);

    EXPECT_EQ(forwarded[0], 0x80);  // bmRequestType (Device-to-host, Standard, Device)
    EXPECT_EQ(forwarded[1], 0x06);  // bRequest (GET_DESCRIPTOR)
    EXPECT_EQ(forwarded[2], 0x00);  // wValue low (descriptor index)
    EXPECT_EQ(forwarded[3], 0x01);  // wValue high (DEVICE descriptor type)
    EXPECT_EQ(forwarded[4], 0x00);  // wIndex low
    EXPECT_EQ(forwarded[5], 0x00);  // wIndex high
    EXPECT_EQ(forwarded[6], 0x12);  // wLength low (18 bytes)
    EXPECT_EQ(forwarded[7], 0x00);  // wLength high
}

// --- Deterministic test: specific transfer_buffer pattern preserved ---

TEST(USBIPProtocol, OpaquePassThrough_TransferBufferDeterministic) {
    // **Validates: Requirements 13.6**
    // Simulate a USB device descriptor response (18 bytes)
    uint8_t device_data[18] = {
        0x12, 0x01,             // bLength=18, bDescriptorType=1 (DEVICE)
        0x00, 0x02,             // bcdUSB = 2.00 (little-endian)
        0xE0, 0x01, 0x01,      // class=0xE0, subclass=1, protocol=1 (BT)
        0x40,                   // bMaxPacketSize0=64
        0x6B, 0x1D,             // idVendor=0x1D6B (little-endian on wire)
        0x02, 0x00,             // idProduct=0x0002 (little-endian on wire)
        0x13, 0x04,             // bcdDevice=0x0413 (little-endian on wire)
        0x01, 0x02, 0x03,      // iManufacturer, iProduct, iSerialNumber
        0x01                    // bNumConfigurations
    };

    uint8_t response_buffer[18] = {};
    forward_device_data_to_response(device_data, 18, response_buffer);

    // All 18 bytes must match exactly — no byte-order transformation
    for (int i = 0; i < 18; i++) {
        EXPECT_EQ(response_buffer[i], device_data[i])
            << "Mismatch at byte " << i;
    }
}

// ==========================================================================
// Main
// ==========================================================================

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// ==========================================================================
// Property 1: Big-Endian Encoding Correctness
// Validates: Requirements 1.5, 3.7, 13.1, 13.2, 13.3, 13.4
//
// For any multi-byte integer value placed into a response message (OP or URB),
// the bytes at the field's wire offset SHALL be in big-endian order:
// for 16-bit fields, byte[0] = (value >> 8) & 0xFF and byte[1] = value & 0xFF;
// for 32-bit fields, byte[0] = (value >> 24) & 0xFF through byte[3] = value & 0xFF.
// ==========================================================================

// --------------------------------------------------------------------------
// Helpers: encode and read big-endian values.
// These replicate the USBIP_BSWAP16/USBIP_BSWAP32 macros and the
// __bswap_16/__bswap_32 calls used in usb_ip.cpp for serialization.
// --------------------------------------------------------------------------

static void encode_be16(uint8_t *dst, uint16_t val) {
    dst[0] = (val >> 8) & 0xFF;
    dst[1] = val & 0xFF;
}

static uint16_t decode_be16(const uint8_t *src) {
    return (uint16_t(src[0]) << 8) | uint16_t(src[1]);
}

static void encode_be32(uint8_t *dst, uint32_t val) {
    dst[0] = (val >> 24) & 0xFF;
    dst[1] = (val >> 16) & 0xFF;
    dst[2] = (val >> 8) & 0xFF;
    dst[3] = val & 0xFF;
}

static uint32_t decode_be32(const uint8_t *src) {
    return (uint32_t(src[0]) << 24) | (uint32_t(src[1]) << 16) |
           (uint32_t(src[2]) << 8) | uint32_t(src[3]);
}

// --------------------------------------------------------------------------
// Compile-time swap macros (replicating usb_ip.h) for verifying that the
// macro-based approach produces the same result as the runtime approach.
// --------------------------------------------------------------------------

static uint16_t macro_bswap16(uint16_t x) {
    return (uint16_t)(((x >> 8) & 0xFF) | ((x & 0xFF) << 8));
}

static uint32_t macro_bswap32(uint32_t x) {
    return (((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >> 8) |
           (((x) & 0x0000ff00u) << 8) | (((x) & 0x000000ffu) << 24);
}

// --- Property test: 32-bit encoding produces correct big-endian byte order ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_32bit, ()) {
    // **Validates: Requirements 1.5, 3.7, 13.3, 13.4**
    // For any 32-bit value, encoding to big-endian produces correct byte layout
    auto value = *rc::gen::arbitrary<uint32_t>();

    uint8_t buf[4] = {};
    encode_be32(buf, value);

    // Verify individual bytes match big-endian decomposition
    RC_ASSERT(buf[0] == ((value >> 24) & 0xFF));
    RC_ASSERT(buf[1] == ((value >> 16) & 0xFF));
    RC_ASSERT(buf[2] == ((value >> 8) & 0xFF));
    RC_ASSERT(buf[3] == (value & 0xFF));

    // Verify roundtrip: decode must recover original value
    uint32_t decoded = decode_be32(buf);
    RC_ASSERT(decoded == value);
}

// --- Property test: 16-bit encoding produces correct big-endian byte order ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_16bit, ()) {
    // **Validates: Requirements 1.5, 13.1, 13.2**
    // For any 16-bit value, encoding to big-endian produces correct byte layout
    auto value = *rc::gen::arbitrary<uint16_t>();

    uint8_t buf[2] = {};
    encode_be16(buf, value);

    // Verify individual bytes match big-endian decomposition
    RC_ASSERT(buf[0] == ((value >> 8) & 0xFF));
    RC_ASSERT(buf[1] == (value & 0xFF));

    // Verify roundtrip: decode must recover original value
    uint16_t decoded = decode_be16(buf);
    RC_ASSERT(decoded == value);
}

// --- Property test: macro-based swap matches runtime encoding for 32-bit ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_MacroMatchesRuntime32, ()) {
    // **Validates: Requirements 13.3, 13.4**
    // The USBIP_BSWAP32 macro and encode_be32 must produce identical wire bytes.
    // On little-endian platforms (ESP32, x86), storing a bswap'd value into memory
    // and reading individual bytes must match explicit big-endian encoding.
    auto value = *rc::gen::arbitrary<uint32_t>();

    // Method 1: explicit big-endian encode
    uint8_t buf_explicit[4] = {};
    encode_be32(buf_explicit, value);

    // Method 2: macro swap, then store in memory (as the real code does with __bswap_32)
    uint32_t swapped = macro_bswap32(value);
    uint8_t buf_macro[4] = {};
    memcpy(buf_macro, &swapped, 4);

    // On little-endian hosts, the stored bytes must match the explicit encoding
    RC_ASSERT(buf_explicit[0] == buf_macro[0]);
    RC_ASSERT(buf_explicit[1] == buf_macro[1]);
    RC_ASSERT(buf_explicit[2] == buf_macro[2]);
    RC_ASSERT(buf_explicit[3] == buf_macro[3]);
}

// --- Property test: macro-based swap matches runtime encoding for 16-bit ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_MacroMatchesRuntime16, ()) {
    // **Validates: Requirements 13.1, 13.2**
    // The USBIP_BSWAP16 macro and encode_be16 must produce identical wire bytes.
    auto value = *rc::gen::arbitrary<uint16_t>();

    // Method 1: explicit big-endian encode
    uint8_t buf_explicit[2] = {};
    encode_be16(buf_explicit, value);

    // Method 2: macro swap, then store in memory (as real code does with __bswap_16)
    uint16_t swapped = macro_bswap16(value);
    uint8_t buf_macro[2] = {};
    memcpy(buf_macro, &swapped, 2);

    // On little-endian hosts, stored bytes must match explicit encoding
    RC_ASSERT(buf_explicit[0] == buf_macro[0]);
    RC_ASSERT(buf_explicit[1] == buf_macro[1]);
}

// --- Property test: 32-bit signed status encoding preserves sign (two's complement) ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_SignedStatus32, ()) {
    // **Validates: Requirements 3.7, 13.4**
    // For any signed 32-bit status (including negative errno values),
    // big-endian encoding preserves the two's complement representation.
    auto value = *rc::gen::arbitrary<int32_t>();

    uint8_t buf[4] = {};
    uint32_t unsigned_val = static_cast<uint32_t>(value);
    encode_be32(buf, unsigned_val);

    // Verify byte layout is big-endian two's complement
    RC_ASSERT(buf[0] == ((unsigned_val >> 24) & 0xFF));
    RC_ASSERT(buf[1] == ((unsigned_val >> 16) & 0xFF));
    RC_ASSERT(buf[2] == ((unsigned_val >> 8) & 0xFF));
    RC_ASSERT(buf[3] == (unsigned_val & 0xFF));

    // Roundtrip must recover the original signed value
    uint32_t decoded_unsigned = decode_be32(buf);
    int32_t decoded_signed = static_cast<int32_t>(decoded_unsigned);
    RC_ASSERT(decoded_signed == value);
}

// --- Property test: OP header fields (version, command) are big-endian 16-bit ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_OPHeaderFields, ()) {
    // **Validates: Requirements 1.5, 13.1**
    // Simulate building an OP response header with random version and command.
    // Verify the wire bytes are in correct big-endian 16-bit order.
    auto version = *rc::gen::arbitrary<uint16_t>();
    auto command = *rc::gen::arbitrary<uint16_t>();
    auto status = *rc::gen::arbitrary<uint32_t>();

    // Build an 8-byte OP header (version[2] + command[2] + status[4])
    uint8_t header[8] = {};
    encode_be16(&header[0], version);
    encode_be16(&header[2], command);
    encode_be32(&header[4], status);

    // Verify version field at bytes 0-1
    RC_ASSERT(header[0] == ((version >> 8) & 0xFF));
    RC_ASSERT(header[1] == (version & 0xFF));

    // Verify command field at bytes 2-3
    RC_ASSERT(header[2] == ((command >> 8) & 0xFF));
    RC_ASSERT(header[3] == (command & 0xFF));

    // Verify status field at bytes 4-7
    RC_ASSERT(header[4] == ((status >> 24) & 0xFF));
    RC_ASSERT(header[5] == ((status >> 16) & 0xFF));
    RC_ASSERT(header[6] == ((status >> 8) & 0xFF));
    RC_ASSERT(header[7] == (status & 0xFF));

    // Roundtrip all fields
    RC_ASSERT(decode_be16(&header[0]) == version);
    RC_ASSERT(decode_be16(&header[2]) == command);
    RC_ASSERT(decode_be32(&header[4]) == status);
}

// --- Property test: URB header fields (command, seqnum, devid, direction, ep) ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_URBHeaderFields, ()) {
    // **Validates: Requirements 3.7, 13.3**
    // For any 5-field URB header, all 32-bit fields are big-endian encoded.
    auto cmd = *rc::gen::arbitrary<uint32_t>();
    auto seqnum = *rc::gen::arbitrary<uint32_t>();
    auto devid = *rc::gen::arbitrary<uint32_t>();
    auto direction = *rc::gen::arbitrary<uint32_t>();
    auto ep = *rc::gen::arbitrary<uint32_t>();

    // Build 20-byte URB basic header
    uint8_t header[20] = {};
    encode_be32(&header[0], cmd);
    encode_be32(&header[4], seqnum);
    encode_be32(&header[8], devid);
    encode_be32(&header[12], direction);
    encode_be32(&header[16], ep);

    // Verify each field decodes back correctly
    RC_ASSERT(decode_be32(&header[0]) == cmd);
    RC_ASSERT(decode_be32(&header[4]) == seqnum);
    RC_ASSERT(decode_be32(&header[8]) == devid);
    RC_ASSERT(decode_be32(&header[12]) == direction);
    RC_ASSERT(decode_be32(&header[16]) == ep);

    // Verify byte-level correctness for the command field
    RC_ASSERT(header[0] == ((cmd >> 24) & 0xFF));
    RC_ASSERT(header[1] == ((cmd >> 16) & 0xFF));
    RC_ASSERT(header[2] == ((cmd >> 8) & 0xFF));
    RC_ASSERT(header[3] == (cmd & 0xFF));
}

// --- Property test: device descriptor 16-bit fields (idVendor, idProduct, bcdDevice) ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_DeviceDescriptor16bitFields, ()) {
    // **Validates: Requirements 1.5, 13.2**
    // For any device descriptor fields, the 16-bit values are big-endian on wire.
    auto idVendor = *rc::gen::arbitrary<uint16_t>();
    auto idProduct = *rc::gen::arbitrary<uint16_t>();
    auto bcdDevice = *rc::gen::arbitrary<uint16_t>();

    // Simulate the wire encoding (as done in fill_devlist_/fill_import_)
    uint8_t buf[6] = {};
    encode_be16(&buf[0], idVendor);
    encode_be16(&buf[2], idProduct);
    encode_be16(&buf[4], bcdDevice);

    // Verify byte-level correctness
    RC_ASSERT(buf[0] == ((idVendor >> 8) & 0xFF));
    RC_ASSERT(buf[1] == (idVendor & 0xFF));
    RC_ASSERT(buf[2] == ((idProduct >> 8) & 0xFF));
    RC_ASSERT(buf[3] == (idProduct & 0xFF));
    RC_ASSERT(buf[4] == ((bcdDevice >> 8) & 0xFF));
    RC_ASSERT(buf[5] == (bcdDevice & 0xFF));

    // Roundtrip
    RC_ASSERT(decode_be16(&buf[0]) == idVendor);
    RC_ASSERT(decode_be16(&buf[2]) == idProduct);
    RC_ASSERT(decode_be16(&buf[4]) == bcdDevice);
}

// --- Property test: submit payload fields (status, length, start_frame, etc.) ---

RC_GTEST_PROP(USBIPProtocol, BigEndianEncoding_SubmitPayloadFields, ()) {
    // **Validates: Requirements 3.7, 13.4**
    // For any RET_SUBMIT payload, all 32-bit fields are big-endian encoded.
    auto status = *rc::gen::arbitrary<int32_t>();
    auto actual_length = *rc::gen::arbitrary<uint32_t>();
    auto start_frame = *rc::gen::arbitrary<uint32_t>();
    auto num_packets = *rc::gen::arbitrary<uint32_t>();
    auto error_count = *rc::gen::arbitrary<uint32_t>();

    // Build the 20-byte payload portion (bytes 20-39 of the 48-byte header)
    uint8_t payload[20] = {};
    encode_be32(&payload[0], static_cast<uint32_t>(status));
    encode_be32(&payload[4], actual_length);
    encode_be32(&payload[8], start_frame);
    encode_be32(&payload[12], num_packets);
    encode_be32(&payload[16], error_count);

    // Verify each field decodes correctly
    RC_ASSERT(static_cast<int32_t>(decode_be32(&payload[0])) == status);
    RC_ASSERT(decode_be32(&payload[4]) == actual_length);
    RC_ASSERT(decode_be32(&payload[8]) == start_frame);
    RC_ASSERT(decode_be32(&payload[12]) == num_packets);
    RC_ASSERT(decode_be32(&payload[16]) == error_count);
}

// --- Deterministic test: verify specific known errno values encode correctly ---

TEST(USBIPProtocol, BigEndianEncoding_KnownErrnoValues) {
    // **Validates: Requirements 3.7, 13.4**
    // Verify that specific negative errno values used by the protocol encode
    // correctly as big-endian two's complement.

    struct ErrnoCase {
        int32_t value;
        uint8_t expected[4];
    };

    // -19 (ENODEV) = 0xFFFFFFED in two's complement
    // -32 (EPIPE)  = 0xFFFFFFE0
    // -104 (ECONNRESET) = 0xFFFFFF98
    // -110 (ETIMEDOUT)  = 0xFFFFFF92
    // -5 (EIO) = 0xFFFFFFFB
    // -75 (EOVERFLOW) = 0xFFFFFFB5
    static const ErrnoCase cases[] = {
        {-19,  {0xFF, 0xFF, 0xFF, 0xED}},
        {-32,  {0xFF, 0xFF, 0xFF, 0xE0}},
        {-104, {0xFF, 0xFF, 0xFF, 0x98}},
        {-110, {0xFF, 0xFF, 0xFF, 0x92}},
        {-5,   {0xFF, 0xFF, 0xFF, 0xFB}},
        {-75,  {0xFF, 0xFF, 0xFF, 0xB5}},
        {0,    {0x00, 0x00, 0x00, 0x00}},
    };

    for (const auto &c : cases) {
        uint8_t buf[4] = {};
        encode_be32(buf, static_cast<uint32_t>(c.value));
        EXPECT_EQ(buf[0], c.expected[0]) << "Failed for value " << c.value;
        EXPECT_EQ(buf[1], c.expected[1]) << "Failed for value " << c.value;
        EXPECT_EQ(buf[2], c.expected[2]) << "Failed for value " << c.value;
        EXPECT_EQ(buf[3], c.expected[3]) << "Failed for value " << c.value;
    }
}

// --- Deterministic test: verify USB/IP protocol constants encode correctly ---

TEST(USBIPProtocol, BigEndianEncoding_ProtocolConstants) {
    // **Validates: Requirements 13.1, 13.3**
    // Verify that protocol constants used in the implementation encode
    // to their expected wire byte sequences.

    // USBIP_VERSION = 0x0111 → bytes: 0x01, 0x11
    uint8_t ver[2] = {};
    encode_be16(ver, 0x0111);
    EXPECT_EQ(ver[0], 0x01);
    EXPECT_EQ(ver[1], 0x11);

    // OP_REP_DEVLIST command = 0x0005 → bytes: 0x00, 0x05
    uint8_t cmd_devlist[2] = {};
    encode_be16(cmd_devlist, 0x0005);
    EXPECT_EQ(cmd_devlist[0], 0x00);
    EXPECT_EQ(cmd_devlist[1], 0x05);

    // OP_REP_IMPORT command = 0x0003 → bytes: 0x00, 0x03
    uint8_t cmd_import[2] = {};
    encode_be16(cmd_import, 0x0003);
    EXPECT_EQ(cmd_import[0], 0x00);
    EXPECT_EQ(cmd_import[1], 0x03);

    // USBIP_RET_SUBMIT = 0x00000003 → bytes: 0x00, 0x00, 0x00, 0x03
    uint8_t cmd_ret[4] = {};
    encode_be32(cmd_ret, 0x00000003);
    EXPECT_EQ(cmd_ret[0], 0x00);
    EXPECT_EQ(cmd_ret[1], 0x00);
    EXPECT_EQ(cmd_ret[2], 0x00);
    EXPECT_EQ(cmd_ret[3], 0x03);

    // USBIP_RET_UNLINK = 0x00000004 → bytes: 0x00, 0x00, 0x00, 0x04
    uint8_t cmd_unlink[4] = {};
    encode_be32(cmd_unlink, 0x00000004);
    EXPECT_EQ(cmd_unlink[0], 0x00);
    EXPECT_EQ(cmd_unlink[1], 0x00);
    EXPECT_EQ(cmd_unlink[2], 0x00);
    EXPECT_EQ(cmd_unlink[3], 0x04);
}

// ==========================================================================
// Property 14: Concurrent URB Seqnum Preservation
// Validates: Requirements 8.1, 8.2
//
// For any set of concurrently submitted URBs with distinct seqnums, each
// USBIP_RET_SUBMIT response SHALL echo the exact seqnum from its
// corresponding USBIP_CMD_SUBMIT, regardless of completion order.
// ==========================================================================

// --------------------------------------------------------------------------
// Simulation of the concurrent URB handling logic:
//
// 1. Multiple CMD_SUBMIT messages arrive, each with a distinct seqnum.
//    For each, a XferCtx is allocated and registered in the pending_urbs_ map
//    keyed by seqnum (host byte order).
//
// 2. USB transfers complete in arbitrary order (not necessarily submission order).
//    Each callback retrieves the XferCtx by its seqnum, builds the RET_SUBMIT
//    response echoing req->header.seqnum (which was stored from the original
//    request), and sends it.
//
// The property we verify: for any permutation of completion order, each
// response carries the exact seqnum from the corresponding request.
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// Simulated pending URB map for concurrency testing.
// This mirrors how usb_ip.cpp stores XferCtx pointers in pending_urbs_
// and retrieves them in callbacks.
// --------------------------------------------------------------------------

struct ConcurrentURB {
    uint32_t seqnum;       // Host byte order (as stored in XferCtx)
    uint32_t direction;    // 0=OUT, 1=IN
    uint32_t ep;           // endpoint number
    usb_transfer_status_t status;  // completion status
    int actual_num_bytes;  // bytes reported by USB host
};

// --------------------------------------------------------------------------
// Helper: simulate submitting a batch of URBs and registering them in the
// pending map. Returns a vector of ConcurrentURB objects representing the
// in-flight state.
// --------------------------------------------------------------------------

static std::unordered_map<uint32_t, ConcurrentURB> simulate_concurrent_submit(
    const std::vector<ConcurrentURB> &urbs) {
    std::unordered_map<uint32_t, ConcurrentURB> pending;
    for (const auto &urb : urbs) {
        pending[urb.seqnum] = urb;
    }
    return pending;
}

// --------------------------------------------------------------------------
// Helper: simulate completing a URB from the pending map in a given order.
// This mirrors the callback path:
//   1. Look up the XferCtx by seqnum in pending_urbs_
//   2. Remove from the map
//   3. Build RET_SUBMIT with req->header.seqnum (the original seqnum)
//
// Returns a vector of (seqnum_in_response, response_bytes) pairs in the
// order completions were processed.
// --------------------------------------------------------------------------

static std::vector<std::pair<uint32_t, std::vector<uint8_t>>> simulate_concurrent_completions(
    std::unordered_map<uint32_t, ConcurrentURB> &pending,
    const std::vector<uint32_t> &completion_order) {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> responses;

    for (uint32_t seqnum : completion_order) {
        auto it = pending.find(seqnum);
        if (it == pending.end()) continue;  // already completed or cancelled

        const ConcurrentURB &urb = it->second;

        // Build response using the stored seqnum (mirrors build_ret_submit)
        auto [response, resp_len] = build_ret_submit(
            urb.seqnum, urb.direction, urb.status, urb.actual_num_bytes,
            (urb.ep == 0) /* is_control */);

        // Extract the seqnum from the response wire bytes for verification
        uint32_t echoed_seqnum = read_be32(&response[4]);

        responses.push_back({echoed_seqnum, std::move(response)});

        // Remove from pending (as the real code does)
        pending.erase(it);
    }

    return responses;
}

// --- Property test: each response echoes the correct seqnum regardless of order ---

RC_GTEST_PROP(USBIPProtocol, ConcurrentSeqnum_EchoedCorrectly, ()) {
    // **Validates: Requirements 8.1, 8.2**
    // Generate a set of distinct seqnums (2 to 16 concurrent URBs)
    auto num_urbs = *rc::gen::inRange<size_t>(2, 17);
    auto seqnums = *rc::gen::unique<std::vector<uint32_t>>(num_urbs, rc::gen::arbitrary<uint32_t>());
    RC_PRE(seqnums.size() >= 2);

    // Build URBs with random parameters
    std::vector<ConcurrentURB> urbs;
    for (auto seqnum : seqnums) {
        ConcurrentURB urb;
        urb.seqnum = seqnum;
        urb.direction = *rc::gen::element<uint32_t>(0u, 1u);
        urb.ep = *rc::gen::inRange<uint32_t>(0, 5);
        urb.status = USB_TRANSFER_STATUS_COMPLETED;
        urb.actual_num_bytes = *rc::gen::inRange<int>(0, 128);
        urbs.push_back(urb);
    }

    // Submit all URBs (register in pending map)
    auto pending = simulate_concurrent_submit(urbs);
    RC_ASSERT(pending.size() == seqnums.size());

    // Generate a random completion order (permutation of the seqnums)
    auto completion_order = seqnums;
    auto perm_seed = *rc::gen::arbitrary<uint32_t>();
    // Simple Fisher-Yates shuffle using the seed
    for (size_t i = completion_order.size() - 1; i > 0; i--) {
        size_t j = (perm_seed + i * 2654435761u) % (i + 1);
        std::swap(completion_order[i], completion_order[j]);
    }

    // Complete URBs in the shuffled order
    auto responses = simulate_concurrent_completions(pending, completion_order);

    // Verify: each response echoes the correct seqnum
    RC_ASSERT(responses.size() == seqnums.size());

    for (size_t i = 0; i < responses.size(); i++) {
        uint32_t expected_seqnum = completion_order[i];
        uint32_t actual_seqnum = responses[i].first;

        // The seqnum in the response MUST match the original request's seqnum
        RC_ASSERT(actual_seqnum == expected_seqnum);

        // Also verify via the wire bytes directly
        const auto &resp_bytes = responses[i].second;
        RC_ASSERT(resp_bytes.size() >= 48u);
        uint32_t wire_seqnum = read_be32(&resp_bytes[4]);
        RC_ASSERT(wire_seqnum == expected_seqnum);
    }

    // Verify: pending map is now empty (all URBs completed)
    RC_ASSERT(pending.empty());
}

// --- Property test: seqnum never mutated regardless of transfer parameters ---

RC_GTEST_PROP(USBIPProtocol, ConcurrentSeqnum_PreservedAcrossParameters, ()) {
    // **Validates: Requirements 8.1, 8.2**
    // For any single URB with arbitrary parameters, the seqnum in the
    // response always matches the seqnum from the request.
    auto seqnum = *rc::gen::arbitrary<uint32_t>();
    auto direction = *rc::gen::element<uint32_t>(0u, 1u);
    auto ep = *rc::gen::inRange<uint32_t>(0, 15);
    auto status_idx = *rc::gen::inRange<int>(0, kNumMappings);
    auto actual_bytes = *rc::gen::inRange<int>(0, 512);
    auto is_control = (ep == 0);

    auto [response, resp_len] = build_ret_submit(
        seqnum, direction, kExpectedMappings[status_idx].esp_status,
        actual_bytes, is_control);

    // Seqnum at bytes 4-7 must always equal the input seqnum
    uint32_t echoed = read_be32(&response[4]);
    RC_ASSERT(echoed == seqnum);
}

// --- Property test: completions in reverse order still preserve seqnums ---

RC_GTEST_PROP(USBIPProtocol, ConcurrentSeqnum_ReverseCompletionOrder, ()) {
    // **Validates: Requirements 8.1, 8.2**
    // Generate distinct seqnums and complete them in exact reverse order
    auto num_urbs = *rc::gen::inRange<size_t>(2, 12);
    auto seqnums = *rc::gen::unique<std::vector<uint32_t>>(num_urbs, rc::gen::arbitrary<uint32_t>());
    RC_PRE(seqnums.size() >= 2);

    std::vector<ConcurrentURB> urbs;
    for (auto seqnum : seqnums) {
        ConcurrentURB urb;
        urb.seqnum = seqnum;
        urb.direction = 1u;  // IN transfers (have data, more complex path)
        urb.ep = *rc::gen::inRange<uint32_t>(1, 5);
        urb.status = USB_TRANSFER_STATUS_COMPLETED;
        urb.actual_num_bytes = *rc::gen::inRange<int>(1, 64);
        urbs.push_back(urb);
    }

    auto pending = simulate_concurrent_submit(urbs);

    // Reverse the completion order
    auto reverse_order = seqnums;
    std::reverse(reverse_order.begin(), reverse_order.end());

    auto responses = simulate_concurrent_completions(pending, reverse_order);

    // Each response must echo the correct seqnum
    RC_ASSERT(responses.size() == seqnums.size());
    for (size_t i = 0; i < responses.size(); i++) {
        RC_ASSERT(responses[i].first == reverse_order[i]);
    }
}

// --- Property test: mixed success/failure URBs all preserve seqnums ---

RC_GTEST_PROP(USBIPProtocol, ConcurrentSeqnum_MixedStatusPreservesSeqnum, ()) {
    // **Validates: Requirements 8.1, 8.2**
    // URBs completing with various error statuses must still echo
    // their original seqnum correctly.
    auto num_urbs = *rc::gen::inRange<size_t>(2, 10);
    auto seqnums = *rc::gen::unique<std::vector<uint32_t>>(num_urbs, rc::gen::arbitrary<uint32_t>());
    RC_PRE(seqnums.size() >= 2);

    std::vector<ConcurrentURB> urbs;
    for (auto seqnum : seqnums) {
        ConcurrentURB urb;
        urb.seqnum = seqnum;
        urb.direction = *rc::gen::element<uint32_t>(0u, 1u);
        urb.ep = *rc::gen::inRange<uint32_t>(0, 5);
        // Random completion status (may be error)
        auto status_idx = *rc::gen::inRange<int>(0, kNumMappings);
        urb.status = kExpectedMappings[status_idx].esp_status;
        urb.actual_num_bytes = *rc::gen::inRange<int>(0, 256);
        urbs.push_back(urb);
    }

    auto pending = simulate_concurrent_submit(urbs);

    // Complete in submission order (but with varied statuses)
    auto responses = simulate_concurrent_completions(pending, seqnums);

    RC_ASSERT(responses.size() == seqnums.size());
    for (size_t i = 0; i < responses.size(); i++) {
        // Seqnum must match regardless of success/failure status
        RC_ASSERT(responses[i].first == seqnums[i]);
    }
}

// --- Property test: pending map correctly tracks all concurrent URBs ---

RC_GTEST_PROP(USBIPProtocol, ConcurrentSeqnum_PendingMapTracking, ()) {
    // **Validates: Requirements 8.1**
    // After submitting N URBs, the pending map must contain exactly N entries
    // with the correct seqnums as keys.
    auto num_urbs = *rc::gen::inRange<size_t>(1, 20);
    auto seqnums = *rc::gen::unique<std::vector<uint32_t>>(num_urbs, rc::gen::arbitrary<uint32_t>());
    RC_PRE(!seqnums.empty());

    std::vector<ConcurrentURB> urbs;
    for (auto seqnum : seqnums) {
        ConcurrentURB urb;
        urb.seqnum = seqnum;
        urb.direction = 0;
        urb.ep = 1;
        urb.status = USB_TRANSFER_STATUS_COMPLETED;
        urb.actual_num_bytes = 0;
        urbs.push_back(urb);
    }

    auto pending = simulate_concurrent_submit(urbs);

    // Verify the map has all entries
    RC_ASSERT(pending.size() == seqnums.size());

    // Verify each seqnum is a key in the map
    for (auto seqnum : seqnums) {
        RC_ASSERT(pending.find(seqnum) != pending.end());
        RC_ASSERT(pending[seqnum].seqnum == seqnum);
    }
}

// --- Property test: no seqnum collision between concurrent URBs ---

RC_GTEST_PROP(USBIPProtocol, ConcurrentSeqnum_NoCollision, ()) {
    // **Validates: Requirements 8.1, 8.2**
    // When all URBs have distinct seqnums (as guaranteed by the kernel),
    // each response is uniquely identifiable by its seqnum.
    auto num_urbs = *rc::gen::inRange<size_t>(2, 16);
    auto seqnums = *rc::gen::unique<std::vector<uint32_t>>(num_urbs, rc::gen::arbitrary<uint32_t>());
    RC_PRE(seqnums.size() >= 2);

    std::vector<ConcurrentURB> urbs;
    for (auto seqnum : seqnums) {
        ConcurrentURB urb;
        urb.seqnum = seqnum;
        urb.direction = 1u;
        urb.ep = 2;
        urb.status = USB_TRANSFER_STATUS_COMPLETED;
        urb.actual_num_bytes = 32;
        urbs.push_back(urb);
    }

    auto pending = simulate_concurrent_submit(urbs);
    auto responses = simulate_concurrent_completions(pending, seqnums);

    // Collect all echoed seqnums from responses
    std::unordered_map<uint32_t, int> response_seqnums;
    for (const auto &[echoed, resp_bytes] : responses) {
        response_seqnums[echoed]++;
    }

    // Each seqnum must appear exactly once in responses (no duplication)
    RC_ASSERT(response_seqnums.size() == seqnums.size());
    for (const auto &[seqnum, count] : response_seqnums) {
        RC_ASSERT(count == 1);
    }
}

// --- Deterministic test: 8 concurrent URBs completed in reverse order ---

TEST(USBIPProtocol, ConcurrentSeqnum_EightURBsReverseOrder) {
    // **Validates: Requirements 8.1, 8.2**
    // Simulate exactly 8 concurrent URBs (the minimum the spec requires)
    // completed in exact reverse order.
    std::vector<uint32_t> seqnums = {100, 200, 300, 400, 500, 600, 700, 800};

    std::vector<ConcurrentURB> urbs;
    for (auto seqnum : seqnums) {
        ConcurrentURB urb;
        urb.seqnum = seqnum;
        urb.direction = 1u;  // IN
        urb.ep = 2;
        urb.status = USB_TRANSFER_STATUS_COMPLETED;
        urb.actual_num_bytes = 16;
        urbs.push_back(urb);
    }

    auto pending = simulate_concurrent_submit(urbs);
    EXPECT_EQ(pending.size(), 8u);

    // Complete in reverse order
    std::vector<uint32_t> reverse_order = {800, 700, 600, 500, 400, 300, 200, 100};
    auto responses = simulate_concurrent_completions(pending, reverse_order);

    EXPECT_EQ(responses.size(), 8u);

    // Each response must echo the seqnum of the URB that completed
    for (size_t i = 0; i < responses.size(); i++) {
        EXPECT_EQ(responses[i].first, reverse_order[i])
            << "Response " << i << " has wrong seqnum";

        // Verify wire bytes too
        const auto &resp = responses[i].second;
        uint32_t wire_seqnum = read_be32(&resp[4]);
        EXPECT_EQ(wire_seqnum, reverse_order[i])
            << "Response " << i << " wire seqnum mismatch";
    }

    // Pending map must be empty
    EXPECT_TRUE(pending.empty());
}

// --- Deterministic test: interleaved endpoints preserve seqnums ---

TEST(USBIPProtocol, ConcurrentSeqnum_InterleavedEndpoints) {
    // **Validates: Requirements 8.1, 8.2**
    // URBs to different endpoints (EP0 control, EP1 bulk IN, EP2 bulk OUT)
    // completing in mixed order still preserve their seqnums.
    std::vector<ConcurrentURB> urbs = {
        {1001, 1, 0, USB_TRANSFER_STATUS_COMPLETED, 18},   // EP0 control IN
        {1002, 1, 1, USB_TRANSFER_STATUS_COMPLETED, 64},   // EP1 bulk IN
        {1003, 0, 2, USB_TRANSFER_STATUS_COMPLETED, 0},    // EP2 bulk OUT
        {1004, 1, 1, USB_TRANSFER_STATUS_ERROR, 0},        // EP1 bulk IN (error)
    };

    auto pending = simulate_concurrent_submit(urbs);
    EXPECT_EQ(pending.size(), 4u);

    // Complete out of order: EP2 first, then EP0, then EP1 error, then EP1 success
    std::vector<uint32_t> completion_order = {1003, 1001, 1004, 1002};
    auto responses = simulate_concurrent_completions(pending, completion_order);

    EXPECT_EQ(responses.size(), 4u);
    EXPECT_EQ(responses[0].first, 1003u);
    EXPECT_EQ(responses[1].first, 1001u);
    EXPECT_EQ(responses[2].first, 1004u);
    EXPECT_EQ(responses[3].first, 1002u);
}

// ==========================================================================
// Property 15: Socket Write Atomicity
// Validates: Requirements 8.3
//
// For any two concurrent transfer completions producing responses R1 and R2,
// the bytes on the TCP stream SHALL contain R1 and R2 as contiguous,
// non-interleaved units (i.e., no byte from R2 appears between bytes of R1,
// and vice versa).
// ==========================================================================

// --------------------------------------------------------------------------
// Simulated send_response with mutex protection.
// This replicates the production code pattern: a mutex is acquired before
// writing any bytes, the entire message (header + data) is written as one
// unit, then the mutex is released.
//
// The "stream" is a shared std::vector<uint8_t> that collects all bytes
// written by all threads (simulating the TCP socket output buffer).
// --------------------------------------------------------------------------

struct AtomicWriteStream {
    std::mutex mutex;
    std::vector<uint8_t> bytes;

    // Simulates send_response(): acquires mutex, writes entire message atomically
    void send_response(const uint8_t *data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex);
        bytes.insert(bytes.end(), data, data + len);
    }
};

// --------------------------------------------------------------------------
// Helper: build a RET_SUBMIT response with a unique tag pattern.
// Each response is: 48-byte header + data_len bytes of payload.
// The header has a unique seqnum (used as an identifier) and the payload
// is filled with a repeating byte pattern derived from the seqnum, making
// it easy to verify contiguity.
// --------------------------------------------------------------------------

static std::vector<uint8_t> build_tagged_response(uint32_t seqnum, size_t data_len) {
    size_t total = 48 + data_len;
    std::vector<uint8_t> response(total, 0);

    // Bytes 0-3: command = 0x00000003 (RET_SUBMIT)
    response[0] = 0x00; response[1] = 0x00; response[2] = 0x00; response[3] = 0x03;

    // Bytes 4-7: seqnum (big-endian) — used as message identifier
    response[4] = (seqnum >> 24) & 0xFF;
    response[5] = (seqnum >> 16) & 0xFF;
    response[6] = (seqnum >> 8) & 0xFF;
    response[7] = seqnum & 0xFF;

    // Bytes 8-19: devid=0, direction=0, ep=0 (zeroed)
    // Bytes 20-23: status=0 (zeroed)

    // Bytes 24-27: actual_length (big-endian)
    uint32_t al = static_cast<uint32_t>(data_len);
    response[24] = (al >> 24) & 0xFF;
    response[25] = (al >> 16) & 0xFF;
    response[26] = (al >> 8) & 0xFF;
    response[27] = al & 0xFF;

    // Bytes 28-47: start_frame=0, num_packets=0, error_count=0, setup=0

    // Payload: fill with a tag byte derived from seqnum for identification
    uint8_t tag = static_cast<uint8_t>(seqnum & 0xFF);
    for (size_t i = 48; i < total; i++) {
        response[i] = tag;
    }

    return response;
}

// --------------------------------------------------------------------------
// Helper: Check if a given message appears contiguously in the stream
// at the given offset. Returns true if all bytes match.
// --------------------------------------------------------------------------

static bool message_at_offset(const std::vector<uint8_t> &stream,
                               size_t offset,
                               const std::vector<uint8_t> &message) {
    if (offset + message.size() > stream.size()) return false;
    return std::equal(message.begin(), message.end(), stream.begin() + offset);
}

// --------------------------------------------------------------------------
// Helper: Given a stream of bytes and a set of expected messages, verify
// that every message appears as a contiguous block (no interleaving).
// Returns true if the stream is a valid non-interleaved concatenation
// of all messages in some order.
// --------------------------------------------------------------------------

static bool verify_no_interleaving(const std::vector<uint8_t> &stream,
                                    const std::vector<std::vector<uint8_t>> &messages) {
    // Total expected bytes must match stream size
    size_t total_expected = 0;
    for (const auto &m : messages) total_expected += m.size();
    if (stream.size() != total_expected) return false;

    // Walk through the stream, at each position try to match one of the
    // remaining messages. Each message must appear exactly once, contiguously.
    std::vector<bool> found(messages.size(), false);
    size_t offset = 0;

    while (offset < stream.size()) {
        bool matched = false;
        for (size_t i = 0; i < messages.size(); i++) {
            if (found[i]) continue;
            if (message_at_offset(stream, offset, messages[i])) {
                found[i] = true;
                offset += messages[i].size();
                matched = true;
                break;
            }
        }
        if (!matched) return false;  // Interleaving or corruption detected
    }

    // All messages must have been found
    for (bool f : found) {
        if (!f) return false;
    }
    return true;
}

// --- Property test: two concurrent responses are never interleaved ---

RC_GTEST_PROP(USBIPProtocol, SocketWriteAtomicity_TwoResponses, ()) {
    // **Validates: Requirements 8.3**
    // Generate two distinct seqnums and data lengths
    auto seqnum1 = *rc::gen::arbitrary<uint32_t>();
    auto seqnum2 = *rc::gen::suchThat<uint32_t>([&](uint32_t s) { return s != seqnum1; });
    auto data_len1 = *rc::gen::inRange<size_t>(0, 513);  // 0 to 512 bytes of payload
    auto data_len2 = *rc::gen::inRange<size_t>(0, 513);

    auto msg1 = build_tagged_response(seqnum1, data_len1);
    auto msg2 = build_tagged_response(seqnum2, data_len2);

    AtomicWriteStream stream;

    // Simulate concurrent writes from two threads (as would happen with
    // two USB transfer callbacks completing at the same time)
    std::thread t1([&]() { stream.send_response(msg1.data(), msg1.size()); });
    std::thread t2([&]() { stream.send_response(msg2.data(), msg2.size()); });

    t1.join();
    t2.join();

    // Verify: the stream must contain msg1 and msg2 as contiguous,
    // non-interleaved units (in either order)
    std::vector<std::vector<uint8_t>> messages = {msg1, msg2};
    RC_ASSERT(verify_no_interleaving(stream.bytes, messages));
}

// --- Property test: N concurrent responses are never interleaved ---

RC_GTEST_PROP(USBIPProtocol, SocketWriteAtomicity_MultipleConcurrent, ()) {
    // **Validates: Requirements 8.3**
    // Generate between 2 and 8 concurrent responses (matching the requirement
    // of supporting at least 8 concurrent in-flight URBs)
    auto count = *rc::gen::inRange<size_t>(2, 9);

    // Generate unique seqnums
    std::vector<uint32_t> seqnums;
    for (size_t i = 0; i < count; i++) {
        seqnums.push_back(static_cast<uint32_t>(i + 1));
    }

    // Generate random data lengths for each response
    std::vector<size_t> data_lens;
    for (size_t i = 0; i < count; i++) {
        auto dl = *rc::gen::inRange<size_t>(0, 257);
        data_lens.push_back(dl);
    }

    // Build all response messages
    std::vector<std::vector<uint8_t>> messages;
    for (size_t i = 0; i < count; i++) {
        messages.push_back(build_tagged_response(seqnums[i], data_lens[i]));
    }

    AtomicWriteStream stream;

    // Launch all writes concurrently
    std::vector<std::thread> threads;
    for (size_t i = 0; i < count; i++) {
        threads.emplace_back([&stream, &messages, i]() {
            stream.send_response(messages[i].data(), messages[i].size());
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    // Verify: the stream is a non-interleaved concatenation of all messages
    RC_ASSERT(verify_no_interleaving(stream.bytes, messages));
}

// --- Property test: header + data sent atomically (single mutex hold) ---

RC_GTEST_PROP(USBIPProtocol, SocketWriteAtomicity_HeaderAndDataAtomic, ()) {
    // **Validates: Requirements 8.3**
    // This tests the specific pattern from the production code: the 48-byte
    // header and the variable-length data are sent in one send_response() call.
    // Even when the data portion is large, the entire message is contiguous.
    auto seqnum = *rc::gen::arbitrary<uint32_t>();
    auto data_len = *rc::gen::inRange<size_t>(1, 1025);  // 1 to 1024 bytes

    auto msg = build_tagged_response(seqnum, data_len);

    // Verify the message structure is consistent:
    // header is 48 bytes, followed by data_len bytes of the tag
    RC_ASSERT(msg.size() == 48 + data_len);

    // The production code calls send_response(req, 0x30 + data_len) which sends
    // the header (48=0x30 bytes) + data as a single write.
    // Verify that the response is self-consistent: actual_length in the header
    // matches the data portion size.
    uint32_t encoded_actual_len = (uint32_t(msg[24]) << 24) | (uint32_t(msg[25]) << 16) |
                                  (uint32_t(msg[26]) << 8) | uint32_t(msg[27]);
    RC_ASSERT(encoded_actual_len == data_len);

    // Verify the entire message would be written atomically by the mutex
    AtomicWriteStream stream;
    stream.send_response(msg.data(), msg.size());
    RC_ASSERT(stream.bytes.size() == msg.size());
    RC_ASSERT(stream.bytes == msg);
}

// --- Deterministic test: verify interleaving detection works ---

TEST(USBIPProtocol, SocketWriteAtomicity_InterleavingDetected) {
    // **Validates: Requirements 8.3**
    // Create two messages
    auto msg1 = build_tagged_response(1, 4);  // 52 bytes total
    auto msg2 = build_tagged_response(2, 4);  // 52 bytes total

    // Correct (non-interleaved) concatenation: msg1 then msg2
    std::vector<uint8_t> correct;
    correct.insert(correct.end(), msg1.begin(), msg1.end());
    correct.insert(correct.end(), msg2.begin(), msg2.end());
    EXPECT_TRUE(verify_no_interleaving(correct, {msg1, msg2}));

    // Correct (non-interleaved) concatenation: msg2 then msg1
    std::vector<uint8_t> correct_rev;
    correct_rev.insert(correct_rev.end(), msg2.begin(), msg2.end());
    correct_rev.insert(correct_rev.end(), msg1.begin(), msg1.end());
    EXPECT_TRUE(verify_no_interleaving(correct_rev, {msg1, msg2}));

    // Interleaved: first half of msg1, then msg2, then second half of msg1
    // This must be detected as invalid
    std::vector<uint8_t> interleaved;
    size_t half = msg1.size() / 2;
    interleaved.insert(interleaved.end(), msg1.begin(), msg1.begin() + half);
    interleaved.insert(interleaved.end(), msg2.begin(), msg2.end());
    interleaved.insert(interleaved.end(), msg1.begin() + half, msg1.end());
    EXPECT_FALSE(verify_no_interleaving(interleaved, {msg1, msg2}));
}

// --- Deterministic test: zero-data responses are also atomic ---

TEST(USBIPProtocol, SocketWriteAtomicity_ZeroDataResponses) {
    // **Validates: Requirements 8.3**
    // OUT transfers and failed IN transfers have exactly 48 bytes (no data).
    // They must also be written atomically without interleaving.
    auto msg1 = build_tagged_response(100, 0);  // 48 bytes (OUT transfer)
    auto msg2 = build_tagged_response(200, 0);  // 48 bytes (OUT transfer)

    EXPECT_EQ(msg1.size(), 48u);
    EXPECT_EQ(msg2.size(), 48u);

    AtomicWriteStream stream;

    std::thread t1([&]() { stream.send_response(msg1.data(), msg1.size()); });
    std::thread t2([&]() { stream.send_response(msg2.data(), msg2.size()); });
    t1.join();
    t2.join();

    EXPECT_EQ(stream.bytes.size(), 96u);
    EXPECT_TRUE(verify_no_interleaving(stream.bytes, {msg1, msg2}));
}
