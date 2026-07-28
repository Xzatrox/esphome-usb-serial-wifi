#pragma once
/**
 * Mock ESP-IDF USB Host API types for host-side testing.
 * Only the types and enums needed by the protocol logic are defined here.
 */

#include <cstdint>

// USB speed enum (ESP-IDF values)
typedef enum {
    USB_SPEED_LOW = 0,
    USB_SPEED_FULL = 1,
    USB_SPEED_HIGH = 2,
} usb_speed_t;

// USB transfer status enum
typedef enum {
    USB_TRANSFER_STATUS_COMPLETED = 0,
    USB_TRANSFER_STATUS_ERROR,
    USB_TRANSFER_STATUS_TIMED_OUT,
    USB_TRANSFER_STATUS_CANCELED,
    USB_TRANSFER_STATUS_STALL,
    USB_TRANSFER_STATUS_OVERFLOW,
    USB_TRANSFER_STATUS_NO_DEVICE,
} usb_transfer_status_t;

// Minimal USB descriptor types
typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} usb_device_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} usb_config_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_intf_desc_t;

typedef struct {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} usb_ep_desc_t;

// Device info structure
typedef struct {
    usb_speed_t speed;
    uint8_t dev_addr;
} usb_device_info_t;

// Transfer structure
typedef struct usb_transfer_s {
    void *device_handle;
    uint8_t bEndpointAddress;
    usb_transfer_status_t status;
    int actual_num_bytes;
    int num_bytes;
    void (*callback)(struct usb_transfer_s *);
    void *context;
    uint8_t data_buffer[1032];
} usb_transfer_t;

// Handle types (opaque pointers)
typedef void *usb_device_handle_t;
typedef void *usb_host_client_handle_t;

// Host client event types
typedef enum {
    USB_HOST_CLIENT_EVENT_NEW_DEV = 0,
    USB_HOST_CLIENT_EVENT_DEV_GONE,
} usb_host_client_event_t;

typedef struct {
    usb_host_client_event_t event;
    union {
        struct { uint8_t address; } new_dev;
    };
} usb_host_client_event_msg_t;

typedef struct {
    bool is_synchronous;
    int max_num_event_msg;
    struct {
        void (*client_event_callback)(const usb_host_client_event_msg_t *, void *);
        void *callback_arg;
    } async;
} usb_host_client_config_t;

// Stub functions (not used in protocol logic tests)
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_STATE 0x103

static inline esp_err_t usb_host_transfer_alloc(size_t, size_t, usb_transfer_t **) { return ESP_OK; }
static inline void usb_host_transfer_free(usb_transfer_t *) {}
static inline esp_err_t usb_host_transfer_submit(usb_transfer_t *) { return ESP_OK; }
static inline esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t, usb_transfer_t *) { return ESP_OK; }
static inline esp_err_t usb_host_client_register(const usb_host_client_config_t *, usb_host_client_handle_t *) { return ESP_OK; }
static inline esp_err_t usb_host_device_open(usb_host_client_handle_t, uint8_t, usb_device_handle_t *) { return ESP_OK; }
static inline esp_err_t usb_host_device_close(usb_host_client_handle_t, usb_device_handle_t) { return ESP_OK; }
static inline esp_err_t usb_host_get_device_descriptor(usb_device_handle_t, const usb_device_desc_t **) { return ESP_OK; }
static inline esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t, const usb_config_desc_t **) { return ESP_OK; }
static inline esp_err_t usb_host_device_info(usb_device_handle_t, usb_device_info_t *) { return ESP_OK; }
static inline esp_err_t usb_host_interface_claim(usb_host_client_handle_t, usb_device_handle_t, uint8_t, uint8_t) { return ESP_OK; }
static inline esp_err_t usb_host_interface_release(usb_host_client_handle_t, usb_device_handle_t, uint8_t) { return ESP_OK; }
static inline void usb_host_client_handle_events(usb_host_client_handle_t, uint32_t) {}
static inline const usb_intf_desc_t *usb_parse_interface_descriptor(const usb_config_desc_t *, int, int, int *) { return nullptr; }
static inline const usb_ep_desc_t *usb_parse_endpoint_descriptor_by_index(const usb_intf_desc_t *, int, uint16_t, int *) { return nullptr; }
static inline size_t usb_round_up_to_mps(size_t, uint16_t mps) { return ((mps + mps - 1) / mps) * mps; }
