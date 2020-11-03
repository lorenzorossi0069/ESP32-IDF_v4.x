#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h" 
#include "esp_wifi.h"
#include "esp_event.h" //deprecated #include "esp_event_loop.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "mdns.h"

#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/netdb.h"

//#define USE_RELAY_HW

#define USE_PNG_IMG

#define EXAMPLE_ESP_WIFI_SSID      CONFIG_EXAMPLE_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_EXAMPLE_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_EXAMPLE_MAXIMUM_RETRY

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1


// HTTP headers and web pages
const static char http_html_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n";
const static char http_png_hdr[] = "HTTP/1.1 200 OK\nContent-type: image/png\n\n";
#ifdef USE_PNG_IMG
const static char http_off_hml[] = "<meta content=\"width=device-width,initial-scale=1\"name=viewport><style>div{width:230px;height:300px;position:absolute;top:0;bottom:0;left:0;right:0;margin:auto}</style><div><h1 align=center>Relay is OFF</h1><a href=on.html><img src=on.png></a></div>";
const static char http_on_hml[] = "<meta content=\"width=device-width,initial-scale=1\"name=viewport><style>div{width:230px;height:300px;position:absolute;top:0;bottom:0;left:0;right:0;margin:auto}</style><div><h1 align=center>Relay is ON</h1><a href=off.html><img src=off.png></a></div>"; 

// embedded binary data
extern const uint8_t on_png_start[] asm("_binary_on_png_start");
extern const uint8_t on_png_end[]   asm("_binary_on_png_end");
extern const uint8_t off_png_start[] asm("_binary_off_png_start");
extern const uint8_t off_png_end[]   asm("_binary_off_png_end");

#else //no IMG
const static char http_off_hml[] = "<meta content=\"width=device-width,initial-scale=1\"name=viewport><style>div{width:230px;height:300px;position:absolute;top:0;bottom:0;left:0;right:0;margin:auto}</style><div><h1 align=center>Relay is OFF</h1></div>";
const static char http_on_hml[] = "<meta content=\"width=device-width,initial-scale=1\"name=viewport><style>div{width:230px;height:300px;position:absolute;top:0;bottom:0;left:0;right:0;margin:auto}</style><div><h1 align=center>Relay is ON</h1></div>"; 
	
#endif

// Event group for inter-task communication
static EventGroupHandle_t s_wifi_event_group;
//const int WIFI_CONNECTED_BIT = BIT0;

// actual relay status
bool relay_status = 1;
static const char *TAG = "my main";

static int s_retry_num = 0;

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

//aaa
	  
static void http_server_netconn_serve(struct netconn *conn) {

	struct netbuf *inbuf;
	char *buf;
	u16_t buflen;
	err_t err;

	err = netconn_recv(conn, &inbuf);

	if (err == ERR_OK) {
	  
		netbuf_data(inbuf, (void**)&buf, &buflen);
		
		// extract the first line, with the request
		char *first_line = strtok(buf, "\n");
		
		if(first_line) {
			
			// default page
			if(strstr(first_line, "GET / ")) {
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
				if(relay_status) {
					printf("Sending default page, relay is ON\n");
					netconn_write(conn, http_on_hml, sizeof(http_on_hml) - 1, NETCONN_NOCOPY);
				}					
				else {
					printf("Sending default page, relay is OFF\n");
					netconn_write(conn, http_off_hml, sizeof(http_off_hml) - 1, NETCONN_NOCOPY);
				}
			}
			
			// ON page
			else if(strstr(first_line, "GET /on.html ")) {
				
				if(relay_status == false) {			
					printf("Turning relay ON\n");
					#ifdef USE_RELAY_HW
					gpio_set_level(CONFIG_RELAY_PIN, 1);
					#endif
					relay_status = true;
				}
				
				printf("Sending OFF page...\n");
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
				netconn_write(conn, http_on_hml, sizeof(http_on_hml) - 1, NETCONN_NOCOPY);
			}			

			// OFF page
			else if(strstr(first_line, "GET /off.html ")) {
				
				if(relay_status == true) {			
					printf("Turning relay OFF\n");
					#ifdef USE_RELAY_HW
					gpio_set_level(CONFIG_RELAY_PIN, 0);
					#endif
					relay_status = false;
				}
				
				
				printf("Sending OFF page...\n");
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
				netconn_write(conn, http_off_hml, sizeof(http_off_hml) - 1, NETCONN_NOCOPY);
			}
			
			// ON image
			else if(strstr(first_line, "GET /on.png ")) {
				printf("Sending ON image...\n");
				netconn_write(conn, http_png_hdr, sizeof(http_png_hdr) - 1, NETCONN_NOCOPY);
				#ifdef USE_PNG_IMG
				netconn_write(conn, on_png_start, on_png_end - on_png_start, NETCONN_NOCOPY);
				#endif
			}
			
			// OFF image
			else if(strstr(first_line, "GET /off.png ")) {
				printf("Sending OFF image...\n");
				netconn_write(conn, http_png_hdr, sizeof(http_png_hdr) - 1, NETCONN_NOCOPY);
				#ifdef USE_PNG_IMG
				netconn_write(conn, off_png_start, off_png_end - off_png_start, NETCONN_NOCOPY);
				#endif
			}
			
			
			else printf("Unkown request: %s\n", first_line);
		}
		else printf("Unkown request\n");
	}
	
	// close the connection and free the buffer
	netconn_close(conn);
	netbuf_delete(inbuf);
}

static void http_server(void *pvParameters) {
	
	struct netconn *conn, *newconn;
	err_t err;
	conn = netconn_new(NETCONN_TCP);
	netconn_bind(conn, NULL, 80);
	netconn_listen(conn);
	printf("HTTP Server listening...\n");
	do {
		err = netconn_accept(conn, &newconn);
		printf("New client connected\n");
		if (err == ERR_OK) {
			http_server_netconn_serve(newconn);
			netconn_delete(newconn);
		}
		vTaskDelay(1); //allows task to be pre-empted
	} while(err == ERR_OK);
	netconn_close(conn);
	netconn_delete(conn);
	printf("\n");
}

//bbb


void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init()); //replaces deprecated tcp_adapter_init() of v3.x!!!!

    ESP_ERROR_CHECK(esp_event_loop_create_default()); //replaces deprecated esp_event_loop_init() of v3.x!!!!
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID, 
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    vEventGroupDelete(s_wifi_event_group);
}


// Main application
void app_main()
{
	// disable the default wifi logging
	//esp_log_level_set("wifi", ESP_LOG_NONE);
	
	//event_group = xEventGroupCreate();

	//Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
	  }
    ESP_ERROR_CHECK(ret);
	
	ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
	
	
	/*
	
	
	ESP_ERROR_CHECK(esp_netif_init()); //replaces deprecated tcpip_adapter_init();	
	ESP_ERROR_CHECK(esp_event_loop_create_default()); //replaces deprecated esp_event_loop_init()
		
	wifi_setup();
	//gpio_setup();
	*/
	
	// start the HTTP Server task
    xTaskCreate(&http_server, "http_server", 2048, NULL, 5, NULL);
	
}