/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#include <stdint.h>          
#include <net/if.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <netdb.h>
#include <linux/ip.h>
#include <linux/icmp.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netinet/if_ether.h>
#include <net/if_arp.h>
#include <asm/byteorder.h>
#include <byteswap.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

/* memif protocol msg, ring and descriptor definitions */
#include <memif.h>
/* memif api */
#include <libmemif.h>
/* socket messaging functions */
#include <socket.h>
/* private structs and functions */
#include <memif_private.h>

#define ERRLIST_LEN 33
#define MAX_ERRBUF_LEN 256

libmemif_main_t libmemif_main;

static char memif_buf[MAX_ERRBUF_LEN];

const char* memif_errlist[ERRLIST_LEN] = { /* MEMIF_ERR_SUCCESS */
    "Success.",
                             /* MEMIF_ERR_SYSCALL */ 
    "Unspecified syscall error (build with -DMEMIF_DBG or make debug).",
                            /* MEMIF_ERR_ACCES */
    "Permission to resoure denied.",
                            /* MEMIF_ERR_FILE_LIMIT */
    "System limit on total numer of open files reached.",
                            /* MEMIF_ERR_PROC_FILE_LIMIT */
    "Per-process limit on total number of open files reached.",
                            /* MEMIF_ERR_ALREADY */
    "Connection already requested.",
                            /* MEMIF_ERR_AGAIN */
    "File descriptor refers to file other than socket, or operation would block.",
                            /* MEMIF_ERR_BAD_FD */
    "Bad file descriptor.",
                            /* MEMIF_ERR_NOMEM */
    "Out of memory.",
                            /* MEMIF_ERR_INVAL_ARG */
    "Invalid argument.",
                            /* MEMIF_ERR_NOCONN */
    "Memif connection handle does not point to existing conenction",
                            /* MEMIF_ERR_CONN */
    "Memif connection handle points to existing connection",
                            /* MEMIF_ERR_CB_FDUPDATE */
    "User defined callback memif_control_fd_update_t returned error",
                            /* MEMIF_ERR_FILE_NOT_SOCK */
    "File specified by socket filename exists and is not socket.",
                            /* MEMIF_ERR_NO_SHMFD */
    "Missing shared memory file descriptor. (internal error)",
                            /* MEMIF_ERR_COOKIE */
    "Invalid cookie on ring. (internal error)",
                            /* MEMIF_ERR_NOBUF_RING */
    "Ring buffer full.",
                            /* MEMIF_ERR_NOBUF */
    "Not enough memif buffers. There are unreceived data in shared memory.",
                            /* MEMIF_ERR_INT_WRITE */
    "Send interrupt error.",
                            /* MEMIF_ERR_MFMSG */
    "Malformed message received on control channel.",
                            /* MEMIF_ERR_PROTO */
    "Incompatible memory interface protocol version.",
                            /* MEMIF_ERR_ID */
    "Unmatched interface id.",
                            /* MEMIF_ERR_ACCSLAVE */
    "Slave cannot accept connection reqest.",
                            /* MEMIF_ERR_ALRCONN */
    "Interface is already connected.",
                            /* MEMIF_ERR_MODE */
    "Mode mismatch.",
                            /* MEMIF_ERR_SECRET */
    "Secret mismatch.",
                            /* MEMIF_ERR_NOSECRET */
    "Secret required.",
                            /* MEMIF_ERR_MAXREG */
    "Limit on total number of regions reached.",
                            /* MEMIF_ERR_MAXRING */
    "Limit on total number of ring reached.",
                            /* MEMIF_ERR_NO_INTFD */
    "Missing interrupt file descriptor. (internal error)",
                            /* MEMIF_ERR_DISCONNECT */
    "Interface received disconnect request.",
                            /* MEMIF_ERR_DISCONNECTED */
    "Interface is disconnected.",
                            /* MEMIF_ERR_UNKNOWN_MSG */
    "Unknown message type received on control channel. (internal error)"
 };

#define MEMIF_ERR_UNDEFINED "undefined error"

char *
memif_strerror (int err_code)
{
    if (err_code > ERRLIST_LEN)
    {
        strncpy (memif_buf, MEMIF_ERR_UNDEFINED, strlen (MEMIF_ERR_UNDEFINED));
        memif_buf[strlen (MEMIF_ERR_UNDEFINED)] = '\0';
    }
    else
    {
        strncpy (memif_buf, memif_errlist[err_code], strlen (memif_errlist[err_code]));
        memif_buf[strlen (memif_errlist[err_code])] = '\0';
    }
    return memif_buf;
}

#define DBG_TX_BUF (0)
#define DBG_RX_BUF (1)

