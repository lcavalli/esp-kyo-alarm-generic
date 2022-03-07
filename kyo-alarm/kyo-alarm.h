/*
* esp-kyo-alarm - ESPHome custom component for KYO alarm panels
* Copyright (C) 2021 Luca Cavalli
* 
* This program is free software: you can redistribute it and/or modify it under
* the terms of the GNU General Public License as published by the Free Software
* Foundation, either version 3 of the License, or (at your option) any later version.
* 
* This program is distributed in the hope that it will be useful, but WITHOUT ANY
* WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
* PARTICULAR PURPOSE. See the GNU General Public License for more details.
* 
* You should have received a copy of the GNU General Public License along with this
* program. If not, see <https://www.gnu.org/licenses/>.
* 
* SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <sstream>
#include "esphome.h"
#include "esp8266_mutex.h"

#define LOG_TAG "esp-key-alarm"

#define UPDATE_INT_MS 2000

#define KYO_MODEL_4   "KYO4"
#define KYO_MODEL_8   "KYO8"
#define KYO_MODEL_8G  "KYO8G"
#define KYO_MODEL_32  "KYO32"
#define KYO_MODEL_32G "KYO32G"
#define KYO_MODEL_8W  "KYO8 W"
#define KYO_MODEL_8GW "KYO8G W"

#define KYO_MAX_ZONES 32

#define KYO_STORED_PINS 63

/* 
 * Kyo Protocol
 * ESP8266 and Kyo are little-endian
 * Request format: <cmd><addr><len><data><cksum>
 * uint8_t cmd: 0xf0 - read, 0x0f - write
 * uint16_t addr
 * uint16_t length - 2
 * variable data
 * uint8_t cksum % 0xff
 */

class KyoAlarmComponent : public esphome::PollingComponent, public uart::UARTDevice, public api::CustomAPIDevice {
    public:
        TextSensor *alarmStatusSensor = new TextSensor();
        TextSensor *modelSensor = new TextSensor();
        TextSensor *firmwareSensor = new TextSensor();
        BinarySensor *zoneSensor = new BinarySensor[KYO_MAX_ZONES];
        BinarySensor *zTamperSensor = new BinarySensor[KYO_MAX_ZONES];
        Sensor *warningSensor = new Sensor();
        Sensor *tamperSensor = new Sensor();
        std::vector<switch_::Switch *> zoneSwitches;

        KyoAlarmComponent(UARTComponent *parent) : UARTDevice(parent) {}

        void setup() override {
            set_update_interval(UPDATE_INT_MS);
            set_setup_priority(setup_priority::AFTER_CONNECTION);

            // Create UART mutex
            CreateMutux(&uartMutex);

            // Register services
            register_service(&KyoAlarmComponent::onAlarmDisarm, "disarm", {"code"});
            register_service(&KyoAlarmComponent::onAlarmArmHome, "arm_home", {"code"});
            register_service(&KyoAlarmComponent::onAlarmArmAway, "arm_away", {"code"});
            register_service(&KyoAlarmComponent::onAlarmReset, "reset");

            // Set initial state
            alarmStatusSensor->publish_state("unavailable");
        }

        void update() override {
            if (GetMutex(&uartMutex) == true) {
                if (alarmModel == AlarmModel::UNKNOWN) {
                    getAlarmInfo();
                } else if (partsList == 0) {
                    getPartitionsList();
                } else {
                    static uint32_t tick = 0;

                    if ((tick++ % 2) == 0) {
                        getStatus();
                    } else {
                        getRealTimeStatus();
                    }
                }

                // Release UART mutex
                ReleaseMutex(&uartMutex);
            }
        }

        void bypassZone(uint32_t zoneId, bool bypassFlag) {
            std::vector<uint8_t> request = cmdZoneBypass;
            std::vector<uint8_t> requestData = cmdZoneBypassData;
            std::vector<uint8_t> requestClose = cmdClose;
            std::vector<uint8_t> reply;
            uint32_t data = 0;

            if(alarmStatus == AlarmStatus::DISARMED) {
                // Obtain UART mutex
                while (GetMutex(&uartMutex) == false) {
                    delay(UPDATE_INT_MS / 10);
                }

                data = 1 << zoneId;

                if (bypassFlag == true) {
                    requestData[0] = (data >> 24) & 0xff;
                    requestData[1] = (data >> 16) & 0xff;
                    requestData[2] = (data >> 8) & 0xff;
                    requestData[3] = data & 0xff;
                } else {
                    requestData[4] = (data >> 24) & 0xff;
                    requestData[5] = (data >> 16) & 0xff;
                    requestData[6] = (data >> 8) & 0xff;
                    requestData[7] = data & 0xff;
                }

                appendChecksum(request);
                appendChecksum(requestData);
                request.insert(request.end(), requestData.begin(), requestData.end());

                if (sendRequest(request, reply, 100) && reply.size() == RPL_ZONE_BYPASS_SIZE) {
                    appendChecksum(requestClose);
                    sendRequest(requestClose, reply, 100);
                } else {
                    ESP_LOGE(LOG_TAG, "Bypass zone request failed");
                }

                // Release UART mutex
                ReleaseMutex(&uartMutex);
            }
        }

