#include "usb_device_monitor.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"

static const char *TAG = "USB_MON";

static usb_host_client_handle_t s_client = NULL;
static TaskHandle_t s_task = NULL;

typedef struct {
    bool connected;
    usb_device_handle_t dev;
    usb_transfer_t *in_xfer;
    usb_transfer_t *out_xfer;
    uint8_t iface;
    uint8_t alt;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t in_mps;
    uint16_t vid;
    uint16_t pid;
    bool pdp_quirk;
    lvgl_port_gamepad_state_t state;
    uint8_t raw[64];
    size_t raw_len;
    uint8_t init_index;
    uint32_t packet_count;
    uint32_t last_logged_buttons;
    uint8_t last_logged_dpad;
    int16_t last_logged_lx;
    int16_t last_logged_ly;
    int16_t last_logged_rx;
    int16_t last_logged_ry;
    uint16_t last_logged_brake;
    uint16_t last_logged_throttle;
    uint8_t last_logged_raw[16];
} xinput_dev_t;

static xinput_dev_t s_xinput;

static const uint8_t s_xboxone_power_on[] = {0x05, 0x20, 0x00, 0x01, 0x00};
static const uint8_t s_xboxone_led_on[] = {0x0a, 0x20, 0x01, 0x03, 0x00, 0x01, 0x14};
static const uint8_t s_xboxone_auth_done[] = {0x06, 0x20, 0x02, 0x01, 0x00};

static const struct {
    const uint8_t *data;
    size_t len;
} s_xboxone_init[] = {
    {s_xboxone_power_on, sizeof(s_xboxone_power_on)},
    {s_xboxone_led_on, sizeof(s_xboxone_led_on)},
    {s_xboxone_auth_done, sizeof(s_xboxone_auth_done)},
};

static const char *usb_class_name(uint8_t cls)
{
    switch (cls) {
    case USB_CLASS_PER_INTERFACE:
        return "per-interface";
    case USB_CLASS_AUDIO:
        return "audio";
    case USB_CLASS_COMM:
        return "comm";
    case USB_CLASS_HID:
        return "hid";
    case USB_CLASS_MASS_STORAGE:
        return "mass-storage";
    case USB_CLASS_HUB:
        return "hub";
    case USB_CLASS_VIDEO:
        return "video";
    case USB_CLASS_WIRELESS_CONTROLLER:
        return "wireless";
    case USB_CLASS_MISC:
        return "misc";
    case USB_CLASS_APP_SPEC:
        return "app-specific";
    case USB_CLASS_VENDOR_SPEC:
        return "vendor";
    default:
        return "unknown";
    }
}

