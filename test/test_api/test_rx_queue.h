// Tests for the Rx queue (RxService in src/core/telegram.cpp), which the UART task fills and
// the main loop empties.

#include <Arduino.h>
#include <unity.h>
#include <emsesp.h>

#include <algorithm>
#include <atomic>
#include <thread>

// enough to cross many of the deque's chunks and to reposition its map repeatedly
static constexpr uint32_t RX_QUEUE_TELEGRAMS = 50000;

static void rx_queue_add_telegrams() {
    // from an id no device has and with an empty body, so that processing stops early and queues nothing
    uint8_t telegram[] = {0x7E, 0x00, 0xFF, 0x00, 0x00, 0x01, 0x00};

    telegram[sizeof(telegram) - 1] = emsesp::EMSESP::rxservice_.calculate_crc(telegram, sizeof(telegram) - 1);
    for (uint32_t i = 0; i < RX_QUEUE_TELEGRAMS; i++) {
        emsesp::EMSESP::rxservice_.add(telegram, sizeof(telegram));
    }
}

void rx_queue_survives_concurrent_add_and_loop() {
    TEST_ASSERT_TRUE(emsesp::EMSESP::rxservice_.queue().empty());

    const uint32_t   count_before = emsesp::EMSESP::rxservice_.telegram_count();
    std::atomic_bool done{false};

    std::thread uart_task([&done] {
        rx_queue_add_telegrams();
        done = true;
    });
    while (!done) {
        emsesp::EMSESP::rxservice_.loop();
    }
    uart_task.join();
    emsesp::EMSESP::rxservice_.loop();

    TEST_ASSERT_EQUAL_UINT32(RX_QUEUE_TELEGRAMS, emsesp::EMSESP::rxservice_.telegram_count() - count_before);
    TEST_ASSERT_TRUE(emsesp::EMSESP::rxservice_.queue().empty());
}

void rx_queue_survives_concurrent_add_empty_loop_and_queue() {
    TEST_ASSERT_TRUE(emsesp::EMSESP::rxservice_.queue().empty());

    const uint32_t   count_before = emsesp::EMSESP::rxservice_.telegram_count();
    std::atomic_bool done{false};
    size_t           deepest = 0;

    std::thread uart_task([&done] {
        for (uint32_t i = 0; i < RX_QUEUE_TELEGRAMS; i++) {
            emsesp::EMSESP::rxservice_.add_empty(0x7E, 0x00, 0x0101, 0);
        }
        done = true;
    });
    while (!done) {
        deepest = std::max(deepest, emsesp::EMSESP::rxservice_.queue().size());
        emsesp::EMSESP::rxservice_.loop();
    }
    uart_task.join();
    emsesp::EMSESP::rxservice_.loop();

    TEST_ASSERT_LESS_OR_EQUAL_size_t(MAX_RX_TELEGRAMS, deepest);
    TEST_ASSERT_GREATER_THAN_UINT32(count_before, emsesp::EMSESP::rxservice_.telegram_count());
    TEST_ASSERT_TRUE(emsesp::EMSESP::rxservice_.queue().empty());
}

void run_rx_queue_tests() {
    RUN_TEST(rx_queue_survives_concurrent_add_and_loop);
    RUN_TEST(rx_queue_survives_concurrent_add_empty_loop_and_queue);
}
