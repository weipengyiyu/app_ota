#ifndef OTA_MQTT_PLATFORM_H
#define OTA_MQTT_PLATFORM_H

#include <sys/time.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

typedef struct Timer {
    struct timeval end_time;
} Timer;

typedef struct Network {
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    int connected;
    int (*mqttread)(struct Network *, unsigned char *, int, int);
    int (*mqttwrite)(struct Network *, unsigned char *, int, int);
} Network;

void TimerInit(Timer *timer);
char TimerIsExpired(Timer *timer);
void TimerCountdownMS(Timer *timer, unsigned int timeout);
void TimerCountdown(Timer *timer, unsigned int timeout);
int TimerLeftMS(Timer *timer);

void NetworkInit(Network *network);
int NetworkConnectTLS(Network *network, const char *host, const char *port,
                      const char *ca_file, const char *cert_file,
                      const char *key_file);
void NetworkDisconnect(Network *network);

#endif