#if MEMIF_DBG_SHM
static void
print_bytes (void *data, uint16_t len, uint8_t q)
{
    if (q == DBG_TX_BUF)
        printf ("\nTX:\n\t");
    else
        printf ("\nRX:\n\t");
    int i;
    for (i = 0; i < len; i++)
        {
            if (i % 8 == 0)
                printf ("\n%d:\t", i);
            printf ("%02X ", ((uint8_t *) (data)) [i]);
        }
    printf ("\n\n");
}
#endif /* MEMIF_DBG */

int
memif_syscall_error_handler (int err_code)
{
    DBG_UNIX ("%s", strerror (err_code));

    if (err_code == 0)
        return MEMIF_ERR_SUCCESS;
    if (err_code == EACCES)
        return MEMIF_ERR_ACCES;
    if (err_code == ENFILE)
        return MEMIF_ERR_FILE_LIMIT;
    if (err_code == EMFILE)
        return MEMIF_ERR_PROC_FILE_LIMIT;
    if (err_code == ENOMEM)
        return MEMIF_ERR_NOMEM;
/* connection refused if master dows not exist
    this error would spam the user until master was created */
    if (err_code == ECONNREFUSED)
        return MEMIF_ERR_SUCCESS;
    if (err_code == EALREADY)
        return MEMIF_ERR_ALREADY;
    if (err_code == EAGAIN)
        return MEMIF_ERR_AGAIN;
    if (err_code == EBADF)
        return MEMIF_ERR_BAD_FD;

    /* other syscall errors */
    return MEMIF_ERR_SYSCALL;
}

int memif_init (memif_control_fd_update_t *on_control_fd_update)
{
    int err = MEMIF_ERR_SUCCESS; /* 0 */
    libmemif_main_t *lm = &libmemif_main;
    lm->control_fd_update = on_control_fd_update;
    memset (&lm->ms, 0, sizeof (memif_socket_t));
    lm->conn = NULL;

    lm->timerfd = timerfd_create (CLOCK_REALTIME, TFD_NONBLOCK);
    if (lm->timerfd < 0)
    {
        err = errno;
        DBG ("timerfd: %s", strerror (err));
        return memif_syscall_error_handler (err);
    }

    lm->arm.it_value.tv_sec = 2;
    lm->arm.it_value.tv_nsec = 0;
    lm->arm.it_interval.tv_sec = 2;
    lm->arm.it_interval.tv_nsec = 0;
    memset (&lm->disarm, 0, sizeof (lm->disarm));

    /* check return or not? */
    if (lm->control_fd_update (lm->timerfd, MEMIF_FD_EVENT_READ) < 0)
    {
        DBG ("user defined callback type memif_control_fd_update_t error!");
        return MEMIF_ERR_CB_FDUPDATE;
    }    

    return 0;
}

static inline memif_ring_t *
memif_get_ring (memif_connection_t *conn, memif_ring_type_t type, uint16_t rn)
{
    if (conn->regions == NULL)
        return NULL;
    /* TODO: support multiple regions */
    void *p = conn->regions->shm;
    int ring_size =
        sizeof (memif_ring_t) +
        sizeof (memif_desc_t) * (1 << conn->args.log2_ring_size);
    p += (rn + type * conn->args.num_s2m_rings) * ring_size;

    return (memif_ring_t *) p;
}

