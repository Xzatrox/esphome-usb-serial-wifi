#if defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3)

#include "usb_ip.h"
#include "esphome/core/log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "byteswap.h"

namespace esphome::usb_ip {

static const char *TAG = "usb_ip";

struct XferCtx { usbip_submit_t *req; int sock; USBIPComponent *self; };

static size_t send_response(int sock, const void *data, size_t len) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  size_t sent = 0;
  while (sent < len) {
    int n = send(sock, p + sent, len - sent, 0);
    if (n <= 0) break;
    sent += n;
  }
  return sent;
}

void USBIPComponent::ctrl_cb_(usb_transfer_t *xfer) {
  auto *ctx = static_cast<XferCtx *>(xfer->context);
  usbip_submit_t *req = ctx->req;
  int sock = ctx->sock;
  int data_len = (req->header.direction != 0) ? xfer->actual_num_bytes : 0;
  ESP_LOGD(TAG, "ctrl_cb: status=%d actual=%d data_len=%d", xfer->status, xfer->actual_num_bytes, data_len);
  req->header.command = USBIP_RET_SUBMIT;
  req->header.devid = req->header.direction = req->header.ep = 0;
  req->status = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? 0 : __bswap_32(-ETIME);
  req->length = __bswap_32(data_len);
  req->start_frame = req->num_packets = req->interval = req->padding = 0;
  if (data_len > 0) memcpy(req->transfer_buffer, xfer->data_buffer, data_len);
  send_response(sock, req, 0x30 + data_len);
  delete ctx; delete req;
  usb_host_transfer_free(xfer);
}

