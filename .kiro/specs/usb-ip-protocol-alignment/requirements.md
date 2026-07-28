# Requirements Document

## Introduction

This specification defines the protocol correctness and robustness requirements for the ESP32-S3 USB/IP server component (`USBIPComponent`). The server exposes a physical USB device (RTL8761BUV Bluetooth dongle) over WiFi to a Linux host running the `vhci-hcd` kernel module. The goal is to ensure full alignment with the Linux kernel's USB/IP protocol expectations as defined in `drivers/usb/usbip/`, handling edge cases, error conditions, and real-world operational scenarios (WiFi drops, device disconnects, concurrent URBs).

## Glossary

- **ESP_Server**: The ESPHome USB/IP component running on ESP32-S3, implementing a USB/IP protocol server over TCP
- **VHCI_Client**: The Linux kernel's vhci-hcd module acting as a USB/IP client, sending URBs and receiving responses
- **URB**: USB Request Block, the fundamental unit of USB communication forwarded over the USB/IP protocol
- **OP_Message**: Protocol-level control messages (OP_REQ_DEVLIST, OP_REP_DEVLIST, OP_REQ_IMPORT, OP_REP_IMPORT) using 16-bit command fields
- **URB_Message**: Data-plane messages (USBIP_CMD_SUBMIT, USBIP_RET_SUBMIT, USBIP_CMD_UNLINK, USBIP_RET_UNLINK) using 32-bit command fields
- **Seqnum**: A monotonically-increasing 32-bit sequence number assigned by the kernel to each URB, used to match responses to requests
- **Transfer_Buffer**: The payload data accompanying a URB, sent after the 48-byte header
- **XferCtx**: Internal ESP allocation holding per-URB state (request copy, socket fd, component pointer) used in asynchronous USB transfer callbacks
- **Setup_Packet**: An 8-byte USB control transfer descriptor located at offset 40 in USBIP_CMD_SUBMIT
- **Devid**: A 32-bit device identifier computed as (busnum << 16) | devnum
- **ECONNRESET**: Linux errno value (-104) that the kernel expects as the unlink status when a target URB was successfully cancelled

## Requirements

### Requirement 1: OP_REP_DEVLIST Response Correctness

**User Story:** As a Linux host running `usbip list`, I want to receive a correctly formatted device list response, so that I can discover available USB devices on the ESP server.

#### Acceptance Criteria

1. WHEN an OP_REQ_DEVLIST message is received, THE ESP_Server SHALL respond with an OP_REP_DEVLIST containing an 8-byte op_common header (version=0x0111, code=0x0005, status=0) followed by a 4-byte device count field
2. WHEN a USB device is attached, THE ESP_Server SHALL include a 312-byte usbip_usb_device structure per device containing: path[256] (null-terminated, zero-padded), busid[32] (null-terminated, zero-padded), busnum(4 bytes), devnum(4 bytes), speed(4 bytes), idVendor(2 bytes), idProduct(2 bytes), bcdDevice(2 bytes), bDeviceClass(1 byte), bDeviceSubClass(1 byte), bDeviceProtocol(1 byte), bConfigurationValue(1 byte), bNumConfigurations(1 byte), and bNumInterfaces(1 byte)
3. WHEN a USB device is attached, THE ESP_Server SHALL append bNumInterfaces × 4-byte interface descriptors (bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, padding=0x00) immediately after each 312-byte device entry
4. WHEN no USB device is attached, THE ESP_Server SHALL respond with the 8-byte op_common header (version=0x0111, code=0x0005, status=0) followed by a 4-byte device count of zero and no device entries (total response length: 12 bytes)
5. THE ESP_Server SHALL encode all multi-byte integer fields (busnum, devnum, speed, idVendor, idProduct, bcdDevice, device count) in network byte order (big-endian)
6. IF an OP_REQ_DEVLIST message is received with fewer than 8 bytes, THEN THE ESP_Server SHALL discard the message without sending a response

### Requirement 2: OP_REP_IMPORT Response Correctness

