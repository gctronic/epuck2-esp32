/* E-puck2 firmware.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "esp_system.h"
#include "esp_heap_alloc_caps.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "soc/gpio_struct.h"
#include "nvs_flash.h"

static const char *TAG = "example";

//#define PACKET_SIZE 1024
#define PORT 10000

// Hardware VSPI pins.
#define PIN_NUM_MOSI 23
#define PIN_NUM_MISO 19
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

#define ESP_INTR_FLAG_DEFAULT 0

#define SPI_BUFF_LEN 16
#define IMAGE_BUFF_MAX_SIZE 320*240 //*2 // 153600

#define PAYLOAD_SIZE 38400 //8192
#define HEADER_SIZE 4
#define PACKET_SIZE (PAYLOAD_SIZE + HEADER_SIZE)


#define EXAMPLE_WIFI_SSID "Sunrise_2.4GHz_BDA268"
#define EXAMPLE_WIFI_PASS "byr1pa3rs4T2"
//#define EXAMPLE_WIFI_SSID "gilpea"
//#define EXAMPLE_WIFI_PASS "cia0te1234567"


SemaphoreHandle_t xSemaphore = NULL, semImage = NULL;
uint8_t imageBuff[IMAGE_BUFF_MAX_SIZE];
uint8_t txBuff[PACKET_SIZE];

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START: // Started being a station.
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
    	//ESP_LOGI(TAG, "Got an IP: " IPSTR, IP2STR(&event->event_info.got_ip.ip_info.ip));

    	//wifi_bandwidth_t bw;
    	//esp_wifi_get_bandwidth(ESP_IF_WIFI_STA, &bw);
    	//ESP_LOGI(TAG, "Bandwidth = %s...", (bw==1)?"HT20":"HT40");

    	//uint8_t prot;
    	//esp_wifi_get_protocol(ESP_IF_WIFI_STA, &prot);
    	//ESP_LOGI(TAG, "Protocol = %x...", prot);

    	//wifi_ap_record_t apInfo;
    	//esp_wifi_sta_get_ap_info(&apInfo);
    	//ESP_LOGI(TAG, "RSSI = %d...", apInfo.rssi);
    	//ESP_LOGI(TAG, "low rate enabled = %d...", apInfo.low_rate_enable);

        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void) {
    tcpip_adapter_init(); // Init TCP/IP stack.

    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );	// Callback function for event handling related to WiFi.
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); 			// Configuration of WiFi task.
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) ); 						// Init WiFi subsystem (otherwise it is off for saving power).
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );		// Configuration stored only in ram.
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    //ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) ); 			// Operation mode: station, AP, station+AP.
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() ); 							// Tell WiFi subsystem to start working based on previous configuration.
}

void closeSocket(int sock) {
	close(sock);
	return;
}

void sendMsg(int sock, uint8_t* msg) {
	if(write(sock, msg, PACKET_SIZE) < 0) {
		printf("Cannot send msg\n");
		closeSocket(sock);
		exit(1);
	}
	return;
}


static void uart_tx_task(void *pvParameters) {

    uart_config_t uart_config = {
       .baud_rate = 115200,
       .data_bits = UART_DATA_8_BITS,
       .parity = UART_PARITY_DISABLE,
       .stop_bits = UART_STOP_BITS_1,
       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
       .rx_flow_ctrl_thresh = 122,
    };
    uart_param_config(UART_NUM_0, &uart_config);
    uart_driver_install(UART_NUM_0, PACKET_SIZE, 0, 0, NULL, 0);
    uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

	uint8_t counter = 0;
	
	uint16_t packetId = 0;
	uint16_t numPackets = 76800/PAYLOAD_SIZE;
	uint32_t remainingBytes = 76800 % PAYLOAD_SIZE;
	uint32_t imageIndex = 76800; //0;

	uint8_t id = 0;
	uint32_t i = 0;

	while(1) {
		if(xSemaphoreTake(semImage, portMAX_DELAY) == pdTRUE) {
//			counter++;
//			if(counter == 10) {
//				counter = 0;
				
				/*
				id = 0;
				for(i=0; i<IMAGE_BUFF_MAX_SIZE; i++) {
					imageBuff[i] = id;
					if(id == 255) {
						id = 0;
					} else {
						id++;
					}
				}
				*/
				
				/*
				imageIndex = 0;
				// Divide the transmission in smaller chunks.				
				for(packetId=0; packetId<numPackets; packetId++) {
					txBuff[0] = packetId&0xFF;
					txBuff[1] = packetId>>8;
					txBuff[2] = PAYLOAD_SIZE&0xFF;
					txBuff[3] = PAYLOAD_SIZE>>8;
                    memcpy(&txBuff[HEADER_SIZE], &imageBuff[imageIndex], PAYLOAD_SIZE);
					//fwrite((const char*) txBuff, 1, PACKET_SIZE, stdout);
					//fflush(stdout);
					//uart_tx_wait_idle(UART_NUM_0);
					uart_wait_tx_done(UART_NUM_0, 100/portTICK_RATE_MS);
					uart_write_bytes(UART_NUM_0, (const char*) txBuff, PACKET_SIZE);
					//vTaskDelay(200 / portTICK_PERIOD_MS);
					imageIndex += PAYLOAD_SIZE;
				}
				if(remainingBytes > 0) {
					txBuff[0] = packetId&0xFF;
					txBuff[1] = packetId>>8;
					txBuff[2] = remainingBytes&0xFF;
					txBuff[3] = remainingBytes>>8;
                    memcpy(&txBuff[HEADER_SIZE], &imageBuff[imageIndex], remainingBytes);
					//fwrite((const char*) txBuff, 1, remainingBytes+HEADER_SIZE, stdout);
					//fflush(stdout);
					//uart_tx_wait_idle(UART_NUM_0);
					uart_wait_tx_done(UART_NUM_0, 100/portTICK_RATE_MS);
					uart_write_bytes(UART_NUM_0, (const char*) txBuff, remainingBytes+HEADER_SIZE);
                }
				*/

				// Write all at once.
				//fwrite((const char*) &imageBuff[76800], 1, 76800, stdout);				
				//fwrite((const char*) &imageBuff[0], 1, 76800, stdout);
