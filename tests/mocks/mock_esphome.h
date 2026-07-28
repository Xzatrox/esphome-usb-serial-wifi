#pragma once
/**
 * Mock ESPHome core types for host-side testing.
 */

#include <cstdio>

namespace esphome {

namespace setup_priority {
    constexpr float AFTER_WIFI = -10.0f;
}

class Component {
public:
    virtual ~Component() = default;
    virtual void setup() {}
    virtual void loop() {}
    virtual float get_setup_priority() const { return 0.0f; }
    virtual void dump_config() {}
    void mark_failed() {}
};

}  // namespace esphome

// Mock ESP logging macros
#define ESP_LOGE(tag, fmt, ...) do {} while(0)
#define ESP_LOGW(tag, fmt, ...) do {} while(0)
#define ESP_LOGI(tag, fmt, ...) do {} while(0)
#define ESP_LOGD(tag, fmt, ...) do {} while(0)
#define ESP_LOGCONFIG(tag, fmt, ...) do {} while(0)