int
memif_create (memif_conn_handle_t *c, memif_conn_args_t *args,
              memif_connection_update_t *on_connect,
              memif_connection_update_t *on_disconnect,
              void *private_ctx)
{
    int err;
    int sockfd = -1;
    memif_connection_t *conn = (memif_connection_t *) *c;
    if (conn != NULL)
    {
        DBG ("This handle already points to existing memif.");
        return MEMIF_ERR_CONN;
    }
    conn = (memif_connection_t *) malloc (sizeof (memif_connection_t));
    if (conn == NULL)
    {
        err = memif_syscall_error_handler (errno);
        goto error;
    }

    libmemif_main_t *lm = &libmemif_main;

    conn->args.interface_id = args->interface_id;
    /* lib or app? */
    if (args->log2_ring_size == 0)
        args->log2_ring_size = MEMIF_DEFAULT_LOG2_RING_SIZE;
    if (!args->buffer_size == 0)
        args->buffer_size = MEMIF_DEFAULT_BUFFER_SIZE;

    conn->args.num_s2m_rings = args->num_s2m_rings;
    conn->args.num_m2s_rings = args->num_m2s_rings;
    conn->args.buffer_size = args->buffer_size;
    conn->args.log2_ring_size = args->log2_ring_size;
    conn->args.is_master = args->is_master;
    conn->msg_queue = NULL;
    conn->regions = NULL;
    conn->tx_queues = NULL;
    conn->rx_queues = NULL;
    conn->fd = -1;
    conn->on_connect = on_connect;
    conn->on_disconnect = on_disconnect;
    conn->private_ctx = private_ctx;

    uint8_t l = strlen ((char *) args->interface_name);
    strncpy ((char *) conn->args.interface_name, (char *) args->interface_name, l);
    
    l = strlen ((char *) args->instance_name);
    strncpy ((char *) conn->args.instance_name, (char *) args->instance_name, l);

    if (args->socket_filename)
    {
        conn->args.socket_filename = (uint8_t *) malloc (
                    strlen ((char *) args->socket_filename));
        if (conn->args.socket_filename == NULL)
            {
                err = memif_syscall_error_handler (errno);
                goto error;
            }
        strncpy ((char *) conn->args.socket_filename, (char *) args->socket_filename,
                    strlen ((char *) args->socket_filename));
    }
    else
    {
        uint16_t sdl = strlen (MEMIF_DEFAULT_SOCKET_DIR);
        uint16_t sfl = strlen (MEMIF_DEFAULT_SOCKET_FILENAME);
        conn->args.socket_filename = (uint8_t *) malloc (sdl + sfl + 1);
        if (conn->args.socket_filename == NULL)
            {
                err = memif_syscall_error_handler (errno);
                goto error;
            }
        strncpy ((char *) conn->args.socket_filename,
                    MEMIF_DEFAULT_SOCKET_DIR, sdl);
        conn->args.socket_filename[sdl] = '/';
        strncpy ((char *) (conn->args.socket_filename + 1 +sdl),
                    MEMIF_DEFAULT_SOCKET_FILENAME, sfl);
    }

    if (args->secret)
    {
        l = strlen ((char *) args->secret);
        strncpy ((char *) conn->args.secret, (char *) args->secret, l);
    }

    if (conn->args.is_master)
    {
        /* master */
        struct sockaddr_un un;
        struct stat file_stat;
        int on = 1;

        memif_socket_t ms = lm->ms;

        DBG ("memif master mode draft...");

        if (lm->ms.fd == 0)
        {
            if (stat ((char *) conn->args.socket_filename, &file_stat) == 0)
            {
                if (S_ISSOCK (file_stat.st_mode))
                    {
                        unlink ((char *) conn->args.socket_filename);
                    }
                else
                    {
                        err = MEMIF_ERR_FILE_NOT_SOCK;
                        DBG ("file with specified socket filename exists but is not socket");
                        goto error;
                    }
            }

            sockfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
            if (sockfd < 0)
            {
                err = memif_syscall_error_handler (errno);
                goto error;
            }
            un.sun_family = AF_UNIX;
            strncpy ((char *) un.sun_path, (char *) conn->args.socket_filename,
                        sizeof (un.sun_path) - 1);

            if (setsockopt (sockfd, SOL_SOCKET, SO_PASSCRED, &on, sizeof (on)) < 0)
            {
                err = memif_syscall_error_handler (errno);
                goto error;
            }
            if (bind (sockfd, (struct sockaddr *) &un, sizeof (un)) == -1)
            {
                err = memif_syscall_error_handler (errno);
            goto error;
            }
            if (listen (sockfd, 1) == -1)
            {
                err = memif_syscall_error_handler (errno);
                goto error;
            }
            if (stat ((char *) conn->args.socket_filename, &file_stat) == -1)
            {
                err = memif_syscall_error_handler (errno);
                goto error;
            }
            lm->ms.fd = sockfd;
            lm->ms.use_count++;
            lm->ms.filename = malloc (strlen ((char *) conn->args.socket_filename));
            if (lm->ms.filename == NULL)
            {
                err = memif_syscall_error_handler (errno);
                goto error;
            }
            strncpy ((char *) lm->ms.filename, (char *) conn->args.socket_filename,
                            strlen ((char *) conn->args.socket_filename));
        }

        DBG ("fd %d, uc %u, filename %s", lm->ms.fd, lm->ms.use_count, lm->ms.filename);
        conn->fd = lm->ms.fd;
        conn->read_fn = memif_conn_fd_accept_ready;
    }
    else
    {
        if (timerfd_settime (lm->timerfd, 0, &lm->arm, NULL) < 0)
        {
            err = memif_syscall_error_handler (errno);
            goto error;
        }
    }

    *c = lm->conn = conn;

    return 0;

error:
    if (sockfd > 0)
        close (sockfd);
    sockfd = -1;
    if (conn->args.socket_filename)
        free (conn->args.socket_filename);
    if (conn != NULL)
        free (conn);
    *c = conn = NULL;
    return err;
}

