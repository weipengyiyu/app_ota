#include "ota_mqtt_platform.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/error.h"

static void mqtt_tls_error(const char *operation, int rc)
{
    char detail[160];

    mbedtls_strerror(rc, detail, sizeof(detail));
    fprintf(stderr, "ota-mqtt-sub: %s failed: -0x%04x (%s)\n",
            operation, (unsigned int)-rc, detail);
}

void TimerInit(Timer *timer)
{
    timer->end_time.tv_sec = 0;
    timer->end_time.tv_usec = 0;
}

char TimerIsExpired(Timer *timer)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    if (now.tv_sec > timer->end_time.tv_sec)
        return 1;
    if (now.tv_sec == timer->end_time.tv_sec &&
        now.tv_usec >= timer->end_time.tv_usec)
        return 1;
    return 0;
}

void TimerCountdownMS(Timer *timer, unsigned int timeout)
{
    struct timeval now;
    unsigned long usec;

    gettimeofday(&now, NULL);
    usec = (unsigned long)now.tv_usec + (timeout % 1000U) * 1000UL;
    timer->end_time.tv_sec = now.tv_sec + (time_t)(timeout / 1000U) +
                             (time_t)(usec / 1000000UL);
    timer->end_time.tv_usec = (suseconds_t)(usec % 1000000UL);
}

void TimerCountdown(Timer *timer, unsigned int timeout)
{
    TimerCountdownMS(timer, timeout * 1000U);
}

int TimerLeftMS(Timer *timer)
{
    struct timeval now;
    long long left;

    gettimeofday(&now, NULL);
    left = ((long long)timer->end_time.tv_sec - now.tv_sec) * 1000LL;
    left += ((long long)timer->end_time.tv_usec - now.tv_usec) / 1000LL;
    if (left <= 0)
        return 0;
    if (left > INT_MAX)
        return INT_MAX;
    return (int)left;
}

static int mqtt_tls_read(Network *network, unsigned char *buffer, int length,
                         int timeout_ms)
{
    int total = 0;

    if (timeout_ms <= 0)
        timeout_ms = 1;
    mbedtls_ssl_conf_read_timeout(&network->conf, (uint32_t)timeout_ms);
    while (total < length) {
        int rc = mbedtls_ssl_read(&network->ssl, buffer + total,
                                  (size_t)(length - total));
        if (rc > 0) {
            total += rc;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
            rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (rc == MBEDTLS_ERR_SSL_TIMEOUT)
            return total;
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == 0)
            return total > 0 ? total : -1;
        mqtt_tls_error("read", rc);
        return -1;
    }
    return total;
}

