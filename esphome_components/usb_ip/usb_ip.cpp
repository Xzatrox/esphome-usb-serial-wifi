#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_ip.h"
#include "esphome/core/log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "byteswap.h"
#include <new>

namespace esphome::usb_ip {

// Byte-order audit (Task 11.1, Requirements 13.1–13.6):
// - All 32-bit URB/OP header fields use __bswap_32() or compile-time USBIP_BSWAP32 macros ✓
// - All 16-bit OP fields use __bswap_16() or compile-time USBIP_BSWAP16 macros ✓
// - Setup packet (8 bytes) and transfer_buffer are passed as opaque byte arrays (no swap) ✓
// - Negative errno values (e.g. -104, -19) are correctly encoded via __bswap_32() as
//   big-endian two's complement on this little-endian platform ✓
// - Fields set to 0 need no byte swap (0x00000000 is identical in all byte orders) ✓
// - Seqnum is received in network byte order and echoed as-is in responses ✓

static const char *TAG = "usb_ip";

size_t USBIPComponent::send_response(const void *data, size_t len) {
  if (!send_mutex_) return 0;
  xSemaphoreTake(send_mutex_, portMAX_DELAY);
  if (client_sock_ == -1) {
    xSemaphoreGive(send_mutex_);
    return 0;
  }
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t sent = 0;
  while (sent < len) {
    int n = send(client_sock_, p + sent, len - sent, 0);
    if (n <= 0) break;
    sent += n;
  }
  xSemaphoreGive(send_mutex_);
  return sent;
}

int32_t USBIPComponent::map_usb_status(usb_transfer_status_t status) {
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

void USBIPComponent::ctrl_cb_(usb_transfer_t *xfer) {
  auto *ctx = static_cast<XferCtx *>(xfer->context);
  USBIPComponent *self = ctx->self;
  usbip_submit_t *req = ctx->req;

  // Check if URB was cancelled by unlink
  xSemaphoreTake(self->pending_mutex_, portMAX_DELAY);
  bool cancelled = ctx->cancelled;
  if (!cancelled) {
    // Remove from pending map (it's completing normally)
    self->pending_urbs_.erase(ctx->seqnum);
  }
  xSemaphoreGive(self->pending_mutex_);

  if (cancelled) {
    // URB was unlinked — suppress response, just free resources
    ESP_LOGD(TAG, "ctrl_cb: seqnum=%lu cancelled, suppressing response", (unsigned long)ctx->seqnum);
    delete ctx;
    delete req;
    usb_host_transfer_free(xfer);
    return;
  }

  // Save original direction before zeroing header fields (non-zero = IN)
  uint32_t direction = req->header.direction;

  ESP_LOGD(TAG, "ctrl_cb: status=%d actual=%d dir=%u", xfer->status, xfer->actual_num_bytes, direction);

  req->header.command = USBIP_RET_SUBMIT;
  req->header.devid = req->header.direction = req->header.ep = 0;
  req->status = __bswap_32(map_usb_status(xfer->status));
  req->start_frame = req->num_packets = req->error_count = 0;
  req->setup = 0;  // Zero bytes 40-47 (setup/padding field)

  int data_len = 0;
  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && direction != 0) {
    // Control IN completed: subtract 8-byte setup packet from actual_num_bytes
    data_len = (xfer->actual_num_bytes > 8) ? (xfer->actual_num_bytes - 8) : 0;
    if (data_len > 0) {
      memcpy(req->transfer_buffer, xfer->data_buffer + 8, data_len);
    }
  }
  // For control OUT or failed transfers: data_len stays 0, no data copy

  req->length = __bswap_32(data_len);
  self->send_response(req, 0x30 + data_len);
  delete ctx;
  delete req;
  usb_host_transfer_free(xfer);
}

void USBIPComponent::ep_cb_(usb_transfer_t *xfer) {
  auto *ctx = static_cast<XferCtx *>(xfer->context);
  USBIPComponent *self = ctx->self;
  usbip_submit_t *req = ctx->req;

  // Check if URB was cancelled by unlink
  xSemaphoreTake(self->pending_mutex_, portMAX_DELAY);
  bool cancelled = ctx->cancelled;
  if (!cancelled) {
    // Remove from pending map (it's completing normally)
    self->pending_urbs_.erase(ctx->seqnum);
  }
  xSemaphoreGive(self->pending_mutex_);

  if (cancelled) {
    // URB was unlinked — suppress response, just free resources
    ESP_LOGD(TAG, "ep_cb: seqnum=%lu cancelled, suppressing response", (unsigned long)ctx->seqnum);
    delete ctx;
    delete req;
    usb_host_transfer_free(xfer);
    return;
  }

  // Save original direction before zeroing header fields
  uint32_t direction = req->header.direction;  // network byte order

  ESP_LOGD(TAG, "ep_cb: ep=0x%02X status=%d actual=%d", xfer->bEndpointAddress, xfer->status, xfer->actual_num_bytes);

  req->header.command = USBIP_RET_SUBMIT;
  req->header.devid = req->header.direction = req->header.ep = 0;
  req->status = __bswap_32(map_usb_status(xfer->status));
  req->start_frame = req->num_packets = req->error_count = 0;
  req->setup = 0;  // Zero bytes 40-47 (padding field)

  int data_len = 0;
  if (xfer->status == USB_TRANSFER_STATUS_COMPLETED && direction != 0) {
    // Bulk/interrupt IN completed successfully
    data_len = xfer->actual_num_bytes;
    if (data_len > 0) {
      memcpy(req->transfer_buffer, xfer->data_buffer, data_len);
    }
  }
  // For OUT transfers or failed transfers: data_len stays 0, no copy

  req->length = __bswap_32(data_len);
  self->send_response(req, 0x30 + data_len);
  delete ctx;
  delete req;
  usb_host_transfer_free(xfer);
}

void USBIPComponent::client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg) {
  auto *self = static_cast<USBIPComponent *>(arg);
  if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
    usb_device_handle_t dev_hdl;
    usb_host_device_open(self->client_hdl_, msg->new_dev.address, &dev_hdl);
    const usb_device_desc_t *dev_desc;
    usb_host_get_device_descriptor(dev_hdl, &dev_desc);
    if ((self->vid_ != 0 || self->pid_ != 0) &&
        (dev_desc->idVendor != self->vid_ || dev_desc->idProduct != self->pid_)) {
      usb_host_device_close(self->client_hdl_, dev_hdl);
      return;
    }
    const usb_config_desc_t *config_desc;
    usb_device_info_t dev_info;
    usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
    usb_host_device_info(dev_hdl, &dev_info);
    self->on_device_connected(dev_hdl, dev_desc, config_desc, dev_info);
  } else {
    self->on_device_disconnected();
  }
}