/* TODO: support multiple interfaces */
int
memif_control_fd_handler (int fd, uint8_t events)
{
    int i, rv, sockfd = -1, err = MEMIF_ERR_SUCCESS; /* 0 */
    memif_connection_t *conn;
    libmemif_main_t *lm = &libmemif_main;
    if (fd == lm->timerfd)
    {
        uint64_t b;
        ssize_t size;
        size = read (fd, &b, sizeof (b));
            conn = lm->conn;
            if (conn->fd < 0)
            {
                DBG ("try connect");
                struct sockaddr_un sun;
                sockfd = socket (AF_UNIX, SOCK_SEQPACKET, 0);
                if (sockfd < 0)
                {
                    err = memif_syscall_error_handler (errno);
                    goto error;
                }

                sun.sun_family = AF_UNIX;
                strncpy (sun.sun_path, conn->args.socket_filename,
                            sizeof (sun.sun_path) -1);

                if (connect (sockfd, (struct sockaddr *) &sun,
                        sizeof (struct sockaddr_un)) == 0)
                {
                    conn->fd = sockfd;
                    conn->read_fn = memif_conn_fd_read_ready;
                    conn->write_fn = memif_conn_fd_write_ready;
                    conn->error_fn = memif_conn_fd_error;

                    lm->control_fd_update (
                            sockfd, MEMIF_FD_EVENT_READ | MEMIF_FD_EVENT_WRITE);

                    /* TODO: with multiple connections support,
                        only disarm if there is no disconnected slave */
                    if (timerfd_settime (lm->timerfd, 0, &lm->disarm, NULL) < 0)
                    {
                        err = memif_syscall_error_handler (errno);
                        goto error;
                    }
                }
                else
                {
                    err = memif_syscall_error_handler (errno);
                    goto error;
                }
            }
    }
    else
    {
        conn = lm->conn;
        if (conn->fd == fd)
        {
            if (events & MEMIF_FD_EVENT_READ)
            {
                err = conn->read_fn (conn);
                if (err != MEMIF_ERR_SUCCESS)
                    return err;
            }
            if (events & MEMIF_FD_EVENT_WRITE)
            {
                err = conn->write_fn (conn);
                if (err != MEMIF_ERR_SUCCESS)
                    return err;
            }
            if (events & MEMIF_FD_EVENT_ERROR)
            {
                err = conn->error_fn (conn);
                if (err != MEMIF_ERR_SUCCESS)
                    return err;
            }
        }
    }

    return MEMIF_ERR_SUCCESS; /* 0 */

error:
    if (sockfd > 0)
        close (sockfd);
    sockfd = -1;
    return err;
}

static void
memif_msg_queue_free (memif_msg_queue_elt_t **e)
{
    if (*e == NULL)
        return;
    memif_msg_queue_free (&(*e)->next);
    free (*e);
    *e = NULL;
    return;
}

/* send disconnect msg and close interface */
int
memif_disconnect_internal (memif_connection_t *c)
{
    if (c == NULL)
    {
        DBG ("no connection");
        return MEMIF_ERR_NOCONN;
    }

    int err = MEMIF_ERR_SUCCESS; /* 0 */

    libmemif_main_t *lm = &libmemif_main;

    if (c->fd > 0)
    {
        memif_msg_send_disconnect (c, c->remote_disconnect_string, 1);
        lm->control_fd_update (c->fd, MEMIF_FD_EVENT_DEL);
        close (c->fd);
    }
    c->fd = -1;

    /* TODO: support multiple rings */
    if (c->tx_queues != NULL)
    {
        if (c->tx_queues->int_fd > 0)
        {
            lm->control_fd_update (c->tx_queues->int_fd, MEMIF_FD_EVENT_DEL);
            close (c->tx_queues->int_fd);
        }
        c->tx_queues->int_fd = -1;
        free (c->tx_queues);
        c->tx_queues = NULL;
    }
    if (c->rx_queues != NULL)
    {
        if (c->rx_queues->int_fd > 0)
        {
            lm->control_fd_update (c->rx_queues->int_fd, MEMIF_FD_EVENT_DEL);
            close (c->rx_queues->int_fd);
        }
        c->rx_queues->int_fd = -1;
        free (c->rx_queues);
        c->rx_queues = NULL;
    }

    /* TODO: support multiple regions */
    if (c->regions != NULL)
    {
        if (munmap (c->regions->shm, c->regions->region_size) < 0)
            return memif_syscall_error_handler (errno);
        if (c->regions->fd > 0)
            close (c->regions->fd);
        c->regions->fd = -1;
        free (c->regions);
        c->regions = NULL;
    }

    memif_msg_queue_free (&c->msg_queue);

    /* TODO: use timerfd_gettime to check if timer is armed
        only arm if timer is disarmed */
    if (timerfd_settime (lm->timerfd, 0, &lm->arm, NULL) < 0)
    {
        err = memif_syscall_error_handler (errno);
        DBG_UNIX ("timerfd_settime: arm"); 
    }

    c->on_disconnect ((void *) c, c->private_ctx);

    return err;
}

