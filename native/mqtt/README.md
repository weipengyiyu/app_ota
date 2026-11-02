# GK1221 OTA network integration

This directory contains the product-specific MQTT transport adapter and the
persistent campaign subscriber. Third-party archives are stored separately:

- `../../third_party/mqtt/paho.mqtt.embedded-c-v1.1.0.tar.gz`
- `../../third_party/http/curl-8.21.0.tar.xz`
- `../../third_party/mbedtls/mbedtls-3.6.4.tar.bz2`

`ota-mqtt-sub` uses MQTT 3.1.1 over TLS 1.2 or newer, verifies the broker
hostname and CA chain, supports a device client certificate, and subscribes
with QoS 1. In production `--continuous` mode it stays connected, maintains a
broker session, and atomically publishes each newest Campaign to `ota-agentd`.
The one-shot timeout mode remains available for diagnostics.

`ota-agentd` supervises the persistent subscriber, reconnects with a bounded
1-to-60-second exponential backoff, and consumes Campaigns without dropping
the MQTT connection during HTTPS download or installation. Randomized HTTPS
Campaign polling runs only while MQTT is not confirmed connected; it is a
fallback rather than a competing primary trigger.

`ota-http` is a size-reduced curl binary supporting HTTP/HTTPS only and linked
to the same mbedTLS build. The daemon forces HTTPS, CA validation and optional
mandatory mTLS, and uses Range requests for resumable downloads.

The MQTT notification is not an installation authority. The daemon downloads
the signed bundle metadata first, verifies it with `/etc/ota_pubkey.pem`, then
downloads and hashes every payload file before invoking `ota-installerd`.
Activation and reboot remain outside this service.

The installed `/etc/ota/agent.conf` is disabled by default. Production
provisioning must replace `DEVICE_ID`, broker/download endpoints, CA, device
certificate and private key, set ownership to root, and finally set
`ENABLED=1`.
