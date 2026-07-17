/*
 * Bidirectional Wireless UART Bridge over ESP-NOW
 * -----------------------------------------------
 * Target : ESP-01s (ESP8266EX, 1MB flash)
 * SDK    : ESP8266_RTOS_SDK v3.4 (IDF-style API)
 *
 * Two identical modules bridge UART0 over the air: bytes entering UART0 on
 * one module are transmitted via ESP-NOW and written to UART0 on the other,
 * and vice-versa. Designed for low latency (no Wi-Fi power save, fixed
 * channel) and stability (strict TX flow control, RX sequence dedup).
 *
 * Module layout:
 *   [1] Configuration      - tunables, MACs, packet struct
 *   [2] Globals            - shared state (flow-control flag, queue, seq)
 *   [3] Wi-Fi module       - STA init, channel lock, power-save off
 *   [4] ESP-NOW callbacks  - non-blocking Wi-Fi task callbacks
 *   [5] ESP-NOW module     - initialization and peer registration
 *   [6] Activity LED       - non-blocking GPIO2 TX/RX indication
 *   [7] UART module        - UART0 driver install
 *   [8] Tasks              - uart_to_air (TX) and air_to_uart (RX)
 *   [9] app_main           - wire everything together
 */

/* ---- Standard / RTOS ---- */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

/* ---- SDK ---- */
#include "nvs_flash.h"
#include "tcpip_adapter.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"

/* ========================================================================
 * [1] CONFIGURATION
 * ==================================================================== */

/* Wi-Fi / ESP-NOW */
#define WUART_WIFI_CHANNEL      1            /* both modules MUST match     */
#define WUART_WIFI_IF           ESP_IF_WIFI_STA

/*
 * Pairing mode.
 *   - Default: broadcast MAC -> zero-config pairing, any two modules talk.
 *   - Define WUART_USE_FIXED_PEER and set WUART_PEER_MAC to lock to one
 *     specific partner (rejects stray traffic, slightly more robust).
 */
/* #define WUART_USE_FIXED_PEER */
#define WUART_PEER_MAC          { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }

/* UART0: 115200 8N1 */
#define WUART_UART_PORT         UART_NUM_0
#define WUART_UART_BAUD         115200
#define WUART_UART_RX_BUF       1024
#define WUART_UART_TX_BUF       1024
#define WUART_UART_READ_TMO_MS  10           /* uart_read_bytes timeout     */

/* ESP-01s onboard activity LED: GPIO2, active-low on common board revisions. */
#define WUART_LED_GPIO          GPIO_NUM_2
#define WUART_LED_ON_LEVEL      0
#define WUART_LED_OFF_LEVEL     1
#define WUART_LED_PULSE_MS      30

/* RX queue depth (air -> uart) */
#define WUART_RX_QUEUE_LEN      8

/* Task tuning */
#define WUART_TASK_STACK        2048
#define WUART_TX_TASK_PRIO      5
#define WUART_RX_TASK_PRIO      5
#define WUART_LED_TASK_STACK    1024
#define WUART_LED_TASK_PRIO     3

/*
 * ESP-NOW payload. Max ESP-NOW frame is 250 bytes (ESP_NOW_MAX_DATA_LEN).
 * seq_num(1) + data_len(1) + data(248) = 250, exactly at the limit.
 */
#define WUART_MAX_DATA          248

typedef struct {
    uint8_t seq_num;
    uint8_t data_len;
    uint8_t data[WUART_MAX_DATA];
} __attribute__((packed)) uart_now_packet_t;

/* Fixed header size (seq_num + data_len) preceding the payload. */
#define WUART_HEADER_LEN        (sizeof(uart_now_packet_t) - WUART_MAX_DATA)

/* ========================================================================
 * [2] GLOBALS
 * ==================================================================== */

static const char *TAG = "wuart";

/* Broadcast MAC: zero-config pairing peer. */
static const uint8_t g_broadcast_mac[ESP_NOW_ETH_ALEN] =
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

#ifdef WUART_USE_FIXED_PEER
static const uint8_t g_peer_mac[ESP_NOW_ETH_ALEN] = WUART_PEER_MAC;
#endif

/*
 * TX flow control. ESP-NOW allows only one in-flight frame; sending again
 * before the previous send callback fires risks ESP_ERR_ESPNOW_NO_MEM and
 * instability. can_send_next gates the TX task: cleared before esp_now_send,
 * set again (unconditionally) in the send callback.
 *
 * Single-producer (uart_to_air) / single-setter (send cb) pattern, so a
 * plain volatile flag is sufficient here.
 */
static volatile bool g_can_send_next = true;

/* TX sequence counter (wraps at 256). */
static uint8_t g_tx_seq = 0;