int
memif_delete (memif_conn_handle_t *conn)
{
    memif_connection_t *c = (memif_connection_t *) *conn;
    libmemif_main_t *lm = &libmemif_main;
    
    int err;

    err = memif_disconnect_internal (c);
    if (err == MEMIF_ERR_NOCONN)
        return err;

    /* TODO: only disarm if this is the only disconnected slave */
    if (timerfd_settime (lm->timerfd, 0, &lm->disarm, NULL) < 0)
    {
        err = memif_syscall_error_handler (errno);
        DBG ("timerfd_settime: disarm");
    }

    if (c->args.socket_filename)
        free (c->args.socket_filename);
    c->args.socket_filename = NULL;

    free (c);
    c = NULL;

    *conn = c;
    return err;
}

int
memif_connect1 (memif_connection_t *c)
{
    libmemif_main_t *lm = &libmemif_main;

    memif_region_t *mr = c->regions;
    /* TODO: support multiple regions */
    if (mr != NULL)
    {
        if (!mr->shm)
        {
            if (mr->fd < 0)
                return MEMIF_ERR_NO_SHMFD;

            if ((mr->shm = mmap (NULL, mr->region_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, mr->fd, 0)) == MAP_FAILED)
            {
                return memif_syscall_error_handler (errno);
            }
        }
    }

    /* TODO: support multiple queues */
    int i = 0;
    memif_queue_t *mq = c->tx_queues;
    if (mq != NULL)
    {
        mq->ring = c->regions->shm + mq->offset;
        if (mq->ring->cookie != MEMIF_COOKIE)
        {
            DBG ("wrong cookie on tx ring %u", i);
            return MEMIF_ERR_COOKIE;
        }
    }
    i = 0;
    mq = c->rx_queues;
    if (mq != NULL)
    {
        mq->ring = c->regions->shm + mq->offset;
        if (mq->ring->cookie != MEMIF_COOKIE)
        {
            DBG ("wrong cookie on rx ring %u", i);
            return MEMIF_ERR_COOKIE;
        }
    }

    /* callback */
    lm->control_fd_update (c->fd, MEMIF_FD_EVENT_READ | MEMIF_FD_EVENT_MOD);

    return 0;
}

int
memif_init_regions_and_queues (memif_connection_t *conn)
{
    memif_ring_t *ring = NULL;
    uint64_t buffer_offset;
    memif_region_t *r;
    int i,j;

    conn->regions = (memif_region_t *) malloc (sizeof (memif_region_t));
    if (conn->regions == NULL)
        return memif_syscall_error_handler (errno);
    r = conn->regions;

    buffer_offset = (conn->args.num_s2m_rings + conn->args.num_m2s_rings) *
        (sizeof (memif_ring_t) +
        sizeof (memif_desc_t) * (1 << conn->args.log2_ring_size));

    r->region_size = buffer_offset +
        conn->args.buffer_size * (1 << conn->args.log2_ring_size) *
        (conn->args.num_s2m_rings + conn->args.num_m2s_rings);
    
    if ((r->fd = memfd_create ("memif region 0", MFD_ALLOW_SEALING)) == -1)
        return memif_syscall_error_handler (errno);

    if ((fcntl (r->fd, F_ADD_SEALS, F_SEAL_SHRINK)) == -1)
        return memif_syscall_error_handler (errno);

    if ((ftruncate (r->fd, r->region_size)) == -1)
        return memif_syscall_error_handler (errno);

    if ((r->shm = mmap (NULL, r->region_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, r->fd, 0)) == MAP_FAILED)
        return memif_syscall_error_handler (errno);

    for (i = 0; i < conn->args.num_s2m_rings; i++)
    {
        ring = memif_get_ring (conn, MEMIF_RING_S2M, i);
        ring->head = ring->tail = 0;
        ring->cookie = MEMIF_COOKIE;
        for (j = 0; j < (1 << conn->args.log2_ring_size); j++)
        {
            uint16_t slot = i * (1 << conn->args.log2_ring_size) + j;
            ring->desc[j].region = 0;
            ring->desc[j].offset = buffer_offset +
                    (uint32_t) (slot * conn->args.buffer_size);
            ring->desc[j].buffer_length = conn->args.buffer_size;
        }
    }
    for (i = 0; i < conn->args.num_m2s_rings; i++)
    {
        ring = memif_get_ring (conn, MEMIF_RING_M2S, i);
        ring->head = ring->tail = 0;
        ring->cookie = MEMIF_COOKIE;
        for (j = 0; j < (1 << conn->args.log2_ring_size); j++)
        {
            uint16_t slot = (i + conn->args.num_s2m_rings) * (1 << conn->args.log2_ring_size) + j;
            ring->desc[j].region = 0;
            ring->desc[j].offset = buffer_offset +
                    (uint32_t) (slot * conn->args.buffer_size);
            ring->desc[j].buffer_length = conn->args.buffer_size;
        }
    }
    memif_queue_t *mq;
    int x;
    for (x = 0; x < conn->args.num_s2m_rings; x++)
    {
        mq = (memif_queue_t *) malloc (sizeof (memif_queue_t));
        if ((mq->int_fd = eventfd (0, EFD_NONBLOCK)) < 0)
            return memif_syscall_error_handler (errno);
        mq->ring = memif_get_ring (conn, MEMIF_RING_S2M, x);
        mq->log2_ring_size = conn->args.log2_ring_size;
        mq->region = 0;
        mq->offset = (void *) mq->ring - (void *) conn->regions->shm;
        mq->alloc_bufs = 0;
        conn->tx_queues = mq;
    }

    for (x = 0; x < conn->args.num_m2s_rings; x++)
    {
        mq = (memif_queue_t *) malloc (sizeof (memif_queue_t));
        if ((mq->int_fd = eventfd (0, EFD_NONBLOCK)) < 0)
            return memif_syscall_error_handler (errno);
        mq->ring = memif_get_ring (conn, MEMIF_RING_M2S, x);
        mq->log2_ring_size = conn->args.log2_ring_size;
        mq->region = 0;
        mq->offset = (void *) mq->ring - (void *) conn->regions->shm;
        mq->last_head = 0; 
        mq->alloc_bufs = 0;
        conn->rx_queues = mq;
    }

    return 0;
}