void USBIPComponent::on_device_connected(usb_device_handle_t dev_hdl, const usb_device_desc_t *dev_desc,
                                          const usb_config_desc_t *config_desc, usb_device_info_t dev_info) {
  dev_hdl_ = dev_hdl;
  dev_desc_ = dev_desc;
  config_desc_ = config_desc;
  dev_info_ = dev_info;
  memset(endpoints_, 0, sizeof(endpoints_));
  int offset = 0;
  for (int n = 0; n < config_desc->bNumInterfaces; n++) {
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc, n, 0, &offset);
    if (!intf) continue;
    for (int i = 0; i < intf->bNumEndpoints; i++) {
      int ep_offset = 0;
      const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf, i, config_desc->wTotalLength, &ep_offset);
      if (!ep) continue;
      uint8_t num = ep->bEndpointAddress & 0x0f;
      uint8_t dir = (ep->bEndpointAddress & 0x80) ? 1 : 0;
      if (num < 15) endpoints_[num][dir] = ep;
    }
  }
  fill_devlist_();
  fill_import_();
  interfaces_claimed_ = false;
  device_ready_ = true;
  ESP_LOGI(TAG, "Device ready: VID=%04X PID=%04X speed_enum=%d", dev_desc->idVendor, dev_desc->idProduct, (int)dev_info.speed);
}

