/*  WiFi Control 

   This code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
//---------------- includes
#include <stdio.h> //+
#include <string.h>
#include <stdlib.h> //+g

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h" //+
#include "esp_system.h" 
#include "esp_wifi.h" 
#include "esp_event.h" //deprecated #include "esp_event_loop.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/queue.h" //+g
#include "driver/gpio.h" 

#include "driver/gpio.h" //+
#include "mdns.h" //+
//#include "mdns/mdns_private.h" //++

#include "lwip/api.h" //+
#include "lwip/err.h"
#include "lwip/netdb.h" //+
//#include "lwip/sys.h" //- non serve?

//---------------- defines
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_EXAMPLE_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_EXAMPLE_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY  CONFIG_EXAMPLE_MAXIMUM_RETRY

#define EXAMPLE_ESP_WIFI_CHANNEL   CONFIG_EXAMPLE_WIFI_CHANNEL
#define EXAMPLE_MAXIMUM_CONN       CONFIG_EXAMPLE_MAXIMUM_CONN

#define GPIO_OUTPUT_IO_0    18
#define GPIO_OUTPUT_IO_1    19
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_IO_0) | (1ULL<<GPIO_OUTPUT_IO_1))
#define GPIO_INPUT_IO_0     4
#define GPIO_INPUT_IO_1     5
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_IO_0) | (1ULL<<GPIO_INPUT_IO_1))
#define ESP_INTR_FLAG_DEFAULT 0

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

//---------------- GLOBAL VARIABLES
int inputLevel_0;
int inputLevel_1;

// HTTP headers 
const static char http_html_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n";
const static char http_png_hdr[] = "HTTP/1.1 200 OK\nContent-type: image/png\n\n";

// HTTP body (concateno più stringhe: inizio + I/O status + fine)

char http_inizio_hml[]="<meta content=\"width=device-width,initial-scale=1\"name=viewport><style>div{width:230px;height:300px;position:absolute;top:0;bottom:0;left:0;right:0;margin:auto}</style><div><h1 align=center>";

char Input_0_LOW[] = "input 0 is LOW";
char Input_0_HIGH[] = "input 0 is HIGH";
char Input_1_LOW[] = "input 1 is LOW";
char Input_1_HIGH[] = "input 1 is HIGH";

char http_fine_hml[]="</h1><a href=update.html><img src=RefreshBtn.png></a></div>";

#define MAX_HTML_STRING 500
char http_html_body[MAX_HTML_STRING]; //attenzion! stay less than MAX_HTML_STRING

// embedded binary data
extern const uint8_t RefreshBtn_png_start[] asm("_binary_RefreshBtn_png_start");
extern const uint8_t RefreshBtn_png_end[]   asm("_binary_RefreshBtn_png_end");

/* FreeRTOS event group to signal when we are connected*/
// Event group for inter-task communication
static EventGroupHandle_t s_wifi_event_group;

static const char *TAG = "WiFiControlMain";

static int s_retry_num = 0;

//---------------- declarations
void gpio_read_and_set();
void fillStrings();

//#define USE_IRQ
#ifdef USE_IRQ
static xQueueHandle gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg){
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void* arg){
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
        }
    }
}
#endif

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data){
	//==================events for STA mode======================
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } 
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
	//===================events for AP mode==========================
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } 
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}
	  
