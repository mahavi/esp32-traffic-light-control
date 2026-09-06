#include "traffic_light.h"

#include <stdbool.h>
#include <stdint.h>

#include "configuration.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#define MILLISECONDS_TO_TICKS(milliseconds) \
  ((uint64_t)(milliseconds) * TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ / 1000U)

typedef enum
{
  TRAFFIC_LIGHT_STATE_GREEN,
  TRAFFIC_LIGHT_STATE_GREEN_BLINKING,
  TRAFFIC_LIGHT_STATE_YELLOW,
  TRAFFIC_LIGHT_STATE_RED,
  TRAFFIC_LIGHT_STATE_RED_YELLOW
} traffic_light_state_t;

static const char *TAG = "traffic_light";
static gptimer_handle_t s_state_timer;
static gptimer_handle_t s_blink_timer;
static gptimer_handle_t s_pedestrian_debounce_timer;
static esp_timer_handle_t s_debounce_timer;

static volatile bool s_state_timer_alarm_pending;
static volatile bool s_blink_timer_alarm_pending;
static volatile bool s_pedestrian_debounce_timer_pending;
static volatile bool s_debounce_timer_pending;
static traffic_light_state_t s_current_state = TRAFFIC_LIGHT_STATE_GREEN;
static traffic_light_mode_t s_current_mode = TRAFFIC_LIGHT_MODE_OFF;

static const char *state_name(traffic_light_state_t state)
{
  switch (state)
  {
  case TRAFFIC_LIGHT_STATE_GREEN:
    return "GREEN";
  case TRAFFIC_LIGHT_STATE_GREEN_BLINKING:
    return "GREEN_BLINKING";
  case TRAFFIC_LIGHT_STATE_YELLOW:
    return "YELLOW";
  case TRAFFIC_LIGHT_STATE_RED:
    return "RED";
  case TRAFFIC_LIGHT_STATE_RED_YELLOW:
    return "RED_YELLOW";
  }

  return "UNKNOWN";
}

static const char *mode_name(traffic_light_mode_t mode)
{
  switch (mode)
  {
  case TRAFFIC_LIGHT_MODE_OFF:
    return "OFF";
  case TRAFFIC_LIGHT_MODE_NORMAL:
    return "NORMAL";
  case TRAFFIC_LIGHT_MODE_YELLOW_BLINKING:
    return "YELLOW_BLINKING";
  case TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED:
    return "PEDESTRIAN_CONTROLLED";
  }

  return "UNKNOWN";
}

static void IRAM_ATTR start_pedestrian_debounce_timer(uint64_t duration_ticks)
{
  gptimer_stop(s_pedestrian_debounce_timer);
  gptimer_set_raw_count(s_pedestrian_debounce_timer, 0);

  gptimer_alarm_config_t alarm = {
      .alarm_count = duration_ticks,
      .flags.auto_reload_on_alarm = false,
  };

  gptimer_set_alarm_action(s_pedestrian_debounce_timer, &alarm);
  gptimer_start(s_pedestrian_debounce_timer);
}

static void IRAM_ATTR pedestrian_button_isr_handler(void *arg)
{
  (void)arg;

  start_pedestrian_debounce_timer(
      MILLISECONDS_TO_TICKS(PEDESTRIAN_BUTTON_DEBOUNCE_MS));
}

static void IRAM_ATTR mode_button_isr_handler(void *arg)
{
  (void)arg;

  esp_timer_stop(s_debounce_timer);
  esp_timer_start_once(s_debounce_timer, TRAFFIC_LIGHT_BUTTON_DEBOUNCE_US);
}

static bool IRAM_ATTR on_state_timer_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *event_data,
    void *user_context)
{
  (void)event_data;
  (void)user_context;

  s_state_timer_alarm_pending = true;
  gptimer_stop(timer);

  return false;
}

static bool IRAM_ATTR on_blink_timer_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *event_data,
    void *user_context)
{
  (void)timer;
  (void)event_data;
  (void)user_context;

  s_blink_timer_alarm_pending = true;

  return false;
}

static bool IRAM_ATTR on_pedestrian_debounce_timer_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *event_data,
    void *user_context)
{
  (void)timer;
  (void)event_data;
  (void)user_context;

  if (gpio_get_level(PEDESTRIAN_BUTTON_GPIO) != 0)
  {
    return false;
  }

  s_pedestrian_debounce_timer_pending = true;

  return false;
}

