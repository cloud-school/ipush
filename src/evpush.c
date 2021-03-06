#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifdef HAVE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#endif
#include <evbase.h>
#ifdef HAVE_EVKQUEUE
#define CONN_MAX 10240
#else
#define CONN_MAX 40960
#endif
#define EV_BUF_SIZE 8192
static int running_status = 0;
static EVBASE *evbase = NULL;
static int ev_sock_type = 0;
static int ev_sock_list[] = {SOCK_STREAM, SOCK_DGRAM, SOCK_RDM};
static int ev_sock_count  = 2;
static int is_use_SSL = 0;
static int ncompleted = 0;
static int nrequest = 0;
static struct sockaddr_in xsa;
static socklen_t xsa_len = sizeof(struct sockaddr);
static int sock_type = 0;
static char *ip = NULL;
static int port = 0;
static int conn_num = 0;
static int limit = 0;
static int keepalive = 0;
void ev_handler(int fd, int ev_flags, void *arg);
void ev_udp_handler(int fd, int ev_flags, void *arg);
#ifdef HAVE_SSL
static SSL_CTX *ctx = NULL;
#endif
typedef struct _CONN
{
    int fd;
    int nreq;
    int nresp;
    char request[EV_BUF_SIZE];
    char response[EV_BUF_SIZE];
    EVENT event;
#ifdef HAVE_SSL
    SSL *ssl;
#endif
}CONN;
static CONN *conns = NULL;
static char *_ymonths[]= {
	"Jan", "Feb", "Mar",
	"Apr", "May", "Jun",
	"Jul", "Aug", "Sep",
	"Oct", "Nov", "Dec"};
#define LOG_HEADER(out)									\
{											\
	struct timeval tv;								\
	time_t timep;  									\
	struct tm *p = NULL;								\
	gettimeofday(&tv, NULL);							\
	time(&timep); 									\
	p = localtime(&timep);								\
	fprintf(out, "[%02d/%s/%04d:%02d:%02d:%02d +%06u] #%u# %s::%d ",		\
			p->tm_mday, _ymonths[p->tm_mon], (1900+p->tm_year), p->tm_hour,	\
			p->tm_min, p->tm_sec, (unsigned int)tv.tv_usec, 			\
			(unsigned int)getpid(), __FILE__, __LINE__);\
}
#ifdef _DEBUG										
#define DEBUG_LOG(format...)								\
{											\
	LOG_HEADER(stdout);								\
	fprintf(stdout, "\"DEBUG:");							\
	fprintf(stdout, format);							\
	fprintf(stdout, "\"\n");							\
	fflush(stdout);									\
}											
#else											
#define  DEBUG_LOG(format...)
#endif

#define FATAL_LOG(format...)								\
{											\
	LOG_HEADER(stderr);								\
	fprintf(stderr, "\"FATAL:");							\
	fprintf(stderr, format);							\
	fprintf(stderr, "\"\n");							\
	fflush(stdout);									\
}
#define WARN_LOG(format...)								\
{											\
	LOG_HEADER(stdout);								\
	fprintf(stdout, "\"WARN:");							\
	fprintf(stdout, format);							\
	fprintf(stdout, "\"\n");							\
}

#define SHOW_LOG(format...)								\
{											\
	LOG_HEADER(stdout);                                                             \
        fprintf(stdout, "\"");                                                    \
        fprintf(stdout, format);                                                        \
        fprintf(stdout, "\"\n");                                                        \
	fflush(stdout);									\
}
int setrlimiter(char *name, int rlimit, int nset)
{
    int ret = -1;
    struct rlimit rlim;
    if(name)
    {
        if(getrlimit(rlimit, &rlim) == -1)
            return -1;
        if(rlim.rlim_cur > nset && rlim.rlim_max > nset)
            return 0;
        rlim.rlim_cur = nset;
        rlim.rlim_max = nset;
        if((ret = setrlimit(rlimit, &rlim)) == 0)
        {
            fprintf(stdout, "setrlimit %s cur[%ld] max[%ld]\n",
                    name, (long)rlim.rlim_cur, (long)rlim.rlim_max);
            return 0;
        }
        else
        {
            fprintf(stderr, "setrlimit %s cur[%ld] max[%ld] failed, %s\n",
                    name, (long)rlim.rlim_cur, (long)rlim.rlim_max, strerror(errno));
        }
    }
    return ret;
}

