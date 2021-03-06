/* NEC remote infrared RMT example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/rmt.h"
#include "driver/periph_ctrl.h"
#include "soc/rmt_reg.h"

static const char* NEC_TAG = "NEC";

#define RMT_RX_SELF_TEST   0

/******************************************************/
/*****                SELF TEST:                  *****/
/*Connect RMT_TX_GPIO_NUM with RMT_RX_GPIO_NUM        */
/*TX task will send NEC data with carrier disabled    */
/*RX task will print NEC data it receives.            */
/******************************************************/
#if RMT_RX_SELF_TEST
#define RMT_TX_CARRIER_EN    0   /*!< Disable carrier for self test mode  */
#else
//Test with infrared LED, we have to enable carrier for transmitter
//When testing via IR led, the receiver waveform is usually active-low.
#define RMT_TX_CARRIER_EN    1   /*!< Enable carrier for IR transmitter test with IR led */
#endif

#define RMT_TX_CHANNEL    1     /*!< RMT channel for transmitter */
#define RMT_TX_GPIO_NUM  17     /*!< GPIO number for transmitter signal */
#define RMT_CLK_DIV      100    /*!< RMT counter clock divider */
#define RMT_TICK_10_US    (80000000/RMT_CLK_DIV/100000)   /*!< RMT counter value for 10 us.(Source clock is APB clock) */

#define NEC_HEADER_HIGH_US    8400                         /*!< NEC protocol header: positive 9ms */
#define NEC_HEADER_LOW_US     4150                         /*!< NEC protocol header: negative 4.5ms*/
#define NEC_BIT_ONE_HIGH_US    550                         /*!< NEC protocol data bit 1: positive 0.56ms */
#define NEC_BIT_ONE_LOW_US    (2200-NEC_BIT_ONE_HIGH_US)   /*!< NEC protocol data bit 1: negative 1.69ms */
#define NEC_BIT_ZERO_HIGH_US   550                         /*!< NEC protocol data bit 0: positive 0.56ms */
#define NEC_BIT_ZERO_LOW_US   (1100-NEC_BIT_ZERO_HIGH_US)  /*!< NEC protocol data bit 0: negative 0.56ms */
#define NEC_BIT_END            560                         /*!< NEC protocol end: positive 0.56ms */

#define NEC_ITEM_DURATION(d)  ((d & 0x7fff)*10/RMT_TICK_10_US)  /*!< Parse duration time from memory register value */


#define rmt_item32_tIMEOUT_US  9500   /*!< RMT receiver timeout value(us) */

/*
   @brief Build register value of waveform for NEC one data bit
*/
static inline void nec_fill_item_level(rmt_item32_t* item, int high_us, int low_us)
{
  ESP_LOGD(NEC_TAG, "ADDING: high_us: %d, low_us: %d", high_us, low_us);
  item->level0 = 1;
  item->duration0 = (high_us) / 10 * RMT_TICK_10_US;
  item->level1 = 0;
  item->duration1 = (low_us) / 10 * RMT_TICK_10_US;
}

/*
   @brief Generate NEC header value: active 9ms + negative 4.5ms
*/
static void nec_fill_item_header(rmt_item32_t* item)
{
  nec_fill_item_level(item, NEC_HEADER_HIGH_US, NEC_HEADER_LOW_US);
}

/*
   @brief Generate NEC data bit 1: positive 0.56ms + negative 1.69ms
*/
static void nec_fill_item_bit_one(rmt_item32_t* item)
{
  nec_fill_item_level(item, NEC_BIT_ONE_HIGH_US, NEC_BIT_ONE_LOW_US);
}

/*
   @brief Generate NEC data bit 0: positive 0.56ms + negative 0.56ms
*/
static void nec_fill_item_bit_zero(rmt_item32_t* item)
{
  nec_fill_item_level(item, NEC_BIT_ZERO_HIGH_US, NEC_BIT_ZERO_LOW_US);
}

/*
   @brief Generate NEC end signal: positive 0.56ms
*/
static void nec_fill_item_end(rmt_item32_t* item)
{
  nec_fill_item_level(item, NEC_BIT_END, 0x7fff);
}