/* air -> uart handoff: RX callback (Wi-Fi task) -> air_to_uart task. */
static QueueHandle_t g_rx_queue = NULL;

/* TX/RX activity notification target. GPIO access is owned by this task. */
static TaskHandle_t g_led_task = NULL;

/* Effective destination MAC for sends (broadcast or fixed peer). */
static const uint8_t *tx_dest_mac(void)
{
#ifdef WUART_USE_FIXED_PEER
    return g_peer_mac;
#else
    return g_broadcast_mac;
#endif
}

/* ========================================================================
 * [3] Wi-Fi MODULE
 * ==================================================================== */

/*
 * STA mode, no AP association. We only need the radio up for ESP-NOW.
 * Channel is locked and power save disabled for lowest latency.
 */
static void wuart_wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Keep config in RAM only; nothing to persist for a bridge. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Lock channel (must be set after start on this SDK). */
    ESP_ERROR_CHECK(esp_wifi_set_channel(WUART_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    /* Disable power save: no beacon-driven sleep, minimal TX latency. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /*
     * Note: esp_wifi_config_espnow_rate() is not available in ESP8266_RTOS_SDK
     * v3.4, so the optional 54M PHY-rate boost is omitted. Default rate is used.
     */
}

/* ========================================================================
 * [4] ESP-NOW CALLBACKS
 * ==================================================================== */

/*
 * Send callback (runs in Wi-Fi task). Re-arm the TX gate unconditionally:
 * whether the frame was ACKed or not, the radio buffer is now free, so the
 * TX task is allowed to queue the next frame.
 */
static void wuart_on_sent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    (void)status;
    g_can_send_next = true;
}

/*
 * Receive callback (runs in Wi-Fi task, NOT a hardware ISR). Keep it short:
 * validate, copy into a packet, hand off to the queue with zero block time.
 * Dropping on a full queue is intentional — better to lose a frame than to
 * stall the Wi-Fi task.
 */
static void wuart_on_recv(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    (void)mac_addr;

    if (data == NULL || len < WUART_HEADER_LEN || len > (int)sizeof(uart_now_packet_t)) {
        return;
    }

    uart_now_packet_t pkt;
    memcpy(&pkt, data, len);

    /* Clamp declared length to what actually arrived. */
    if (pkt.data_len > (len - WUART_HEADER_LEN)) {
        pkt.data_len = len - WUART_HEADER_LEN;
    }

    /* Non-blocking: never stall the Wi-Fi task. */
    (void)xQueueSend(g_rx_queue, &pkt, 0);
}

/* ========================================================================
 * [5] ESP-NOW MODULE
 * ==================================================================== */

static void wuart_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(wuart_on_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(wuart_on_recv));

    /* Register the destination peer (broadcast by default, fixed if configured). */
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    peer.channel = WUART_WIFI_CHANNEL;
    peer.ifidx   = ESP_IF_WIFI_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, tx_dest_mac(), ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

/* ========================================================================
 * [6] ACTIVITY LED MODULE
 * ==================================================================== */

/*
 * Report activity without delaying the caller. Task notifications are used
 * instead of sleeping in the UART or Wi-Fi paths. Multiple events within the
 * pulse window are coalesced and extend the visible LED pulse.
 */
static void wuart_led_activity(void)
{
    if (g_led_task != NULL) {
        (void)xTaskNotifyGive(g_led_task);
    }
}

static void wuart_led_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(gpio_set_level(WUART_LED_GPIO, WUART_LED_OFF_LEVEL));

    for (;;) {
        /* Wait indefinitely for the first TX or RX event. */
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_ERROR_CHECK(gpio_set_level(WUART_LED_GPIO, WUART_LED_ON_LEVEL));

        /* Keep the LED lit until traffic has been idle for one pulse period. */
        while (ulTaskNotifyTake(pdTRUE,
                                pdMS_TO_TICKS(WUART_LED_PULSE_MS)) > 0) {
        }

        ESP_ERROR_CHECK(gpio_set_level(WUART_LED_GPIO, WUART_LED_OFF_LEVEL));
    }
}

static void wuart_led_init(void)
{
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1U << WUART_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led_cfg));
    ESP_ERROR_CHECK(gpio_set_level(WUART_LED_GPIO, WUART_LED_OFF_LEVEL));

    BaseType_t created = xTaskCreate(wuart_led_task, "activity_led",
                                     WUART_LED_TASK_STACK, NULL,
                                     WUART_LED_TASK_PRIO, &g_led_task);
    configASSERT(created == pdPASS);
}

/* ========================================================================
 * [7] UART MODULE
 * ==================================================================== */