//				uart_wait_tx_done(UART_NUM_0, 100/portTICK_RATE_MS);
//				uart_write_bytes(UART_NUM_0, (const char*) imageBuff, 76800);
//
//			}
			vTaskDelay(200 / portTICK_PERIOD_MS);
			xSemaphoreGive(xSemaphore);
		}
	}
}

static void tcp_client_task(void *pvParameters) {
	struct sockaddr_in serverAddr;
	int sock;
	int err;
	float txTime = 0, throughput = 0;
	struct timeval startTime, exitTime;
	int i = 0;

    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
    //ESP_LOGI(TAG, "Connected to AP");

	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(PORT);
	serverAddr.sin_addr.s_addr = inet_addr("192.168.1.41");

	sock = socket(AF_INET, SOCK_STREAM, 0);
	fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) & ~O_NONBLOCK); // Set to blocking mode.
	printf("Connecting to server: %s:%d\n", inet_ntoa(serverAddr.sin_addr), ntohs(serverAddr.sin_port));
	err = connect(sock, (struct sockaddr*) &serverAddr, sizeof(serverAddr));
	printf("Connected to server!\n");	

	while(1) {
		if(xSemaphoreTake(semImage, portMAX_DELAY) == pdTRUE) {
			gettimeofday(&startTime, NULL);
			for(i=0; i<75; i++) { // 76800/1024=75 packets
				sendMsg(sock, &imageBuff[i*PACKET_SIZE]);
			}
			gettimeofday(&exitTime, NULL);
			txTime = (exitTime.tv_sec*1000000 + exitTime.tv_usec)-(startTime.tv_sec*1000000 + startTime.tv_usec);
			// (PACKET_SIZE*NUM_PACKETS*8)/1'000'000 => Mbits
			// txTime/1'000'000 => seconds
			// ((PACKET_SIZE*NUM_PACKETS*8)/1'000'000) / (txTime/1'000'000) = (PACKET_SIZE*NUM_PACKETS*8)/txTime
			throughput = (float)(PACKET_SIZE*i*8)/txTime;
			
			printf("\r\n");
			printf("%d bytes sent in %.3f ms\r\n", PACKET_SIZE*i, txTime/1000.0);
			printf("Throughput = %.3f Mbit/s\r\n", throughput);
			
			xSemaphoreGive(xSemaphore);
		}
	}

	vTaskDelete(NULL);
	return;
}

// Interrupt service routine, called when the button is pressed.
void IRAM_ATTR button_isr_handler(void* arg) {
	xSemaphoreGiveFromISR(xSemaphore, NULL);	// Notify the button task.
}