static void debounce_timer_cb(void *arg)
{
  (void)arg;

  if (gpio_get_level(TRAFFIC_LIGHT_MODE_BUTTON_GPIO) != 0)
  {
    return;
  }

  s_debounce_timer_pending = true;
}

static void start_state_timer(uint64_t duration_ticks)
{
  ESP_ERROR_CHECK(gptimer_stop(s_state_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_state_timer, 0));

  gptimer_alarm_config_t alarm = {
      .alarm_count = duration_ticks,
      .flags.auto_reload_on_alarm = false,
  };

  ESP_ERROR_CHECK(gptimer_set_alarm_action(s_state_timer, &alarm));
  ESP_ERROR_CHECK(gptimer_start(s_state_timer));
}

static void stop_state_timer(void)
{
  ESP_ERROR_CHECK(gptimer_stop(s_state_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_state_timer, 0));

  s_state_timer_alarm_pending = false;
}

static void start_blink_timer(uint64_t duration_ticks)
{
  ESP_ERROR_CHECK(gptimer_stop(s_blink_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_blink_timer, 0));

  gptimer_alarm_config_t alarm = {
      .alarm_count = duration_ticks,
      .flags.auto_reload_on_alarm = true,
  };

  ESP_ERROR_CHECK(gptimer_set_alarm_action(s_blink_timer, &alarm));
  ESP_ERROR_CHECK(gptimer_start(s_blink_timer));
}

static void stop_blink_timer(void)
{
  ESP_ERROR_CHECK(gptimer_stop(s_blink_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_blink_timer, 0));

  s_blink_timer_alarm_pending = false;
}

static void set_lights(bool red_on, bool yellow_on, bool green_on)
{
  gpio_set_level(TRAFFIC_LIGHT_RED_LED_GPIO, red_on);
  gpio_set_level(TRAFFIC_LIGHT_YELLOW_LED_GPIO, yellow_on);
  gpio_set_level(TRAFFIC_LIGHT_GREEN_LED_GPIO, green_on);

  if (s_current_mode == TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED)
  {
    bool pedestrian_may_cross = red_on && !yellow_on;

    gpio_set_level(PEDESTRIAN_RED_LED_GPIO, !pedestrian_may_cross);
    gpio_set_level(PEDESTRIAN_GREEN_LED_GPIO, pedestrian_may_cross);
  }
  else
  {
    gpio_set_level(PEDESTRIAN_RED_LED_GPIO, 0);
    gpio_set_level(PEDESTRIAN_GREEN_LED_GPIO, 0);
  }
}

static void enter_state(traffic_light_state_t new_state)
{
  s_current_state = new_state;
  ESP_LOGI(TAG, "State: %s", state_name(s_current_state));

  switch (s_current_state)
  {
  case TRAFFIC_LIGHT_STATE_GREEN:
    set_lights(false, false, true);
    if (s_current_mode == TRAFFIC_LIGHT_MODE_NORMAL)
    {
      start_state_timer(
          MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_GREEN_DURATION_MS));
    }
    break;

  case TRAFFIC_LIGHT_STATE_GREEN_BLINKING:
    set_lights(false, false, true);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_GREEN_BLINK_DURATION_MS));
    start_blink_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_BLINK_INTERVAL_MS));
    break;

  case TRAFFIC_LIGHT_STATE_YELLOW:
    set_lights(false, true, false);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_YELLOW_DURATION_MS));
    break;

  case TRAFFIC_LIGHT_STATE_RED:
    set_lights(true, false, false);
    start_state_timer(MILLISECONDS_TO_TICKS(
    s_current_mode == TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED
        ? PEDESTRIAN_CROSSING_DURATION_MS
        : TRAFFIC_LIGHT_RED_DURATION_MS));
    break;

  case TRAFFIC_LIGHT_STATE_RED_YELLOW:
    set_lights(true, true, false);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_RED_YELLOW_DURATION_MS));
    break;
  }
}

static void handle_state_timer_alarm(void)
{
  switch (s_current_state)
  {
  case TRAFFIC_LIGHT_STATE_GREEN:
    enter_state(TRAFFIC_LIGHT_STATE_GREEN_BLINKING);
    break;

  case TRAFFIC_LIGHT_STATE_GREEN_BLINKING:
    stop_blink_timer();
    enter_state(TRAFFIC_LIGHT_STATE_YELLOW);
    break;

  case TRAFFIC_LIGHT_STATE_YELLOW:
    enter_state(TRAFFIC_LIGHT_STATE_RED);
    break;

  case TRAFFIC_LIGHT_STATE_RED:
    enter_state(TRAFFIC_LIGHT_STATE_RED_YELLOW);
    break;

  case TRAFFIC_LIGHT_STATE_RED_YELLOW:
    enter_state(TRAFFIC_LIGHT_STATE_GREEN);
    break;
  }
}

