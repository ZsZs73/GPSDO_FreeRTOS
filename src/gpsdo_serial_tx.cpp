#include "gpsdo_serial_tx.h"

#include "gpsdo_config.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#ifndef GPSDO_SERIAL_TX_CHUNK
#define GPSDO_SERIAL_TX_CHUNK 128u
#endif

#ifndef GPSDO_SERIAL_TX_QUEUE_DEPTH
#define GPSDO_SERIAL_TX_QUEUE_DEPTH 24u
#endif

#ifndef GPSDO_SERIAL_TX_STACK
#define GPSDO_SERIAL_TX_STACK 512u
#endif

#ifndef GPSDO_SERIAL_TX_PRIORITY
#define GPSDO_SERIAL_TX_PRIORITY (tskIDLE_PRIORITY + 2u)
#endif

#ifndef GPSDO_SERIAL2_LINE_GAP_MS
#define GPSDO_SERIAL2_LINE_GAP_MS 5u
#endif

typedef struct {
    uint16_t len;
    uint8_t data[GPSDO_SERIAL_TX_CHUNK];
} serial_tx_item_t;

static QueueHandle_t s_tx_queue = nullptr;
static TaskHandle_t s_tx_task = nullptr;

GpsdoSerial g_serial;

static bool scheduler_running(void)
{
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
}

static void write_all(Stream &port, const uint8_t *buf, size_t len)
{
    size_t off = 0u;

    while (off < len) {
        int room = port.availableForWrite();

        if (room <= 0) {
            taskYIELD();
            continue;
        }

        size_t chunk = len - off;
        if (chunk > (size_t)room)
            chunk = (size_t)room;

        size_t n = port.write(buf + off, chunk);
        if (n == 0u) {
            taskYIELD();
            continue;
        }

        off += n;
    }
}

#if defined(GPSDO_BLUETOOTH) || defined(GPSDO_BLUETOOTH_PARALLEL)
static void write_serial2_paced(const uint8_t *buf, size_t len)
{
    size_t start = 0u;

    for (size_t i = 0u; i < len; ++i) {
        if (buf[i] != '\n')
            continue;

        write_all(Serial2, buf + start, i - start + 1u);
        Serial2.flush();

#if GPSDO_SERIAL2_LINE_GAP_MS > 0
        if (scheduler_running())
            vTaskDelay(pdMS_TO_TICKS(GPSDO_SERIAL2_LINE_GAP_MS));
        else
            delay(GPSDO_SERIAL2_LINE_GAP_MS);
#endif
        start = i + 1u;
    }

    if (start < len)
        write_all(Serial2, buf + start, len - start);
}
#endif

static void write_boot_direct(const uint8_t *buf, size_t len)
{
#if defined(GPSDO_BLUETOOTH_PARALLEL)
    Serial.write(buf, len);
    write_serial2_paced(buf, len);
#elif defined(GPSDO_BLUETOOTH)
    write_serial2_paced(buf, len);
#else
    Serial.write(buf, len);
#endif
}

static void write_runtime_item(const serial_tx_item_t &item)
{
#if defined(GPSDO_BLUETOOTH_PARALLEL)
    /*
     * Serial2 first: it is the persistent remote transport. An absent USB
     * host must never prevent the remote peer from receiving a record.
     */
    write_serial2_paced(item.data, item.len);

    /*
     * USB is a best-effort mirror in parallel mode. Never let an absent or
     * stalled CDC host back-pressure the primary Serial2 transport.
     * Write only the bytes that fit immediately; any remainder is not mirrored.
     */
    int room = Serial.availableForWrite();
    if (room > 0) {
        size_t n = item.len;
        if (n > (size_t)room)
            n = (size_t)room;

        Serial.write(item.data, n);
    }
#elif defined(GPSDO_BLUETOOTH)
    write_serial2_paced(item.data, item.len);

#else
    /* USB-only build: USB is the primary transport, so apply backpressure. */
    write_all(Serial, item.data, item.len);
#endif
}

