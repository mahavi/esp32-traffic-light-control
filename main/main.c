#include "configuration.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "traffic_light.h"

static void loop(void)
{
  while (1)
  {
    traffic_light_process();
    vTaskDelay(pdMS_TO_TICKS(TRAFFIC_LIGHT_PROCESS_INTERVAL_MS));
  }
}

static void setup(void)
{
  traffic_light_init();
  traffic_light_set_mode(TRAFFIC_LIGHT_MODE_OFF);
}

void app_main(void)
{
  setup();
  loop();
}