static void handle_blink_timer_alarm(void)
{
  if (s_current_mode == TRAFFIC_LIGHT_MODE_YELLOW_BLINKING)
  {
    set_lights(
        false,
        !gpio_get_level(TRAFFIC_LIGHT_YELLOW_LED_GPIO),
        false);
  }
  else if (
      (s_current_mode == TRAFFIC_LIGHT_MODE_NORMAL ||
       s_current_mode == TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED) &&
      s_current_state == TRAFFIC_LIGHT_STATE_GREEN_BLINKING)
  {
    set_lights(
        false,
        false,
        !gpio_get_level(TRAFFIC_LIGHT_GREEN_LED_GPIO));
  }
}

static void handle_pedestrian_debounce_timer_alarm(void)
{
  ESP_LOGI(TAG, "Pedestrian crossing requested");
  
  if (s_current_mode != TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED)
  {
    return;
  }

  if (s_current_state != TRAFFIC_LIGHT_STATE_GREEN)
  {
    return;
  }

  enter_state(TRAFFIC_LIGHT_STATE_GREEN_BLINKING);
}

static void handle_debounce_timer_alarm(void)
{
  switch (s_current_mode)
  {
  case TRAFFIC_LIGHT_MODE_NORMAL:
    traffic_light_set_mode(TRAFFIC_LIGHT_MODE_YELLOW_BLINKING);
    break;

  case TRAFFIC_LIGHT_MODE_YELLOW_BLINKING:
    traffic_light_set_mode(TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED);
    break;

  case TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED:
    traffic_light_set_mode(TRAFFIC_LIGHT_MODE_OFF);
    break;

  case TRAFFIC_LIGHT_MODE_OFF:
    traffic_light_set_mode(TRAFFIC_LIGHT_MODE_NORMAL);
    break;
  }
}

static void setup_state_timer(void)
{
  gptimer_config_t config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(gptimer_new_timer(&config, &s_state_timer));

  gptimer_event_callbacks_t callbacks = {
      .on_alarm = on_state_timer_alarm,
  };
  ESP_ERROR_CHECK(
      gptimer_register_event_callbacks(s_state_timer, &callbacks, NULL));
  ESP_ERROR_CHECK(gptimer_enable(s_state_timer));
}

static void setup_blink_timer(void)
{
  gptimer_config_t config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(gptimer_new_timer(&config, &s_blink_timer));

  gptimer_event_callbacks_t callbacks = {
      .on_alarm = on_blink_timer_alarm,
  };
  ESP_ERROR_CHECK(
      gptimer_register_event_callbacks(s_blink_timer, &callbacks, NULL));
  ESP_ERROR_CHECK(gptimer_enable(s_blink_timer));
}

static void setup_pedestrian_debounce_timer(void)
{
  gptimer_config_t config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(gptimer_new_timer(&config, &s_pedestrian_debounce_timer));

  gptimer_event_callbacks_t callbacks = {
      .on_alarm = on_pedestrian_debounce_timer_alarm,
  };
  ESP_ERROR_CHECK(
      gptimer_register_event_callbacks(s_pedestrian_debounce_timer, &callbacks, NULL));
  ESP_ERROR_CHECK(gptimer_enable(s_pedestrian_debounce_timer));
}

static void setup_debounce_timer(void)
{
  const esp_timer_create_args_t args = {
      .callback = debounce_timer_cb,
      .name = "debounce",
  };
  ESP_ERROR_CHECK(esp_timer_create(&args, &s_debounce_timer));
}

