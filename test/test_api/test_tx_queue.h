// Tests for the Tx queue (TxService in src/core/telegram.cpp), which the UART task sends from
// and adds retries to, and which other tasks add to.

#include <Arduino.h>
#include <unity.h>
#include <emsesp.h>

#include <atomic>
#include <cstdio>
#include <thread>

static constexpr uint8_t  TX_QUEUE_DEST      = 0x7E; // an id no device has
static constexpr uint16_t TX_QUEUE_TYPE      = 0x01;
static constexpr uint32_t TX_QUEUE_TELEGRAMS = 20000;
static constexpr uint32_t TX_QUEUE_RETRIES   = 200; // each one prints the telegram it transmits

// what retry_tx() is given as the reply that did not match
static uint8_t tx_queue_reply[] = {TX_QUEUE_DEST, 0x0B, TX_QUEUE_TYPE, 0x00, 0x00, 0x00};

// with Tx off, send() takes telegrams off the queue without transmitting them
static void tx_queue_drain() {
    const uint8_t tx_mode = emsesp::EMSbus::tx_mode();
    emsesp::EMSbus::tx_mode(EMS_TXMODE_OFF);
    while (!emsesp::EMSESP::txservice_.tx_queue_empty()) {
        emsesp::EMSESP::txservice_.send();
    }
    emsesp::EMSbus::tx_mode(tx_mode);
}

static void tx_queue_add_reads(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST, 0, 0, (i % 8) == 0);
    }
}

static void tx_queue_retry() {
    emsesp::EMSESP::txservice_.retry_tx(emsesp::Telegram::Operation::TX_READ, tx_queue_reply, sizeof(tx_queue_reply));
}

void tx_queue_send_takes_the_front_telegram_off() {
    tx_queue_drain();
    emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST, 1);
    emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST, 2);
    emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST, 3, 0, true);

    emsesp::EMSESP::txservice_.send();
    emsesp::EMSbus::tx_state(emsesp::Telegram::Operation::NONE);

    const auto queue = emsesp::EMSESP::txservice_.queue();
    TEST_ASSERT_EQUAL_size_t(2, queue.size());
    TEST_ASSERT_EQUAL_UINT8(1, queue.front().telegram_->offset);
    TEST_ASSERT_EQUAL_UINT8(2, queue.back().telegram_->offset);
    tx_queue_drain();
}

void tx_queue_keeps_a_telegram_added_to_the_front_during_transmit() {
    tx_queue_drain();
    const uint8_t tx_mode = emsesp::EMSbus::tx_mode();
    emsesp::EMSbus::tx_mode(EMS_TXMODE_HW);
    emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST, 1);
    emsesp::EMSuart::on_transmit = [] { emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST, 2, 0, true); };

    emsesp::EMSESP::txservice_.send();

    emsesp::EMSuart::on_transmit = nullptr;
    emsesp::EMSbus::tx_state(emsesp::Telegram::Operation::NONE);
    emsesp::EMSbus::tx_mode(tx_mode);
    const auto queue = emsesp::EMSESP::txservice_.queue();
    TEST_ASSERT_EQUAL_size_t(1, queue.size());
    TEST_ASSERT_EQUAL_UINT8(2, queue.front().telegram_->offset);
    tx_queue_drain();
}

void tx_queue_drops_the_next_telegram_when_full() {
    tx_queue_drain();
    const uint32_t fails_before = emsesp::EMSESP::txservice_.telegram_read_fail_count();
    for (uint32_t i = 0; i <= MAX_TX_TELEGRAMS; i++) {
        emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST, i % 256);
    }

    const auto queue = emsesp::EMSESP::txservice_.queue();
    TEST_ASSERT_EQUAL_size_t(MAX_TX_TELEGRAMS, queue.size());
    TEST_ASSERT_EQUAL_UINT8(1, queue.front().telegram_->offset);
    TEST_ASSERT_EQUAL_UINT32(fails_before + 1, emsesp::EMSESP::txservice_.telegram_read_fail_count());
    tx_queue_drain();
}

void tx_queue_gives_up_on_an_id_nobody_polls() {
    tx_queue_drain();
    const uint8_t foreign_id = emsesp::EMSbus::ems_bus_id() + 1;
    char          raw[32];
    snprintf(raw, sizeof(raw), "%02X %02X 01 00 FF", foreign_id, TX_QUEUE_DEST);
    TEST_ASSERT_TRUE(emsesp::EMSESP::txservice_.send_raw(raw));

    for (uint32_t i = 0; i < 500; i++) {
        TEST_ASSERT_EQUAL_UINT8(foreign_id, emsesp::EMSESP::txservice_.get_send_id());
    }

    TEST_ASSERT_EQUAL_UINT8(emsesp::EMSbus::ems_bus_id(), emsesp::EMSESP::txservice_.get_send_id());
    TEST_ASSERT_TRUE(emsesp::EMSESP::txservice_.tx_queue_empty());
}