    private:
        const std::vector<uint8_t> cmdGetAlarmInfo = {0xf0, 0x00, 0x00, 0x0b, 0x00};
        const size_t RPL_GET_ALARM_INFO_SIZE = 13;

        const std::vector<uint8_t> cmdGetPinsList1 = {0xf0, 0xb4, 0x01, 0x3f, 0x00};
        const size_t RPL_GET_PINS_LIST_1_SIZE = 65;
        const std::vector<uint8_t> cmdGetPinsList2 = {0xf0, 0xf4, 0x01, 0x07, 0x00};
        const size_t RPL_GET_PINS_LIST_2_SIZE = 9;

        const std::vector<uint8_t> cmdGetPartitionsList = {0xf0, 0xff, 0x01, 0x01, 0x00};
        const size_t RPL_GET_PARTITIONS_LIST_SIZE = 3;

        const std::vector<uint8_t> cmdGetRealTimeStatus = {0xf0, 0x04, 0xf0, 0x0a, 0x00};
        const size_t RPL_GET_REAL_TIME_STATUS_SIZE = 12;

        const std::vector<uint8_t> cmdGetStatus = {0xf0, 0x02, 0x15, 0x12, 0x00};
        const size_t RPL_GET_STATUS_SIZE = 20;

        const std::vector<uint8_t> cmdCtrlPartitions = {0x0f, 0x00, 0xf0, 0x03, 0x00};
        const std::vector<uint8_t> cmdCtrlPartitionsData = {0x00, 0x00, 0x00, 0x00};
        const size_t RPL_CTRL_PARTITIONS_SIZE = 0;

        const std::vector<uint8_t> cmdZoneBypass = {0x0f, 0x01, 0xf0, 0x07, 0x00};
        const std::vector<uint8_t> cmdZoneBypassData = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const size_t RPL_ZONE_BYPASS_SIZE = 0;

        const std::vector<uint8_t> cmdReset = {0x0f, 0x05, 0xf0, 0x01, 0x00};
        const std::vector<uint8_t> cmdResetData = {0xff, 0x00};
        const size_t RPL_RESET_SIZE = 0;

        const std::vector<uint8_t> cmdClose = {0x3c, 0x03, 0x00, 0x00, 0x00};
        const size_t RPL_CLOSE_SIZE = 0;

        mutex_t uartMutex;

        uint8_t pinsList[KYO_STORED_PINS * 3] = {0};
        uint8_t partsList = 0;

        enum class AlarmModel {UNKNOWN, KYO_4, KYO_8, KYO_8G, KYO_32, KYO_32G, KYO_8W, KYO_8GW};
        AlarmModel alarmModel = AlarmModel::UNKNOWN;

        enum class AlarmStatus {UNAVAILABLE, PENDING, ARMING, ARMED_AWAY, ARMED_HOME, DISARMED, TRIGGERED};
        AlarmStatus alarmStatus = AlarmStatus::UNAVAILABLE;

        void onAlarmReset() {
            std::vector<uint8_t> request = cmdReset;
            std::vector<uint8_t> requestData = cmdResetData;
            std::vector<uint8_t> requestClose = cmdClose;
            std::vector<uint8_t> reply;

            // Obtain UART mutex
            while (GetMutex(&uartMutex) == false) {
                delay(UPDATE_INT_MS / 10);
            }

            requestData[0] = partsList;

            appendChecksum(request);
            appendChecksum(requestData);
            request.insert(request.end(), requestData.begin(), requestData.end());

            if (sendRequest(request, reply, 500) && reply.size() == RPL_RESET_SIZE) {
                appendChecksum(requestClose);
                sendRequest(requestClose, reply, 100);
            } else {
                ESP_LOGE(LOG_TAG, "Reset alarm request failed");
            }

            // Release UART mutex
            ReleaseMutex(&uartMutex);
        }

