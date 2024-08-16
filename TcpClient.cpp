#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h> // for IPPROTO_TCP
#include <unistd.h>
#include <memory.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include<sys/wait.h>
#include "TcpClient.h"
#include "TcpServerController.h"
#include "TcpClientServiceManager.h"
#include "network_utils.h"

TcpClient::TcpClient(uint32_t ip_addr, uint16_t port_no) {

    this->ip_addr = ip_addr;
    this->port_no = port_no;
    sem_init(&this->wait_for_thread_operation_to_complete, 0, 0);
    this->ref_count = 0;
}

TcpClient::TcpClient() {

    this->ip_addr = 0;
    this->port_no = 0;
    sem_init(&this->wait_for_thread_operation_to_complete, 0, 0);
    this->ref_count = 0;
}

TcpClient::TcpClient(TcpClient *tcp_client) {

    this->comm_fd = tcp_client->comm_fd;
    this->ip_addr = tcp_client->ip_addr;
    this->port_no = tcp_client->port_no;
    sem_init(&this->wait_for_thread_operation_to_complete, 0, 0);
    this->ref_count = 0;
}

TcpClient::~TcpClient() {

    assert(!this->comm_fd);
    assert(!this->client_thread);
    assert(!this->active_connect_thread);
    assert(!this->ref_count);
    assert(!this->msgd);
    printf ("Client Deleted : ");
    this->Display();
}

int
TcpClient::SendMsg(char *msg, uint32_t msg_size) {

    if (this->comm_fd == 0) return -1;

    int rc = sendto(this->comm_fd, msg, msg_size, 0, NULL, 0);
    if (rc < 0) {
        printf ("sendto failed\n");
        return rc;
    }
    this->conn.bytes_sent += (uint32_t )rc;
    return rc;
}

void
TcpClient::Abort() {

    assert (!this->client_thread);
    assert (!this->active_connect_thread);

    if (this->comm_fd) {
        close(this->comm_fd);
        this->comm_fd = 0;
    }
    sem_destroy(&this->wait_for_thread_operation_to_complete);
    this->tcp_ctrlr = NULL;

    if (this->msgd) {
        this->msgd->Destroy();
        delete this->msgd;
        this->msgd = NULL;
    }

    delete this;
}

TcpClient *
TcpClient::Dereference() {

    if (this->ref_count == 0) {
        this->Abort();
        return NULL;
    }
    
    this->ref_count--;

    if (this->ref_count) return this;
    this->Abort();
    return NULL;
}

void
TcpClient::Reference() {

    ref_count++;
}

void
TcpClient::ClientThreadFunction() {

    int rcv_bytes;
    struct sockaddr_in client_addr;

    socklen_t addr_len = sizeof(client_addr);

#if 0
    if (this->tcp_ctrlr->client_connected) {
        this->tcp_ctrlr->client_connected(this->tcp_ctrlr, this);
    }
#endif

    while (1) {

        pthread_testcancel();

        rcv_bytes = recvfrom(this->comm_fd,
                             this->recv_buffer,
                             sizeof(this->recv_buffer),
                             0,
                             (struct sockaddr *)&client_addr, &addr_len);

        if (rcv_bytes == 0 || rcv_bytes == 65535 || rcv_bytes < 0) {

            if (this->tcp_ctrlr->client_disconnected) {
                this->tcp_ctrlr->client_disconnected(this->tcp_ctrlr, this);
            }

            if (this->IsStateSet (TCP_CLIENT_STATE_ACTIVE_OPENER)) {
                 this->tcp_ctrlr->EnqueMsg(CTRLR_ACTION_TCP_CLIENT_RECONNECT, (void *)this, true);
            }
            else {
                this->tcp_ctrlr->EnqueMsg(CTRLR_ACTION_TCP_CLIENT_DELETE, (void *)this, true);
            }
            assert(0);
        }

        else if (this->msgd) {
            this->conn.bytes_recvd += rcv_bytes;
            this->msgd->ProcessMsg(this, this->recv_buffer, rcv_bytes);
        }
        else if (this->tcp_ctrlr->client_msg_recvd) {
            this->conn.bytes_recvd += rcv_bytes;
            if (this->tcp_ctrlr->client_msg_recvd) {
                this->tcp_ctrlr->client_msg_recvd(this->tcp_ctrlr, this, this->recv_buffer, rcv_bytes);
            }
        }
     }
}