/*
 * Install the UART0 driver at 115200 8N1. The driver's internal RX ring
 * buffer decouples byte arrival from our polling task. No event queue is
 * used (pass NULL) — the TX task polls with a short timeout instead.
 *
 * NOTE: UART0 is also the SDK's default log/console port. To keep the data
 * stream clean, logging is disabled (see app_main) so nothing but bridged
 * bytes goes out on TX.
 */
static void wuart_uart_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = WUART_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(WUART_UART_PORT, &uart_cfg));

    /* Args: port, rx_buf, tx_buf, queue_len, queue_handle, no_use */
    ESP_ERROR_CHECK(uart_driver_install(WUART_UART_PORT,
                                        WUART_UART_RX_BUF,
                                        WUART_UART_TX_BUF,
                                        0, NULL, 0));
}

/* ========================================================================
 * [8] TASKS
 * ==================================================================== */

/*
 * Task 1 - uart_to_air (TX path)
 * Poll UART0 for incoming bytes and forward them over ESP-NOW.
 *
 * Flow control: a frame may only be sent when g_can_send_next is true (the
 * previous send callback has fired). We read at most WUART_MAX_DATA bytes per
 * frame. The read timeout keeps the loop responsive without busy-spinning.
 */
static void uart_to_air_task(void *arg)
{
    (void)arg;
    uart_now_packet_t pkt;

    for (;;) {
        /*
         * Gate on flow control first. If the radio is still busy with the
         * previous frame, yield briefly and retry rather than reading UART
         * (which would let bytes pile up faster than we can send).
         */
        if (!g_can_send_next) {
            vTaskDelay(1);
            continue;
        }

        int n = uart_read_bytes(WUART_UART_PORT, pkt.data, WUART_MAX_DATA,
                                pdMS_TO_TICKS(WUART_UART_READ_TMO_MS));
        if (n <= 0) {
            continue;   /* timeout, nothing to send */
        }

        pkt.seq_num  = ++g_tx_seq;
        pkt.data_len = (uint8_t)n;

        /* Clear the gate BEFORE sending so the send cb can't race us. */
        g_can_send_next = false;

        esp_err_t err = esp_now_send(tx_dest_mac(),
                                     (const uint8_t *)&pkt,
                                     WUART_HEADER_LEN + n);
        if (err != ESP_OK) {
            /* Send was rejected: re-arm so we can retry the next byte batch. */
            ESP_LOGW(TAG, "esp_now_send failed: %d", err);
            g_can_send_next = true;
        } else {
            /* Indicate a frame accepted by the ESP-NOW transmit subsystem. */
            wuart_led_activity();
        }
    }
}

/*
 * Task 2 - air_to_uart (RX path)
 * Block on the RX queue, drop hardware-retransmitted duplicates by comparing
 * seq_num against the last one seen, and write payloads out to UART0.
 */
static void air_to_uart_task(void *arg)
{
    (void)arg;
    uart_now_packet_t pkt;

    /*
     * Last accepted sequence number. Initialized to a value that cannot
     * collide with the first real packet's seq_num on the very first receive.
     */
    int last_seq = -1;

    for (;;) {
        if (xQueueReceive(g_rx_queue, &pkt, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Deduplicate: ESP-NOW may retransmit a frame at the PHY layer. */
        if ((int)pkt.seq_num == last_seq) {
            continue;
        }
        last_seq = pkt.seq_num;

        if (pkt.data_len > 0 && pkt.data_len <= WUART_MAX_DATA) {
            /* Blink only for valid, non-duplicate data delivered to UART. */
            wuart_led_activity();
            uart_write_bytes(WUART_UART_PORT, (const char *)pkt.data, pkt.data_len);
        }
    }
}

/* ========================================================================
 * [9] app_main
 * ==================================================================== */

void app_main(void)
{
    /*
     * Silence SDK logging so nothing corrupts the UART0 data stream. The ROM
     * bootloader still emits a brief banner at reset (unavoidable without a
     * custom bootloader), but no runtime logs follow.
     */
    esp_log_level_set("*", ESP_LOG_NONE);

    /* NVS is required by the Wi-Fi stack. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    /* RX handoff queue (Wi-Fi task -> air_to_uart task). */
    g_rx_queue = xQueueCreate(WUART_RX_QUEUE_LEN, sizeof(uart_now_packet_t));
    configASSERT(g_rx_queue != NULL);

    wuart_led_init();
    wuart_wifi_init();
    wuart_espnow_init();
    wuart_uart_init();

    xTaskCreate(uart_to_air_task, "uart_to_air", WUART_TASK_STACK,
                NULL, WUART_TX_TASK_PRIO, NULL);
    xTaskCreate(air_to_uart_task, "air_to_uart", WUART_TASK_STACK,
                NULL, WUART_RX_TASK_PRIO, NULL);
}
