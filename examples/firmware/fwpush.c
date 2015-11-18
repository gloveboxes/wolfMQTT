/* fwpush.c
 *
 * Copyright (C) 2006-2015 wolfSSL Inc.
 *
 * This file is part of wolfMQTT.
 *
 * wolfMQTT is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfMQTT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

/* Include the autoconf generated config.h */
#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/signature.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfmqtt/mqtt_client.h>

#include "examples/mqttclient/mqttclient.h"
#include "examples/mqttnet.h"
#include "examples/firmware/fwpush.h"
#include "examples/firmware/firmware.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Configuration */
#define DEFAULT_MQTT_HOST       "iot.eclipse.org"
#define DEFAULT_CMD_TIMEOUT_MS  1000
#define DEFAULT_CON_TIMEOUT_MS  5000
#define DEFAULT_MQTT_QOS        MQTT_QOS_2
#define DEFAULT_KEEP_ALIVE_SEC  60
#define DEFAULT_CLIENT_ID       "WolfMQTTFwPush"

#define MAX_BUFFER_SIZE         FIRMWARE_MAX_PACKET

/* Globals */
static int mStopRead = 0;
const char* mTlsFile = NULL;

/* Usage */
static void Usage(void)
{
    printf("fwpush:\n");
    printf("-?          Help, print this usage\n");
    printf("-f <file>   Firmware file to send\n");
    printf("-h <host>   Host to connect to, default %s\n",
        DEFAULT_MQTT_HOST);
    printf("-p <num>    Port to connect on, default: Normal %d, TLS %d\n",
        MQTT_DEFAULT_PORT, MQTT_SECURE_PORT);
    printf("-t          Enable TLS\n");
    printf("-c <file>   Use provided certificate file\n");
    printf("-q <num>    Qos Level 0-2, default %d\n",
        DEFAULT_MQTT_QOS);
    printf("-s          Disable clean session connect flag\n");
    printf("-k <num>    Keep alive seconds, default %d\n",
        DEFAULT_KEEP_ALIVE_SEC);
    printf("-i <id>     Client Id, default %s\n",
        DEFAULT_CLIENT_ID);
    printf("-l          Enable LWT (Last Will and Testament)\n");
    printf("-u <str>    Username\n");
    printf("-w <str>    Password\n");
}


/* Argument Parsing */
typedef struct func_args {
    int    argc;
    char** argv;
    int    return_code;
} func_args;

#define MY_EX_USAGE 2 /* Exit reason code */

static int myoptind = 0;
static char* myoptarg = NULL;

static int mygetopt(int argc, char** argv, const char* optstring)
{
    static char* next = NULL;

    char  c;
    char* cp;

    if (myoptind == 0)
        next = NULL;   /* we're starting new/over */

    if (next == NULL || *next == '\0') {
        if (myoptind == 0)
            myoptind++;

        if (myoptind >= argc || argv[myoptind][0] != '-' ||
                                argv[myoptind][1] == '\0') {
            myoptarg = NULL;
            if (myoptind < argc)
                myoptarg = argv[myoptind];

            return -1;
        }

        if (strcmp(argv[myoptind], "--") == 0) {
            myoptind++;
            myoptarg = NULL;

            if (myoptind < argc)
                myoptarg = argv[myoptind];

            return -1;
        }

        next = argv[myoptind];
        next++;                  /* skip - */
        myoptind++;
    }

    c  = *next++;
    /* The C++ strchr can return a different value */
    cp = (char*)strchr(optstring, c);

    if (cp == NULL || c == ':')
        return '?';

    cp++;

    if (*cp == ':') {
        if (*next != '\0') {
            myoptarg = next;
            next     = NULL;
        }
        else if (myoptind < argc) {
            myoptarg = argv[myoptind];
            myoptind++;
        }
        else
            return '?';
    }

    return c;
}

static void err_sys(const char* msg)
{
    printf("wolfMQTT error: %s\n", msg);
    if (msg) {
        exit(EXIT_FAILURE);
    }
}

#define MAX_PACKET_ID   ((1 << 16) - 1)
static int mPacketIdLast;
static word16 mqttclient_get_packetid(void)
{
    mPacketIdLast = (mPacketIdLast >= MAX_PACKET_ID) ? 1 : mPacketIdLast + 1;
    return (word16)mPacketIdLast;
}

