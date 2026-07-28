#pragma once

#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "esphome/core/component.h"
#include "usb/usb_host.h"
#include <string.h>
#include <vector>

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

 private:
  void on_device_connected(usb_device_handle_t dev_hdl, const usb_device_desc_t *dev_desc,
                           const usb_config_desc_t *config_desc, usb_device_info_t dev_info);
  void on_device_disconnected();
  void fill_devlist_();
  void fill_import_();
  esp_err_t req_ctrl_xfer_(usbip_submit_t *req);
  esp_err_t req_ep_xfer_(usbip_submit_t *req);
  void ensure_interfaces_claimed_();

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
};

}  // namespace esphome::usb_ip

#endif