void tx_queue_skips_a_retry_when_full() {
    tx_queue_drain();
    const uint8_t tx_mode = emsesp::EMSbus::tx_mode();
    emsesp::EMSbus::tx_mode(EMS_TXMODE_HW);
    emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST);
    emsesp::EMSESP::txservice_.send(); // gives the retry a last telegram
    tx_queue_add_reads(MAX_TX_TELEGRAMS);
    tx_queue_retry();
    emsesp::EMSbus::tx_state(emsesp::Telegram::Operation::NONE);
    emsesp::EMSbus::tx_mode(tx_mode);

    TEST_ASSERT_EQUAL_size_t(MAX_TX_TELEGRAMS, emsesp::EMSESP::txservice_.queue().size());
    TEST_ASSERT_EQUAL_UINT8(0, emsesp::EMSESP::txservice_.retry_count());
    tx_queue_drain();
}

void tx_queue_holds_the_last_retry_back_for_the_delay() {
    tx_queue_drain();
    const uint8_t tx_mode = emsesp::EMSbus::tx_mode();
    emsesp::EMSbus::tx_mode(EMS_TXMODE_HW);
    emsesp::EMSESP::txservice_.read_request(TX_QUEUE_TYPE, TX_QUEUE_DEST);
    for (uint8_t i = 0; i < emsesp::TxService::MAXIMUM_TX_RETRIES; i++) {
        emsesp::EMSESP::txservice_.send();
        tx_queue_retry();
    }

    emsesp::EMSESP::txservice_.send();
    const bool held_back = !emsesp::EMSESP::txservice_.tx_queue_empty();

    // the standalone esp_timer_get_time() returns what delay() adds up, read in microseconds
    delay(emsesp::TxService::POST_SEND_DELAY * 1000);
    uuid::loop();
    emsesp::EMSESP::txservice_.send();
    const bool sent = emsesp::EMSESP::txservice_.tx_queue_empty();

    // a telegram still held back would keep the delay running and hang the next test's drain
    if (!sent) {
        delay(emsesp::TxService::POST_SEND_DELAY * 1000);
        uuid::loop();
        emsesp::EMSESP::txservice_.send();
    }

    emsesp::EMSESP::txservice_.reset_retry_count();
    emsesp::EMSbus::tx_state(emsesp::Telegram::Operation::NONE);
    emsesp::EMSbus::tx_mode(tx_mode);
    TEST_ASSERT_TRUE(held_back);
    TEST_ASSERT_TRUE(sent);
}

void tx_queue_survives_concurrent_add_and_send() {
    tx_queue_drain();
    const uint8_t tx_mode = emsesp::EMSbus::tx_mode();
    emsesp::EMSbus::tx_mode(EMS_TXMODE_OFF);

    std::atomic_uint adders{2};
    const auto       adder = [&adders] {
        tx_queue_add_reads(TX_QUEUE_TELEGRAMS);
        adders--;
    };
    std::thread web_server(adder);
    std::thread main_loop(adder);
    std::thread uart_task([&adders] {
        while (true) {
            const bool finished = adders == 0; // read first, so that an empty queue then means the end
            if (emsesp::EMSESP::txservice_.tx_queue_empty() && finished) {
                break;
            }
            emsesp::EMSESP::txservice_.get_send_id();
            emsesp::EMSESP::txservice_.send();
        }
    });
    while (adders > 0) {
        (void)emsesp::EMSESP::txservice_.queue();
    }
    web_server.join();
    main_loop.join();
    uart_task.join();
    emsesp::EMSbus::tx_mode(tx_mode);

    TEST_ASSERT_TRUE(emsesp::EMSESP::txservice_.tx_queue_empty());
}

void tx_queue_survives_concurrent_add_and_retry() {
    tx_queue_drain();
    const uint8_t tx_mode = emsesp::EMSbus::tx_mode();
    emsesp::EMSbus::tx_mode(EMS_TXMODE_HW);

    std::atomic_bool done{false};
    std::thread      uart_task([&done] {
        bool retry = false;
        while (!done || !emsesp::EMSESP::txservice_.tx_queue_empty()) {
            emsesp::EMSbus::tx_state(emsesp::Telegram::Operation::NONE);
            emsesp::EMSESP::txservice_.send();
            if (emsesp::EMSbus::tx_state() == emsesp::Telegram::Operation::NONE) {
                continue;
            }
            // a telegram goes out twice, as a retry needs the one just sent and uses it up
            retry = !retry;
            if (retry) {
                tx_queue_retry();
                emsesp::EMSESP::txservice_.reset_retry_count();
            }
        }
    });
    tx_queue_add_reads(TX_QUEUE_RETRIES);
    done = true;
    uart_task.join();
    emsesp::EMSbus::tx_state(emsesp::Telegram::Operation::NONE);
    emsesp::EMSbus::tx_mode(tx_mode);

    TEST_ASSERT_TRUE(emsesp::EMSESP::txservice_.tx_queue_empty());
}

void run_tx_queue_tests() {
    RUN_TEST(tx_queue_send_takes_the_front_telegram_off);
    RUN_TEST(tx_queue_keeps_a_telegram_added_to_the_front_during_transmit);
    RUN_TEST(tx_queue_drops_the_next_telegram_when_full);
    RUN_TEST(tx_queue_gives_up_on_an_id_nobody_polls);
    RUN_TEST(tx_queue_skips_a_retry_when_full);
    RUN_TEST(tx_queue_holds_the_last_retry_back_for_the_delay);
    RUN_TEST(tx_queue_survives_concurrent_add_and_send);
    RUN_TEST(tx_queue_survives_concurrent_add_and_retry);
}
