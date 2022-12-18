//-------------------------------------------------------------------------------------
// ESPHome P1 Electricity Meter custom sensor
// Copyright 2022 Johnny Johansson
// Copyright 2020 Pär Svanström
// 
// History
//  0.1.0 2020-11-05:   Initial release
//  0.2.0 2022-04-13:   Major rewrite
//  0.3.0 2022-04-23:   Passthrough to secondary P1 device
//
// MIT License
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), 
// to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, 
// and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS 
// IN THE SOFTWARE.
//-------------------------------------------------------------------------------------

#include "esphome.h"

class P1Reader : public Component, public UARTDevice {
public:

    // Call from a lambda in the yaml file to set up each sensor.
    Sensor *AddSensor(int major, int minor, int micro)
    {
        m_sensor_list = new SensorListItem(m_sensor_list, OBIS(major, minor, micro));
        return m_sensor_list->GetSensor();
    }

    P1Reader(UARTComponent *parent,
        Number *update_period_number,
        esphome::gpio::GPIOSwitch *CTS_switch,
        esphome::gpio::GPIOSwitch *status_switch = nullptr,
        esphome::gpio::GPIOBinarySensor * secondary_RTS = nullptr)
        : UARTDevice(parent)
        , m_minimum_period_ms{ static_cast<unsigned long>(update_period_number->state * 1000.0f + 0.5f) }
        , m_CTS_switch{ CTS_switch }
        , m_status_switch{ status_switch }
        , m_update_period_number{ update_period_number }
        , m_secondary_RTS{ secondary_RTS }
    {}

private:
    unsigned long m_minimum_period_ms{ 0 };
    unsigned long m_reading_message_time;
    unsigned long m_reading_crc_time;
    unsigned long m_processing_time;
    unsigned long m_resending_time;
    unsigned long m_waiting_time;
    unsigned long m_error_recovery_time;
    int m_num_message_loops;
    int m_num_processing_loops;
    bool m_display_time_stats{ false };

    // Store the main message as it is beeing received:
    constexpr static int message_buffer_size{ 2048 };
    char m_message_buffer[message_buffer_size];
    int m_message_buffer_position{ 0 };

    // Store the CRC part of the message as it is beeing received:
    constexpr static int crc_buffer_size{ 8 };
    char m_crc_buffer[crc_buffer_size];
    int m_crc_buffer_position{ 0 };

    // Calculate the CRC while the message is beeing received
    uint16_t m_crc{ 0 };

    // Keeps track of the start of the next line while processing.
    char *m_start_of_line;

    // Keeps track of bytes sent when resending the message
    int m_bytes_resent;

    enum class states {
        READING_MESSAGE,
        READING_CRC,
        PROCESSING,
        RESENDING, // To the optional secondary P1-port
        WAITING,
        ERROR_RECOVERY
    };
    enum states m_state { states::READING_MESSAGE };

    void ChangeState(enum states new_state)
    {
        unsigned long const current_time{ millis() };
        switch (new_state) {
        case states::READING_MESSAGE:
            m_reading_message_time = current_time;
            m_num_message_loops = m_num_processing_loops = 0;
            m_CTS_switch->turn_on();
            if (m_status_switch != nullptr) m_status_switch->turn_on();
            m_crc = m_message_buffer_position = 0;
            break;
        case states::READING_CRC:
            m_reading_crc_time = current_time;
            m_crc_buffer_position = 0;
            break;
        case states::PROCESSING:
            m_processing_time = current_time;
            m_CTS_switch->turn_off();
            m_start_of_line = m_message_buffer;
            break;
        case states::RESENDING:
            m_resending_time = current_time;
            if (!m_secondary_RTS->state) {
                ChangeState(states::WAITING);
                return;
            }
            m_bytes_resent = 0;
            break;
        case states::WAITING:
            if (m_state != states::ERROR_RECOVERY) m_display_time_stats = true;
            m_waiting_time = current_time;
            if (m_status_switch != nullptr) m_status_switch->turn_off();
            break;
        case states::ERROR_RECOVERY:
            m_error_recovery_time = current_time;
            m_CTS_switch->turn_off();
        }
        m_state = new_state;
    }

    // Combine the three values defining a sensor into a single unsigned int for easier
    // handling and comparison
    static uint32_t OBIS(uint32_t major, uint32_t minor, uint32_t micro)
    {
        return (major & 0xfff) << 16 | (minor & 0xff) << 8 | (micro & 0xff);
    }

    class SensorListItem {
        uint32_t const m_obisCode;
        Sensor m_sensor;
        SensorListItem *const m_next{ nullptr };
    public:
        SensorListItem(SensorListItem *next, uint32_t obisCode)
            : m_obisCode(obisCode)
            , m_next(next)
        {}

        Sensor *GetSensor() { return &m_sensor; }
        uint32_t GetCode() const { return m_obisCode; }
        SensorListItem *Next() const { return m_next; }
    };

    // Linked list of all sensors
    SensorListItem *m_sensor_list{ nullptr };

    esphome::gpio::GPIOSwitch *const m_CTS_switch;
    esphome::gpio::GPIOSwitch *const m_status_switch;
    Number const *const m_update_period_number{ nullptr };
    esphome::gpio::GPIOBinarySensor const * const m_secondary_RTS{ nullptr };

public:

    void setup() override
    {
        ChangeState(states::READING_MESSAGE);
    }

