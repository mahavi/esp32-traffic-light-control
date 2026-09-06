#include "traffic_light.h"

#include <stdbool.h>
#include <stdint.h>

#include "button.h"
#include "configuration.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

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
static button_t s_pedestrian_button;
static button_t s_mode_button;

static volatile bool s_state_timer_alarm_pending;
static volatile bool s_blink_timer_alarm_pending;
static traffic_light_state_t s_current_state = TRAFFIC_LIGHT_STATE_GREEN;
static traffic_light_mode_t s_current_mode = TRAFFIC_LIGHT_MODE_OFF;
static bool s_blink_light_on;

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

static void start_gptimer(
    gptimer_handle_t timer,
    uint64_t duration_ticks,
    bool auto_reload)
{
  ESP_ERROR_CHECK(gptimer_stop(timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(timer, 0));

  gptimer_alarm_config_t alarm = {
      .alarm_count = duration_ticks,
      .flags.auto_reload_on_alarm = auto_reload,
  };

  ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &alarm));
  ESP_ERROR_CHECK(gptimer_start(timer));
}

static void stop_gptimer(gptimer_handle_t timer)
{
  ESP_ERROR_CHECK(gptimer_stop(timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(timer, 0));
}

static void start_state_timer_ms(uint32_t duration_ms)
{
  start_gptimer(
      s_state_timer,
      MILLISECONDS_TO_TICKS(duration_ms),
      false);
}

static void stop_state_timer(void)
{
  stop_gptimer(s_state_timer);

  s_state_timer_alarm_pending = false;
}

static void start_blink_timer_ms(uint32_t duration_ms)
{
  s_blink_light_on = true;
  start_gptimer(
      s_blink_timer,
      MILLISECONDS_TO_TICKS(duration_ms),
      true);
}

static void stop_blink_timer(void)
{
  stop_gptimer(s_blink_timer);

  s_blink_timer_alarm_pending = false;
}

static void set_vehicle_lights(bool red_on, bool yellow_on, bool green_on)
{
  gpio_set_level(TRAFFIC_LIGHT_RED_LED_GPIO, red_on);
  gpio_set_level(TRAFFIC_LIGHT_YELLOW_LED_GPIO, yellow_on);
  gpio_set_level(TRAFFIC_LIGHT_GREEN_LED_GPIO, green_on);
}

static void set_pedestrian_lights(bool red_on, bool green_on)
{
  gpio_set_level(PEDESTRIAN_RED_LED_GPIO, red_on);
  gpio_set_level(PEDESTRIAN_GREEN_LED_GPIO, green_on);
}

static void set_lights(bool red_on, bool yellow_on, bool green_on)
{
  set_vehicle_lights(red_on, yellow_on, green_on);

  if (s_current_mode == TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED)
  {
    bool pedestrian_may_cross = red_on && !yellow_on;

    set_pedestrian_lights(!pedestrian_may_cross, pedestrian_may_cross);
  }
  else
  {
    set_pedestrian_lights(false, false);
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
      start_state_timer_ms(TRAFFIC_LIGHT_GREEN_DURATION_MS);
    }
    break;

  case TRAFFIC_LIGHT_STATE_GREEN_BLINKING:
    set_lights(false, false, true);
    start_state_timer_ms(TRAFFIC_LIGHT_GREEN_BLINK_DURATION_MS);
    start_blink_timer_ms(TRAFFIC_LIGHT_BLINK_INTERVAL_MS);
    break;

  case TRAFFIC_LIGHT_STATE_YELLOW:
    set_lights(false, true, false);
    start_state_timer_ms(TRAFFIC_LIGHT_YELLOW_DURATION_MS);
    break;

  case TRAFFIC_LIGHT_STATE_RED:
    set_lights(true, false, false);
    start_state_timer_ms(
        s_current_mode == TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED
            ? PEDESTRIAN_CROSSING_DURATION_MS
            : TRAFFIC_LIGHT_RED_DURATION_MS);
    break;

  case TRAFFIC_LIGHT_STATE_RED_YELLOW:
    set_lights(true, true, false);
    start_state_timer_ms(TRAFFIC_LIGHT_RED_YELLOW_DURATION_MS);
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
  s_blink_light_on = !s_blink_light_on;

  if (s_current_mode == TRAFFIC_LIGHT_MODE_YELLOW_BLINKING)
  {
    set_lights(false, s_blink_light_on, false);
  }
  else if (
      (s_current_mode == TRAFFIC_LIGHT_MODE_NORMAL ||
       s_current_mode == TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED) &&
      s_current_state == TRAFFIC_LIGHT_STATE_GREEN_BLINKING)
  {
    set_lights(false, false, s_blink_light_on);
  }
}

static void handle_pedestrian_button_press(void)
{
  if (s_current_mode != TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED)
  {
    return;
  }

  if (s_current_state != TRAFFIC_LIGHT_STATE_GREEN)
  {
    return;
  }

  ESP_LOGI(TAG, "Pedestrian crossing requested");
  enter_state(TRAFFIC_LIGHT_STATE_GREEN_BLINKING);
}

static void handle_mode_button_press(void)
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

static gptimer_handle_t create_gptimer(gptimer_alarm_cb_t on_alarm)
{
  gptimer_handle_t timer;
  gptimer_config_t config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(gptimer_new_timer(&config, &timer));

  gptimer_event_callbacks_t callbacks = {
      .on_alarm = on_alarm,
  };
  ESP_ERROR_CHECK(
      gptimer_register_event_callbacks(timer, &callbacks, NULL));
  ESP_ERROR_CHECK(gptimer_enable(timer));

  return timer;
}

static void setup_output(gpio_num_t gpio)
{
  ESP_ERROR_CHECK(gpio_reset_pin(gpio));
  ESP_ERROR_CHECK(gpio_set_direction(gpio, GPIO_MODE_OUTPUT));
  ESP_ERROR_CHECK(gpio_set_level(gpio, 0));
}

static void setup_leds(void)
{
  setup_output(TRAFFIC_LIGHT_RED_LED_GPIO);
  setup_output(TRAFFIC_LIGHT_YELLOW_LED_GPIO);
  setup_output(TRAFFIC_LIGHT_GREEN_LED_GPIO);
  setup_output(PEDESTRIAN_RED_LED_GPIO);
  setup_output(PEDESTRIAN_GREEN_LED_GPIO);
}

static void setup_buttons(void)
{
  button_service_init();

  button_init(
      &s_pedestrian_button,
      PEDESTRIAN_BUTTON_GPIO,
      PEDESTRIAN_BUTTON_DEBOUNCE_US,
      "pedestrian_button");

  button_init(
      &s_mode_button,
      TRAFFIC_LIGHT_MODE_BUTTON_GPIO,
      TRAFFIC_LIGHT_BUTTON_DEBOUNCE_US,
      "mode_button");
}

void traffic_light_init(void)
{
  s_state_timer = create_gptimer(on_state_timer_alarm);
  s_blink_timer = create_gptimer(on_blink_timer_alarm);
  setup_leds();
  setup_buttons();

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
    start_blink_timer_ms(TRAFFIC_LIGHT_BLINK_INTERVAL_MS);
    break;

  case TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED:
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

  if (button_take_press(&s_pedestrian_button))
  {
    handle_pedestrian_button_press();
  }

  if (button_take_press(&s_mode_button))
  {
    handle_mode_button_press();
  }
}