static void *
client_listening_thread(void *arg) {

    TcpClient *tcp_client = (TcpClient *)arg;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    /* This TcpClient object is being operated by this thread */
    tcp_client->Reference();
    sem_post(&tcp_client->wait_for_thread_operation_to_complete);

    tcp_client->ClientThreadFunction();
    return NULL;
}

void
TcpClient::StartThread() {

    pthread_attr_t attr;

    assert (!this->client_thread );
    this->client_thread = (pthread_t *)calloc(1, sizeof(pthread_t));
    pthread_attr_init(&attr);

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    this->SetState (TCP_CLIENT_STATE_THREADED);
    pthread_create(this->client_thread, &attr, client_listening_thread, (void *)this);
    sem_wait(&this->wait_for_thread_operation_to_complete);
}

void
TcpClient::StopThread() {

    assert(this->client_thread);
    pthread_cancel(*(this->client_thread));
    pthread_join(*(this->client_thread), NULL);
    free(this->client_thread);
    this->client_thread = NULL;
    this->UnSetState (TCP_CLIENT_STATE_THREADED);
    this->Dereference();
}

void
TcpClient::StopConnectorThread () {

    assert (this->active_connect_thread);
    pthread_cancel(*(this->active_connect_thread));
    pthread_join(*(this->active_connect_thread), NULL);
    free(this->active_connect_thread);
    this->active_connect_thread = NULL;
    this->UnSetState (TCP_CLIENT_STATE_CONNECT_IN_PROGRESS);
    this->Dereference();
}

void 
TcpClient::Display() {

    char ip_addr_str1[16];
    char ip_addr_str2[16];
    printf ("Tcp Client (%p): [%s , %d]  connected to  [%s , %d] , ref count = %d\n", 
        this,
        network_convert_ip_n_to_p((this->ip_addr), ip_addr_str1), 
        this->port_no,
        network_convert_ip_n_to_p(this->server_ip_addr, ip_addr_str2),
        this->server_port_no,
        this->ref_count);

        printf ("flags : \n");
        printf (" %sSet TCP_CLIENT_STATE_CONNECT_IN_PROGRESS\n", this->IsStateSet (TCP_CLIENT_STATE_CONNECT_IN_PROGRESS) ? "" : "Un");
        printf (" %sSet TCP_CLIENT_STATE_CONNECTED\n", this->IsStateSet (TCP_CLIENT_STATE_CONNECTED) ? "":"Un");
        printf (" %sSet TCP_CLIENT_STATE_PASSIVE_OPENER\n", this->IsStateSet (TCP_CLIENT_STATE_PASSIVE_OPENER) ? "":"Un");
        printf (" %sSet TCP_CLIENT_STATE_ACTIVE_OPENER\n", this->IsStateSet (TCP_CLIENT_STATE_ACTIVE_OPENER) ? "":"Un");
        printf (" %sSet TCP_CLIENT_STATE_KA_BASED\n", this->IsStateSet (TCP_CLIENT_STATE_KA_BASED) ? "":"Un");
        printf (" %sSet TCP_CLIENT_STATE_KA_EXPIRED\n", this->IsStateSet (TCP_CLIENT_STATE_KA_EXPIRED) ? "":"Un");
        printf (" %sSet TCP_CLIENT_STATE_MULTIPLEX_LISTEN\n", this->IsStateSet (TCP_CLIENT_STATE_MULTIPLEX_LISTEN) ? "":"Un");
        printf (" %sSet TCP_CLIENT_STATE_THREADED\n", this->IsStateSet (TCP_CLIENT_STATE_THREADED) ? "":"Un");
        
}

void 
TcpClient::SetTcpMsgDemarcar(TcpMsgDemarcar  *msgd) {
    
    this->msgd = msgd;
}

void 
TcpClient::SetConnectionType(tcp_connection_type_t conn_type) {

    this->conn.conn_type = conn_type;
}