int
memif_buffer_alloc (memif_conn_handle_t conn, uint16_t qid,
                    memif_buffer_t **bufs, uint16_t count, uint16_t *count_out)
{
    memif_connection_t *c = (memif_connection_t *) conn;
    if (c == NULL)
        return MEMIF_ERR_NOCONN;
    if (c->fd < 0)
        return MEMIF_ERR_DISCONNECTED;
    memif_queue_t *mq = c->tx_queues;
    memif_ring_t *ring = mq->ring;
    memif_buffer_t *b0, *b1;
    uint16_t mask = (1 << mq->log2_ring_size) - 1;
    uint16_t s0, s1, ns;
    *count_out = 0;
    int err = MEMIF_ERR_SUCCESS; /* 0 */

    if (ring->tail != ring->head)
    {
        if (ring->head > ring->tail)
            ns = (1 << mq->log2_ring_size) - ring->head + ring->tail;
        else
            ns = ring->tail - ring->head;
    }
    else
        ns = (1 << mq->log2_ring_size);

    /* (head == tail) ? receive function will asume that no packets are available */
    ns -= 1;

    while (count && ns)
    {
        while ((count > 2) && (ns > 2))
        {
            s0 = (ring->head + mq->alloc_bufs + *count_out) & mask;
            s1 = (ring->head + mq->alloc_bufs + *count_out + 1) & mask;

            b0 = (memif_buffer_t *) malloc (sizeof (memif_buffer_t));
            if (b0 == NULL)
                return memif_syscall_error_handler (errno);
            b1 = (memif_buffer_t *) malloc (sizeof (memif_buffer_t));
            if (b1 == NULL)
                return memif_syscall_error_handler (errno);

            b0->desc_index = s0;
            b1->desc_index = s1;
            b0->buffer_len = ring->desc[s0].buffer_length;
            b1->buffer_len = ring->desc[s1].buffer_length;
            /* TODO: support multiple regions -> ring descriptor contains region index */
            b0->data = c->regions->shm + ring->desc[s0].offset;
            b1->data = c->regions->shm + ring->desc[s1].offset;

            (*(bufs + *count_out)) = b0;
            (*(bufs + *count_out + 1)) = b1;
            DBG ("allocated ring slots %u, %u", s0, s1);
            count -= 2;
            ns -= 2;
            *count_out += 2;
        }
        s0 = (ring->head + mq->alloc_bufs + *count_out) & mask;

        b0 = (memif_buffer_t *) malloc (sizeof (memif_buffer_t));
        if (b0 == NULL)
            return memif_syscall_error_handler (errno);

        b0->desc_index = s0;
        b0->buffer_len = ring->desc[s0].buffer_length;
        b0->data = c->regions->shm + ring->desc[s0].offset;

        (*(bufs + *count_out)) = b0;
        DBG ("allocated ring slot %u", s0);
        count--;
        ns--;
        *count_out += 1;
    }

    mq->alloc_bufs += *count_out;

    DBG ("allocated: %u/%u bufs. Total %u allocated bufs", *count_out, count, mq->alloc_bufs);

    if (count)
    {
        DBG ("ring buffer full! qid: %u", qid);
        err = MEMIF_ERR_NOBUF_RING;
    }

    return err;
}