**User Story:** As a Linux host running `usbip attach`, I want to receive a correctly formatted import response, so that the kernel can initialize the vhci device and begin USB enumeration.

#### Acceptance Criteria

1. WHEN an OP_REQ_IMPORT message is received with a busid matching an available device (compared as a null-terminated string within a 32-byte field), THE ESP_Server SHALL respond with exactly 320 bytes: an 8-byte op_common header (version=0x0111, code=0x0003, status=0) followed by a 312-byte usbip_usb_device structure, with all multi-byte integer fields in network byte order (big-endian)
2. IF the requested busid does not match any available device or no device is currently attached, THEN THE ESP_Server SHALL respond with only the 8-byte op_common header containing version=0x0111, code=0x0003, and a non-zero status value (0x00000001 for not available), and SHALL NOT send any usbip_usb_device payload
3. THE ESP_Server SHALL populate the busid field of the usbip_usb_device structure as a null-terminated ASCII string within a 32-byte fixed-size array, echoing the busid value from the OP_REQ_IMPORT request, with remaining bytes after the null terminator set to zero
4. WHEN the physical device operates at High Speed (USB 2.0) or below (Full Speed, Low Speed), THE ESP_Server SHALL report speed=3 (USB_SPEED_HIGH) in the usbip_usb_device structure, causing the Linux kernel to assign the device to a High Speed vhci hub port
5. IF the physical device operates at Super Speed (USB 3.x), THEN THE ESP_Server SHALL report the actual device speed value (5 for USB_SPEED_SUPER or 6 for USB_SPEED_SUPER_PLUS) in the usbip_usb_device structure, causing the Linux kernel to assign the device to a Super Speed vhci hub port
6. THE ESP_Server SHALL populate the idVendor, idProduct, bDeviceClass, bDeviceSubClass, bDeviceProtocol, bcdDevice, bNumConfigurations, and bNumInterfaces fields of the usbip_usb_device structure with values obtained from the physical USB device's device descriptor

### Requirement 3: USBIP_RET_SUBMIT Response Correctness

**User Story:** As a Linux kernel vhci_rx thread, I want to receive correctly formatted URB responses, so that I can complete USB transfers and pass data to device drivers.

#### Acceptance Criteria

1. WHEN a USBIP_CMD_SUBMIT is processed successfully, THE ESP_Server SHALL respond with a 48-byte USBIP_RET_SUBMIT header with command=0x00000003, the original seqnum echoed back, devid=0, direction=0, ep=0, status=0, and actual_length set to the number of data bytes transferred, where actual_length SHALL NOT exceed the transfer_buffer_length from the original USBIP_CMD_SUBMIT request
2. WHEN a USBIP_CMD_SUBMIT is for an IN transfer and status=0, THE ESP_Server SHALL append exactly actual_length bytes of transfer data after the 48-byte header
3. WHEN a USBIP_CMD_SUBMIT is for an OUT transfer, THE ESP_Server SHALL send only the 48-byte header with no trailing transfer data
4. THE ESP_Server SHALL set start_frame=0, number_of_packets=0, and error_count=0 in USBIP_RET_SUBMIT responses
5. THE ESP_Server SHALL set the padding/setup field (bytes 40-47) to zero in USBIP_RET_SUBMIT responses
6. WHEN a USB transfer fails on the physical device, THE ESP_Server SHALL set the status field to the negative errno value corresponding to the USB transfer error mapped as follows: device not responding=-ENODEV (-19), transfer stall=-EPIPE (-32), transfer cancelled=-ECONNRESET (-104), overflow=-EOVERFLOW (-75), timeout=-ETIMEDOUT (-110), and other errors=-EIO (-5); the status SHALL be encoded in big-endian byte order as a signed 32-bit integer
7. THE ESP_Server SHALL encode all 32-bit header fields (command, seqnum, devid, direction, ep, status, actual_length, start_frame, number_of_packets, error_count) in big-endian byte order
8. WHEN a USB transfer fails (status != 0) and the transfer direction is IN, THE ESP_Server SHALL set actual_length=0 and SHALL NOT append any transfer data after the 48-byte header

