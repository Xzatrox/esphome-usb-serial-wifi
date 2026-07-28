#pragma once

#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/component.h"
#include "usb/usb_host.h"
#include <string.h>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace esphome::usb_ip {

#define USBIP_BSWAP16(x) ((__uint16_t)((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))
#define USBIP_BSWAP32(x) ((((x) & 0xff000000u) >> 24) | (((x) & 0x00ff0000u) >> 8) | (((x) & 0x0000ff00u) << 8) | (((x) & 0x000000ffu) << 24))

#define OP_REQ_DEVLIST   USBIP_BSWAP16(0x8005)
#define OP_REP_DEVLIST   USBIP_BSWAP16(0x0005)
#define OP_REQ_IMPORT    USBIP_BSWAP16(0x8003)
#define OP_REP_IMPORT    USBIP_BSWAP16(0x0003)
#define USBIP_CMD_SUBMIT USBIP_BSWAP32(0x00000001)
#define USBIP_RET_SUBMIT USBIP_BSWAP32(0x00000003)
#define USBIP_CMD_UNLINK USBIP_BSWAP32(0x00000002)
#define USBIP_RET_UNLINK USBIP_BSWAP32(0x00000004)
#define USBIP_VERSION    USBIP_BSWAP16(0x0111)
#define USB_HIGH_SPEED   USBIP_BSWAP32(3)
#define USB_FULL_SPEED   USBIP_BSWAP32(2)
#define USB_LOW_SPEED    USBIP_BSWAP32(1)

#pragma pack(push, 1)
struct usbip_request_t { uint16_t version; uint16_t command; uint32_t status; };
struct usbip_interface_t { uint8_t bInterfaceClass; uint8_t bInterfaceSubClass; uint8_t bInterfaceProtocol; uint8_t padding; };
struct usbip_devlist_t {
  usbip_request_t request; uint32_t count;
  char path[256]; char busid[32];
  uint32_t busnum, devnum, speed;
  uint16_t idVendor, idProduct, bcdDevice;
  uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bConfigurationValue, bNumConfigurations, bNumInterfaces;
  usbip_interface_t intfs[10];
};
struct usbip_import_t {
  usbip_request_t request;
  char path[256]; char busid[32];
  uint32_t busnum, devnum, speed;
  uint16_t idVendor, idProduct, bcdDevice;
  uint8_t bDeviceClass, bDeviceSubClass, bDeviceProtocol, bConfigurationValue, bNumConfigurations, bNumInterfaces;
};
struct usbip_header_basic_t { uint32_t command, seqnum, devid, direction, ep; };
struct usbip_submit_t {
  usbip_header_basic_t header;
  union { uint32_t flags; uint32_t status; };
  uint32_t length, start_frame, num_packets;
  union { uint32_t interval; uint32_t error_count; };
  union { uint64_t setup; uint64_t padding; };
  uint8_t transfer_buffer[1024];
};
struct usbip_unlink_t {
  usbip_header_basic_t header;
  union { int32_t unlink_seqnum; int32_t status; };
  uint8_t padding[24];
};
#pragma pack(pop)

// Wire format size assertions — ensure packing produces correct protocol sizes
static_assert(sizeof(usbip_request_t) == 8, "usbip_request_t must be 8 bytes (OP header)");
static_assert(sizeof(usbip_interface_t) == 4, "usbip_interface_t must be 4 bytes");
static_assert(sizeof(usbip_header_basic_t) == 20, "usbip_header_basic_t must be 20 bytes");
static_assert(sizeof(usbip_unlink_t) == 48, "usbip_unlink_t must be 48 bytes");

// OP_REP_IMPORT: 8 (header) + 312 (device struct) = 320 bytes
static_assert(sizeof(usbip_import_t) == 320, "usbip_import_t must be 320 bytes (8 header + 312 device)");

// Verify import device descriptor offsets match devlist (after 8-byte header)
static_assert(offsetof(usbip_import_t, path) == 8, "import path must start at offset 8");
static_assert(offsetof(usbip_import_t, busid) == 264, "import busid must start at offset 264");
static_assert(offsetof(usbip_import_t, busnum) == 296, "import busnum must start at offset 296");
static_assert(offsetof(usbip_import_t, idVendor) == 308, "import idVendor must start at offset 308");
static_assert(offsetof(usbip_import_t, bNumInterfaces) == 319, "import bNumInterfaces must be at offset 319");

// OP_REP_DEVLIST: 12 (header+count) + 312 (device struct) + 40 (10 interfaces) = 364 bytes
static_assert(sizeof(usbip_devlist_t) == 364, "usbip_devlist_t must be 364 bytes (12 header + 312 device + 40 intfs)");