int
memif_buffer_free (memif_conn_handle_t conn, uint16_t qid,
                   memif_buffer_t **bufs, uint16_t count, uint16_t *count_out)
{
    memif_connection_t *c = (memif_connection_t *) conn;
    if (c == NULL)
        return MEMIF_ERR_NOCONN;
    if (c->fd < 0)
        return MEMIF_ERR_DISCONNECTED;
    libmemif_main_t *lm = &libmemif_main;
    memif_queue_t *mq = c->rx_queues;
    memif_ring_t *ring = mq->ring;
    uint16_t tail = ring->tail;
    uint16_t mask = (1 << mq->log2_ring_size) - 1;
    memif_buffer_t *b0, *b1;
    *count_out = 0;

    if (mq->alloc_bufs < count)
        count = mq->alloc_bufs;

    while (count)
    {
        while (count > 2)
        {
            b0 = (*(bufs + *count_out));
            b1 = (*(bufs + *count_out + 1));
            tail = (b0->desc_index + 1) & mask;
            tail = (b1->desc_index + 1) & mask;
            b0->data = NULL;
            b1->data = NULL;
            free (b0);
            free (b1);
            (*(bufs + *count_out)) = b0 = NULL;
            (*(bufs + *count_out + 1)) = b1 = NULL;

            count -= 2;
            *count_out += 2;
        }
        b0 = (*(bufs + *count_out));
        tail = (b0->desc_index + 1) & mask;
        b0->data = NULL;
        free (b0);
        (*(bufs + *count_out)) = b0 = NULL;

        count--;
        *count_out += 1;
    }

    ring->tail = tail;
        
    return MEMIF_ERR_SUCCESS; /* 0 */
}

int
memif_tx_burst (memif_conn_handle_t conn, uint16_t qid,
                memif_buffer_t **bufs, uint16_t count, uint16_t *tx)
{
    memif_connection_t *c = (memif_connection_t *) conn;
    if (c == NULL)
        return MEMIF_ERR_NOCONN;
    if (c->fd < 0)
        return MEMIF_ERR_DISCONNECTED;
    memif_queue_t *mq = c->tx_queues;
    memif_ring_t *ring = mq->ring;
    uint16_t head = ring->head;
    uint16_t mask = (1 << mq->log2_ring_size) - 1;
    *tx = 0;
    memif_buffer_t *b0, *b1;

    while (count)
    {
        while (count > 2)
        {
            b0 = (*(bufs + *tx));
            b1 = (*(bufs + *tx + 1));
            ring->desc[b0->desc_index].length = b0->data_len;
            ring->desc[b1->desc_index].length = b1->data_len;

#ifdef MEMIF_DBG_SHM
            print_bytes (b0->data , b0->data_len, DBG_TX_BUF);
            print_bytes (b1->data , b1->data_len, DBG_TX_BUF);
#endif

            head = (b0->desc_index + 1) & mask;
            head = (b1->desc_index + 1) & mask;

            b0->data = NULL;
            b0->data_len = 0;
            b1->data = NULL;
            b1->data_len = 0;

            count -= 2;
            *tx += 2;
        }

        b0 = (*(bufs + *tx));
        ring->desc[b0->desc_index].length = b0->data_len;

#ifdef MEMIF_DBG_SHM
        print_bytes (b0->data , b0->data_len, DBG_TX_BUF);
#endif

        head = (b0->desc_index + 1) & mask;

        b0->data = NULL;
        b0->data_len = 0;

        count--;
        *tx += 1;
    }
    ring->head = head;

    mq->alloc_bufs -= *tx;

    if ((ring->flags & MEMIF_RING_FLAG_MASK_INT) == 0)
    {
        uint64_t a = 1;
        int r = write (mq->int_fd, &a, sizeof (a));
        if (r < 0)
            return MEMIF_ERR_INT_WRITE;
    }

    return MEMIF_ERR_SUCCESS; /* 0 */
}