### Requirement 4: USBIP_RET_UNLINK Response Correctness

**User Story:** As a Linux kernel vhci_rx thread processing an unlink result, I want to receive the correct unlink status, so that I can properly complete or discard the target URB.

#### Acceptance Criteria

1. WHEN a USBIP_CMD_UNLINK is received and the target URB (identified by unlink_seqnum) is still pending on the physical device, THE ESP_Server SHALL cancel the target transfer, suppress any subsequent USBIP_RET_SUBMIT for the cancelled URB, and respond with USBIP_RET_UNLINK containing status=-ECONNRESET (-104) in big-endian byte order
2. WHEN a USBIP_CMD_UNLINK is received and the USBIP_RET_SUBMIT for the target URB has already been sent to the client, THE ESP_Server SHALL respond with USBIP_RET_UNLINK containing status=0
3. THE ESP_Server SHALL respond with a 48-byte USBIP_RET_UNLINK message with command=0x00000004, the unlink request's own seqnum echoed back, devid=0, direction=0, ep=0, status at bytes 20-23, and bytes 24-47 set to zero; all 32-bit fields (command, seqnum, devid, direction, ep, status) SHALL be encoded in big-endian byte order
4. THE ESP_Server SHALL send the USBIP_RET_UNLINK response regardless of whether the target URB was found or not; IF the unlink_seqnum does not match any pending or recently-completed URB, THEN THE ESP_Server SHALL respond with USBIP_RET_UNLINK containing status=0

### Requirement 5: Control Transfer Data Length Accuracy

**User Story:** As a Linux kernel processing a control transfer response, I want the actual_length field to reflect only the data portion (excluding the 8-byte setup packet), so that buffer handling works correctly.

#### Acceptance Criteria

1. WHEN a control IN transfer completes successfully (USB_TRANSFER_STATUS_COMPLETED), THE ESP_Server SHALL set actual_length in USBIP_RET_SUBMIT to the number of data bytes returned by the device (xfer->actual_num_bytes minus 8), excluding the 8-byte setup packet
2. WHEN the ESP-IDF USB host reports actual_num_bytes that includes the 8-byte setup packet for a completed control transfer, THE ESP_Server SHALL subtract 8 from actual_num_bytes before setting actual_length in the response; IF the result of the subtraction is 0, THE ESP_Server SHALL set actual_length to 0 and SHALL NOT copy any data into the transfer_buffer
3. WHEN a control IN transfer returns data (actual_num_bytes greater than 8), THE ESP_Server SHALL copy only the data portion (starting at byte offset 8 in xfer->data_buffer, for a length of actual_num_bytes minus 8 bytes) into the transfer_buffer of the response
4. WHEN a control OUT transfer completes successfully, THE ESP_Server SHALL set actual_length to 0 in the USBIP_RET_SUBMIT response and SHALL NOT include any data payload after the 48-byte response header
5. IF a control transfer fails (xfer->status is not USB_TRANSFER_STATUS_COMPLETED), THEN THE ESP_Server SHALL set actual_length to 0 in the USBIP_RET_SUBMIT response, set the status field to a non-zero error value, and SHALL NOT copy any data into the transfer_buffer

### Requirement 6: Transfer Buffer Size Handling

**User Story:** As a system designer, I want the ESP server to handle transfer requests of any size the kernel might send, so that USB enumeration and normal device operation succeed without truncation.

#### Acceptance Criteria

1. WHEN a USBIP_CMD_SUBMIT for an OUT bulk or interrupt transfer specifies a transfer_buffer_length exceeding the ESP_Server's internal buffer capacity of 1024 bytes, THE ESP_Server SHALL respond with USBIP_RET_SUBMIT containing status=-ENOMEM (-12) in big-endian byte order, actual_length=0, and no trailing transfer data
2. THE ESP_Server SHALL support a transfer buffer of at least 1024 bytes to accommodate standard USB descriptors (up to 255 bytes), USB configuration descriptors (up to 512 bytes typical), and BT HCI data packets
3. WHEN a control IN transfer (including GET_DESCRIPTOR) specifies a wLength in the setup packet greater than the internal buffer capacity of 1024 bytes, THE ESP_Server SHALL reduce wLength to the buffer capacity in the setup packet forwarded to the physical device, and report the actual number of bytes received from the device in actual_length
4. WHEN an IN bulk or interrupt transfer completes with data, THE ESP_Server SHALL report the actual number of bytes received from the device in actual_length, not the requested transfer_buffer_length
5. WHEN the ESP_Server responds with status=-ENOMEM to an oversized request, THE ESP_Server SHALL still echo the original seqnum from the USBIP_CMD_SUBMIT in the USBIP_RET_SUBMIT response header