// Verify the device descriptor portion offset: after header(8) + count(4) = 12 bytes,
// the 312-byte device struct spans from offset 12 to 323
static_assert(offsetof(usbip_devlist_t, path) == 12, "devlist path must start at offset 12");
static_assert(offsetof(usbip_devlist_t, busid) == 268, "devlist busid must start at offset 268");
static_assert(offsetof(usbip_devlist_t, busnum) == 300, "devlist busnum must start at offset 300");
static_assert(offsetof(usbip_devlist_t, devnum) == 304, "devlist devnum must start at offset 304");
static_assert(offsetof(usbip_devlist_t, speed) == 308, "devlist speed must start at offset 308");
static_assert(offsetof(usbip_devlist_t, idVendor) == 312, "devlist idVendor must start at offset 312");
static_assert(offsetof(usbip_devlist_t, idProduct) == 314, "devlist idProduct must start at offset 314");
static_assert(offsetof(usbip_devlist_t, bcdDevice) == 316, "devlist bcdDevice must start at offset 316");
static_assert(offsetof(usbip_devlist_t, bDeviceClass) == 318, "devlist bDeviceClass must start at offset 318");
static_assert(offsetof(usbip_devlist_t, bDeviceSubClass) == 319, "devlist bDeviceSubClass must start at offset 319");
static_assert(offsetof(usbip_devlist_t, bDeviceProtocol) == 320, "devlist bDeviceProtocol must start at offset 320");
static_assert(offsetof(usbip_devlist_t, bConfigurationValue) == 321, "devlist bConfigurationValue must start at offset 321");
static_assert(offsetof(usbip_devlist_t, bNumConfigurations) == 322, "devlist bNumConfigurations must start at offset 322");
static_assert(offsetof(usbip_devlist_t, bNumInterfaces) == 323, "devlist bNumInterfaces must start at offset 323");
static_assert(offsetof(usbip_devlist_t, intfs) == 324, "devlist intfs must start at offset 324 (12 + 312)");

// Forward declaration for XferCtx
class USBIPComponent;

// Per-URB tracking context (heap-allocated)
struct XferCtx {
  usbip_submit_t *req;
  int sock;
  USBIPComponent *self;
  uint32_t seqnum;
  bool cancelled;
};

class USBIPComponent : public esphome::Component {
 public:
  void set_port(uint16_t port) { port_ = port; }
  void set_vid(uint16_t vid) { vid_ = vid; }
  void set_pid(uint16_t pid) { pid_ = pid; }

  void setup() override;
  void loop() override {}
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void dump_config() override;

  void parse_request(int sock, uint8_t *buf, size_t len);
  size_t send_response(const void *data, size_t len);

 private:
  void on_device_connected(usb_device_handle_t dev_hdl, const usb_device_desc_t *dev_desc,
                           const usb_config_desc_t *config_desc, usb_device_info_t dev_info);
  void on_device_disconnected();
  void fill_devlist_();
  void fill_import_();
  esp_err_t req_ctrl_xfer_(usbip_submit_t *req, XferCtx *ctx);
  esp_err_t req_ep_xfer_(usbip_submit_t *req, XferCtx *ctx);
  bool ensure_interfaces_claimed_();

  static void tcp_task_(void *arg);
  static void client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg);
  static void ctrl_cb_(usb_transfer_t *xfer);
  static void ep_cb_(usb_transfer_t *xfer);

  uint16_t port_{3240}, vid_{0}, pid_{0};
  usb_host_client_handle_t client_hdl_{nullptr};
  usb_device_handle_t dev_hdl_{nullptr};
  bool device_ready_{false};
  bool interfaces_claimed_{false};
  const usb_ep_desc_t *endpoints_[15][2]{};
  const usb_config_desc_t *config_desc_{nullptr};
  const usb_device_desc_t *dev_desc_{nullptr};
  usb_device_info_t dev_info_{};
  usbip_devlist_t devlist_{};
  usbip_import_t import_{};
  int listen_sock_{-1};
  int client_sock_{-1};
  uint8_t rx_buf_[4096]{};

  SemaphoreHandle_t send_mutex_{nullptr};
  SemaphoreHandle_t pending_mutex_{nullptr};
  std::unordered_map<uint32_t, XferCtx*> pending_urbs_;

  void cleanup_connection_();
  static int32_t map_usb_status(usb_transfer_status_t status);
  static uint32_t map_device_speed(usb_speed_t speed);
};

}  // namespace esphome::usb_ip

#endif