void
TcpClient::SetState(client_state_bit flag_bit) {

    this->state_flags |= flag_bit;
}

void 
TcpClient::UnSetState(client_state_bit flag_bit) {

    this->state_flags &= ~flag_bit;
}

bool 
TcpClient::IsStateSet(client_state_bit flag_bit) {

    return (this->state_flags & flag_bit);
}

static void *
connect_retry_fn(void *arg) {

    char ip_addr_str[16];
    struct sockaddr_in dest;
    struct sockaddr_in self_addr;
    socklen_t sock_len = sizeof(self_addr);

    TcpClient *tcp_client = (TcpClient *)arg;
    
    dest.sin_family = AF_INET;
    dest.sin_port = htons(tcp_client->server_port_no);
    dest.sin_addr.s_addr = htonl(tcp_client->server_ip_addr);

    while (1) {
        
        sleep(10);
        
        printf ("Trying connecting to [%s, %d] again ....\n", 
            network_convert_ip_n_to_p(tcp_client->server_ip_addr, ip_addr_str), tcp_client->server_port_no);

         int rc = connect(tcp_client->comm_fd, (struct sockaddr *)&dest, sizeof(struct sockaddr));

         if (!rc) {
             tcp_client->UnSetState(TCP_CLIENT_STATE_CONNECT_IN_PROGRESS);
             tcp_client->SetState(TCP_CLIENT_STATE_CONNECTED);
             assert(!getsockname( tcp_client->comm_fd, (struct sockaddr *)&self_addr, &sock_len));
             tcp_client->port_no = htons(self_addr.sin_port);
             
             if (tcp_client->tcp_ctrlr->client_connected)
             {
                 tcp_client->tcp_ctrlr->client_connected(tcp_client->tcp_ctrlr, tcp_client);
             }

             tcp_client->tcp_ctrlr->EnqueMsg(CTRLR_ACTION_TCP_CLIENT_ACTIVE_CONNECT_SUCCESS,
                                             (void *)tcp_client, false);
             free(tcp_client->active_connect_thread);
             tcp_client->active_connect_thread = NULL;
             tcp_client->Dereference();
             pthread_exit(0);
         }
        continue;
    }
    return NULL;
}

int
TcpClient::TryClientConnect (bool try_again) {

    struct sockaddr_in dest;
    struct sockaddr_in self_addr;
    socklen_t sock_len = sizeof(self_addr);

    TcpClient *tcp_client = this;

    assert (!tcp_client->active_connect_thread);
    assert (tcp_client->ip_addr);
    assert (!tcp_client->port_no);
    assert (tcp_client->server_ip_addr);
    assert (tcp_client->server_port_no);
    assert (!tcp_client->comm_fd);

    tcp_client->SetState(TCP_CLIENT_STATE_CONNECT_IN_PROGRESS);

    dest.sin_family = AF_INET;
    dest.sin_port = htons(tcp_client->server_port_no);
    dest.sin_addr.s_addr = htonl(tcp_client->server_ip_addr);

    tcp_client->comm_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    int rc = connect(tcp_client->comm_fd, (struct sockaddr *)&dest, sizeof(struct sockaddr));
    
    if (!rc) {
        tcp_client->UnSetState(TCP_CLIENT_STATE_CONNECT_IN_PROGRESS);
        tcp_client->SetState(TCP_CLIENT_STATE_CONNECTED);
        assert(!getsockname( tcp_client->comm_fd, (struct sockaddr *)&self_addr, &sock_len));
        tcp_client->port_no = htons(self_addr.sin_port);
        if (tcp_client->tcp_ctrlr->client_connected) {
            tcp_client->tcp_ctrlr->client_connected(tcp_client->tcp_ctrlr, tcp_client);
        }
        return 0;
    }

    if (!try_again) return (-1);

    tcp_client->active_connect_thread = (pthread_t *)calloc (1, sizeof(pthread_t));
    tcp_client->Reference();
    pthread_create (tcp_client->active_connect_thread, NULL, connect_retry_fn, (void *)tcp_client);
    return 1;
}