### Requirement 7: Isochronous Transfer Handling

**User Story:** As a protocol-compliant server, I want to properly reject isochronous transfer requests, so that the kernel receives a valid error response instead of undefined behavior.

#### Acceptance Criteria

1. WHEN a USBIP_CMD_SUBMIT is received with number_of_packets > 0 (indicating an isochronous transfer), THE ESP_Server SHALL read and discard the full request payload (transfer_buffer of transfer_buffer_length bytes and iso_packet_descriptor array of number_of_packets entries) from the TCP stream before sending the response, to maintain stream synchronization for subsequent commands
2. WHEN a USBIP_CMD_SUBMIT is received with number_of_packets > 0, THE ESP_Server SHALL respond with a complete USBIP_RET_SUBMIT header containing: command=0x00000003, seqnum matching the original request, devid=0, direction=0, ep=0, status=-38 (ENOSYS) in big-endian two's complement, actual_length=0, start_frame=0, number_of_packets=0, error_count=0, and no transfer_buffer or iso_packet_descriptor payload
3. THE ESP_Server SHALL NOT attempt to submit isochronous transfers to the physical USB host

### Requirement 8: Concurrent URB Handling

**User Story:** As a Linux kernel that queues multiple URBs simultaneously, I want each URB to be processed independently and responses sent in completion order, so that USB throughput is not artificially serialized.

#### Acceptance Criteria

1. WHEN multiple USBIP_CMD_SUBMIT messages arrive before prior transfers complete, THE ESP_Server SHALL allocate independent state (XferCtx and usbip_submit_t) for each URB, supporting at least 8 concurrent in-flight URBs across all endpoints
2. THE ESP_Server SHALL send USBIP_RET_SUBMIT responses in the order that physical USB transfers complete, not necessarily in the order requests were received, echoing the original seqnum from each request in the corresponding response
3. WHEN the send_response function is called concurrently from multiple USB transfer callbacks, THE ESP_Server SHALL serialize socket writes such that each complete USBIP_RET_SUBMIT message (48-byte header plus any transfer data) is sent as one uninterrupted unit, preventing interleaved or corrupted TCP data
4. WHEN concurrent transfers target different endpoints, THE ESP_Server SHALL submit them independently to the physical USB host without mutual blocking
5. WHEN multiple USBIP_CMD_SUBMIT messages target the same endpoint, THE ESP_Server SHALL submit each transfer to the physical USB host independently; if the USB host driver queues them internally, THE ESP_Server SHALL not reject or defer subsequent submissions to the same endpoint
6. IF the ESP_Server cannot allocate memory for a new XferCtx or usbip_submit_t (heap exhaustion), THEN THE ESP_Server SHALL respond with USBIP_RET_SUBMIT containing the original seqnum, status=-ENOMEM (-12) in big-endian byte order, and actual_length=0

### Requirement 9: Socket Error and Disconnection Handling

**User Story:** As an ESP server operating over WiFi, I want to cleanly handle TCP connection drops, so that resources are freed and the server can accept new connections.

#### Acceptance Criteria

