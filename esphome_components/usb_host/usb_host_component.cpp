#if defined(USE_ESP32_VARIANT_ESP32P4) || defined(USE_ESP32_VARIANT_ESP32S2) || defined(USE_ESP32_VARIANT_ESP32S3) || \
    defined(USE_ESP32_VARIANT_ESP32S31) || defined(USE_ESP32_VARIANT_ESP32H4)
#include "usb_host.h"
#include <cinttypes>
#include "esphome/core/log.h"
#include <esp_private/usb_phy.h>
#include <driver/gpio.h>

namespace esphome::usb_host {

void USBHost::setup() {
  // Drive GPIO12 high to enable VBUS on XIAO ESP32-S3 USB-C host port
  gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_12, 1);
  ESP_LOGI("usb_host", "VBUS enabled on GPIO12");
  vTaskDelay(pdMS_TO_TICKS(100));  // allow hub to power up

  usb_phy_handle_t phy_handle;
  usb_phy_config_t phy_config = {
      .controller = USB_PHY_CTRL_OTG,
      .target = USB_PHY_TARGET_INT,
      .otg_mode = USB_OTG_MODE_HOST,
      .otg_speed = USB_PHY_SPEED_UNDEFINED,
  };
  esp_err_t phy_err = usb_new_phy(&phy_config, &phy_handle);
  if (phy_err != ESP_OK) {
    ESP_LOGE("usb_host", "usb_new_phy failed: %s", esp_err_to_name(phy_err));
  } else {
    ESP_LOGI("usb_host", "USB PHY initialized in host mode");
  }

  usb_host_config_t config{};
  config.skip_phy_setup = true;
  esp_err_t err = usb_host_install(&config);
  if (err != ESP_OK) {
    ESP_LOGE("usb_host", "usb_host_install failed: %s", esp_err_to_name(err));
    this->status_set_error(LOG_STR("usb_host_install failed"));
    this->mark_failed();
    return;
  }
}

void USBHost::loop() {
  int err;
  uint32_t event_flags;
  err = usb_host_lib_handle_events(0, &event_flags);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
    ESP_LOGD("usb_host", "lib_handle_events failed: %s", esp_err_to_name(err));
  }
  if (event_flags != 0) {
    ESP_LOGD("usb_host", "Event flags %" PRIu32 "X", event_flags);
  }
}

}  // namespace esphome::usb_host

#endif
