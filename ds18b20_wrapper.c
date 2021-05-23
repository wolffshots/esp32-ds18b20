/*
 * MIT License
 *
 * Copyright (c) 2017-2019 David Antliff
 * Copyright (c) 2021 wolffshots
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * 
 * A note on the copyright line: https://www.copyright.gov/title17/92chap4.html#408
 */

/**
 * @file ds18b20_wrapper.c
 * @brief implementation for wrapper component to help setup and interface with temp sensor
 */

#include <stdbool.h>

#include "ds18b20_wrapper.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"

#include "owb.h"
#include "owb.h"
#include "ds18b20.h"

#define GPIO_DS18B20_0 (CONFIG_TEMP_OWB_GPIO)          ///< the gpio pin to search for sensors on
#define MAX_DEVICES (CONFIG_TEMP_MAX_DEVS)             ///< maximum number of devices to search for
#define DS18B20_RESOLUTION (DS18B20_RESOLUTION_12_BIT) ///< the resolution of the temp sensor

OneWireBus *owb;                                  ///< onewire bus pointer
int num_devices = 0;                              ///< current number of devices found
DS18B20_Info *devices[MAX_DEVICES] = {0};         ///< list of devices
owb_rmt_driver_info rmt_driver_info;              ///< the rmt driver info for communicating over the owb
static const char *TAG = CONFIG_TEMP_WRAPPER_TAG; ///< tag for logging

/**
 * @brief init the sensor
 * intitialises the onewire bus and finds and intialises ds18b20 sensors along the pin
 * @return the number of devices it found on the bus as an int
 */
int ds18b20_wrapped_init(void)
{
    ESP_LOGI(TAG, "setting up temp sensor");
    owb = owb_rmt_initialize(&rmt_driver_info, GPIO_DS18B20_0, RMT_CHANNEL_1, RMT_CHANNEL_0);
    owb_use_crc(owb, true); // enable CRC check for ROM code

    // Find all connected devices
    ESP_LOGD(TAG, "find devices:");
    OneWireBus_ROMCode device_rom_codes[MAX_DEVICES] = {0};

    OneWireBus_SearchState search_state = {0};
    bool found = false;
    owb_search_first(owb, &search_state, &found);
    while (found)
    {
        char rom_code_s[17];
        owb_string_from_rom_code(search_state.rom_code, rom_code_s, sizeof(rom_code_s));
        ESP_LOGD(TAG, "  %d : %s", num_devices, rom_code_s);
        device_rom_codes[num_devices] = search_state.rom_code;
        ++num_devices;
        owb_search_next(owb, &search_state, &found);
    }
    ESP_LOGI(TAG, "found %d device%s", num_devices, num_devices == 1 ? "" : "s");

    // In this example, if a single device is present, then the ROM code is probably
    // not very interesting, so just print it out. If there are multiple devices,
    // then it may be useful to check that a specific device is present.

    if (num_devices == 1)
    {
        // For a single device only:
        OneWireBus_ROMCode rom_code;
        owb_status status = owb_read_rom(owb, &rom_code);
        if (status == OWB_STATUS_OK)
        {
            char rom_code_s[OWB_ROM_CODE_STRING_LENGTH];
            owb_string_from_rom_code(rom_code, rom_code_s, sizeof(rom_code_s));
            ESP_LOGD(TAG, "single device %s present", rom_code_s);
        }
        else
        {
            ESP_LOGE(TAG, "an error occurred reading ROM code: %d", status);
        }
    }
    else
    {
        // Search for a known ROM code (LSB first):
        // For example: 0x1502162ca5b2ee28
        OneWireBus_ROMCode known_device = {
            .fields.family = {0x28},
            .fields.serial_number = {0xee, 0xb2, 0xa5, 0x2c, 0x16, 0x02},
            .fields.crc = {0x15},
        };
        char rom_code_s[OWB_ROM_CODE_STRING_LENGTH];
        owb_string_from_rom_code(known_device, rom_code_s, sizeof(rom_code_s));
        bool is_present = false;

        owb_status search_status = owb_verify_rom(owb, known_device, &is_present);
        if (search_status == OWB_STATUS_OK)
        {
            ESP_LOGD(TAG, "device %s is %s", rom_code_s, is_present ? "present" : "not present");
        }
        else
        {
            ESP_LOGE(TAG, "an error occurred searching for known device: %d", search_status);
        }
    }

    // Create DS18B20 devices on the 1-Wire bus

    for (int i = 0; i < num_devices; ++i)
    {
        DS18B20_Info *ds18b20_info = ds18b20_malloc(); // heap allocation
        devices[i] = ds18b20_info;

        if (num_devices == 1)
        {
            ESP_LOGI(TAG, "single device optimisations enabled");
            ds18b20_init_solo(ds18b20_info, owb); // only one device on bus
        }
        else
        {
            ds18b20_init(ds18b20_info, owb, device_rom_codes[i]); // associate with bus and device
        }
        ds18b20_use_crc(ds18b20_info, true); // enable CRC check on all reads
        ds18b20_set_resolution(ds18b20_info, DS18B20_RESOLUTION);
    }

    // Check for parasitic-powered devices
    bool parasitic_power = false;
    ds18b20_check_for_parasite_power(owb, &parasitic_power);
    if (parasitic_power)
    {
        ESP_LOGI(TAG, "parasitic-powered devices detected");
    }

    // In parasitic-power mode, devices cannot indicate when conversions are complete,
    // so waiting for a temperature conversion must be done by waiting a prescribed duration
    owb_use_parasitic_power(owb, parasitic_power);

#ifdef CONFIG_ENABLE_STRONG_PULLUP_GPIO
    // An external pull-up circuit is used to supply extra current to OneWireBus devices
    // during temperature conversions.
    owb_use_strong_pullup_gpio(owb, CONFIG_STRONG_PULLUP_GPIO);
#endif

    ESP_LOGI(TAG, "finished sensor init");
    return num_devices;
}
/**
 * @brief deinit the sensor
 * cleans up and frees all of the devices and the onewire bus
 */