static int16_t le16s(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t le16u(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int16_t apply_axis_deadzone(int16_t value)
{
    const int16_t deadzone = 4096;
    return (value > -deadzone && value < deadzone) ? 0 : value;
}

static void parse_xinput_report(const uint8_t *data, size_t len)
{
    size_t copy_len = len;
    if (copy_len > sizeof(s_xinput.raw)) {
        copy_len = sizeof(s_xinput.raw);
    }
    memcpy(s_xinput.raw, data, copy_len);
    s_xinput.raw_len = copy_len;

    if (s_xinput.pdp_quirk) {
        lvgl_port_gamepad_state_t state = {0};
        state.connected = 1;
        s_xinput.state = state;

        size_t raw_log_len = copy_len < sizeof(s_xinput.last_logged_raw) ? copy_len : sizeof(s_xinput.last_logged_raw);
        if (raw_log_len > 0 &&
            (s_xinput.packet_count < 5 || (s_xinput.packet_count % 100) == 0 ||
             memcmp(s_xinput.last_logged_raw + 4, data + 4, raw_log_len > 4 ? raw_log_len - 4 : 0) != 0)) {
            ESP_LOGI(TAG,
                     "PDP raw len=%u first=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     (unsigned)len,
                     raw_log_len > 0 ? data[0] : 0,
                     raw_log_len > 1 ? data[1] : 0,
                     raw_log_len > 2 ? data[2] : 0,
                     raw_log_len > 3 ? data[3] : 0,
                     raw_log_len > 4 ? data[4] : 0,
                     raw_log_len > 5 ? data[5] : 0,
                     raw_log_len > 6 ? data[6] : 0,
                     raw_log_len > 7 ? data[7] : 0,
                     raw_log_len > 8 ? data[8] : 0,
                     raw_log_len > 9 ? data[9] : 0,
                     raw_log_len > 10 ? data[10] : 0,
                     raw_log_len > 11 ? data[11] : 0,
                     raw_log_len > 12 ? data[12] : 0,
                     raw_log_len > 13 ? data[13] : 0,
                     raw_log_len > 14 ? data[14] : 0,
                     raw_log_len > 15 ? data[15] : 0);
            memcpy(s_xinput.last_logged_raw, data, raw_log_len);
        }
        return;
    }

    size_t base = 0;
    if (len >= 18 && data[0] == 0x20) {
        base = 0;
    } else if (len >= 19 && data[1] == 0x20) {
        base = 1;
    } else {
        return;
    }

    lvgl_port_gamepad_state_t state = {0};
    state.connected = 1;

    const uint16_t buttons = le16u(data + base + 4);

    if (buttons & 0x0001) state.dpad |= LVGL_PORT_GAMEPAD_DPAD_UP;
    if (buttons & 0x0002) state.dpad |= LVGL_PORT_GAMEPAD_DPAD_DOWN;
    if (buttons & 0x0004) state.dpad |= LVGL_PORT_GAMEPAD_DPAD_LEFT;
    if (buttons & 0x0008) state.dpad |= LVGL_PORT_GAMEPAD_DPAD_RIGHT;
    if (buttons & 0x0010) state.buttons |= LVGL_PORT_GAMEPAD_BTN_L3;
    if (buttons & 0x0020) state.buttons |= LVGL_PORT_GAMEPAD_BTN_SELECT;
    if (buttons & 0x0040) state.buttons |= LVGL_PORT_GAMEPAD_BTN_START;
    if (buttons & 0x0080) state.buttons |= LVGL_PORT_GAMEPAD_BTN_R3;
    if (buttons & 0x0100) state.buttons |= LVGL_PORT_GAMEPAD_BTN_L1;
    if (buttons & 0x0200) state.buttons |= LVGL_PORT_GAMEPAD_BTN_R1;
    if (buttons & 0x0400) state.buttons |= LVGL_PORT_GAMEPAD_BTN_HOME;
    if (buttons & 0x1000) state.buttons |= LVGL_PORT_GAMEPAD_BTN_A;
    if (buttons & 0x2000) state.buttons |= LVGL_PORT_GAMEPAD_BTN_B;
    if (buttons & 0x4000) state.buttons |= LVGL_PORT_GAMEPAD_BTN_X;
    if (buttons & 0x8000) state.buttons |= LVGL_PORT_GAMEPAD_BTN_Y;

    state.brake = le16u(data + base + 6);
    state.throttle = le16u(data + base + 8);
    if (state.brake > 0) state.buttons |= LVGL_PORT_GAMEPAD_BTN_L2;
    if (state.throttle > 0) state.buttons |= LVGL_PORT_GAMEPAD_BTN_R2;

    state.axis_lx = apply_axis_deadzone(le16s(data + base + 10));
    state.axis_ly = apply_axis_deadzone((int16_t)~le16s(data + base + 12));
    state.axis_rx = apply_axis_deadzone(le16s(data + base + 14));
    state.axis_ry = apply_axis_deadzone((int16_t)~le16s(data + base + 16));

    s_xinput.state = state;

    if (state.buttons != s_xinput.last_logged_buttons ||
        state.dpad != s_xinput.last_logged_dpad ||
        state.axis_lx != s_xinput.last_logged_lx ||
        state.axis_ly != s_xinput.last_logged_ly ||
        state.axis_rx != s_xinput.last_logged_rx ||
        state.axis_ry != s_xinput.last_logged_ry ||
        state.brake != s_xinput.last_logged_brake ||
        state.throttle != s_xinput.last_logged_throttle) {
        ESP_LOGI(TAG,
                 "XInput state buttons=0x%08lx dpad=0x%02x lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d",
                 (unsigned long)state.buttons,
                 state.dpad,
                 state.brake,
                 state.throttle,
                 state.axis_lx,
                 state.axis_ly,
                 state.axis_rx,
                 state.axis_ry);
        s_xinput.last_logged_buttons = state.buttons;
        s_xinput.last_logged_dpad = state.dpad;
        s_xinput.last_logged_lx = state.axis_lx;
        s_xinput.last_logged_ly = state.axis_ly;
        s_xinput.last_logged_rx = state.axis_rx;
        s_xinput.last_logged_ry = state.axis_ry;
        s_xinput.last_logged_brake = state.brake;
        s_xinput.last_logged_throttle = state.throttle;
    }
}

static void submit_xinput_in(void);

static void xinput_in_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
        parse_xinput_report(transfer->data_buffer, transfer->actual_num_bytes);
        s_xinput.packet_count++;
        if (s_xinput.packet_count <= 5 || (s_xinput.packet_count % 200) == 0) {
            ESP_LOGI(TAG, "XInput packet #%lu len=%d first=%02x %02x %02x %02x %02x %02x %02x %02x",
                     (unsigned long)s_xinput.packet_count,
                     transfer->actual_num_bytes,
                     transfer->data_buffer[0], transfer->data_buffer[1],
                     transfer->data_buffer[2], transfer->data_buffer[3],
                     transfer->data_buffer[4], transfer->data_buffer[5],
                     transfer->data_buffer[6], transfer->data_buffer[7]);
        }
        submit_xinput_in();
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGI(TAG, "XInput device removed");
        return;
    } else {
        ESP_LOGW(TAG, "XInput IN transfer status=%d", transfer->status);
        return;
    }
}