void USBIPComponent::on_device_disconnected() {
  device_ready_ = false;

  // Send -ENODEV for any pending URBs if a client is connected
  if (client_sock_ != -1) {
    xSemaphoreTake(pending_mutex_, portMAX_DELAY);
    for (auto &[seqnum, ctx] : pending_urbs_) {
      // Build error response for this URB
      usbip_submit_t err_resp{};
      err_resp.header.command = USBIP_RET_SUBMIT;
      err_resp.header.seqnum = __bswap_32(seqnum);  // convert back to network byte order
      err_resp.header.devid = err_resp.header.direction = err_resp.header.ep = 0;
      err_resp.status = __bswap_32(-19);  // -ENODEV
      err_resp.length = 0;
      err_resp.start_frame = err_resp.num_packets = err_resp.error_count = 0;
      err_resp.setup = 0;
      send_response(&err_resp, 0x30);
      ctx->cancelled = true;  // so callback won't send again
    }
    pending_urbs_.clear();
    xSemaphoreGive(pending_mutex_);
  }

  if (dev_hdl_) {
    if (interfaces_claimed_) {
      for (int n = 0; n < (config_desc_ ? config_desc_->bNumInterfaces : 0); n++)
        usb_host_interface_release(client_hdl_, dev_hdl_, n);
    }
    usb_host_device_close(client_hdl_, dev_hdl_); dev_hdl_ = nullptr;
  }
  memset(&devlist_, 0, sizeof(devlist_));
  memset(&import_, 0, sizeof(import_));
}

void USBIPComponent::cleanup_connection_() {
  // 1. Mark socket as invalid (prevents callbacks from sending)
  int old_sock = client_sock_;
  client_sock_ = -1;

  // 2. Cancel all pending USB transfers
  xSemaphoreTake(pending_mutex_, portMAX_DELAY);
  for (auto &[seqnum, ctx] : pending_urbs_) {
    ctx->cancelled = true;
    // Note: USB transfers will complete with CANCELED status,
    // callbacks will see cancelled flag and just free resources
  }
  pending_urbs_.clear();
  xSemaphoreGive(pending_mutex_);

  // 3. Close socket
  if (old_sock != -1) {
    shutdown(old_sock, SHUT_RDWR);
    close(old_sock);
  }

  // 4. Reset interface state
  interfaces_claimed_ = false;
}

uint32_t USBIPComponent::map_device_speed(usb_speed_t speed) {
  switch (speed) {
    case USB_SPEED_LOW:  return 1;  // USB/IP LOW_SPEED
    case USB_SPEED_FULL: return 2;  // USB/IP FULL_SPEED
    case USB_SPEED_HIGH: return 3;  // USB/IP HIGH_SPEED
    default:
      ESP_LOGW(TAG, "Unrecognized speed %d, defaulting to Full Speed", (int)speed);
      return 2;
  }
}

void USBIPComponent::fill_devlist_() {
  memset(&devlist_, 0, sizeof(devlist_));
  devlist_.request = {USBIP_VERSION, OP_REP_DEVLIST, 0};
  devlist_.count = __bswap_32(1);
  strncpy(devlist_.path, "/esphome/usbip/usb1", sizeof(devlist_.path) - 1);
  strncpy(devlist_.busid, "1-1", sizeof(devlist_.busid) - 1);
  devlist_.busnum = __bswap_32(1); devlist_.devnum = __bswap_32(1);
  devlist_.speed = __bswap_32(map_device_speed(dev_info_.speed));
  devlist_.idVendor = __bswap_16(dev_desc_->idVendor);
  devlist_.idProduct = __bswap_16(dev_desc_->idProduct);
  devlist_.bcdDevice = __bswap_16(dev_desc_->bcdDevice);
  devlist_.bDeviceClass = dev_desc_->bDeviceClass;
  devlist_.bDeviceSubClass = dev_desc_->bDeviceSubClass;
  devlist_.bDeviceProtocol = dev_desc_->bDeviceProtocol;
  devlist_.bConfigurationValue = config_desc_->bConfigurationValue;
  devlist_.bNumConfigurations = dev_desc_->bNumConfigurations;
  devlist_.bNumInterfaces = config_desc_->bNumInterfaces;
  int offset = 0;
  for (int n = 0; n < config_desc_->bNumInterfaces && n < 10; n++) {
    const usb_intf_desc_t *intf = usb_parse_interface_descriptor(config_desc_, n, 0, &offset);
    if (intf) devlist_.intfs[n] = {intf->bInterfaceClass, intf->bInterfaceSubClass, intf->bInterfaceProtocol, 0};
  }
}