int
memif_rx_burst (memif_conn_handle_t conn, uint16_t qid,
                memif_buffer_t **bufs, uint16_t count, uint16_t *rx)
{
    memif_connection_t *c = (memif_connection_t *) conn;
    if (c == NULL)
        return MEMIF_ERR_NOCONN;
    if (c->fd < 0)
        return MEMIF_ERR_DISCONNECTED;
    memif_queue_t *mq = c->rx_queues;
    memif_ring_t *ring = mq->ring;
    uint16_t head = ring->head;
    uint16_t ns;
    uint16_t mask = (1 << mq->log2_ring_size) - 1;
    memif_buffer_t *b0, *b1;
    *rx = 0;

    if (head == mq->last_head)
        return 0;

    if (head > mq->last_head)
        ns = head - mq->last_head;
    else
        ns = (1 << mq->log2_ring_size) - mq->last_head + head;

    while (ns && count)
    {
        while ((ns > 2) && (count > 2))
        {
            b0 = (memif_buffer_t *) malloc (sizeof (memif_buffer_t));
            if (b0 == NULL)
                return memif_syscall_error_handler (errno);
            b1 = (memif_buffer_t *) malloc (sizeof (memif_buffer_t));
            if (b1 == NULL)
                return memif_syscall_error_handler (errno);

            b0->desc_index = mq->last_head;
            b1->desc_index = mq->last_head + 1;
            b0->data = memif_get_buffer (conn, ring, mq->last_head);
            b1->data = memif_get_buffer (conn, ring, mq->last_head + 1);
            b0->data_len = ring->desc[mq->last_head].length;
            b1->data_len = ring->desc[mq->last_head + 1].length;
            b0->buffer_len = ring->desc[mq->last_head].buffer_length;
            b1->buffer_len = ring->desc[mq->last_head + 1].buffer_length;

            (*(bufs + *rx)) = b0;
            (*(bufs + *rx + 1)) = b1;

#ifdef MEMIF_DBG_SHM
            print_bytes (b0->data , b0->data_len, DBG_RX_BUF);
            print_bytes (b1->data , b1->data_len, DBG_RX_BUF);
#endif

            mq->last_head = (mq->last_head + 2) & mask;

            ns -= 2;
            count -= 2;
            *rx += 2;
        }
        b0 = (memif_buffer_t *) malloc (sizeof (memif_buffer_t));
        if (b0 == NULL)
            return memif_syscall_error_handler (errno);

        b0->desc_index = mq->last_head;
        b0->data = memif_get_buffer (conn, ring, mq->last_head);
        b0->data_len = ring->desc[mq->last_head].length;
        b0->buffer_len = ring->desc[mq->last_head].buffer_length;

        (*(bufs + *rx)) = b0;

#ifdef MEMIF_DBG_SHM
        print_bytes (b0->data , b0->data_len, DBG_RX_BUF);
#endif

        mq->last_head = (mq->last_head + 1) & mask;

        ns--;
        count--;
        *rx += 1;
    }

    mq->alloc_bufs += *rx;

    if (ns)
    {
        DBG ("not enough buffers!");
        return MEMIF_ERR_NOBUF;
    }

    return MEMIF_ERR_SUCCESS; /* 0 */
}

int
memif_get_queue_efd (memif_conn_handle_t conn, uint16_t qid, int *efd)
{
    memif_connection_t *c = (memif_connection_t *) conn;
    if (c == NULL)
    {
        *efd = -1;
        return MEMIF_ERR_NOCONN;
    }
    if (c->fd < 0)
        return MEMIF_ERR_DISCONNECTED;
    *efd = c->rx_queues->int_fd;
    return MEMIF_ERR_SUCCESS; /* 0 */
}

memif_details_t
memif_get_details (memif_conn_handle_t conn)
{
    memif_connection_t *c = (memif_connection_t *) conn;
    memif_details_t md;
    memset (&md, 0, sizeof (md));
    /* interface name */
    strncpy ((char *) md.if_name, (char *) c->args.interface_name,
                strlen ((char *) c->args.interface_name));
    /* instance name */
    strncpy ((char *) md.inst_name, (char *) c->args.instance_name,
                strlen ((char *) c->args.instance_name));
    /* remote interface name */
    strncpy ((char *) md.remote_if_name, (char *) c->remote_if_name,
                strlen ((char *) c->remote_if_name));
    /* remote instance name */
    strncpy ((char *) md.remote_inst_name, (char *) c->remote_name,
                strlen ((char *) c->remote_name));
    /* interface id */
    md.id = c->args.interface_id;
    /* secret */
    if (c->args.secret)
        strncpy ((char *) md.secret, (char *) c->args.secret,
                    strlen ((char *) c->args.secret));
    /* role */
    md.role = (c->args.is_master) ? 0 : 1;
    /* mode */
    md.mode = c->args.mode;
    /* socket filename */
    uint32_t l = strlen ((char *) c->args.socket_filename);
    if (l > 128)
        l = 128;
    strncpy ((char *) md.socket_filename, (char *) c->args.socket_filename, l);
    /* ring size */
    md.ring_size = (1 << c->args.log2_ring_size);
    /* buffer size */
    md.buffer_size = c->args.buffer_size;
    /* rx queues */
    md.rx_queues = (c->args.is_master) ? c->args.num_s2m_rings : c->args.num_m2s_rings;
    /* tx queues */
    md.tx_queues = (c->args.is_master) ? c->args.num_m2s_rings : c->args.num_s2m_rings;
    /* link up down */
    md.link_up_down = (c->fd > 0) ? 1 : 0;

    return md;
}