static int mqttclient_tls_cb(MqttClient* client)
{
    int rc = SSL_SUCCESS;
    (void)client; /* Supress un-used argument */

    printf("MQTT TLS Setup\n");
    if (mTlsFile) {
#if !defined(NO_FILESYSTEM) && !defined(NO_CERTS)
        //rc = wolfSSL_CTX_load_verify_locations(client->tls.ctx, mTlsFile, 0);
#endif
    }
    return rc;
}

static int mqttclient_message_cb(MqttClient *client, MqttMessage *msg,
    byte msg_new, byte msg_done)
{
    (void)client; /* Supress un-used argument */
    (void)msg;
    (void)msg_new;
    (void)msg_done;

    /* Return negative to termine publish processing */
    return MQTT_CODE_SUCCESS;
}

static int fwfile_load(const char* filePath, byte** fileBuf, int *fileLen)
{
    int rc = 0;
    FILE* file = NULL;

    /* Check arguments */
    if (filePath == NULL || strlen(filePath) == 0 || fileLen == NULL ||
        fileBuf == NULL) {
        return EXIT_FAILURE;
    }

    /* Open file */
    file = fopen(filePath, "rb");
    if (file == NULL) {
        printf("File %s does not exist!\n", filePath);
        rc = EXIT_FAILURE;
        goto exit;
    }

    /* Determine length of file */
    fseek(file, 0, SEEK_END);
    *fileLen = (int) ftell(file);
    fseek(file, 0, SEEK_SET);
    //printf("File %s is %d bytes\n", filePath, *fileLen);

    /* Allocate buffer for image */
    *fileBuf = malloc(*fileLen);
    if (*fileBuf == NULL) {
        printf("File buffer malloc failed!\n");
        rc = EXIT_FAILURE;
        goto exit;
    }

    /* Load file into buffer */
    rc = (int)fread(*fileBuf, 1, *fileLen, file);
    if (rc != *fileLen) {
        printf("Error reading file! %d", rc);
        rc = EXIT_FAILURE;
        goto exit;
    }
    rc = 0; /* Success */

exit:
    if (file) {
        fclose(file);
    }
    if (rc != 0) {
        if (*fileBuf) {
            free(*fileBuf);
            *fileBuf = NULL;
        }
    }
    return rc;
}

static int fw_message_build(const char* fwFile, byte **p_msgBuf, int *p_msgLen)
{
    int rc;
    byte *msgBuf = NULL, *sigBuf = NULL, *keyBuf = NULL, *fwBuf = NULL;
    int msgLen = 0, fwLen = 0;
    word32 keyLen = 0, sigLen = 0;
    FirmwareHeader *header;
    ecc_key eccKey;
    RNG rng;

    wc_InitRng(&rng);

    /* Verify file can be loaded */
    rc = fwfile_load(fwFile, &fwBuf, &fwLen);
    if (rc < 0) {
        printf("Firmware File %s Load Error!\n", fwFile);
        Usage();
        goto exit;
    }

    /* Generate Key */
    /* Note: Real implementation would use previously exchanged/signed key */
    wc_ecc_init(&eccKey);
    rc = wc_ecc_make_key(&rng, 32, &eccKey);
    if (rc != 0) {
        printf("Make ECC Key Failed! %d\n", rc);
        goto exit;
    }
    keyLen = ECC_BUFSIZE;
    keyBuf = malloc(keyLen);
    if (!keyBuf) {
        printf("Key malloc failed! %d\n", keyLen);
        rc = EXIT_FAILURE;
        goto exit;
    }
    rc = wc_ecc_export_x963(&eccKey, keyBuf, &keyLen);
    if (rc != 0) {
        printf("ECC public key x963 export failed! %d\n", rc);
        goto exit;
    }

    /* Sign Firmware */
    sigLen = wc_SignatureGetSize(FIRMWARE_SIG_TYPE, &eccKey, sizeof(eccKey));
    if (sigLen <= 0) {
        printf("Signature type %d not supported!\n", FIRMWARE_SIG_TYPE);
        rc = EXIT_FAILURE;
        goto exit;
    }
    sigBuf = malloc(sigLen);
    if (!sigBuf) {
        printf("Signature malloc failed!\n");
        rc = EXIT_FAILURE;
        goto exit;
    }

    /* Display lengths */
    printf("Firmware Message: Sig %d bytes, Key %d bytes, File %d bytes\n",
        sigLen, keyLen, fwLen);

    /* Generate Signature */
    rc = wc_SignatureGenerate(
        FIRMWARE_HASH_TYPE, FIRMWARE_SIG_TYPE,
        fwBuf, fwLen,
        sigBuf, &sigLen,
        &eccKey, sizeof(eccKey),
        &rng);
    if (rc != 0) {
        printf("Signature Generate Failed! %d\n", rc);
        rc = EXIT_FAILURE;
        goto exit;
    }

    /* Assemble message */
    msgLen = sizeof(FirmwareHeader) + sigLen + keyLen + fwLen;
    msgBuf = malloc(msgLen);
    if (!msgBuf) {
        printf("Message malloc failed! %d\n", msgLen);
        rc = EXIT_FAILURE;
        goto exit;
    }
    header = (FirmwareHeader*)msgBuf;
    header->sigLen = sigLen;
    header->pubKeyLen = keyLen;
    header->fwLen = fwLen;
    memcpy(&msgBuf[sizeof(FirmwareHeader)], sigBuf, sigLen);
    memcpy(&msgBuf[sizeof(FirmwareHeader) + sigLen], keyBuf, keyLen);
    memcpy(&msgBuf[sizeof(FirmwareHeader) + sigLen + keyLen], fwBuf, fwLen);

    rc = 0;

exit:

    if (rc == 0) {
        /* Return values */
        if (p_msgBuf) *p_msgBuf = msgBuf;
        if (p_msgLen) *p_msgLen = msgLen;
    }
    else {
        if (msgBuf) free(msgBuf);
    }

    /* Free resources */
    if (keyBuf) free(keyBuf);
    if (sigBuf) free(sigBuf);
    if (fwBuf) free(fwBuf);

    wc_ecc_free(&eccKey);
    wc_FreeRng(&rng);

    return rc;
}