static void serial_tx_task(void *)
{
    serial_tx_item_t item;

    for (;;) {
        if (xQueueReceive(s_tx_queue, &item, portMAX_DELAY) == pdTRUE)
            write_runtime_item(item);
    }
}

bool gpsdo_serial_start(void)
{
    if (s_tx_queue != nullptr)
        return true;

    s_tx_queue = xQueueCreate(GPSDO_SERIAL_TX_QUEUE_DEPTH,
                              sizeof(serial_tx_item_t));
    if (s_tx_queue == nullptr)
        return false;

    BaseType_t ok = xTaskCreate(serial_tx_task,
                                "SerialTX",
                                GPSDO_SERIAL_TX_STACK,
                                nullptr,
                                GPSDO_SERIAL_TX_PRIORITY,
                                &s_tx_task);
    if (ok != pdPASS) {
        vQueueDelete(s_tx_queue);
        s_tx_queue = nullptr;
        s_tx_task = nullptr;
        return false;
    }

    return true;
}

bool gpsdo_serial_ready(void)
{
    return s_tx_queue != nullptr;
}

size_t GpsdoSerial::write(uint8_t c)
{
    return write(&c, 1u);
}

size_t GpsdoSerial::write(const uint8_t *buf, size_t size)
{
    if (buf == nullptr || size == 0u)
        return 0u;

    if (!scheduler_running()) {
        write_boot_direct(buf, size);
        return size;
    }

    if (s_tx_queue == nullptr)
        return 0u;

    size_t off = 0u;

    while (off < size) {
        serial_tx_item_t item;
        size_t n = size - off;

        if (n > GPSDO_SERIAL_TX_CHUNK)
            n = GPSDO_SERIAL_TX_CHUNK;

        item.len = (uint16_t)n;
        memcpy(item.data, buf + off, n);

        /*
         * Reliability rule: wait for queue space. A slow receiver causes
         * latency/backpressure, never a silent missing block.
         */
        if (xQueueSend(s_tx_queue, &item, portMAX_DELAY) != pdTRUE)
            break;

        off += n;
    }

    return off;
}

int GpsdoSerial::availableForWrite(void)
{
    if (!scheduler_running() || s_tx_queue == nullptr)
        return (int)GPSDO_SERIAL_TX_CHUNK;

    UBaseType_t free_items = uxQueueSpacesAvailable(s_tx_queue);
    size_t bytes = (size_t)free_items * GPSDO_SERIAL_TX_CHUNK;

    if (bytes > 0x7FFFu)
        bytes = 0x7FFFu;

    return (int)bytes;
}

int GpsdoSerial::available(void)
{
#if defined(GPSDO_BLUETOOTH_PARALLEL)
    return Serial.available() + Serial2.available();
#elif defined(GPSDO_BLUETOOTH)
    return Serial2.available();
#else
    return Serial.available();
#endif
}

int GpsdoSerial::read(void)
{
#if defined(GPSDO_BLUETOOTH_PARALLEL)
    int c = Serial.read();
    if (c >= 0)
        return c;
    return Serial2.read();
#elif defined(GPSDO_BLUETOOTH)
    return Serial2.read();
#else
    return Serial.read();
#endif
}

int GpsdoSerial::peek(void)
{
#if defined(GPSDO_BLUETOOTH_PARALLEL)
    int c = Serial.peek();
    if (c >= 0)
        return c;
    return Serial2.peek();
#elif defined(GPSDO_BLUETOOTH)
    return Serial2.peek();
#else
    return Serial.peek();
#endif
}

void GpsdoSerial::flush(void)
{
    if (scheduler_running() && s_tx_queue != nullptr) {
        while (uxQueueMessagesWaiting(s_tx_queue) != 0u)
            vTaskDelay(1);
    }

#if defined(GPSDO_BLUETOOTH_PARALLEL)
    Serial2.flush();
    if (Serial.availableForWrite() > 0)
        Serial.flush();
#elif defined(GPSDO_BLUETOOTH)
    Serial2.flush();
#else
    Serial.flush();
#endif
}