void USBIPComponent::fill_import_() {
  memset(&import_, 0, sizeof(import_));
  import_.request = {USBIP_VERSION, OP_REP_IMPORT, 0};
  strncpy(import_.path, "/esphome/usbip/usb1", sizeof(import_.path) - 1);
  strncpy(import_.busid, "1-1", sizeof(import_.busid) - 1);
  import_.busnum = __bswap_32(1); import_.devnum = __bswap_32(1);
  import_.speed = __bswap_32(map_device_speed(dev_info_.speed));
  import_.idVendor = __bswap_16(dev_desc_->idVendor);
  import_.idProduct = __bswap_16(dev_desc_->idProduct);
  import_.bcdDevice = __bswap_16(dev_desc_->bcdDevice);
  import_.bDeviceClass = dev_desc_->bDeviceClass;
  import_.bDeviceSubClass = dev_desc_->bDeviceSubClass;
  import_.bDeviceProtocol = dev_desc_->bDeviceProtocol;
  import_.bConfigurationValue = config_desc_->bConfigurationValue;
  import_.bNumConfigurations = dev_desc_->bNumConfigurations;
  import_.bNumInterfaces = config_desc_->bNumInterfaces;
}

esp_err_t USBIPComponent::req_ctrl_xfer_(usbip_submit_t *req, XferCtx *ctx) {
  usb_transfer_t *xfer = nullptr;
  usb_host_transfer_alloc(1032, 0, &xfer);
  if (!xfer) { ESP_LOGE(TAG, "ctrl alloc failed"); return ESP_ERR_NO_MEM; }
  xfer->device_handle = dev_hdl_;
  xfer->callback = ctrl_cb_;
  xfer->bEndpointAddress = (__bswap_32(req->header.direction) != 0) ? 0x80 : 0x00;
  int out_len = (__bswap_32(req->header.direction) == 0) ? __bswap_32(req->length) : 0;
  memcpy(xfer->data_buffer, &req->setup, 8);
  if (out_len > 0) memcpy(xfer->data_buffer + 8, req->transfer_buffer, out_len);
  // ESP-IDF USB Host requires num_bytes = 8 (setup) + wLength for control transfers.
  // For OUT: wLength == out_len (data we're sending).
  // For IN: wLength comes from the setup packet (bytes 6-7, little-endian).
  usb_setup_packet_t *setup = (usb_setup_packet_t *)xfer->data_buffer;
  xfer->num_bytes = 8 + setup->wLength;
  xfer->context = ctx;
  esp_err_t err = usb_host_transfer_submit_control(client_hdl_, xfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ctrl submit failed: 0x%x", err);
    usb_host_transfer_free(xfer);
  }
  return err;
}

bool USBIPComponent::ensure_interfaces_claimed_() {
  if (interfaces_claimed_) return true;
  if (!dev_hdl_ || !config_desc_) {
    ESP_LOGE(TAG, "Cannot claim interfaces: device handle or config descriptor not available");
    return false;
  }
  for (int n = 0; n < config_desc_->bNumInterfaces; n++) {
    esp_err_t err = usb_host_interface_claim(client_hdl_, dev_hdl_, n, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "usb_host_interface_claim(%d) failed: 0x%x", n, err);
      // Release any already-claimed interfaces
      for (int i = 0; i < n; i++) {
        usb_host_interface_release(client_hdl_, dev_hdl_, i);
      }
      return false;
    }
  }
  interfaces_claimed_ = true;
  return true;
}

esp_err_t USBIPComponent::req_ep_xfer_(usbip_submit_t *req, XferCtx *ctx) {
  uint8_t ep_num = __bswap_32(req->header.ep) & 0x0f;
  uint8_t dir = (__bswap_32(req->header.direction) != 0) ? 1 : 0;
  const usb_ep_desc_t *ep = (ep_num < 15) ? endpoints_[ep_num][dir] : nullptr;
  if (!ep) { ESP_LOGE(TAG, "Unknown EP%d dir=%d", ep_num, dir); return ESP_ERR_NOT_FOUND; }
  size_t req_len = __bswap_32(req->length);
  size_t alloc_len = dir ? usb_round_up_to_mps(req_len, ep->wMaxPacketSize) : req_len;
  if (alloc_len == 0) alloc_len = ep->wMaxPacketSize;
  usb_transfer_t *xfer = nullptr;
  usb_host_transfer_alloc(alloc_len, 0, &xfer);
  if (!xfer) { ESP_LOGE(TAG, "ep alloc failed"); return ESP_ERR_NO_MEM; }
  xfer->device_handle = dev_hdl_;
  xfer->callback = ep_cb_;
  xfer->bEndpointAddress = ep->bEndpointAddress;
  xfer->num_bytes = alloc_len;
  if (dir == 0) memcpy(xfer->data_buffer, req->transfer_buffer, req_len);
  xfer->context = ctx;
  esp_err_t err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ep submit failed ep=0x%02X: 0x%x", ep->bEndpointAddress, err);
    usb_host_transfer_free(xfer);
  }
  return err;
}