        void onAlarmDisarm(const std::string code) {
            processCommandRequest("disarm", code);
        }

        void onAlarmArmHome(const std::string code) {
            processCommandRequest("arm_home", code);
        }

        void onAlarmArmAway(const std::string code) {
            processCommandRequest("arm_away", code);
        }

        void processCommandRequest(const std::string action, const std::string code) {
            std::vector<uint8_t> request = cmdCtrlPartitions;
            std::vector<uint8_t> requestData = cmdCtrlPartitionsData;
            std::vector<uint8_t> requestClose = cmdClose;
            std::vector<uint8_t> reply;

            // Obtain UART mutex
            while (GetMutex(&uartMutex) == false) {
                delay(UPDATE_INT_MS / 10);
            }

            // Verify PIN
            if (verifyPin(code) == true) {
                // Build request data
                if (action == "arm_home") {
                    // Arm home partitions request
                    requestData[0] = armed_home->value() & partsList;
                    alarmStatusSensor->publish_state("arming");
                    alarmStatus = AlarmStatus::ARMING;
                } else if (action == "arm_away") {
                    // Arm away partitions request
                    requestData[0] = armed_away->value() & partsList;
                    alarmStatusSensor->publish_state("arming");
                    alarmStatus = AlarmStatus::ARMING;
                } else if (action == "disarm") {
                    // Disarm partitions request
                    requestData[3] = partsList;
                    alarmStatusSensor->publish_state("pending");
                    alarmStatus = AlarmStatus::PENDING;
                } else {
                    // Release UART mutex
                    ReleaseMutex(&uartMutex);
                    return;
                }

                appendChecksum(request);
                appendChecksum(requestData);
                request.insert(request.end(), requestData.begin(), requestData.end());

                if (sendRequest(request, reply, 1000) && reply.size() == RPL_CTRL_PARTITIONS_SIZE) {
                    appendChecksum(requestClose);
                    sendRequest(requestClose, reply, 100);
                } else {
                    ESP_LOGE(LOG_TAG, "Process command request failed");
                }
            }

            // Release UART mutex
            ReleaseMutex(&uartMutex);
        }

        bool getAlarmInfo() {
            std::vector<uint8_t> request = cmdGetAlarmInfo;
            std::vector<uint8_t> reply;

            appendChecksum(request);

            if (sendRequest(request, reply, 100) && reply.size() == RPL_GET_ALARM_INFO_SIZE) {
                // Parse reply
                std::ostringstream convert;
                for (int i = 0; i < RPL_GET_ALARM_INFO_SIZE - 1; i++) {
                    convert << reply[i];
                }

                // Trim spaces from model sub-string
                std::string model = convert.str().substr(0, 7);
                rtrim(model);

                // Trim spaces from firmware substring
                std::string firmware = convert.str().substr(8, 11);
                rtrim(firmware);

                modelSensor->publish_state(model.c_str());
                firmwareSensor->publish_state(firmware.c_str());

                if (model == KYO_MODEL_4) alarmModel = AlarmModel::KYO_4;
                else if (model == KYO_MODEL_8) alarmModel = AlarmModel::KYO_8;
                else if (model == KYO_MODEL_8G) alarmModel = AlarmModel::KYO_8G;
                else if (model == KYO_MODEL_32) alarmModel = AlarmModel::KYO_32;
                else if (model == KYO_MODEL_32G) alarmModel = AlarmModel::KYO_32G;
                else if (model == KYO_MODEL_8W) alarmModel = AlarmModel::KYO_8W;
                else if (model == KYO_MODEL_8GW) alarmModel = AlarmModel::KYO_8GW;
                else alarmModel = AlarmModel::UNKNOWN;

                ESP_LOGCONFIG(LOG_TAG, "KYO model request completed [%s %s]", model.c_str(), firmware.c_str());
                return (true);
            }

            ESP_LOGE(LOG_TAG, "KYO model request failed");
            return (false);
        }