static void submit_xinput_in(void)
{
    if (!s_xinput.connected || !s_xinput.in_xfer || !s_xinput.dev || s_xinput.ep_in == 0) {
        return;
    }

    s_xinput.in_xfer->device_handle = s_xinput.dev;
    s_xinput.in_xfer->bEndpointAddress = s_xinput.ep_in;
    s_xinput.in_xfer->num_bytes = s_xinput.in_xfer->data_buffer_size;
    esp_err_t ret = usb_host_transfer_submit(s_xinput.in_xfer);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
        ESP_LOGW(TAG, "XInput IN submit failed: %s", esp_err_to_name(ret));
    }
}

static void xinput_out_cb(usb_transfer_t *transfer)
{
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "XInput init packet status=%d", transfer->status);
    }

    s_xinput.init_index++;
    if (s_xinput.init_index >= (sizeof(s_xboxone_init) / sizeof(s_xboxone_init[0]))) {
        ESP_LOGI(TAG, "XInput init complete, listening for reports");
        submit_xinput_in();
        return;
    }

    const size_t idx = s_xinput.init_index;
    memcpy(transfer->data_buffer, s_xboxone_init[idx].data, s_xboxone_init[idx].len);
    transfer->num_bytes = s_xboxone_init[idx].len;
    transfer->device_handle = s_xinput.dev;
    transfer->bEndpointAddress = s_xinput.ep_out;
    esp_err_t ret = usb_host_transfer_submit(transfer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XInput init submit failed: %s", esp_err_to_name(ret));
        submit_xinput_in();
    }
}