void ds18b20_wrapped_deinit(void)
{
    ESP_LOGI(TAG, "temp deinit start");

    // clean up dynamically allocated data
    for (int i = 0; i < num_devices; ++i)
    {
        ds18b20_free(&devices[i]);
    }
    owb_uninitialize(owb);

    ESP_LOGI(TAG, "temp deinit end");

    fflush(stdout);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}
/**
 * @brief print the temps
 * runs conversion on all the owb devices, waits for the conversion and then
 * prints out the results when it receives them
 */
void ds18b20_wrapped_read(void)
{
    ESP_LOGD(TAG, "temp read");
    // Read temperatures more efficiently by starting conversions on all devices at the same time
    int errors_count[MAX_DEVICES] = {0};
    int sample_count = 0;
    if (num_devices > 0)
    {
        TickType_t last_wake_time = xTaskGetTickCount();

        ds18b20_convert_all(owb);

        // In this application all devices use the same resolution,
        // so use the first device to determine the delay
        ds18b20_wait_for_conversion(devices[0]);

        // Read the results immediately after conversion otherwise it may fail
        float readings[MAX_DEVICES] = {0};
        DS18B20_ERROR errors[MAX_DEVICES] = {0};

        for (int i = 0; i < num_devices; ++i)
        {
            errors[i] = ds18b20_read_temp(devices[i], &readings[i]);
        }

        // Print results in a separate loop, after all have been read
        ESP_LOGI(TAG, "temperature readings (degrees C): sample %d", ++sample_count);
        for (int i = 0; i < num_devices; ++i)
        {
            if (errors[i] != DS18B20_OK)
            {
                ++errors_count[i];
            }

            ESP_LOGI(TAG, "  %d: %.1f    %d errors", i, readings[i], errors_count[i]);
        }

        vTaskDelayUntil(&last_wake_time, CONFIG_TEMP_SAMPLE_PERIOD / portTICK_PERIOD_MS);
    }
    else
    {
        ESP_LOGE(TAG, "no DS18B20 devices detected!");
    }
}
/**
 * @brief capture temps to results
 * this function runs conversion on all the owb devices, waits for conversion to 
 * finish and then reads the temperatures into the provided results array
 *  
 * @param[in] results a pointer to the array to capture results to
 * @param[out] results the array pointer that has been populated with data
 * @param size the number of devices found and the size of the results array
 */
void ds18b20_wrapped_capture(float *results, int size)
{
    if (size > 0)
    {
        TickType_t last_wake_time = xTaskGetTickCount();
        ds18b20_convert_all(owb);
        ds18b20_wait_for_conversion(devices[0]);
        for (int i = 0; i < size; ++i)
        {
            ds18b20_read_temp(devices[i], &results[i]);
        }
        vTaskDelayUntil(&last_wake_time, CONFIG_TEMP_SAMPLE_PERIOD / portTICK_PERIOD_MS);
    }
    else
    {
        ESP_LOGE(TAG, "no DS18B20 devices detected or invalid size provided");
    }
}