static void http_server_netconn_serve(struct netconn *conn) {

	//static bool relay_status = false;
	struct netbuf *inbuf;
	char *buf;
	u16_t buflen;
	err_t err;
	
	////////////////////////////static char dbgToggleStatus=0;

	err = netconn_recv(conn, &inbuf);

	if (err == ERR_OK) {
		netbuf_data(inbuf, (void**)&buf, &buflen);
		
		// extract the first line, with the request
		char *first_line = strtok(buf, "\n");
		
		if(first_line) {	
			// default page
			if(strstr(first_line, "GET / ")) {
				printf("Sending default page\n");
				gpio_read_and_set();
				fillStrings();	
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);		
				netconn_write(conn, http_html_body, sizeof(http_html_body) - 1, NETCONN_NOCOPY);
			}
			
			// update page
			else if(strstr(first_line, "GET /update.html ")) {		
				printf("Sending update page...\n");
				gpio_read_and_set();
				fillStrings();	
				netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
				netconn_write(conn, http_html_body, sizeof(http_html_body) - 1, NETCONN_NOCOPY);
			}
						
			// button image
			else if(strstr(first_line, "GET /RefreshBtn.png ")) {
				printf("Sending button image...\n");
				netconn_write(conn, http_png_hdr, sizeof(http_png_hdr) - 1, NETCONN_NOCOPY);
				netconn_write(conn, RefreshBtn_png_start, RefreshBtn_png_end - RefreshBtn_png_start, NETCONN_NOCOPY);
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


void wifi_init_sta(void) {
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

void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAXIMUM_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}

void gpio_read_and_set() {
	
	inputLevel_0 = gpio_get_level(GPIO_INPUT_IO_0);	
	inputLevel_1 = gpio_get_level(GPIO_INPUT_IO_1);	
	gpio_set_level(GPIO_OUTPUT_IO_0, inputLevel_0);
	gpio_set_level(GPIO_OUTPUT_IO_1, inputLevel_1);	
	printf("\n\ningressi:\n\n");
	printf("in_0 = %d\n",inputLevel_0);
	printf("in_1 = %d\n",inputLevel_1);
}

void fillStrings() {	
	http_html_body[0]='\0'; //clean string
	
	//inizio html body
	strcat(http_html_body,http_inizio_hml);
	
	//input 0
	if (inputLevel_0==0){
		strcat(http_html_body,Input_0_LOW);
	}
	else {
		strcat(http_html_body,Input_0_HIGH);
	}
	
	strcat(http_html_body,"<p>"); //a capo
	
	//input 1
	if (inputLevel_1==0){
		strcat(http_html_body,Input_1_LOW);
	}
	else {
		strcat(http_html_body,Input_1_HIGH);
	}
	
	//fine html body 
	strcat(http_html_body,http_fine_hml);
		
	printf("\nhttp_html_body:\n");
	printf(http_html_body);
	
}

void gpio_setup(void) {
	gpio_config_t io_conf;
	
    //disable interrupt
    io_conf.intr_type = GPIO_PIN_INTR_DISABLE;
	
    //==================set as output mode===================
    io_conf.mode = GPIO_MODE_OUTPUT;
	
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
	
    //disable pull-down mode 
    io_conf.pull_down_en = 0; 
	
    //disable pull-up mode
    io_conf.pull_up_en = 0;
	
    //configure output GPIO with the given settings
    gpio_config(&io_conf);

#ifdef USE_IRQ
	//==================set as interrupt mode===================
    //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
#endif
	
	//==================set as input mode===================
    io_conf.mode = GPIO_MODE_INPUT;

    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;	
	
    //NOT enable pull-up mode
    //io_conf.pull_up_en = 1;
	
	//configure input GPIO with the given settings
    gpio_config(&io_conf);
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
	
	//--------------------------------------
	//WIfi Station or WiFi AP (uncomment the choice)
	#define WIFI_STA	0
	#define WIFI_AP		1
	
	const char wifi_mode = WIFI_AP;

    if (wifi_mode == WIFI_STA) {
		ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
		wifi_init_sta();
	}
	else {//default is AP
		ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");
		wifi_init_softap();
	}
	//--------------------------------------
	
	gpio_setup();
	
	// run the mDNS daemon
	/*
	mdns_server_t* mDNS = NULL;
	ESP_ERROR_CHECK(mdns_init(TCPIP_ADAPTER_IF_STA, &mDNS));
	ESP_ERROR_CHECK(mdns_set_hostname(mDNS, "esp32"));
	ESP_ERROR_CHECK(mdns_set_instance(mDNS, "Basic HTTP Server"));
	printf("mDNS started\n");
	*/
	
	// start the HTTP Server task
    xTaskCreate(&http_server, "http_server", 2048, NULL, 5, NULL);
	
	#ifdef USE_IRQ
	//create a queue to handle gpio event from isr
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	
    //start gpio task
    xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);
	#endif
	
}