static void send_xinput_init_or_listen(void)
{
    if (!s_xinput.connected || !s_xinput.out_xfer || s_xinput.ep_out == 0) {
        submit_xinput_in();
        return;
    }

    s_xinput.init_index = 0;
    memcpy(s_xinput.out_xfer->data_buffer, s_xboxone_init[0].data, s_xboxone_init[0].len);
    s_xinput.out_xfer->num_bytes = s_xboxone_init[0].len;
    s_xinput.out_xfer->device_handle = s_xinput.dev;
    s_xinput.out_xfer->bEndpointAddress = s_xinput.ep_out;
    s_xinput.out_xfer->callback = xinput_out_cb;
    s_xinput.out_xfer->context = &s_xinput;

    esp_err_t ret = usb_host_transfer_submit(s_xinput.out_xfer);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "XInput init started on OUT ep=0x%02x", s_xinput.ep_out);
    } else {
        ESP_LOGW(TAG, "XInput init submit failed: %s", esp_err_to_name(ret));
        submit_xinput_in();
    }
}

static void log_interfaces(const usb_config_desc_t *cfg)
{
    if (!cfg) {
        return;
    }

    ESP_LOGI(TAG, "Config value=%u interfaces=%u total_len=%u max_power=%umA attrs=0x%02x",
             cfg->bConfigurationValue,
             cfg->bNumInterfaces,
             cfg->wTotalLength,
             cfg->bMaxPower * 2,
             cfg->bmAttributes);

    const uint8_t *ptr = (const uint8_t *)cfg;
    const uint8_t *end = ptr + cfg->wTotalLength;
    while ((ptr + USB_STANDARD_DESC_SIZE) <= end) {
        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)ptr;
        if (desc->bLength < USB_STANDARD_DESC_SIZE || (ptr + desc->bLength) > end) {
            ESP_LOGW(TAG, "Descriptor walk stopped at malformed desc len=%u type=0x%02x",
                     desc->bLength, desc->bDescriptorType);
            break;
        }

        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
            desc->bLength >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)ptr;
            ESP_LOGI(TAG,
                     "Interface %u alt=%u eps=%u class=0x%02x(%s) sub=0x%02x proto=0x%02x",
                     intf->bInterfaceNumber,
                     intf->bAlternateSetting,
                     intf->bNumEndpoints,
                     intf->bInterfaceClass,
                     usb_class_name(intf->bInterfaceClass),
                     intf->bInterfaceSubClass,
                     intf->bInterfaceProtocol);
        } else if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   desc->bLength >= USB_EP_DESC_SIZE) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)ptr;
            ESP_LOGI(TAG,
                     "Endpoint addr=0x%02x type=%u mps=%u interval=%u",
                     ep->bEndpointAddress,
                     (unsigned)USB_EP_DESC_GET_XFERTYPE(ep),
                     (unsigned)USB_EP_DESC_GET_MPS(ep),
                     ep->bInterval);
        }

        ptr += desc->bLength;
    }
}

static bool find_xinput_interface(const usb_config_desc_t *cfg,
                                  uint8_t *iface,
                                  uint8_t *alt,
                                  uint8_t *ep_in,
                                  uint8_t *ep_out,
                                  uint16_t *in_mps)
{
    if (!cfg) {
        return false;
    }

    bool in_candidate = false;
    bool found = false;
    uint8_t cand_iface = 0;
    uint8_t cand_alt = 0;
    uint8_t cand_in = 0;
    uint8_t cand_out = 0;
    uint16_t cand_mps = 0;
    usb_transfer_type_t cand_type = USB_TRANSFER_TYPE_CTRL;

    const uint8_t *ptr = (const uint8_t *)cfg;
    const uint8_t *end = ptr + cfg->wTotalLength;
    while ((ptr + USB_STANDARD_DESC_SIZE) <= end) {
        const usb_standard_desc_t *desc = (const usb_standard_desc_t *)ptr;
        if (desc->bLength < USB_STANDARD_DESC_SIZE || (ptr + desc->bLength) > end) {
            break;
        }

        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
            desc->bLength >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)ptr;
            if (in_candidate && cand_in != 0 && cand_type == USB_TRANSFER_TYPE_INTR) {
                found = true;
                break;
            }
            in_candidate = intf->bInterfaceClass == USB_CLASS_VENDOR_SPEC &&
                           intf->bInterfaceSubClass == 0x47 &&
                           intf->bInterfaceProtocol == 0xd0 &&
                           intf->bNumEndpoints > 0;
            cand_iface = intf->bInterfaceNumber;
            cand_alt = intf->bAlternateSetting;
            cand_in = 0;
            cand_out = 0;
            cand_mps = 0;
            cand_type = USB_TRANSFER_TYPE_CTRL;
        } else if (in_candidate &&
                   desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
                   desc->bLength >= USB_EP_DESC_SIZE) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *)ptr;
            usb_transfer_type_t ep_type = USB_EP_DESC_GET_XFERTYPE(ep);
            if ((ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) &&
                (ep_type == USB_TRANSFER_TYPE_INTR || cand_in == 0)) {
                cand_in = ep->bEndpointAddress;
                cand_mps = USB_EP_DESC_GET_MPS(ep);
                cand_type = ep_type;
            } else if (!(ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) &&
                       (ep_type == USB_TRANSFER_TYPE_INTR || cand_out == 0)) {
                cand_out = ep->bEndpointAddress;
            }
        }

        ptr += desc->bLength;
    }

    if (!found && (!in_candidate || cand_in == 0)) {
        return false;
    }

    *iface = cand_iface;
    *alt = cand_alt;
    *ep_in = cand_in;
    *ep_out = cand_out;
    *in_mps = cand_mps ? cand_mps : 64;
    return true;
}