int new_request()
{
    int fd = 0, flag = 0, opt = 1, prot = 0;
    struct sockaddr_in  lsa;
    socklen_t lsa_len = sizeof(struct sockaddr);

    if(sock_type == SOCK_DGRAM) prot = IPPROTO_UDP;
    if(nrequest < limit && (fd = socket(AF_INET, sock_type, prot)) > 0)
    {
        conns[fd].fd = fd;
        if(is_use_SSL && sock_type == SOCK_STREAM)
        {
            /* Connect */
            if(connect(fd, (struct sockaddr *)&xsa, xsa_len) != 0)
            {
                FATAL_LOG("Connect to %s:%d failed, %s", ip, port, strerror(errno));
                _exit(-1);
            }
#ifdef HAVE_SSL
            conns[fd].ssl = SSL_new(ctx);
            if(conns[fd].ssl == NULL )
            {
                FATAL_LOG("new SSL with created CTX failed:%s\n",
                        ERR_reason_error_string(ERR_get_error()));
                _exit(-1);
            }
            if(SSL_set_fd(conns[fd].ssl, fd) == 0)
            {
                FATAL_LOG("add SSL to tcp socket failed:%s\n",
                        ERR_reason_error_string(ERR_get_error()));
                _exit(-1);
            }
            /* SSL Connect */
            if(SSL_connect(conns[fd].ssl) < 0)
            {
                SHOW_LOG("SSL connection failed:%s\n",
                        ERR_reason_error_string(ERR_get_error()));
                _exit(-1);
            }
#endif
        }
        /* set FD NON-BLOCK */
        if(sock_type == SOCK_STREAM)
        {
            if(!is_use_SSL)
            {
                flag = fcntl(fd, F_GETFL, 0)|O_NONBLOCK;
                fcntl(fd, F_SETFL, flag);
            }
            event_set(&conns[fd].event, fd, E_READ|E_WRITE|E_PERSIST, 
                    (void *)&(conns[fd].event), &ev_handler);
        }
        else
        {
            memset(&lsa, 0, sizeof(struct sockaddr));
            lsa.sin_family = AF_INET;
            if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, 
                        (char *)&opt, (socklen_t) sizeof(int)) != 0
#ifdef SO_REUSEPORT
                    || setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, 
                        (char *)&opt, (socklen_t) sizeof(int)) != 0
#endif
                    || bind(fd, (struct sockaddr *)&lsa, sizeof(struct sockaddr)) != 0) 
            {
                FATAL_LOG("Bind %d to %s:%d failed, %s",
                        fd, inet_ntoa(lsa.sin_addr), ntohs(lsa.sin_port), strerror(errno));
                close(fd);
                return -1;
            }
            //while(1)sleep(1);
            /* Connect */
            /*
            if(connect(fd, (struct sockaddr *)&xsa, xsa_len) != 0)
            {
                FATAL_LOG("Connect to %s:%d failed, %s", ip, port, strerror(errno));
                _exit(-1);
            }
            */
            getsockname(fd, (struct sockaddr *)&lsa, &lsa_len);
            SHOW_LOG("Connected to remote[%s:%d] local[%s:%d] via %d", ip, port, inet_ntoa(lsa.sin_addr), ntohs(lsa.sin_port), fd);
            event_set(&conns[fd].event, fd, E_READ|E_WRITE|E_PERSIST, 
                    (void *)&(conns[fd].event), &ev_udp_handler);
        }
        evbase->add(evbase, &(conns[fd].event));
        conns[fd].nresp = 0;
        conns[fd].nreq = sprintf(conns[fd].request, "{\"last\":\"2013-10-08T07:38:19.958Z\",\"oauth_key\":\"136EQsilHsLMAU2mjSw7tC\",\"installation_id\":\"55416459-b816-4dd7-aa1f-03679a261ab9\",\"v\":\"a1.3.4\"}\n");
        /*
        if(keepalive)
            conns[fd].nreq = sprintf(conns[fd].request, "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
        else
            conns[fd].nreq = sprintf(conns[fd].request, "GET / HTTP/1.0\r\n\r\n");
        */
    }
    else
    {
        if(ncompleted >= limit) running_status = 0;
    }
    return 0;
}

/* sock_dgram /UCP handler */
void ev_udp_handler(int fd, int ev_flags, void *arg)
{
    int n = 0;
    struct sockaddr_in rsa;
    socklen_t rsa_len = sizeof(struct sockaddr);
    if(ev_flags & E_READ)
    {
        if((n = recvfrom(fd, conns[fd].response, EV_BUF_SIZE - 1, 
                        0, (struct sockaddr *)&rsa, &rsa_len)) > 0 )
        {
            SHOW_LOG("Read %d bytes from %d", n, fd);
            conns[fd].response[n] = 0;
            SHOW_LOG("Updating event[%p] on %d ", &conns[fd].event, fd);
            event_add(&conns[fd].event, E_WRITE);	
        }		
        else
        {
            if(n < 0 )
                FATAL_LOG("Reading from %d failed, %s", fd, strerror(errno));
            goto err;
        }
    }
    if(ev_flags & E_WRITE)
    {
        if((n = sendto(fd, conns[fd].request, conns[fd].nreq, 0, &xsa, sizeof(struct sockaddr))) == conns[fd].nreq)
        {
            SHOW_LOG("Wrote %d bytes via %d", n, fd);
        }
        else
        {
            if(n < 0)
                FATAL_LOG("Wrote data via %d failed, %s", fd, strerror(errno));	
            goto err;
        }
        event_del(&conns[fd].event, E_WRITE);
    }
    return ;
err:
    {
        event_destroy(&conns[fd].event);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        SHOW_LOG("Connection %d closed", fd);
    }
}