void* fwpush_test(void* args)
{
    int rc;
    char ch;
    word16 port = 0;
    const char* host = DEFAULT_MQTT_HOST;
    MqttClient client;
    int use_tls = 0;
    byte qos = DEFAULT_MQTT_QOS;
    byte clean_session = 1;
    word16 keep_alive_sec = DEFAULT_KEEP_ALIVE_SEC;
    const char* client_id = DEFAULT_CLIENT_ID;
    int enable_lwt = 0;
    const char* username = NULL;
    const char* password = NULL;
    MqttNet net;
    byte *tx_buf = NULL, *rx_buf = NULL;
    byte *msgBuf = NULL;
    int msgLen = 0;
    const char* fwFile = NULL;

    int     argc = ((func_args*)args)->argc;
    char**  argv = ((func_args*)args)->argv;

    ((func_args*)args)->return_code = -1; /* error state */

    while ((rc = mygetopt(argc, argv, "?f:h:p:tc:q:sk:i:lu:w:")) != -1) {
        ch = (char)rc;
        switch (ch) {
            case '?' :
                Usage();
                exit(EXIT_SUCCESS);

            case 'f':
                fwFile = myoptarg;
                break;

            case 'h' :
                host   = myoptarg;
                break;

            case 'p' :
                port = (word16)atoi(myoptarg);
                if (port == 0) {
                    err_sys("Invalid Port Number!");
                }
                break;

            case 't':
                use_tls = 1;
                break;

            case 'c':
                mTlsFile = myoptarg;
                break;

            case 'q' :
                qos = (byte)atoi(myoptarg);
                if (qos > MQTT_QOS_2) {
                    err_sys("Invalid QoS value!");
                }
                break;

            case 's':
                clean_session = 0;
                break;

            case 'k':
                keep_alive_sec = atoi(myoptarg);
                break;

            case 'i':
                client_id = myoptarg;
                break;

            case 'l':
                enable_lwt = 1;
                break;

            case 'u':
                username = myoptarg;
                break;

            case 'w':
                password = myoptarg;
                break;

            default:
                Usage();
                exit(MY_EX_USAGE);
        }
    }

    myoptind = 0; /* reset for test cases */

    /* Start example MQTT Client */
    printf("MQTT Firmware Push Client\n");

    /* Load firmware, sign firmware and create message */
    rc = fw_message_build(fwFile, &msgBuf, &msgLen);
    if (rc != 0) {
        printf("Firmware message build failed! %d\n", rc);
        exit(rc);
    }

    /* Initialize Network */
    rc = MqttClientNet_Init(&net);
    printf("MQTT Net Init: %s (%d)\n",
        MqttClient_ReturnCodeToString(rc), rc);

    /* Initialize MqttClient structure */
    tx_buf = malloc(MAX_BUFFER_SIZE);
    rx_buf = malloc(MAX_BUFFER_SIZE);
    rc = MqttClient_Init(&client, &net, mqttclient_message_cb,
        tx_buf, MAX_BUFFER_SIZE, rx_buf, MAX_BUFFER_SIZE,
        DEFAULT_CMD_TIMEOUT_MS);
    printf("MQTT Init: %s (%d)\n",
        MqttClient_ReturnCodeToString(rc), rc);

    /* Connect to broker */
    rc = MqttClient_NetConnect(&client, host, port, DEFAULT_CON_TIMEOUT_MS,
        use_tls, mqttclient_tls_cb);
    printf("MQTT Socket Connect: %s (%d)\n",
        MqttClient_ReturnCodeToString(rc), rc);

    if (rc == 0) {
        /* Define connect parameters */
        MqttConnect connect;
        MqttMessage lwt_msg;
        connect.keep_alive_sec = keep_alive_sec;
        connect.clean_session = clean_session;
        connect.client_id = client_id;
        /* Last will and testament sent by broker to subscribers of topic when
           broker connection is lost */
        memset(&lwt_msg, 0, sizeof(lwt_msg));
        connect.lwt_msg = &lwt_msg;
        connect.enable_lwt = enable_lwt;
        if (enable_lwt) {
            lwt_msg.qos = qos;
            lwt_msg.retain = 0;
            lwt_msg.topic_name = "lwttopic";
            lwt_msg.buffer = (byte*)DEFAULT_CLIENT_ID;
            lwt_msg.total_len = (word16)strlen(DEFAULT_CLIENT_ID);
        }
        /* Optional authentication */
        connect.username = username;
        connect.password = password;

        /* Send Connect and wait for Connect Ack */
        rc = MqttClient_Connect(&client, &connect);
        printf("MQTT Connect: %s (%d)\n",
            MqttClient_ReturnCodeToString(rc), rc);
        if (rc == MQTT_CODE_SUCCESS) {
            MqttPublish publish;

            /* Validate Connect Ack info */
            printf("MQTT Connect Ack: Return Code %u, Session Present %d\n",
                connect.ack.return_code,
                (connect.ack.flags & MQTT_CONNECT_ACK_FLAG_SESSION_PRESENT) ?
                    1 : 0
            );

            /* Publish Topic */
            publish.retain = 0;
            publish.qos = qos;
            publish.duplicate = 0;
            publish.topic_name = FIRMWARE_TOPIC_NAME;
            publish.packet_id = mqttclient_get_packetid();
            publish.buffer = msgBuf;
            publish.total_len = msgLen;
            rc = MqttClient_Publish(&client, &publish);
            printf("MQTT Publish: Topic %s, %s (%d)\n",
                publish.topic_name, MqttClient_ReturnCodeToString(rc), rc);

            /* Disconnect */
            rc = MqttClient_Disconnect(&client);
            printf("MQTT Disconnect: %s (%d)\n",
                MqttClient_ReturnCodeToString(rc), rc);
        }

        rc = MqttClient_NetDisconnect(&client);
        printf("MQTT Socket Disconnect: %s (%d)\n",
            MqttClient_ReturnCodeToString(rc), rc);
    }

    /* Free resources */
    if (tx_buf) free(tx_buf);
    if (rx_buf) free(rx_buf);
    if (msgBuf) free(msgBuf);

    /* Cleanup network */
    rc = MqttClientNet_DeInit(&net);
    printf("MQTT Net DeInit: %s (%d)\n",
        MqttClient_ReturnCodeToString(rc), rc);

    ((func_args*)args)->return_code = rc;

    return 0;
}


/* so overall tests can pull in test function */
#ifndef NO_MAIN_DRIVER
    #ifdef USE_WINDOWS_API
        BOOL CtrlHandler(DWORD fdwCtrlType)
        {
            if (fdwCtrlType == CTRL_C_EVENT) {
                mStopRead = 1;
                printf("Received Ctrl+c\n");
                return TRUE;
            }
            return FALSE;
        }
    #elif HAVE_SIGNAL
        #include <signal.h>
        static void sig_handler(int signo)
        {
            if (signo == SIGINT) {
                mStopRead = 1;
                printf("Received SIGINT\n");
            }
        }
    #endif

    int main(int argc, char** argv)
    {
        func_args args;

        args.argc = argc;
        args.argv = argv;

#ifdef USE_WINDOWS_API
        if (SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE)) {
            printf("Error setting Ctrl Handler!\n");
        }
#elif HAVE_SIGNAL
        if (signal(SIGINT, sig_handler) == SIG_ERR) {
            printf("Can't catch SIGINT\n");
        }
#endif

        fwpush_test(&args);

        return args.return_code;
    }

#endif /* NO_MAIN_DRIVER */
