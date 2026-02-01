/*
 * RPC Client for Video Stream Request Handling
 *
 * Connects to gkapi's local binary API (port 18086) and responds to
 * video_stream_request messages, pretending to be gkcam.
 */

#ifndef RPC_CLIENT_H
#define RPC_CLIENT_H

#include <stdint.h>
#include <pthread.h>

/* RPC settings */
#define RPC_HOST        "127.0.0.1"
#define RPC_PORT        18086
#define RPC_TIMEOUT_SEC 30
#define RPC_RECV_BUF    4096

/* Message delimiter (ETX - End of Text) */
#define RPC_ETX         0x03

/* RPC client state */
typedef struct {
    int sock_fd;
    volatile int running;
    volatile int connected;
    pthread_t thread;
} RPCClient;

/* Global RPC client instance */
extern RPCClient g_rpc_client;

/* Initialize and start RPC client thread */
int rpc_client_start(void);

/* Stop RPC client */
void rpc_client_stop(void);

#endif /* RPC_CLIENT_H */