/* sock stream/TCP handler */
void ev_handler(int fd, int ev_flags, void *arg)
{
    int n = 0;

    if(ev_flags & E_READ)
    {
        if(is_use_SSL)
        {
#ifdef HAVE_SSL
            n = SSL_read(conns[fd].ssl, conns[fd].response, EV_BUF_SIZE);
#else
            n = read(fd, conns[fd].response, EV_BUF_SIZE);
#endif
        }
        else
        {
            n = read(fd, conns[fd].response, EV_BUF_SIZE);
        }
        if(n > 0 )
        {
            conns[fd].response[n] = 0;
            SHOW_LOG("Read %d bytes from %d", n, fd);
            SHOW_LOG("%s", conns[fd].response);
            conns[fd].nresp = 0;
        }	
        else
        {
            if(n < 0 )
                FATAL_LOG("Reading from %d failed, %s", fd, strerror(errno));
            goto err;
        }
    }
    if(ev_flags & E_WRITE)
    {
        if(is_use_SSL)
        {
#ifdef HAVE_SSL
            n = SSL_write(conns[fd].ssl, conns[fd].request, conns[fd].nreq);
#else
            n = write(fd, conns[fd].request, conns[fd].nreq);
#endif
        }
        else
        {
            n = write(fd, conns[fd].request, conns[fd].nreq);
        }
        if(n == conns[fd].nreq)
        {
            conns[fd].nresp = 0;
            nrequest++;
            SHOW_LOG("Wrote %d bytes via %d", n, fd);
        }
        else
        {
            if(n < 0)
                FATAL_LOG("Wrote data via %d failed, %s", fd, strerror(errno));	
            goto err;
        }
        event_del(&conns[fd].event, E_WRITE);
    }
    return ;
err:
    {
        event_destroy(&(conns[fd].event));
#ifdef HAVE_SSL
        if(conns[fd].ssl)
        {
            SSL_shutdown(conns[fd].ssl);
            SSL_free(conns[fd].ssl);
            conns[fd].ssl = NULL;
        }
#endif
        memset(&(conns[fd].event), 0, sizeof(EVENT));
        conns[fd].nresp = 0;
        shutdown(fd, SHUT_RDWR);
        conns[fd].fd = 0;
        close(fd);
       //SHOW_LOG("Connection %d closed", fd);
        ncompleted++; 
        new_request();
    }
}

int main(int argc, char **argv)
{
    int i = 0;

    if(argc < 7)
    {
        fprintf(stderr, "Usage:%s sock_type(0:TCP|1:UDP) iskeepalive ip port conn_num is_use_SSL\n", argv[0]);	
        _exit(-1);
    }	
    ev_sock_type = atoi(argv[1]);
    if(ev_sock_type < 0 || ev_sock_type > ev_sock_count)
    {
        fprintf(stderr, "sock_type must be 0:TCP OR 1:UDP\n");
        _exit(-1);
    }
    sock_type = ev_sock_list[ev_sock_type];
    keepalive = atoi(argv[2]);
    ip = argv[3];
    port = atoi(argv[4]);
    limit = conn_num = atoi(argv[5]);
    is_use_SSL = atoi(argv[6]);
    /* Set resource limit */
    setrlimiter("RLIMIT_NOFILE", RLIMIT_NOFILE, CONN_MAX);	
    /* Initialize global vars */
    if((conns = (CONN *)calloc(CONN_MAX, sizeof(CONN))))
    {
        //memset(events, 0, sizeof(EVENT *) * CONN_MAX);
        /* Initialize inet */ 
        memset(&xsa, 0, sizeof(struct sockaddr_in));	
        xsa.sin_family = AF_INET;
        xsa.sin_addr.s_addr = inet_addr(ip);
        xsa.sin_port = htons(port);
        xsa_len = sizeof(struct sockaddr);
        /* set evbase */
        if((evbase = evbase_init(0)))
        {
            if(is_use_SSL)
            {
#ifdef HAVE_SSL
                SSL_library_init();
                OpenSSL_add_all_algorithms();
                SSL_load_error_strings();
                if((ctx = SSL_CTX_new(SSLv23_client_method())) == NULL)
                {
                    ERR_print_errors_fp(stdout);
                    _exit(-1);
                }
#endif
            }
            for(i = 0; i < conn_num; i++)
            {
                new_request();
            }
            running_status = 1;
            do
            {
                evbase->loop(evbase, 0, NULL);
                //usleep(1000);
            }while(running_status);
            for(i = 0; i < CONN_MAX; i++)
            {
                if(conns[i].fd > 0)
                {
                    event_destroy(&conns[i].event);
                    shutdown(conns[i].fd, SHUT_RDWR);
                    close(conns[i].fd);
#ifdef HAVE_SSL
                    if(conns[i].ssl)
                    {
                        SSL_shutdown(conns[i].ssl);
                        SSL_free(conns[i].ssl); 
                    }
#endif
                }
            }
#ifdef HAVE_SSL
            if(is_use_SSL)
            {
                ERR_free_strings();
                SSL_CTX_free(ctx);
            }
#endif
        }
        free(conns);
    }
    return -1;
}