static void clear_xinput_state(bool release_interface)
{
    if (s_xinput.in_xfer) {
        usb_host_transfer_free(s_xinput.in_xfer);
    }
    if (s_xinput.out_xfer) {
        usb_host_transfer_free(s_xinput.out_xfer);
    }
    if (s_xinput.dev) {
        if (release_interface) {
            usb_host_interface_release(s_client, s_xinput.dev, s_xinput.iface);
        }
        usb_host_device_close(s_client, s_xinput.dev);
    }
    memset(&s_xinput, 0, sizeof(s_xinput));
}

static bool maybe_start_xinput(uint8_t addr, usb_device_handle_t dev, uint16_t vid, uint16_t pid, const usb_config_desc_t *cfg)
{
    if (vid == 0x0e6f && pid == 0x02ea) {
        ESP_LOGW(TAG, "PDP 0e6f:02ea detected but not claimed; this controller needs a dedicated init/report driver");
        return false;
    }

    if (s_xinput.connected) {
        ESP_LOGW(TAG, "XInput device already active, ignoring addr=%u", addr);
        return false;
    }

    uint8_t iface = 0;
    uint8_t alt = 0;
    uint8_t ep_in = 0;
    uint8_t ep_out = 0;
    uint16_t in_mps = 0;
    if (!find_xinput_interface(cfg, &iface, &alt, &ep_in, &ep_out, &in_mps)) {
        return false;
    }

    esp_err_t ret = usb_host_interface_claim(s_client, dev, iface, alt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XInput claim failed iface=%u alt=%u: %s", iface, alt, esp_err_to_name(ret));
        return false;
    }

    size_t in_size = in_mps;
    if (in_size < 64) {
        in_size = 64;
    }
    ret = usb_host_transfer_alloc(in_size, 0, &s_xinput.in_xfer);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "XInput IN transfer alloc failed: %s", esp_err_to_name(ret));
        usb_host_interface_release(s_client, dev, iface);
        return false;
    }

    if (ep_out != 0) {
        ret = usb_host_transfer_alloc(64, 0, &s_xinput.out_xfer);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "XInput OUT transfer alloc failed: %s", esp_err_to_name(ret));
        }
    }

    s_xinput.connected = true;
    s_xinput.dev = dev;
    s_xinput.iface = iface;
    s_xinput.alt = alt;
    s_xinput.ep_in = ep_in;
    s_xinput.ep_out = ep_out;
    s_xinput.in_mps = in_mps;
    s_xinput.vid = vid;
    s_xinput.pid = pid;
    s_xinput.state.connected = 1;
    s_xinput.in_xfer->callback = xinput_in_cb;
    s_xinput.in_xfer->context = &s_xinput;

    ESP_LOGI(TAG, "XInput claimed VID:PID=%04x:%04x iface=%u alt=%u IN=0x%02x OUT=0x%02x mps=%u",
             vid, pid, iface, alt, ep_in, ep_out, in_mps);

    send_xinput_init_or_listen();
    return true;
}

