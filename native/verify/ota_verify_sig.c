#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/ecp.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#define OTA_READ_CHUNK 4096
#define OTA_MAX_SIG_SIZE 4096

static void print_mbedtls_error(const char *operation, int error)
{
    char message[128];

    mbedtls_strerror(error, message, sizeof(message));
    fprintf(stderr, "ota_verify_sig: %s: %s\n", operation, message);
}

static int sha256_file(const char *path, unsigned char digest[32])
{
    unsigned char buffer[OTA_READ_CHUNK];
    mbedtls_sha256_context context;
    FILE *input;
    size_t bytes_read;
    int result = -1;

    input = fopen(path, "rb");
    if (input == NULL) {
        fprintf(stderr, "ota_verify_sig: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }

    mbedtls_sha256_init(&context);
    if (mbedtls_sha256_starts(&context, 0) != 0) {
        goto done;
    }
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (mbedtls_sha256_update(&context, buffer, bytes_read) != 0) {
            goto done;
        }
    }
    if (ferror(input) == 0 && mbedtls_sha256_finish(&context, digest) == 0) {
        result = 0;
    }

done:
    mbedtls_sha256_free(&context);
    fclose(input);
    return result;
}

static int read_signature(const char *path, unsigned char **signature, size_t *signature_size)
{
    struct stat status;
    FILE *input;
    size_t bytes_read;

    if (stat(path, &status) != 0 || status.st_size <= 0 || status.st_size > OTA_MAX_SIG_SIZE) {
        fprintf(stderr, "ota_verify_sig: invalid signature file %s\n", path);
        return -1;
    }
    input = fopen(path, "rb");
    if (input == NULL) {
        fprintf(stderr, "ota_verify_sig: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    *signature_size = (size_t)status.st_size;
    *signature = malloc(*signature_size);
    if (*signature == NULL) {
        fclose(input);
        return -1;
    }
    bytes_read = fread(*signature, 1, *signature_size, input);
    fclose(input);
    if (bytes_read != *signature_size) {
        free(*signature);
        *signature = NULL;
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    unsigned char digest[32];
    unsigned char *signature = NULL;
    size_t signature_size = 0;
    mbedtls_pk_context public_key;
    int result;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <public-key.pem> <signed-data> <signature.der>\n", argv[0]);
        return 2;
    }
    if (sha256_file(argv[2], digest) != 0 || read_signature(argv[3], &signature, &signature_size) != 0) {
        return 1;
    }

    mbedtls_pk_init(&public_key);
    result = mbedtls_pk_parse_public_keyfile(&public_key, argv[1]);
    if (result != 0) {
        print_mbedtls_error("cannot parse public key", result);
        goto done;
    }
    if (!mbedtls_pk_can_do(&public_key, MBEDTLS_PK_ECDSA) ||
        mbedtls_pk_get_bitlen(&public_key) != 256 ||
        mbedtls_pk_ec(public_key)->MBEDTLS_PRIVATE(grp).id != MBEDTLS_ECP_DP_SECP256R1) {
        fprintf(stderr, "ota_verify_sig: public key must be ECDSA P-256\n");
        result = -1;
        goto done;
    }
    result = mbedtls_pk_verify(&public_key, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature, signature_size);
    if (result != 0) {
        print_mbedtls_error("signature verification failed", result);
        goto done;
    }
    result = 0;

done:
    mbedtls_pk_free(&public_key);
    free(signature);
    return result == 0 ? 0 : 1;
}