static void setup_leds(void)
{
  gpio_reset_pin(TRAFFIC_LIGHT_RED_LED_GPIO);
  gpio_set_direction(TRAFFIC_LIGHT_RED_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(TRAFFIC_LIGHT_RED_LED_GPIO, 0);

  gpio_reset_pin(TRAFFIC_LIGHT_YELLOW_LED_GPIO);
  gpio_set_direction(TRAFFIC_LIGHT_YELLOW_LED_GPIO, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_level(TRAFFIC_LIGHT_YELLOW_LED_GPIO, 0);

  gpio_reset_pin(TRAFFIC_LIGHT_GREEN_LED_GPIO);
  gpio_set_direction(TRAFFIC_LIGHT_GREEN_LED_GPIO, GPIO_MODE_INPUT_OUTPUT);
  gpio_set_level(TRAFFIC_LIGHT_GREEN_LED_GPIO, 0);

  gpio_reset_pin(PEDESTRIAN_RED_LED_GPIO);
  gpio_set_direction(PEDESTRIAN_RED_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(PEDESTRIAN_RED_LED_GPIO, 0);

  gpio_reset_pin(PEDESTRIAN_GREEN_LED_GPIO);
  gpio_set_direction(PEDESTRIAN_GREEN_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(PEDESTRIAN_GREEN_LED_GPIO, 0);
}

static void setup_gpio_isr_service(void)
{
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
}

static void setup_pedestrian_button(void)
{
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << PEDESTRIAN_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));
  ESP_ERROR_CHECK(gpio_isr_handler_add(
      PEDESTRIAN_BUTTON_GPIO,
      pedestrian_button_isr_handler,
      NULL));
}

static void setup_mode_button(void)
{
  gpio_config_t io = {
      .pin_bit_mask = 1ULL << TRAFFIC_LIGHT_MODE_BUTTON_GPIO,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));
  ESP_ERROR_CHECK(gpio_isr_handler_add(
      TRAFFIC_LIGHT_MODE_BUTTON_GPIO,
      mode_button_isr_handler,
      NULL));
}

void traffic_light_init(void)
{
  setup_state_timer();
  setup_blink_timer();
  setup_pedestrian_debounce_timer();
  setup_debounce_timer();
  setup_leds();
  setup_gpio_isr_service();
  setup_pedestrian_button();
  setup_mode_button();

  ESP_LOGI(
      TAG,
      "Initialized (vehicle LEDs: red GPIO=%d, yellow GPIO=%d, green GPIO=%d; "
      "pedestrian LEDs: red GPIO=%d, green GPIO=%d; "
      "buttons: mode GPIO=%d, pedestrian GPIO=%d)",
      (int)TRAFFIC_LIGHT_RED_LED_GPIO,
      (int)TRAFFIC_LIGHT_YELLOW_LED_GPIO,
      (int)TRAFFIC_LIGHT_GREEN_LED_GPIO,
      (int)PEDESTRIAN_RED_LED_GPIO,
      (int)PEDESTRIAN_GREEN_LED_GPIO,
      (int)TRAFFIC_LIGHT_MODE_BUTTON_GPIO,
      (int)PEDESTRIAN_BUTTON_GPIO);
}

void traffic_light_set_mode(traffic_light_mode_t new_mode)
{
  if (new_mode == s_current_mode)
  {
    ESP_LOGD(TAG, "Mode is already %s", mode_name(new_mode));
    return;
  }

  stop_state_timer();
  stop_blink_timer();

  ESP_LOGI(
      TAG,
      "Mode: %s -> %s",
      mode_name(s_current_mode),
      mode_name(new_mode));

  s_current_mode = new_mode;
  switch (s_current_mode)
  {
  case TRAFFIC_LIGHT_MODE_NORMAL:
    enter_state(TRAFFIC_LIGHT_STATE_GREEN);
    break;

  case TRAFFIC_LIGHT_MODE_YELLOW_BLINKING:
    set_lights(false, true, false);
    start_blink_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_BLINK_INTERVAL_MS));
    break;

  case TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED:
    stop_blink_timer();
    stop_state_timer();
    enter_state(TRAFFIC_LIGHT_STATE_GREEN);
    break;

  case TRAFFIC_LIGHT_MODE_OFF:
    set_lights(false, false, false);
    break;
  }
}

void traffic_light_process(void)
{
  if (s_state_timer_alarm_pending)
  {
    s_state_timer_alarm_pending = false;
    handle_state_timer_alarm();
  }

  if (s_blink_timer_alarm_pending)
  {
    s_blink_timer_alarm_pending = false;
    handle_blink_timer_alarm();
  }

  if (s_pedestrian_debounce_timer_pending)
  {
    s_pedestrian_debounce_timer_pending = false;
    handle_pedestrian_debounce_timer_alarm();
  }

  if (s_debounce_timer_pending)
  {
    s_debounce_timer_pending = false;
    handle_debounce_timer_alarm();
  }
}
