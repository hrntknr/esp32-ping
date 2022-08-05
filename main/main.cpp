#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "ping/ping_sock.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "driver/gpio.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_task.h"

#include "ESP32RMTChannel.h"
#include "DStripData.h"
#include "DLEDController.h"

#define IP4_READY_BIT BIT0
#define IP6_READY_BIT BIT1
#define CONFIG_RMT_OUTPUT_PIN 27
#define CONFIG_RMT_CHANNEL 0
#define CONFIG_RMT_LED_COUNT 25

static const char *ssid = "meraki";
static const char *password = "hirano-ap";
static const char *target_hosts[] = {
    "8.8.8.8",
    "1.1.1.1",
    NULL,
    "10.196.0.1",
    "10.0.0.1",

    "2001:4860:4860::8888",
    "2606:4700:4700::1111",
    NULL,
    "240b:10:9ab0:2201::1",
    "2400:4050:2223:4f00::2",

    NULL,
    NULL,
    NULL,
    NULL,
    NULL,

    NULL,
    NULL,
    NULL,
    NULL,
    NULL,

    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

static const char *TAG = "main";
static bool ip4_ready = false;
static bool ip6_ready = false;

static esp_netif_t *esp_netif;
static EventGroupHandle_t wifi_event_group;
static DStripData strip_data;
static DLEDController led_controller;
static ESP32RMTChannel rmt_channel;

extern "C"
{
  void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
  {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
      ip4_ready = false;
      ip6_ready = false;
      esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
      ip4_ready = false;
      ip6_ready = false;
      esp_wifi_connect();
      ESP_LOGI(TAG, "retry to connect to the AP");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
      esp_netif_create_ip6_linklocal(esp_netif);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
      ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
      ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
      ip4_ready = true;
      xEventGroupSetBits(wifi_event_group, IP4_READY_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6)
    {
      ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
      if ((ntohl(event->ip6_info.ip.addr[0]) & 0xffc00000) == 0xfe800000)
      {
        ESP_LOGI(TAG, "got ip6 link local: " IPV6STR, IPV62STR(event->ip6_info.ip));
      }
      else
      {
        ESP_LOGI(TAG, "got ip6: " IPV6STR, IPV62STR(event->ip6_info.ip));
        ip6_ready = true;
        xEventGroupSetBits(wifi_event_group, IP6_READY_BIT);
      }
    }
  }

  esp_err_t wifi_init_sta()
  {
    esp_err_t ret_value = ESP_OK;
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, ssid);
    strcpy((char *)wifi_config.sta.password, password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config((wifi_interface_t)ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    xEventGroupWaitBits(wifi_event_group, IP4_READY_BIT | IP6_READY_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    ESP_LOGI(TAG, "connect to ap SSID:%s", ssid);
    return ret_value;
  }

  void cmd_ping_on_ping_success(esp_ping_handle_t hdl, void *args)
  {
    uint8_t index = *((uint8_t *)args);
    uint32_t elapsed_time;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    ESP_LOGI(TAG, "success: %dms %s", elapsed_time, target_hosts[index]);

    uint8_t *data = strip_data.Data();
    data += index * strip_data.BytesPerLED();
    *data = 0x00;
    data++;
    *data = 0x03;
    data++;
    *data = 0x0f;
    data++;
    *data = 0x03;
    led_controller.SetLEDs(strip_data, rmt_channel);
  }

  void cmd_ping_on_ping_timeout(esp_ping_handle_t hdl, void *args)
  {
    uint8_t index = *((uint8_t *)args);
    ESP_LOGI(TAG, "timeout: %s", target_hosts[index]);

    uint8_t *data = strip_data.Data();
    data += index * strip_data.BytesPerLED();
    *data = 0x00;
    data++;
    *data = 0x0f;
    data++;
    *data = 0x03;
    data++;
    *data = 0x03;
    led_controller.SetLEDs(strip_data, rmt_channel);
  }

  void cmd_ping_on_ping_end(esp_ping_handle_t hdl, void *args)
  {
    uint8_t index = *((uint8_t *)args);
    ESP_LOGW(TAG, "end: %s", target_hosts[index]);
  }

  void initialize_ping(void *args)
  {
    uint8_t *i = (uint8_t *)args;
    const char *target_host = target_hosts[*i];
    if (!target_host)
      vTaskDelete(NULL);

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    struct addrinfo hint;
    memset(&hint, 0, sizeof(hint));
    struct addrinfo *res = NULL;

    int err = getaddrinfo(target_host, NULL, &hint, &res);
    if (err != 0 || res == NULL)
    {
      ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
      abort();
    }

    if (res->ai_family == AF_INET)
    {
      struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
      inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
      if (!ip4_ready)
        xEventGroupWaitBits(wifi_event_group, IP4_READY_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    }
    else if (res->ai_family == AF_INET6)
    {
      struct in6_addr addr6 = ((struct sockaddr_in6 *)(res->ai_addr))->sin6_addr;
      inet6_addr_to_ip6addr(ip_2_ip6(&target_addr), &addr6);
      if (!ip6_ready)
      {
        vTaskDelay(3000 / portTICK_PERIOD_MS);
        xEventGroupWaitBits(wifi_event_group, IP6_READY_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
      }
    }
    else
    {
      ESP_LOGE(TAG, "Unsupported address family %d", res->ai_family);
      abort();
    }
    freeaddrinfo(res);

    ping_config.count = ESP_PING_COUNT_INFINITE;
    ping_config.target_addr = target_addr;
    ping_config.interval_ms = 1000;

    esp_ping_callbacks_t cbs = {
        .cb_args = (void *)i,
        .on_ping_success = cmd_ping_on_ping_success,
        .on_ping_timeout = cmd_ping_on_ping_timeout,
        .on_ping_end = cmd_ping_on_ping_end,
    };

    esp_ping_handle_t ping;
    esp_ping_new_session(&ping_config, &cbs, &ping);
    esp_ping_start(ping);
    ESP_LOGI(TAG, "ping started: %s", target_host);
    vTaskDelete(NULL);
  }

  void app_main()
  {
    esp_err_t esp_err = nvs_flash_init();
    if (esp_err == ESP_ERR_NVS_NO_FREE_PAGES || esp_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
      ESP_ERROR_CHECK(nvs_flash_erase());
      esp_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_err);

    ESP_ERROR_CHECK(wifi_init_sta());

    strip_data.Create(CONFIG_RMT_LED_COUNT);
    for (uint8_t i = 0; i < strip_data.StripLength(); i++)
    {
      uint8_t *data = strip_data.Data();
      data += i * strip_data.BytesPerLED();
      *data = 0x00;
      data++;
      *data = 0x00;
      data++;
      *data = 0x00;
      data++;
      *data = 0x00;
    }

    rmt_channel.Initialize((rmt_channel_t)CONFIG_RMT_CHANNEL, (gpio_num_t)CONFIG_RMT_OUTPUT_PIN, strip_data.StripLength() * 24);
    rmt_channel.ConfigureForWS2812x();
    led_controller.SetLEDType(LEDType::WS2812);
    led_controller.SetLEDs(strip_data, rmt_channel);

    for (uint8_t i = 0; i < strip_data.StripLength(); i++)
    {
      uint8_t *index = (uint8_t *)malloc(sizeof(uint8_t));
      memcpy(index, &i, sizeof(uint8_t));
      xTaskCreate((TaskFunction_t)initialize_ping, "ping", 4096, (void *)index, 5, NULL);
    }
  }
}
