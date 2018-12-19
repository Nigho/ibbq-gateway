/*
MIT License

Copyright (c) 2017 Olof Astrand (Ebiroll)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "dns_server.h"

#include <lwip/sockets.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_err.h>
#include <nvs_flash.h>

#include <lwip/err.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <lwip/netdb.h>
#include <lwip/dns.h>

#define STANDARD_QUERY 0b00000000

static const char TAG[] = "DNSSRV";

uint16_t bigEndianInt(char *pData)
{
    uint16_t val = pData[0] << 8;
    val = val | pData[1];
    return val;
}

void receive_thread(void *pvParameters)
{
    dns_server_config_t *cfg = (dns_server_config_t *)pvParameters;
    if (cfg->answer_all)
    {
        ESP_LOGI(TAG, "Answering all A questions with out IP");
    }
    else
    {
        ESP_LOGI(TAG, "Only answering %s with our IP", cfg->hostname);
    }
    int socket_fd;
    struct sockaddr_in sa, ra;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        ESP_LOGE(TAG, "Failed to create socket");
        exit(0);
    }

    memset(&sa, 0, sizeof(struct sockaddr_in));

    tcpip_adapter_ip_info_t ip;
    tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &ip);
    ra.sin_family = AF_INET;
    ra.sin_addr.s_addr = ip.ip.addr;
    ra.sin_port = htons(53);
    if (bind(socket_fd, (struct sockaddr *)&ra, sizeof(struct sockaddr_in)) == -1)
    {
        ESP_LOGE(TAG, "Failed to bind to 53/udp");
        close(socket_fd);
        exit(1);
    }
    ESP_LOGI(TAG, "Listening on local IP: %s", ip4addr_ntoa(&ip.ip.addr));

    struct sockaddr_in client;
    socklen_t client_len;
    client_len = sizeof(client);
    int length;
    char data[80];
    char response[100];
    char ipAddress[INET_ADDRSTRLEN];
    int idx;
    int err;

    ESP_LOGI(TAG, "DNS Server listening on 53/udp");
    while (1)
    {
        length = recvfrom(socket_fd, data, sizeof(data), 0, (struct sockaddr *)&client, &client_len);
        if (length > 0)
        {
            data[length] = '\0';

            inet_ntop(AF_INET, &(client.sin_addr), ipAddress, INET_ADDRSTRLEN);

            uint16_t questionCount = bigEndianInt(&data[4]); //And implicitly data[5]
            uint16_t answerCount = bigEndianInt(&data[6]);
            uint16_t nsCount = bigEndianInt(&data[8]);
            uint16_t arCount = bigEndianInt(&data[10]);

            // Prepare our response
            // Copy ID (16 bits)
            response[0] = data[0];
            response[1] = data[1];
            response[2] = 0b10000100 | (0b00000001 & data[2]); //response, authorative answer, not truncated, copy the recursion bit
            response[3] = 0b00000000;                          //no recursion available, no errors
            response[4] = data[4];
            response[5] = data[5]; //Question count
            response[6] = data[4];
            response[7] = data[5]; //answer count
            response[8] = 0x00;
            response[9] = 0x00; //NS record count
            response[10] = 0x00;
            response[11] = 0x00; //Resource record count

            memcpy(response + 12, data + 12, length - 12); //Copy the rest of the query section
            idx = length;

            if ((data[1] & STANDARD_QUERY) > 0)
            {
                ESP_LOGW(TAG, "Received non query response, responding with SERVFAIL");
                response[3] = 0b00000100; //no recursion available, NOTIMP error
                response[6] = 0x00;
                response[7] = 0x00;
                err = sendto(socket_fd, response, length, 0, (struct sockaddr *)&client, client_len);
                continue;
            }

            size_t label_count = 0;
            size_t buf_pos = 0;
            char label_buf[128];
            for (int i = 12; i < length;)
            {
                if (data[i] == 0x00)
                {
                    break;
                }
                size_t curr_label_len = data[i];
                label_count++;
                if (length < curr_label_len + i)
                {
                    ESP_LOGE(TAG, "malformed DNS request");
                    break;
                }
                memcpy(label_buf + buf_pos, data + i + 1, curr_label_len);
                label_buf[buf_pos + curr_label_len] = '.';
                buf_pos = buf_pos + curr_label_len + 1;
                i += curr_label_len + 1;
            }
            if (buf_pos > 62)
            {
                buf_pos = 62;
            }
            label_buf[buf_pos] = '\0';

            if (strcmp(cfg->hostname, label_buf) != 0)
            {
                ESP_LOGD(TAG, "We don't know %s", label_buf);
                // TODO answer with no unknown
                response[3] = 0b00000101; //no recursion available, REFUSED error
                response[6] = 0x00;
                response[7] = 0x00;
                err = sendto(socket_fd, response, length, 0, (struct sockaddr *)&client, client_len);
                continue;
            }

            memcpy(response + 12, data + 12, length - 12); //Copy the rest of the query section
            idx = length;

            // Prune off the OPT
            // FIXME: We should parse the packet better than this!
            if ((response[idx - 11] == 0x00) && (response[idx - 10] == 0x00) && (response[idx - 9] == 0x29))
                idx -= 11;

            //Set a pointer to the domain name in the question section
            response[idx] = 0xC0;
            response[idx + 1] = 0x0C;

            //Set the type to "Host Address"
            response[idx + 2] = 0x00;
            response[idx + 3] = 0x01;

            //Set the response class to IN
            response[idx + 4] = 0x00;
            response[idx + 5] = 0x01;

            //A 32 bit integer specifying TTL in seconds, 0 means no caching
            response[idx + 6] = 0x00;
            response[idx + 7] = 0x00;
            response[idx + 8] = 0x00;
            response[idx + 9] = 0x00;

            //RDATA length
            response[idx + 10] = 0x00;
            response[idx + 11] = 0x04; //4 byte IP address

            //The IP address
            // TODO use IP address from interface
            memcpy(&response[idx + 12], &ip.ip.addr, 4);
            /*response[idx + 12] = 192;
            response[idx + 13] = 168;
            response[idx + 14] = 4;
            response[idx + 15] = 1;*/

            err = sendto(socket_fd, response, idx + 16, 0, (struct sockaddr *)&client, client_len);
            if (err < 0)
            {
                ESP_LOGE(TAG, "sendto failed: %s", strerror(errno));
            }
        }
    }
    close(socket_fd);
}

void init_dns_server(dns_server_config_t *cfg)
{
    xTaskCreate(&receive_thread, "receive_thread", 3048, cfg, 5, NULL);
}