1. WHEN the TCP connection drops while URBs are in flight, THE ESP_Server SHALL free all pending XferCtx allocations associated with the disconnected socket and cancel any outstanding USB transfer requests before returning to the listening state
2. WHEN a send_response call fails (returns <= 0), THE ESP_Server SHALL stop sending further responses on that socket, close the socket, free all XferCtx allocations associated with that connection, and return to accepting new connections within 1 second
3. WHEN the TCP connection is closed by the client, THE ESP_Server SHALL close the socket, free all pending XferCtx allocations, cancel any in-progress USB transfers, and return to accepting new connections within 1 second
4. WHEN recv returns 0 (TCP FIN), THE ESP_Server SHALL treat the connection as closed and break out of the receive loop
5. WHEN recv returns EAGAIN/EWOULDBLOCK after the SO_RCVTIMEO timeout (60 seconds), THE ESP_Server SHALL close the connection, free all pending XferCtx allocations, and return to accepting new connections
6. IF the USB device disconnects while a TCP client is connected, THEN THE ESP_Server SHALL send error responses (status=-ENODEV) for any pending URBs and allow the TCP connection to remain open until the client closes the connection or the SO_RCVTIMEO timeout (60 seconds) expires, whichever occurs first
7. WHILE the ESP_Server is performing connection cleanup (freeing resources and closing a socket), THE ESP_Server SHALL NOT accept a new client connection until all XferCtx allocations from the prior connection have been freed and the listening socket is ready

### Requirement 10: Interface Claiming Race Condition Prevention

**User Story:** As a system handling rapid USB enumeration, I want interface claiming to complete atomically before bulk/interrupt transfers are submitted, so that no transfer fails with ESP_ERR_INVALID_STATE.

#### Acceptance Criteria

1. WHEN the first non-EP0 URB arrives and interfaces are not yet claimed, THE ESP_Server SHALL claim all interfaces enumerated in the USB configuration descriptor (interface indices 0 through bNumInterfaces-1) before submitting the transfer to the physical USB host
2. WHEN multiple non-EP0 URBs arrive before interface claiming completes, THE ESP_Server SHALL ensure that only one interface claiming operation executes and subsequent URBs proceed only after the claiming operation has completed and the interfaces_claimed state is set
3. IF interface claiming fails for any interface, THEN THE ESP_Server SHALL respond to the triggering URB and any subsequently queued non-EP0 URBs with USBIP_RET_SUBMIT containing status=-ENODEV (-19) in big-endian byte order and actual_length=0
4. WHEN interfaces are already claimed, THE ESP_Server SHALL submit non-EP0 URBs directly to the physical USB host without re-executing the interface claiming sequence
5. IF a non-EP0 URB arrives and the device handle or configuration descriptor is not available, THEN THE ESP_Server SHALL respond with USBIP_RET_SUBMIT containing status=-ENODEV (-19) in big-endian byte order and actual_length=0 without attempting interface claiming

### Requirement 11: Speed Reporting Accuracy

**User Story:** As a Linux kernel allocating vhci ports based on device speed, I want the reported speed to match the physical device's actual speed, so that the correct hub type (HS vs SS) is used and no protocol errors occur.

#### Acceptance Criteria

1. WHEN the physical USB device enumerates at Full Speed (12 Mbps), THE ESP_Server SHALL report speed=2 (USB_SPEED_FULL) in both OP_REP_DEVLIST and OP_REP_IMPORT responses
2. WHEN the physical USB device enumerates at High Speed (480 Mbps), THE ESP_Server SHALL report speed=3 (USB_SPEED_HIGH) in both OP_REP_DEVLIST and OP_REP_IMPORT responses
3. WHEN the physical USB device enumerates at Low Speed (1.5 Mbps), THE ESP_Server SHALL report speed=1 (USB_SPEED_LOW) in both OP_REP_DEVLIST and OP_REP_IMPORT responses
4. THE ESP_Server SHALL determine the speed value dynamically from the connected physical device at enumeration time; WHEN a different physical device is connected (e.g., replacing a Full Speed device with a Low Speed device), THE ESP_Server SHALL report the new device's actual speed without requiring a firmware change or reboot
5. IF the physical device reports a speed that does not map to USB_SPEED_LOW (1), USB_SPEED_FULL (2), or USB_SPEED_HIGH (3), THEN THE ESP_Server SHALL default to reporting speed=2 (USB_SPEED_FULL) and log a warning containing the unrecognized speed value
6. THE ESP_Server SHALL report the same speed value for a given device in both OP_REP_DEVLIST and OP_REP_IMPORT responses within the same device session (between device enumeration and disconnection)