void USBIPComponent::ep_cb_(usb_transfer_t *xfer) {
  auto *ctx = static_cast<XferCtx *>(xfer->context);
  usbip_submit_t *req = ctx->req;
  int sock = ctx->sock;
  int data_len = (req->header.direction != 0) ? xfer->actual_num_bytes : 0;
  ESP_LOGD(TAG, "ep_cb: ep=0x%02X status=%d actual=%d", xfer->bEndpointAddress, xfer->status, xfer->actual_num_bytes);
  req->header.command = USBIP_RET_SUBMIT;
  req->header.devid = req->header.direction = req->header.ep = 0;
  req->status = (xfer->status == USB_TRANSFER_STATUS_COMPLETED) ? 0 : __bswap_32(-ETIME);
  req->length = __bswap_32(data_len);
  req->start_frame = req->num_packets = req->interval = req->padding = 0;
  if (data_len > 0) memcpy(req->transfer_buffer, xfer->data_buffer, data_len);
  send_response(sock, req, 0x30 + data_len);
  delete ctx; delete req;
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

void USBIPComponent::fill_devlist_() {
  memset(&devlist_, 0, sizeof(devlist_));
  devlist_.request = {USBIP_VERSION, OP_REP_DEVLIST, 0};
  devlist_.count = __bswap_32(1);
  strncpy(devlist_.path, "/esphome/usbip/usb1", sizeof(devlist_.path) - 1);
  strncpy(devlist_.busid, "1-1", sizeof(devlist_.busid) - 1);
  devlist_.busnum = __bswap_32(1); devlist_.devnum = __bswap_32(1);
  devlist_.speed = __bswap_32(3);  // High Speed
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
  import_.speed = __bswap_32(3);  // High Speed
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

esp_err_t USBIPComponent::req_ctrl_xfer_(usbip_submit_t *req) {
  usb_transfer_t *xfer = nullptr;
  usb_host_transfer_alloc(1032, 0, &xfer);
  if (!xfer) { ESP_LOGE(TAG, "ctrl alloc failed"); return ESP_ERR_NO_MEM; }
  xfer->device_handle = dev_hdl_;
  xfer->callback = ctrl_cb_;
  xfer->bEndpointAddress = (__bswap_32(req->header.direction) != 0) ? 0x80 : 0x00;
  int out_len = (__bswap_32(req->header.direction) == 0) ? __bswap_32(req->length) : 0;
  memcpy(xfer->data_buffer, &req->setup, 8);
  if (out_len > 0) memcpy(xfer->data_buffer + 8, req->transfer_buffer, out_len);
  xfer->num_bytes = 8 + out_len;
  xfer->context = new XferCtx{req, client_sock_, this};
  esp_err_t err = usb_host_transfer_submit_control(client_hdl_, xfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ctrl submit failed: 0x%x", err);
    delete static_cast<XferCtx *>(xfer->context);
    usb_host_transfer_free(xfer);
  }
  return err;
}

void USBIPComponent::ensure_interfaces_claimed_() {
  if (interfaces_claimed_ || !dev_hdl_ || !config_desc_) return;
  for (int n = 0; n < config_desc_->bNumInterfaces; n++)
    usb_host_interface_claim(client_hdl_, dev_hdl_, n, 0);
  interfaces_claimed_ = true;
}

esp_err_t USBIPComponent::req_ep_xfer_(usbip_submit_t *req) {
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
  xfer->context = new XferCtx{req, client_sock_, this};
  esp_err_t err = usb_host_transfer_submit(xfer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ep submit failed ep=0x%02X: 0x%x", ep->bEndpointAddress, err);
    delete static_cast<XferCtx *>(xfer->context);
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
      send_response(sock, &empty, 12);
    } else {
      send_response(sock, &devlist_, sizeof(usbip_devlist_t) - sizeof(devlist_.intfs) + sizeof(usbip_interface_t) * config_desc_->bNumInterfaces);
    }
    return;
  }
  if (base->command == OP_REQ_IMPORT) {
    char req_busid[32] = {};
    memcpy(req_busid, buf + 8, 31);
    // Log raw first 16 bytes of request for debugging
    ESP_LOGD(TAG, "OP_REQ_IMPORT raw: %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
      buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
      buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
    ESP_LOGD(TAG, "OP_REQ_IMPORT busid=%s", req_busid);
    if (!device_ready_) {
      usbip_import_t fail{}; fail.request = {USBIP_VERSION, OP_REP_IMPORT, __bswap_32(1)};
      send_response(sock, &fail, sizeof(fail));
    } else {
      memcpy(import_.busid, req_busid, sizeof(import_.busid));
      memcpy(devlist_.busid, req_busid, sizeof(devlist_.busid));
      ESP_LOGD(TAG, "Sending import: busid=%s busnum=%u devnum=%u speed=%u vid=%04X pid=%04X class=%u sub=%u proto=%u cfg=%u ncfg=%u nintf=%u",
        import_.busid, __bswap_32(import_.busnum), __bswap_32(import_.devnum), __bswap_32(import_.speed),
        __bswap_16(import_.idVendor), __bswap_16(import_.idProduct),
        import_.bDeviceClass, import_.bDeviceSubClass, import_.bDeviceProtocol,
        import_.bConfigurationValue, import_.bNumConfigurations, import_.bNumInterfaces);
      size_t sent = send_response(sock, &import_, sizeof(import_));
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
    if (!device_ready_) { ESP_LOGW(TAG, "SUBMIT but device not ready"); return; }
    auto *req = new usbip_submit_t();
    int out_len = (__bswap_32(submit->header.direction) == 0) ? __bswap_32(submit->length) : 0;
    memcpy(req, submit, 0x30 + out_len);
    uint32_t ep = __bswap_32(submit->header.ep);
    uint32_t dir = __bswap_32(submit->header.direction);
    ESP_LOGD(TAG, "SUBMIT ep=%lu dir=%lu out_len=%d", ep, dir, out_len);
    if (ep == 0) {
      esp_err_t e = req_ctrl_xfer_(req);
      ESP_LOGD(TAG, "ctrl_xfer ret=%d", e);
    } else {
      ensure_interfaces_claimed_();
      esp_err_t e = req_ep_xfer_(req);
      ESP_LOGD(TAG, "ep_xfer ep=%lu ret=%d", ep, e);
    }
    return;
  }
  if (submit->header.command == USBIP_CMD_UNLINK) {
    auto *unlink = reinterpret_cast<usbip_unlink_t *>(buf);
    usbip_unlink_t resp{};
    resp.header = {USBIP_RET_UNLINK, unlink->header.seqnum, 0, 0, 0};
    resp.status = 0;
    send_response(sock, &resp, sizeof(resp));
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
    while (true) {
      int n = recv(sock, rx_buf + buf_len, sizeof(self->rx_buf_) - buf_len, 0);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
      if (n == 0) { ESP_LOGW(TAG, "recv FIN (n=0) buf_len=%d", (int)buf_len); break; }
      if (n < 0) { ESP_LOGW(TAG, "recv err: errno=%d buf_len=%d", errno, (int)buf_len); break; }
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
          } else {
            uint32_t out_len = (__bswap_32(sub->header.direction) == 0) ? __bswap_32(sub->length) : 0;
            needed = 48 + out_len;
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
    }
    shutdown(sock, SHUT_RDWR); close(sock);
    self->client_sock_ = -1;
    buf_len = 0;
    ESP_LOGI(TAG, "Client disconnected, waiting for next");
  }
}

void USBIPComponent::setup() {
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