void USBIPComponent::parse_request(int sock, uint8_t *buf, size_t len) {
  client_sock_ = sock;
  auto *base = reinterpret_cast<usbip_request_t *>(buf);
  ESP_LOGD(TAG, "parse cmd=0x%04X len=%d", (unsigned)base->command, (int)len);
  if (base->command == OP_REQ_DEVLIST) {
    if (!device_ready_) {
      usbip_devlist_t empty{}; empty.request = {USBIP_VERSION, OP_REP_DEVLIST, 0}; empty.count = 0;
      send_response(&empty, 12);  // 8-byte header + 4-byte count(0), no device entries
    } else {
      // Send: 12 (header+count) + 312 (device struct) + N×4 (interface descriptors)
      size_t resp_len = sizeof(usbip_devlist_t) - sizeof(devlist_.intfs) + sizeof(usbip_interface_t) * config_desc_->bNumInterfaces;
      send_response(&devlist_, resp_len);
    }
    return;
  }
  if (base->command == OP_REQ_IMPORT) {
    char req_busid[32] = {};
    memcpy(req_busid, buf + 8, 31);  // Copy at most 31 bytes, ensuring null termination
    // Log raw first 16 bytes of request for debugging
    ESP_LOGD(TAG, "OP_REQ_IMPORT raw: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
      buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
      buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
    ESP_LOGD(TAG, "OP_REQ_IMPORT busid=%s", req_busid);
    // Validate busid matches and device is ready; busid compared as null-terminated string
    if (!device_ready_ || strcmp(req_busid, "1-1") != 0) {
      // Failed import: send only the 8-byte header with non-zero status
      usbip_request_t fail_hdr = {USBIP_VERSION, OP_REP_IMPORT, __bswap_32(1)};
      send_response(&fail_hdr, sizeof(fail_hdr));  // Exactly 8 bytes
      ESP_LOGD(TAG, "OP_REP_IMPORT failed: device_ready=%d busid_match=%d", device_ready_, strcmp(req_busid, "1-1") == 0);
    } else {
      // Success: echo the requested busid back in the response device structure
      memcpy(import_.busid, req_busid, sizeof(import_.busid));
      memcpy(devlist_.busid, req_busid, sizeof(devlist_.busid));
      ESP_LOGD(TAG, "Sending import: busid=%s busnum=%u devnum=%u speed=%u vid=%04X pid=%04X class=%u sub=%u proto=%u cfg=%u ncfg=%u nintf=%u",
        import_.busid, __bswap_32(import_.busnum), __bswap_32(import_.devnum), __bswap_32(import_.speed),
        __bswap_16(import_.idVendor), __bswap_16(import_.idProduct),
        import_.bDeviceClass, import_.bDeviceSubClass, import_.bDeviceProtocol,
        import_.bConfigurationValue, import_.bNumConfigurations, import_.bNumInterfaces);
      // Send exactly 320 bytes: 8-byte header + 312-byte device struct
      size_t sent = send_response(&import_, sizeof(import_));
      const uint8_t *ib = reinterpret_cast<const uint8_t *>(&import_);
      ESP_LOGD(TAG, "import[296..319]:%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
        ib[296],ib[297],ib[298],ib[299],ib[300],ib[301],ib[302],ib[303],
        ib[304],ib[305],ib[306],ib[307],ib[308],ib[309],ib[310],ib[311],
        ib[312],ib[313],ib[314],ib[315],ib[316],ib[317],ib[318],ib[319]);
      ESP_LOGD(TAG, "import sent %d/%d bytes", (int)sent, (int)sizeof(import_));
    }
    return;
  }
  auto *submit = reinterpret_cast<usbip_submit_t *>(buf);
  if (submit->header.command == USBIP_CMD_SUBMIT) {
    if (!device_ready_) {
      ESP_LOGW(TAG, "SUBMIT but device not ready, responding -ENODEV");
      usbip_submit_t err_resp{};
      err_resp.header.command = USBIP_RET_SUBMIT;
      err_resp.header.seqnum = submit->header.seqnum;  // echo original (already in NBO)
      err_resp.header.devid = err_resp.header.direction = err_resp.header.ep = 0;
      err_resp.status = __bswap_32(-19);  // -ENODEV
      err_resp.length = 0;
      err_resp.start_frame = err_resp.num_packets = err_resp.error_count = 0;
      err_resp.setup = 0;
      send_response(&err_resp, 0x30);
      return;
    }

    uint32_t transfer_buffer_length = __bswap_32(submit->length);
    uint32_t direction = __bswap_32(submit->header.direction);

    // Check OUT buffer overflow: reject if transfer_buffer_length > 1024
    if (direction == 0 && transfer_buffer_length > 1024) {
      ESP_LOGW(TAG, "SUBMIT OUT buffer too large: %lu", (unsigned long)transfer_buffer_length);
      usbip_submit_t err_resp{};
      err_resp.header.command = USBIP_RET_SUBMIT;
      err_resp.header.seqnum = submit->header.seqnum;  // echo original (already in NBO)
      err_resp.header.devid = err_resp.header.direction = err_resp.header.ep = 0;
      err_resp.status = __bswap_32(-12);  // -ENOMEM
      err_resp.length = 0;  // actual_length = 0
      err_resp.start_frame = err_resp.num_packets = err_resp.error_count = 0;
      err_resp.setup = 0;
      send_response(&err_resp, 0x30);
      return;
    }

    // Check for isochronous transfer (number_of_packets > 0)
    uint32_t num_packets = __bswap_32(submit->num_packets);
    if (num_packets > 0) {
      ESP_LOGW(TAG, "SUBMIT isochronous rejected: num_packets=%lu", (unsigned long)num_packets);
      // The full payload has already been consumed from the TCP stream by the reassembly layer
      // (transfer_buffer + iso_packet_descriptors were included in `needed` calculation)
      // We just need to send the rejection response.
      usbip_submit_t err_resp{};
      err_resp.header.command = USBIP_RET_SUBMIT;
      err_resp.header.seqnum = submit->header.seqnum;  // echo original seqnum (already NBO)
      err_resp.header.devid = err_resp.header.direction = err_resp.header.ep = 0;
      err_resp.status = __bswap_32(-38);  // -ENOSYS
      err_resp.length = 0;  // actual_length = 0
      err_resp.start_frame = 0;
      err_resp.num_packets = 0;  // number_of_packets = 0 in response
      err_resp.error_count = 0;
      err_resp.setup = 0;  // Zero bytes 40-47
      send_response(&err_resp, 0x30);
      return;
    }

    auto *req = new (std::nothrow) usbip_submit_t();
    if (!req) {
      ESP_LOGE(TAG, "Heap exhausted: cannot allocate usbip_submit_t");
      usbip_submit_t err_resp{};
      err_resp.header.command = USBIP_RET_SUBMIT;
      err_resp.header.seqnum = submit->header.seqnum;  // echo original (already in NBO)
      err_resp.header.devid = err_resp.header.direction = err_resp.header.ep = 0;
      err_resp.status = __bswap_32(-12);  // -ENOMEM
      err_resp.length = 0;
      err_resp.start_frame = err_resp.num_packets = err_resp.error_count = 0;
      err_resp.setup = 0;
      send_response(&err_resp, 0x30);
      return;
    }
    int out_len = (direction == 0) ? transfer_buffer_length : 0;
    memcpy(req, submit, 0x30 + out_len);

    uint32_t ep = __bswap_32(submit->header.ep);
    uint32_t dir = direction;
    uint32_t seqnum = __bswap_32(req->header.seqnum);
    ESP_LOGD(TAG, "SUBMIT ep=%lu dir=%lu out_len=%d seqnum=%lu", ep, dir, out_len, (unsigned long)seqnum);

    // Control IN: clamp wLength in setup packet to 1024
    if (ep == 0 && dir != 0) {
      uint8_t *setup = reinterpret_cast<uint8_t *>(&req->setup);
      uint16_t wLength = setup[6] | (setup[7] << 8);  // little-endian in setup packet
      if (wLength > 1024) {
        setup[6] = 0x00;  // 1024 & 0xFF = 0x00
        setup[7] = 0x04;  // 1024 >> 8 = 0x04
        ESP_LOGD(TAG, "Clamping control IN wLength %u -> 1024", wLength);
      }
    }

    // Create XferCtx and register in pending URB map
    auto *ctx = new (std::nothrow) XferCtx{req, client_sock_, this, seqnum, false};
    if (!ctx) {
      ESP_LOGE(TAG, "Heap exhausted: cannot allocate XferCtx");
      req->header.command = USBIP_RET_SUBMIT;
      req->header.devid = req->header.direction = req->header.ep = 0;
      req->status = __bswap_32(-12);  // -ENOMEM
      req->length = 0;
      req->start_frame = req->num_packets = req->error_count = 0;
      req->setup = 0;
      send_response(req, 0x30);
      delete req;
      return;
    }
    xSemaphoreTake(pending_mutex_, portMAX_DELAY);
    pending_urbs_[seqnum] = ctx;
    xSemaphoreGive(pending_mutex_);

    esp_err_t e;
    if (ep == 0) {
      e = req_ctrl_xfer_(req, ctx);
      ESP_LOGD(TAG, "ctrl_xfer ret=%d", e);
    } else {
      if (!ensure_interfaces_claimed_()) {
        // Send -ENODEV error response
        req->header.command = USBIP_RET_SUBMIT;
        req->header.devid = req->header.direction = req->header.ep = 0;
        req->status = __bswap_32(-19);  // -ENODEV
        req->length = 0;
        req->start_frame = req->num_packets = req->error_count = 0;
        req->setup = 0;
        send_response(req, 0x30);
        // Remove from pending map and free
        xSemaphoreTake(pending_mutex_, portMAX_DELAY);
        pending_urbs_.erase(seqnum);
        xSemaphoreGive(pending_mutex_);
        delete ctx;
        delete req;
        return;
      }
      e = req_ep_xfer_(req, ctx);
      ESP_LOGD(TAG, "ep_xfer ep=%lu ret=%d", ep, e);
    }

    if (e != ESP_OK) {
      // Remove from pending map
      xSemaphoreTake(pending_mutex_, portMAX_DELAY);
      pending_urbs_.erase(seqnum);
      xSemaphoreGive(pending_mutex_);
      // Send error response
      req->header.command = USBIP_RET_SUBMIT;
      req->header.devid = req->header.direction = req->header.ep = 0;
      req->status = __bswap_32(-5);  // -EIO
      req->length = 0;
      req->start_frame = req->num_packets = req->error_count = 0;
      req->setup = 0;
      send_response(req, 0x30);
      delete ctx;
      delete req;
    }
    return;
  }
  if (submit->header.command == USBIP_CMD_UNLINK) {
    auto *unlink = reinterpret_cast<usbip_unlink_t *>(buf);
    uint32_t target_seqnum = __bswap_32(unlink->unlink_seqnum);  // seqnum of URB to cancel (host byte order)

    int32_t status = 0;
    xSemaphoreTake(pending_mutex_, portMAX_DELAY);
    auto it = pending_urbs_.find(target_seqnum);
    if (it != pending_urbs_.end()) {
      // URB still pending — mark as cancelled and remove from map
      it->second->cancelled = true;
      pending_urbs_.erase(it);
      status = -104;  // -ECONNRESET
    }
    // If not found: URB already completed, respond with status=0
    xSemaphoreGive(pending_mutex_);

    ESP_LOGD(TAG, "UNLINK target_seq=%lu status=%d", (unsigned long)target_seqnum, status);

    usbip_unlink_t resp{};
    resp.header = {USBIP_RET_UNLINK, unlink->header.seqnum, 0, 0, 0};  // Echo unlink request's own seqnum
    resp.status = __bswap_32(status);  // big-endian
    memset(resp.padding, 0, sizeof(resp.padding));
    send_response(&resp, sizeof(resp));
    return;
  }
}

void USBIPComponent::tcp_task_(void *arg) {
  auto *self = static_cast<USBIPComponent *>(arg);
  uint8_t *rx_buf = self->rx_buf_;
  while (true) {
    struct sockaddr_in src{}; socklen_t src_len = sizeof(src);
    int sock = accept(self->listen_sock_, reinterpret_cast<sockaddr *>(&src), &src_len);
    if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    int nodelay = 1; setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    struct timeval tv{60, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char addr[32]; inet_ntoa_r(src.sin_addr, addr, sizeof(addr));
    ESP_LOGI(TAG, "Client connected: %s", addr);
    size_t buf_len = 0;
    bool protocol_error = false;
    while (true) {
      int n = recv(sock, rx_buf + buf_len, sizeof(self->rx_buf_) - buf_len, 0);
      if (n == 0) { ESP_LOGW(TAG, "recv FIN (n=0) buf_len=%d", (int)buf_len); break; }
      if (n < 0) { ESP_LOGW(TAG, "recv err/timeout: errno=%d buf_len=%d", errno, (int)buf_len); break; }
      buf_len += n;
      while (buf_len >= 8) {
        auto *base = reinterpret_cast<usbip_request_t *>(rx_buf);
        size_t needed;
        if (base->command == OP_REQ_DEVLIST) {
          needed = 8;
        } else if (base->command == OP_REQ_IMPORT) {
          needed = 40;
        } else {
          if (buf_len < 48) break;
          auto *sub = reinterpret_cast<usbip_submit_t *>(rx_buf);
          if (sub->header.command == USBIP_CMD_UNLINK) {
            needed = 48;
          } else if (sub->header.command == USBIP_CMD_SUBMIT) {
            uint32_t out_len = (__bswap_32(sub->header.direction) == 0) ? __bswap_32(sub->length) : 0;
            uint32_t num_packets = __bswap_32(sub->num_packets);
            uint32_t iso_len = (num_packets > 0) ? (num_packets * 16) : 0;
            needed = 48 + out_len + iso_len;
          } else {
            ESP_LOGW(TAG, "Unrecognized command: 0x%08lX, closing connection", (unsigned long)__bswap_32(sub->header.command));
            protocol_error = true;
            break;
          }
        }
        if (buf_len < needed) break;
        auto *_hdr = reinterpret_cast<usbip_submit_t *>(rx_buf);
        ESP_LOGD(TAG, "recv %d bytes cmd=%08lX seq=%lu ep=%lu dir=%lu", (int)needed,
          (unsigned long)_hdr->header.command, (unsigned long)__bswap_32(_hdr->header.seqnum),
          (unsigned long)__bswap_32(_hdr->header.ep), (unsigned long)__bswap_32(_hdr->header.direction));
        self->parse_request(sock, rx_buf, needed);
        buf_len -= needed;
        if (buf_len > 0) memmove(rx_buf, rx_buf + needed, buf_len);
      }
      if (protocol_error) break;  // break outer recv loop → closes connection
      // Buffer overflow protection: full buffer with no complete message
      if (buf_len >= sizeof(self->rx_buf_)) {
        ESP_LOGW(TAG, "rx_buf full without complete message, closing connection");
        break;
      }
    }
    self->cleanup_connection_();
    buf_len = 0;
    ESP_LOGI(TAG, "Client disconnected, waiting for next");
  }
}

void USBIPComponent::setup() {
  send_mutex_ = xSemaphoreCreateMutex();
  pending_mutex_ = xSemaphoreCreateMutex();

  usb_host_client_config_t cfg{};
  cfg.is_synchronous = false;
  cfg.max_num_event_msg = 10;
  cfg.async.client_event_callback = client_event_cb_;
  cfg.async.callback_arg = this;
  if (usb_host_client_register(&cfg, &client_hdl_) != ESP_OK) {
    ESP_LOGE(TAG, "usb_host_client_register failed");
    this->mark_failed(); return;
  }

  xTaskCreate([](void *arg) {
    auto *self = static_cast<USBIPComponent *>(arg);
    while (true) usb_host_client_handle_events(self->client_hdl_, portMAX_DELAY);
  }, "usbip_usb", 4096, this, 5, nullptr);

  listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (listen_sock_ < 0) {
    ESP_LOGE(TAG, "socket() failed: %d", errno);
    this->mark_failed(); return;
  }
  int opt = 1; setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port_);
  if (bind(listen_sock_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "bind() failed: %d", errno);
    this->mark_failed(); return;
  }
  listen(listen_sock_, 1);
  xTaskCreate(tcp_task_, "usbip_tcp", 8192, this, 5, nullptr);
  ESP_LOGI(TAG, "USB/IP server on port %u", port_);
}

void USBIPComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "USB/IP Server:");
  ESP_LOGCONFIG(TAG, "  Port: %u", port_);
  ESP_LOGCONFIG(TAG, "  VID: 0x%04X  PID: 0x%04X", vid_, pid_);
  ESP_LOGCONFIG(TAG, "  Device ready: %s", device_ready_ ? "YES" : "NO");
}

}  // namespace esphome::usb_ip

#endif