/*
   @brief Check whether duration is around target_us
*/
inline bool nec_check_in_range(int duration_ticks, int target_us, int margin_us)
{
  if (( NEC_ITEM_DURATION(duration_ticks) < (target_us + margin_us))
      && ( NEC_ITEM_DURATION(duration_ticks) > (target_us - margin_us))) {
    return true;
  } else {
    return false;
  }
}

/*
   @brief Build NEC 32bit waveform.
*/
static int nec_build_items(int channel, rmt_item32_t* item, int item_num, uint8_t* data_array)
{
  int i = 0;
  nec_fill_item_header(item++);
  i++;
  bool need_stop = false;
  while (!need_stop) {
    uint8_t data_byte = *data_array;
    data_array++;
    for (int j = 0; j < 8; j++) {
      if (data_byte & 0x80) {
        nec_fill_item_bit_one(item);
      } else {
        nec_fill_item_bit_zero(item);
      }
      item++;
      i++;
      data_byte <<= 1;
      if (i >= item_num - 1) {
        need_stop = true;
        break;
      }
    }
  }
  nec_fill_item_end(item);
  i++;
  return i;
}

/*
   @brief RMT transmitter initialization
*/
static void nec_tx_init()
{
  rmt_config_t rmt_tx;
  rmt_tx.channel = (rmt_channel_t)RMT_TX_CHANNEL;
  rmt_tx.gpio_num = (gpio_num_t)RMT_TX_GPIO_NUM;
  rmt_tx.mem_block_num = 1;
  rmt_tx.clk_div = RMT_CLK_DIV;
  rmt_tx.tx_config.loop_en = false;
  rmt_tx.tx_config.carrier_duty_percent = 50;
  rmt_tx.tx_config.carrier_freq_hz = 38000;
  rmt_tx.tx_config.carrier_level = (rmt_carrier_level_t)1;
  rmt_tx.tx_config.carrier_en = RMT_TX_CARRIER_EN;
  rmt_tx.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  rmt_tx.tx_config.idle_output_en = true;
  rmt_tx.rmt_mode = RMT_MODE_TX;
  rmt_config(&rmt_tx);
  rmt_driver_install(rmt_tx.channel, 0, 0);
}

/**
   @brief RMT transmitter demo, this task will periodically send NEC data. (100 * 32 bits each time.)

*/
static void rmt_example_nec_tx_task(void*)
{
  vTaskDelay(10);
  nec_tx_init();
  esp_log_level_set(NEC_TAG, ESP_LOG_INFO);
  int channel = RMT_TX_CHANNEL;
  uint8_t data_array[15] = {
    0x6A, 0xAE, 0x00, 0x00,
    0x4C, 0x43, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xB8
  };
  for (;;) {
    int NEC_DATA_ITEM_NUM = sizeof(data_array) * 8 + 2; // NEC code item number: header + 120 bit data + end
    ESP_LOGI(NEC_TAG, "RMT TX DATA");
    size_t size = (sizeof(rmt_item32_t) * NEC_DATA_ITEM_NUM );
    rmt_item32_t* items = (rmt_item32_t*) malloc(size);
    int item_num = NEC_DATA_ITEM_NUM ;
    memset((void*) items, 0, size);
    //To build a series of waveforms.
    nec_build_items(channel, items, item_num, data_array);
    // dump
    for (int i = 0; i < item_num; i++) {
      rmt_item32_t rmt = items[i];
      // ESP_LOGD(NEC_TAG, "%d: level0=%d, duration0=%d, level1=%d, duration1=%d", i, rmt.level0,  rmt.duration0, rmt.level1, rmt.duration1);
    }
    //To send data according to the waveform items.
    rmt_write_items((rmt_channel_t)channel, items, item_num, true);
    //Wait until sending is done.
    rmt_wait_tx_done((rmt_channel_t)channel, portMAX_DELAY);
    //before we free the data, make sure sending is already done.
    free(items);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void setup() {
  xTaskCreate(rmt_example_nec_tx_task, "rmt_nec_tx_task", 2048, NULL, 10, NULL);
}

void loop() {
}
