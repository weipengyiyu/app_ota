#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "MQTTClient.h"

#define MQTT_BUFFER_SIZE 8192
#define CAMPAIGN_MAX_SIZE 4096

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t message_received;
static volatile sig_atomic_t message_error;
static const char *output_path;
static const char *ready_path;
static int continuous_mode;

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --host HOST --port PORT --topic TOPIC --client-id ID "
            "--ca FILE --output FILE [--cert FILE --key FILE] "
            "[--username USER --password PASS] [--timeout SEC] "
            "[--continuous --ready-file FILE]\n",
            program);
    exit(2);
}

static void on_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int write_all(int fd, const unsigned char *data, size_t length)
{
    size_t written = 0;

    while (written < length) {
        ssize_t rc = write(fd, data + written, length - written);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return -1;
        written += (size_t)rc;
    }
    return 0;
}

static int publish_atomic_file(const char *path, const void *payload,
                               size_t payload_length)
{
    char temporary[512];
    int fd = -1;
    int rc = -1;

    if (path == NULL || strlen(path) > sizeof(temporary) - 32U)
        return -1;
    if (payload_length == 0 || payload_length > CAMPAIGN_MAX_SIZE ||
        memchr(payload, '\0', payload_length) != NULL)
        return -1;

    snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
             (long)getpid());
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        goto out;
    if (write_all(fd, payload, payload_length) != 0 ||
        write_all(fd, (const unsigned char *)"\n", 1U) != 0 ||
        fsync(fd) != 0)
        goto out;
    if (close(fd) != 0) {
        fd = -1;
        goto out;
    }
    fd = -1;
    if (rename(temporary, path) != 0)
        goto out;
    rc = 0;

out:
    if (fd >= 0)
        close(fd);
    if (rc != 0)
        unlink(temporary);
    return rc;
}

static int publish_campaign_file(const void *payload, size_t payload_length)
{
    return publish_atomic_file(output_path, payload, payload_length);
}

static int publish_ready_file(void)
{
    static const char ready[] = "CONNECTED";

    if (ready_path == NULL)
        return 0;
    return publish_atomic_file(ready_path, ready, sizeof(ready) - 1U);
}

static void campaign_arrived(MessageData *message_data)
{
    MQTTMessage *message = message_data->message;

    if (!continuous_mode && message_received)
        return;
    if (publish_campaign_file(message->payload, message->payloadlen) != 0) {
        fprintf(stderr, "ota-mqtt-sub: rejected or could not persist campaign\n");
        message_error = 1;
        stop_requested = 1;
        return;
    }
    message_received = 1;
}

static const char *next_value(int argc, char **argv, int *index)
{
    if (*index + 1 >= argc)
        usage(argv[0]);
    *index += 1;
    return argv[*index];
}

int main(int argc, char **argv)
{
    const char *host = NULL;
    const char *port = NULL;
    const char *topic = NULL;
    const char *client_id = NULL;
    const char *ca_file = NULL;
    const char *cert_file = NULL;
    const char *key_file = NULL;
    const char *username = NULL;
    const char *password = NULL;
    unsigned char send_buffer[MQTT_BUFFER_SIZE];
    unsigned char receive_buffer[MQTT_BUFFER_SIZE];
    int timeout_seconds = 300;
    time_t deadline;
    Network network;
    MQTTClient client;
    MQTTPacket_connectData connect_data = MQTTPacket_connectData_initializer;
    int mqtt_connected = 0;
    int rc = 1;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--host") == 0)
            host = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--port") == 0)
            port = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--topic") == 0)
            topic = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--client-id") == 0)
            client_id = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--ca") == 0)
            ca_file = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--cert") == 0)
            cert_file = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--key") == 0)
            key_file = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--username") == 0)
            username = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--password") == 0)
            password = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--output") == 0)
            output_path = next_value(argc, argv, &i);
        else if (strcmp(argv[i], "--timeout") == 0)
            timeout_seconds = atoi(next_value(argc, argv, &i));
        else if (strcmp(argv[i], "--continuous") == 0)
            continuous_mode = 1;
        else if (strcmp(argv[i], "--ready-file") == 0)
            ready_path = next_value(argc, argv, &i);
        else
            usage(argv[0]);
    }

    if (host == NULL || port == NULL || topic == NULL || client_id == NULL ||
        ca_file == NULL || output_path == NULL || timeout_seconds < 1 ||
        strlen(topic) > 512U || strlen(client_id) > 128U)
        usage(argv[0]);
    if ((cert_file == NULL) != (key_file == NULL))
        usage(argv[0]);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    unlink(output_path);
    if (ready_path != NULL)
        unlink(ready_path);

    NetworkInit(&network);
    if (NetworkConnectTLS(&network, host, port, ca_file, cert_file,
                          key_file) != 0)
        goto out_network;

    MQTTClientInit(&client, &network, 10000U,
                   send_buffer, sizeof(send_buffer),
                   receive_buffer, sizeof(receive_buffer));
    connect_data.MQTTVersion = 4;
    connect_data.clientID.cstring = (char *)client_id;
    connect_data.username.cstring = (char *)username;
    connect_data.password.cstring = (char *)password;
    connect_data.keepAliveInterval = 30;
    /* A long-running production listener keeps its broker session. The
     * retained Campaign remains the recovery source after a clean restart. */
    connect_data.cleansession = continuous_mode ? 0 : 1;

    if (MQTTConnect(&client, &connect_data) != SUCCESS) {
        fprintf(stderr, "ota-mqtt-sub: MQTT CONNECT rejected\n");
        goto out_network;
    }
    mqtt_connected = 1;
    if (MQTTSubscribe(&client, topic, QOS1, campaign_arrived) != SUCCESS) {
        fprintf(stderr, "ota-mqtt-sub: MQTT SUBSCRIBE rejected\n");
        goto out_mqtt;
    }
    if (publish_ready_file() != 0) {
        fprintf(stderr, "ota-mqtt-sub: could not publish ready state\n");
        goto out_mqtt;
    }
    if (continuous_mode)
        fprintf(stderr, "ota-mqtt-sub: connected and subscribed\n");

    deadline = time(NULL) + timeout_seconds;
    while (!stop_requested &&
           (continuous_mode || !message_received) &&
           (continuous_mode || time(NULL) < deadline)) {
        if (MQTTYield(&client, 1000) != SUCCESS) {
            fprintf(stderr, "ota-mqtt-sub: MQTT connection lost\n");
            goto out_mqtt;
        }
    }
    if (message_error)
        rc = 4;
    else if (continuous_mode)
        rc = stop_requested ? 0 : 1;
    else
        rc = message_received ? 0 : 3;

out_mqtt:
    if (ready_path != NULL)
        unlink(ready_path);
    if (mqtt_connected)
        (void)MQTTDisconnect(&client);
out_network:
    NetworkDisconnect(&network);
    return rc;
}