        bool getPartitionsList() {
            std::vector<uint8_t> request = cmdGetPartitionsList;
            std::vector<uint8_t> reply;

            appendChecksum(request);

            if (sendRequest(request, reply, 100) && reply.size() == RPL_GET_PARTITIONS_LIST_SIZE) {
                // Parse reply
                partsList = reply[1];

                ESP_LOGCONFIG(LOG_TAG, "Partitions list request completed [" BYTE_TO_BINARY_PATTERN "]", BYTE_TO_BINARY(partsList));
                ESP_LOGCONFIG(LOG_TAG, "Arm home partitions [" BYTE_TO_BINARY_PATTERN "]", BYTE_TO_BINARY(armed_home->value() & partsList));
                ESP_LOGCONFIG(LOG_TAG, "Arm away partitions [" BYTE_TO_BINARY_PATTERN "]", BYTE_TO_BINARY(armed_away->value() & partsList));
                return (true);
            }

            ESP_LOGE(LOG_TAG, "Partitions list request failed");
            return (false);
        }

        bool getRealTimeStatus () {
            std::vector<uint8_t> request = cmdGetRealTimeStatus;
            std::vector<uint8_t> reply;

            appendChecksum(request);

            if (sendRequest(request, reply, 1000) && reply.size() == RPL_GET_REAL_TIME_STATUS_SIZE) {
                uint32_t info = 0;

                // Parse zones alarm status
                info = (reply[0] << 24) | (reply[1] << 16) | (reply[2] << 8) | reply[3];
                for (int i = 0; i < KYO_MAX_ZONES; i++) {
                    zoneSensor[i].publish_state((info >> i) & 0x01);
                }

                // Parse zones tamper status
                info = (reply[4] << 24) | (reply[5] << 16) | (reply[6] << 8) | reply[7];
                for (int i = 0; i < KYO_MAX_ZONES; i++) {
                    zTamperSensor[i].publish_state((info >> i) & 0x01);
                }

                // Parse partitions alarm status
                if (reply[9] > 0) {
                    // At least one partition in alarm status
                    if (alarmStatus != AlarmStatus::TRIGGERED) {
                        alarmStatusSensor->publish_state("triggered");
                        alarmStatus = AlarmStatus::TRIGGERED;
                    }
                } else {
                    // Alarm status reset, next getStatus() call will set proper alarm status
                    if (alarmStatus == AlarmStatus::TRIGGERED) {
                        alarmStatusSensor->publish_state("pending");
                        alarmStatus = AlarmStatus::PENDING;
                    }
                }

                // Parse warnings and tampers
                warningSensor->publish_state(reply[8]);
                tamperSensor->publish_state(reply[10]);
                return (true);
            }

            ESP_LOGE(LOG_TAG, "Real-time status request failed");
            return (false);
        }

        bool getStatus () {
            std::vector<uint8_t> request = cmdGetStatus;
            std::vector<uint8_t> reply;

            appendChecksum(request);

            if (sendRequest(request, reply, 1000) && reply.size() == RPL_GET_STATUS_SIZE) {
                uint8_t armed = 0;
                uint8_t disarmed = 0;
                uint32_t info = 0;

                if (alarmStatus != AlarmStatus::TRIGGERED) {
                    // Parse armed partitions (away, stay and stay 0 delay modes are all armed)
                    armed = (reply[0] | reply[1] | reply[2]) & partsList;

                    // Parse disarmed partitions
                    disarmed = reply[3] & partsList;

                    // Publish alarm status
                    if ((disarmed == partsList) && (alarmStatus != AlarmStatus::DISARMED)) {
                        // All partitions are disarmed
                        alarmStatusSensor->publish_state("disarmed");
                        alarmStatus = AlarmStatus::DISARMED;
                    } else if ((armed == (armed_home->value() & partsList)) && (alarmStatus != AlarmStatus::ARMED_HOME)) {
                        // All partitions are armed home
                        alarmStatusSensor->publish_state("armed_home");
                        alarmStatus = AlarmStatus::ARMED_HOME;
                    } else if ((armed == (armed_away->value() & partsList)) && (alarmStatus != AlarmStatus::ARMED_AWAY)) {
                        // All partitions are armed away
                        alarmStatusSensor->publish_state("armed_away");
                        alarmStatus = AlarmStatus::ARMED_AWAY;
                    }
                }

                // Parse bypassed zones
                info = (reply[7] << 24) | (reply[8] << 16) | (reply[9] << 8) | reply[10];
                for (int i = 0; i < KYO_MAX_ZONES; i++) {
                    zoneSwitches[i]->publish_state((info >> i) & 0x01);
                }

                // TODO: Publish also:
                //       - Outputs status
                //       - Alarm memory zones
                //       - Tamper memory zones

                return (true);
            }

            ESP_LOGE(LOG_TAG, "Status request failed");
            return (false);
        }