    void loop() override {
        unsigned long const loop_start_time{ millis() };
        m_minimum_period_ms = static_cast<unsigned long>(m_update_period_number->state * 1000.0f + 0.5f);
        switch (m_state) {
        case states::READING_MESSAGE:
        case states::READING_CRC:
            ++m_num_message_loops;
            while (available()) {
                // While data is available, read it one byte at a time.
                char const read_byte{ (char)read() };
                if (m_state == states::READING_MESSAGE) {
                    crc16_update(read_byte);
                    if (read_byte == '!') {
                        // The exclamation mark indicates that the main message is complete
                        // and the CRC will come next.
                        m_message_buffer[m_message_buffer_position] = '\0';
                        ChangeState(states::READING_CRC);
                    }
                    else {
                        m_message_buffer[m_message_buffer_position++] = read_byte;
                        if (m_message_buffer_position == message_buffer_size) {
                            ESP_LOGW("p1reader", "Message buffer overrun. Resetting.");
                            ChangeState(states::ERROR_RECOVERY);
                            return;
                        }
                    }
                }
                else { // READING_CRC
                    if (read_byte == '\n') {
                        // The CRC is a single line, so once we reach end of line, we are
                        // ready to verify and process the message.
                        m_crc_buffer[m_crc_buffer_position] = '\0';
                        int const crcFromMsg = (int)strtol(m_crc_buffer, NULL, 16);
                        if (m_crc != crcFromMsg) {
                            ESP_LOGW("p1reader", "CRC missmatch, calculated %04X != %04X. Message ignored.", m_crc, crcFromMsg);
                            ESP_LOGD("p1reader", "Buffer:\n%s", m_message_buffer);
                            ChangeState(states::ERROR_RECOVERY);
                            return;
                        }
                        else {
                            ChangeState(states::PROCESSING);
                            return;
                        }
                    }
                    else {
                        m_crc_buffer[m_crc_buffer_position++] = read_byte;
                        if (m_crc_buffer_position == crc_buffer_size) {
                            ESP_LOGW("p1reader", "CRC buffer overrun. Resetting.");
                            ChangeState(states::ERROR_RECOVERY);
                            return;
                        }
                    }
                }
            }
            break;
        case states::PROCESSING:
            ++m_num_processing_loops;
            do {
                while (*m_start_of_line == '\n' || *m_start_of_line == '\r') ++m_start_of_line;
                char *end_of_line{ m_start_of_line };
                while (*end_of_line != '\n' && *end_of_line != '\r' && *end_of_line != '\0') ++end_of_line;
                char const end_of_line_char{ *end_of_line };
                *end_of_line = '\0';

                if (end_of_line != m_start_of_line) {
                    int minor{ -1 }, major{ -1 }, micro{ -1 };
                    double value{ -1.0 };
                    if (sscanf(m_start_of_line, "1-0:%d.%d.%d(%lf", &major, &minor, &micro, &value) != 4) {
                        ESP_LOGD("p1reader", "Could not parse value from line '%s'", m_start_of_line);
                    }
                    else {
                        uint32_t const obisCode{ OBIS(major, minor, micro) };
                        Sensor *S{ GetSensor(obisCode) };
                        if (S != nullptr) S->publish_state(value);
                        else {
                            ESP_LOGD("p1reader", "No sensor matching: %d.%d.%d (0x%x)", major, minor, micro, obisCode);
                        }
                    }
                }
                *end_of_line = end_of_line_char;
                if (end_of_line_char == '\0') {
                    ChangeState(states::RESENDING);
                    return;
                }
                m_start_of_line = end_of_line + 1;
            } while (millis() - loop_start_time < 25);
            break;
        case states::RESENDING:
            if (m_bytes_resent < m_message_buffer_position) {
                int max_bytes_to_send{ 200 };
                do {
                    write(m_message_buffer[m_bytes_resent++]);
                } while (m_bytes_resent < m_message_buffer_position && max_bytes_to_send-- != 0);
            }
            else {
                write('!');
                char const *crc_pos{ m_crc_buffer };
                while (*crc_pos != '\0') write(*crc_pos++);
                write('\n');
                ChangeState(states::WAITING);
            }
            break;
        case states::WAITING:
            if (m_display_time_stats) {
                m_display_time_stats = false;
                ESP_LOGD("p1reader", "Cycle times: Message = %d ms (%d loops), Processing = %d ms (%d loops), (Total = %d ms)",
                    m_processing_time - m_reading_message_time,
                    m_num_message_loops,
                    m_waiting_time - m_processing_time,
                    m_num_processing_loops,
                    m_waiting_time - m_reading_message_time
                );
            }
            if (m_minimum_period_ms < loop_start_time - m_reading_message_time) {
                ChangeState(states::READING_MESSAGE);
            }
            break;
        case states::ERROR_RECOVERY:
            if (available()) {
                int max_bytes_to_discard{ 200 };
                do { read(); } while (available() && max_bytes_to_discard-- != 0);
            }
            else if (500 < loop_start_time - m_error_recovery_time) ChangeState(states::WAITING);
            break;
        }
    }

private:
    void crc16_update(uint8_t a) {
        int i;
        m_crc ^= a;
        for (i = 0; i < 8; ++i) {
            if (m_crc & 1) {
                m_crc = (m_crc >> 1) ^ 0xA001;
            }
            else {
                m_crc = (m_crc >> 1);
            }
        }
    }


    // Find the matching sensor in the linked list (or return nullptr
    // if it does not exist.
    Sensor *GetSensor(uint32_t obisCode) const
    {
        SensorListItem *sensor_list{ m_sensor_list };
        while (sensor_list != nullptr) {
            if (obisCode == sensor_list->GetCode()) return sensor_list->GetSensor();
            sensor_list = sensor_list->Next();
        }
        return nullptr;
    }

};