static void inspect_device(uint8_t addr)
{
    usb_device_handle_t dev = NULL;
    esp_err_t ret = usb_host_device_open(s_client, addr, &dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Could not open USB device addr=%u: %s", addr, esp_err_to_name(ret));
        return;
    }

    const usb_device_desc_t *device_desc = NULL;
    ret = usb_host_get_device_descriptor(dev, &device_desc);
    if (ret == ESP_OK && device_desc) {
        ESP_LOGI(TAG,
                 "Device addr=%u VID:PID=%04x:%04x class=0x%02x(%s) sub=0x%02x proto=0x%02x usb=0x%04x configs=%u",
                 addr,
                 device_desc->idVendor,
                 device_desc->idProduct,
                 device_desc->bDeviceClass,
                 usb_class_name(device_desc->bDeviceClass),
                 device_desc->bDeviceSubClass,
                 device_desc->bDeviceProtocol,
                 device_desc->bcdUSB,
                 device_desc->bNumConfigurations);
    } else {
        ESP_LOGW(TAG, "Could not read USB device descriptor addr=%u: %s", addr, esp_err_to_name(ret));
    }

    const usb_config_desc_t *cfg = NULL;
    bool device_promoted_to_xinput = false;
    ret = usb_host_get_active_config_descriptor(dev, &cfg);
    if (ret == ESP_OK && cfg) {
        log_interfaces(cfg);
        device_promoted_to_xinput = maybe_start_xinput(addr,
                                                       dev,
                                                       device_desc ? device_desc->idVendor : 0,
                                                       device_desc ? device_desc->idProduct : 0,
                                                       cfg);
    } else {
        ESP_LOGW(TAG, "Could not read active config descriptor addr=%u: %s", addr, esp_err_to_name(ret));
    }

    if (!device_promoted_to_xinput) {
        ret = usb_host_device_close(s_client, dev);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Could not close USB device addr=%u: %s", addr, esp_err_to_name(ret));
        }
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;

    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "New USB device addr=%u", event_msg->new_dev.address);
        inspect_device(event_msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "USB device gone");
        if (s_xinput.connected && event_msg->dev_gone.dev_hdl == s_xinput.dev) {
            clear_xinput_state(false);
        }
        break;
    default:
        break;
    }
}

static void usb_device_monitor_task(void *arg)
{
    (void)arg;

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t ret = usb_host_client_register(&client_config, &s_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB monitor client register failed: %s", esp_err_to_name(ret));
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "USB descriptor monitor started");

    while (true) {
        ret = usb_host_client_handle_events(s_client, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK && ret != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "USB monitor event error: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

esp_err_t usb_device_monitor_start(void)
{
    if (s_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        usb_device_monitor_task,
        "usb_mon",
        4096,
        NULL,
        4,
        &s_task,
        1);

    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool usb_device_monitor_xinput_is_connected(void)
{
    return s_xinput.connected;
}

void usb_device_monitor_xinput_get_state(lvgl_port_gamepad_state_t *state)
{
    if (!state) {
        return;
    }
    *state = s_xinput.state;
}

void usb_device_monitor_xinput_get_vid_pid(uint16_t *vid, uint16_t *pid)
{
    if (vid) {
        *vid = s_xinput.vid;
    }
    if (pid) {
        *pid = s_xinput.pid;
    }
}

int usb_device_monitor_xinput_get_raw_report(uint8_t *buf, size_t buf_size)
{
    if (!buf || buf_size == 0 || s_xinput.raw_len == 0) {
        return 0;
    }

    size_t copy_len = s_xinput.raw_len;
    if (copy_len > buf_size) {
        copy_len = buf_size;
    }
    memcpy(buf, s_xinput.raw, copy_len);
    return (int)copy_len;
}