        bool getPinsList() {
            std::vector<uint8_t> request1 = cmdGetPinsList1;
            std::vector<uint8_t> request2 = cmdGetPinsList2;
            std::vector<uint8_t> reply1;
            std::vector<uint8_t> reply2;

            appendChecksum(request1);
            appendChecksum(request2);

            if (sendRequest(request1, reply1, 500) && reply1.size() == RPL_GET_PINS_LIST_1_SIZE) {
                if (sendRequest(request2, reply2, 500) && reply2.size() == RPL_GET_PINS_LIST_2_SIZE) {
                    memcpy(pinsList, reply1.data(), RPL_GET_PINS_LIST_1_SIZE - 1);
                    memcpy(&pinsList[RPL_GET_PINS_LIST_1_SIZE - 1], reply2.data(), RPL_GET_PINS_LIST_2_SIZE - 1);

                    return (true);
                }
            }

            ESP_LOGE(LOG_TAG, "PINs list request failed");
            return (false);
        }

        bool verifyPin(std::string pin) {
            uint32_t pinCode = 0;

            // Check PIN size (4 - 6 digits)
            if ((pin.length() > 3) && (pin.length() < 7) && (std::all_of(pin.begin(), pin.end(), ::isdigit))) {
                // Pad PIN to 6 digits with 'f'
                pin.insert(pin.end(), 6 - pin.length(), 'f');

                // Encode PIN as unsigned 32 bit integer
                for (char c: pin) {
                    pinCode <<= 4;
                    pinCode |= (c == 'f') ? 0xf : c - 0x30;
                }

                // Read PIN list from alarm
                if (getPinsList() == true) {
                    for (int i = 0; i < KYO_STORED_PINS * 3; i += 3) {
                        uint32_t pinRef = 0;

                        // Encode reference PIN as unsigned 32 bit integer
                        pinRef |= (pinsList[i] << 16) & 0x00FF0000;
                        pinRef |= (pinsList[i + 1] << 8) & 0x0000FF00;
                        pinRef |=  pinsList[i + 2] & 0x000000FF;

                        if (pinCode == pinRef) {
                            return (true);
                        }
                    }
                }

                ESP_LOGW(LOG_TAG, "Unknown PIN provided.");
                return (false);
            }

            ESP_LOGE(LOG_TAG, "Invalid PIN provided.");
            return (false);
        }

        uint8_t getChecksum(const std::vector<uint8_t> &data, size_t offset = 0) {
            uint8_t ckSum = 0;

            for (int i = offset; i < data.size(); i++) {
                ckSum += data[i];
            }

            return (ckSum);
        }

        void appendChecksum(std::vector<uint8_t> &data, size_t offset = 0) {
            data.push_back(getChecksum(const_cast<std::vector<uint8_t>&>(data), offset));
        }

        bool verifyChecksum(const std::vector<uint8_t> &data, size_t offset = 0) {
            uint8_t ckSum = 0;

            for (int i = offset; i < data.size() - 1; i++) {
                ckSum += data[i];
            }

            return (data.back() == ckSum);
        }

        static inline void rtrim(std::string &str) {
            str.erase(std::find_if(str.rbegin(), str.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), str.end());
        }

        bool sendRequest(std::vector<uint8_t> request, std::vector<uint8_t> &reply, uint wait = 0) {
            ESP_LOGD(LOG_TAG, "Request: %s", format_hex_pretty(request).c_str());

            // Empty receiveing buffer
            while (available() > 0) {
                read();
            }

            // Send request
            write_array(reinterpret_cast<byte*> (request.data()), request.size());

            delay(wait);

            // Read reply
            reply.clear();

            // Wait for request echo
            if (available() >= 6) {
                while (available() > 0) {
                    reply.push_back(read());
                }

                // Strip request echo
                reply.erase(reply.begin(), reply.begin() + 6);

                if (reply.size() > 0) {
                    ESP_LOGD(LOG_TAG, "Reply: %s", format_hex_pretty(reply).c_str());

                    // Verify checksum
                    return (verifyChecksum(const_cast<std::vector<uint8_t>&>(reply)));
                }

                return (true);
            }

            return (false);
        }
};

class KyoZoneSwitch : public esphome::Component, public switch_::Switch {
    public:
        KyoZoneSwitch(int id) {
            zoneId = id;
        }

        void setup() override {
        }

        void write_state(bool state) override {
            if (zoneId < KYO_MAX_ZONES) {
                ((KyoAlarmComponent*) kyo)->bypassZone(zoneId, state);
            }
        }

    private:
        uint32_t zoneId;
};