static int mqtt_tls_write(Network *network, unsigned char *buffer, int length,
                          int timeout_ms)
{
    int total = 0;

    (void)timeout_ms;
    while (total < length) {
        int rc = mbedtls_ssl_write(&network->ssl, buffer + total,
                                   (size_t)(length - total));
        if (rc > 0) {
            total += rc;
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
            rc == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        mqtt_tls_error("write", rc);
        return -1;
    }
    return total;
}

void NetworkInit(Network *network)
{
    memset(network, 0, sizeof(*network));
    mbedtls_net_init(&network->net);
    mbedtls_ssl_init(&network->ssl);
    mbedtls_ssl_config_init(&network->conf);
    mbedtls_x509_crt_init(&network->ca);
    mbedtls_x509_crt_init(&network->client_cert);
    mbedtls_pk_init(&network->client_key);
    mbedtls_entropy_init(&network->entropy);
    mbedtls_ctr_drbg_init(&network->ctr_drbg);
    network->mqttread = mqtt_tls_read;
    network->mqttwrite = mqtt_tls_write;
}

int NetworkConnectTLS(Network *network, const char *host, const char *port,
                      const char *ca_file, const char *cert_file,
                      const char *key_file)
{
    static const unsigned char personalization[] = "gk1221-ota-mqtt";
    uint32_t verify_flags;
    int rc;

    rc = mbedtls_ctr_drbg_seed(&network->ctr_drbg, mbedtls_entropy_func,
                               &network->entropy, personalization,
                               sizeof(personalization) - 1U);
    if (rc != 0)
        goto fail_seed;

    rc = mbedtls_x509_crt_parse_file(&network->ca, ca_file);
    if (rc < 0)
        goto fail_ca;

    if ((cert_file == NULL) != (key_file == NULL)) {
        fprintf(stderr, "ota-mqtt-sub: client cert and key must be supplied together\n");
        return -1;
    }
    if (cert_file != NULL) {
        rc = mbedtls_x509_crt_parse_file(&network->client_cert, cert_file);
        if (rc < 0)
            goto fail_cert;
        rc = mbedtls_pk_parse_keyfile(&network->client_key, key_file, NULL,
                                      mbedtls_ctr_drbg_random,
                                      &network->ctr_drbg);
        if (rc != 0)
            goto fail_key;
    }

    rc = mbedtls_ssl_config_defaults(&network->conf,
                                     MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0)
        goto fail_config;
    mbedtls_ssl_conf_authmode(&network->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&network->conf, &network->ca, NULL);
    mbedtls_ssl_conf_rng(&network->conf, mbedtls_ctr_drbg_random,
                         &network->ctr_drbg);
    mbedtls_ssl_conf_min_tls_version(&network->conf,
                                     MBEDTLS_SSL_VERSION_TLS1_2);
    if (cert_file != NULL) {
        rc = mbedtls_ssl_conf_own_cert(&network->conf,
                                       &network->client_cert,
                                       &network->client_key);
        if (rc != 0)
            goto fail_own_cert;
    }

    rc = mbedtls_ssl_setup(&network->ssl, &network->conf);
    if (rc != 0)
        goto fail_setup;
    rc = mbedtls_ssl_set_hostname(&network->ssl, host);
    if (rc != 0)
        goto fail_hostname;
    rc = mbedtls_net_connect(&network->net, host, port,
                             MBEDTLS_NET_PROTO_TCP);
    if (rc != 0)
        goto fail_connect;
    mbedtls_ssl_set_bio(&network->ssl, &network->net,
                        mbedtls_net_send, mbedtls_net_recv,
                        mbedtls_net_recv_timeout);
    do {
        rc = mbedtls_ssl_handshake(&network->ssl);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ ||
             rc == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (rc != 0)
        goto fail_handshake;

    verify_flags = mbedtls_ssl_get_verify_result(&network->ssl);
    if (verify_flags != 0) {
        char verify_info[256];
        mbedtls_x509_crt_verify_info(verify_info, sizeof(verify_info), "",
                                    verify_flags);
        fprintf(stderr, "ota-mqtt-sub: broker certificate rejected: %s\n",
                verify_info);
        return -1;
    }
    network->connected = 1;
    return 0;

fail_handshake: mqtt_tls_error("TLS handshake", rc); return -1;
fail_connect: mqtt_tls_error("TCP connect", rc); return -1;
fail_hostname: mqtt_tls_error("set hostname", rc); return -1;
fail_setup: mqtt_tls_error("TLS setup", rc); return -1;
fail_own_cert: mqtt_tls_error("set client certificate", rc); return -1;
fail_config: mqtt_tls_error("TLS config", rc); return -1;
fail_key: mqtt_tls_error("parse client key", rc); return -1;
fail_cert: mqtt_tls_error("parse client certificate", rc); return -1;
fail_ca: mqtt_tls_error("parse CA certificate", rc); return -1;
fail_seed: mqtt_tls_error("seed RNG", rc); return -1;
}

void NetworkDisconnect(Network *network)
{
    if (network->connected)
        (void)mbedtls_ssl_close_notify(&network->ssl);
    mbedtls_net_free(&network->net);
    mbedtls_ssl_free(&network->ssl);
    mbedtls_ssl_config_free(&network->conf);
    mbedtls_x509_crt_free(&network->ca);
    mbedtls_x509_crt_free(&network->client_cert);
    mbedtls_pk_free(&network->client_key);
    mbedtls_ctr_drbg_free(&network->ctr_drbg);
    mbedtls_entropy_free(&network->entropy);
    network->connected = 0;
}