// Task that will react to button clicks.
void button_task(void* arg) {

	for(;;) {
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}

void spi_task(void* arg) {

	uint8_t temp = 0;
	esp_err_t ret;
	uint8_t id = 0;
	uint32_t i = 0;	
	uint8_t error = 0;
	uint16_t transCount = 0; // image size / SPI_BUFF_LEN

	uint8_t* spiTxBuff = (uint8_t*) pvPortMallocCaps(SPI_BUFF_LEN, MALLOC_CAP_DMA);
	uint8_t* spiRxBuff = (uint8_t*) pvPortMallocCaps(SPI_BUFF_LEN, MALLOC_CAP_DMA);

	for(temp=0; temp<SPI_BUFF_LEN; temp++) {
		spiTxBuff[temp] = (SPI_BUFF_LEN-1)-temp; // From (SPI_BUFF_LEN-1) to 0.
	}
	spiTxBuff[0]=0xAA;
	spiTxBuff[1]=0xBB;

	spi_slave_transaction_t transaction;
	memset(&transaction, 0, sizeof(transaction));
	transaction.rx_buffer = spiRxBuff;
	transaction.tx_buffer = spiTxBuff;
	transaction.length = SPI_BUFF_LEN*8;
	transaction.user=(void*)0;	// Optional user parameter for the callback.

	for(;;) {
		//vTaskDelay(1000 / portTICK_PERIOD_MS);
		memset (spiRxBuff, 0, SPI_BUFF_LEN);

		// Send command...
		transaction.tx_buffer = spiTxBuff;
		transaction.rx_buffer = spiRxBuff;

		ret = spi_slave_transmit(VSPI_HOST, &transaction, portMAX_DELAY);	
		assert(ret==ESP_OK);
		//for(i=0; i<10; i++) {
		//	printf("%d, ", spiRxBuff[i]);
		//}
		//printf("\r\n\n");
		printf("recv: %d, %d, %d, %d, %d, %d, %d\r\n", spiRxBuff[0], spiRxBuff[1], spiRxBuff[2], spiRxBuff[3], spiRxBuff[SPI_BUFF_LEN-3], spiRxBuff[SPI_BUFF_LEN-2], spiRxBuff[SPI_BUFF_LEN-1]);


/*
		// Receive image.
		for(transCount=0; transCount<1200; transCount++) {
			transaction.rx_buffer = &imageBuff[transCount*SPI_BUFF_LEN];
			ret = spi_slave_transmit(VSPI_HOST, &transaction, portMAX_DELAY);
			assert(ret==ESP_OK);
		}
			
		error = 0;
		id = 0;
		for(i=0; i<76800; i++) {
			if(imageBuff[i] != id) {
				error = 1;
				break;
			}
			if(id == 255) {
				id = 0;
			} else {
				id++;
			}
		}
		if(error == 1) {
			printf("data not received correctly\r\n");
			printf("err: ind=%d, exp=%d, recv=%d\r\n", i, id, imageBuff[i]);
		} else {
			printf("data received correctly\r\n");
		}
*/
	}
}

//This function is called (in irq context!) just before a transmission starts. It will
//set the D/C line to the value indicated in the user field.
void ili_spi_pre_transfer_callback(spi_slave_transaction_t *t)  {
	int dc=(int)t->user;
}

void app_main()
{
    esp_err_t ret;  

	esp_log_level_set("*", ESP_LOG_NONE);  // Set all components to NONE level.

	uint8_t id = 0;
	uint32_t i = 0;
/*
	for(i=0; i<IMAGE_BUFF_MAX_SIZE; i++) {
		imageBuff[i] = id;
		if(id == 255) {
			id = 0;
		} else {
			id++;
		}
	}
*/

	// Create the binary semaphores.
	xSemaphore = xSemaphoreCreateBinary();
	semImage = xSemaphoreCreateBinary();

	// Configure button as GPIO input pin.
	gpio_pad_select_gpio(0);
	gpio_set_direction(0, GPIO_MODE_INPUT);
	
	// Enable interrupt on falling (1->0) edge for button pin.
	gpio_set_intr_type(0, GPIO_INTR_NEGEDGE);
	
	// Start the task that will handle the button.
	xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);
	
	// Install ISR service with default configuration.
	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	
	// Attach the interrupt service routine.
	gpio_isr_handler_add(0, button_isr_handler, NULL);

  	// Configuration for the SPI bus.
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        //.quadwp_io_num = -1,
        //.quadhd_io_num = -1
    };

    // Configuration for the SPI slave interface.
    spi_slave_interface_config_t slvcfg = {
        .mode = 0,							// SPI mode0: CPOL=0, CPHA=0.
        .spics_io_num = PIN_NUM_CS,			// CS pin.
        .queue_size = 3,					// We want to be able to queue 3 transactions at a time.
        .flags = 0,
        //.post_setup_cb=my_post_setup_cb,
        //.post_trans_cb=my_post_trans_cb
    };

    // Enable pull-ups on SPI lines so we don't detect rogue pulses when no master is connected.
    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);

    // Initialize the SPI bus.
    ret = spi_slave_initialize(VSPI_HOST, &buscfg, &slvcfg, 1);
    assert(ret==ESP_OK);

    //nvs_flash_init();
    //initialise_wifi();
    //xTaskCreate(&tcp_client_task, "tcp_client_task", 2048, NULL, 5, NULL);
	//xTaskCreate(&uart_tx_task, "uart_tx_task", 2048, NULL, 5, NULL);
	xTaskCreate(&spi_task, "spi_task", 2048, NULL, 5, NULL);

	//uint8_t data[8] = {'\n', '\n', 'A', 'B', 'C', 'D', '\n', '\n'};
	//fwrite((const char*) data, 1, 8, stdout);

	while(1) {
	
	}

}