### Requirement 12: TCP Reassembly Correctness

**User Story:** As a TCP-based protocol server, I want to correctly reassemble protocol messages that may arrive in partial TCP segments, so that no messages are lost or misinterpreted.

#### Acceptance Criteria

1. WHEN a TCP segment contains less than the required number of bytes for the current message, THE ESP_Server SHALL buffer the partial data in the receive buffer and wait for additional segments before processing, up to a maximum buffer capacity of 4096 bytes
2. WHEN a TCP segment contains multiple complete messages, THE ESP_Server SHALL process each message sequentially in arrival order, shifting any remaining partial data to the start of the receive buffer after each message is consumed, before waiting for more data
3. WHEN computing the required bytes for USBIP_CMD_SUBMIT, THE ESP_Server SHALL calculate needed = 48 + transfer_buffer_length (when the direction field equals 0, indicating OUT direction) or needed = 48 (when the direction field is non-zero, indicating IN direction)
4. WHEN computing the required bytes for USBIP_CMD_UNLINK, THE ESP_Server SHALL require exactly 48 bytes regardless of the content of the unlink-specific fields
5. THE ESP_Server SHALL distinguish OP-layer messages from URB-layer messages by first reading 8 bytes and checking the 2-byte command field (at offset 2) against OP_REQ_DEVLIST (0x8005) and OP_REQ_IMPORT (0x8003); IF the command does not match an OP code, THEN THE ESP_Server SHALL read at least 48 bytes total and check the 4-byte command field (at offset 0) for URB commands USBIP_CMD_SUBMIT (0x00000001) and USBIP_CMD_UNLINK (0x00000002)
6. WHEN computing the required bytes for OP_REQ_DEVLIST, THE ESP_Server SHALL require exactly 8 bytes; WHEN computing the required bytes for OP_REQ_IMPORT, THE ESP_Server SHALL require exactly 40 bytes
7. IF the receive buffer reaches its 4096-byte capacity without containing a complete message, THEN THE ESP_Server SHALL close the client connection and reset the buffer to accept new connections

### Requirement 13: Wire Format Endianness

**User Story:** As a protocol implementation communicating with the Linux kernel, I want all fields to use the correct byte order, so that values are interpreted correctly on both sides.

#### Acceptance Criteria

1. THE ESP_Server SHALL encode the OP_Message header fields as big-endian using 16-bit swap for `version` (2 bytes) and `command` (2 bytes), and 32-bit swap for `status` (4 bytes)
2. THE ESP_Server SHALL encode the OP_REP_DEVLIST and OP_REP_IMPORT device descriptor fields as big-endian using 32-bit swap for `busnum`, `devnum`, and `speed`, and 16-bit swap for `idVendor`, `idProduct`, and `bcdDevice`; single-byte fields (`bDeviceClass`, `bDeviceSubClass`, `bDeviceProtocol`, `bConfigurationValue`, `bNumConfigurations`, `bNumInterfaces`) and string fields (`path`, `busid`) SHALL NOT be byte-swapped
3. THE ESP_Server SHALL encode all URB_Message header fields (`command`, `seqnum`, `devid`, `direction`, `ep`) as big-endian 32-bit integers, applying the same encoding for both CMD_SUBMIT/RET_SUBMIT and CMD_UNLINK/RET_UNLINK message types
4. THE ESP_Server SHALL encode the submit/return payload fields (`flags`/`status`, `transfer_buffer_length`/`actual_length`, `start_frame`, `number_of_packets`, `interval`/`error_count`) as big-endian 32-bit integers, and the unlink payload field (`unlink_seqnum`/`status`) as a big-endian 32-bit integer
5. THE ESP_Server SHALL NOT byte-swap the 8-byte `setup` packet field, as it is treated as an opaque byte array by the protocol
6. THE ESP_Server SHALL NOT byte-swap the `transfer_buffer` payload data, as it is treated as an opaque byte array by the protocol
