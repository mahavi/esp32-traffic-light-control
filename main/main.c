#include <stdio.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tlc";

static void setup(void)
{
}

static void loop(void)
{
  while(1)
  {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void app_main(void)
{
  setup();
  loop();
}
