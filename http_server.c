#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/tcp.h>

#include "http_parser.h"
#include "http_server.h"
#include "mime_type.h"

#define CRLF "\r\n"

#define FILE_CHUNK_SIZE 65536

#define SEND_HTTP_MSG(socket, buf, fmt, ...)                           \
    do {                                                               \
        int len = snprintf(buf, SEND_BUFFER_SIZE, fmt, ##__VA_ARGS__); \
        if (len >= SEND_BUFFER_SIZE)                                   \
            len = SEND_BUFFER_SIZE - 1;                                \
        char chunk[16];                                                \
        int n = snprintf(chunk, sizeof(chunk), "%x\r\n", len);         \
        http_server_send(socket, chunk, n);                            \
        http_server_send(socket, buf, len);                            \
        http_server_send(socket, "\r\n", 2);                           \
    } while (0)

struct http_request {
    struct socket *socket;
    struct work_struct khttpd_work;
    struct list_head node;
    // 以下為每次 HTTP 請求解析時需要重設的 Metadata
    struct dir_context dir_context;
    char request_url[128];
    enum http_method method;
    int complete;
};

extern struct workqueue_struct *khttpd_wq;
extern char *wwwroot;
struct httpd_service daemon_list = {
    .is_stopped = false,
    .head = LIST_HEAD_INIT(daemon_list.head),
    .lock = __MUTEX_INITIALIZER(daemon_list.lock),
};

static int http_server_recv(struct socket *sock, char *buf, size_t size)
{
    struct kvec iov = {.iov_base = (void *) buf, .iov_len = size};
    struct msghdr msg = {
        .msg_name = 0,
        .msg_namelen = 0,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    return kernel_recvmsg(sock, &msg, &iov, 1, size, msg.msg_flags);
}

static int http_server_send(struct socket *sock, const char *buf, size_t size)
{
    struct msghdr msg = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_control = NULL,
        .msg_controllen = 0,
        .msg_flags = 0,
    };
    int done = 0;
    while (done < size) {
        struct kvec iov = {
            .iov_base = (void *) ((char *) buf + done),
            .iov_len = size - done,
        };
        int length = kernel_sendmsg(sock, &msg, &iov, 1, iov.iov_len);
        if (length < 0) {
            if (length != -EPIPE && length != -ECONNRESET)
                pr_err("write error: %d\n", length);
            break;
        }
        done += length;
    }
    return done;
}

static void send_404(struct http_request *request)
{
    char buf[SEND_BUFFER_SIZE];
    int len = snprintf(buf, SEND_BUFFER_SIZE,
                       "HTTP/1.1 404 Not Found\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: 13\r\n"
                       "Connection: Close\r\n\r\n"
                       "404 Not Found");
    http_server_send(request->socket, buf, len);
    kernel_sock_shutdown(request->socket, SHUT_WR);
}

static void send_301_redirect(struct http_request *request)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "HTTP/1.1 301 Moved Permanently\r\n"
                       "Location: %s/\r\n"
                       "Content-Length: 0\r\n"
                       "Connection: Close\r\n\r\n",
                       request->request_url);
    http_server_send(request->socket, buf, len);
    kernel_sock_shutdown(request->socket, SHUT_WR);
}

// callback for 'iterate_dir', trace entry.
static bool tracedir(struct dir_context *dir_context,
                     const char *name,
                     int namelen,
                     loff_t offset,
                     u64 ino,
                     unsigned int d_type)
{
    if (strcmp(name, ".") && strcmp(name, "..")) {
        struct http_request *request =
            container_of(dir_context, struct http_request, dir_context);

        char buf[SEND_BUFFER_SIZE] = {0};
        SEND_HTTP_MSG(request->socket, buf,
                      "<tr><td><a href=\"%s\">%s</a></td></tr>", name, name);
    }
    return true;
}

static bool handle_directory(struct http_request *request, struct file *fp)
{
    char buf[SEND_BUFFER_SIZE];
    int len;
    request->dir_context.actor = tracedir;

    if (request->method != HTTP_GET) {
        len = snprintf(buf, SEND_BUFFER_SIZE,
                       "HTTP/1.1 501 Not Implemented\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: 19\r\n"
                       "Connection: Close\r\n\r\n"
                       "501 Not Implemented");
        http_server_send(request->socket, buf, len);
        kernel_sock_shutdown(request->socket, SHUT_WR);
        return false;
    }

    len = snprintf(buf, SEND_BUFFER_SIZE,
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: text/html\r\n"
                   "Transfer-Encoding: chunked\r\n"
                   "Connection: Keep-Alive\r\n\r\n");
    http_server_send(request->socket, buf, len);

    SEND_HTTP_MSG(request->socket, buf,
                  "<html><head><style>"
                  "body{font-family: monospace; font-size: 15px;}"
                  "td {padding: 1.5px 6px;}"
                  "</style></head><body><table>");

    iterate_dir(fp, &request->dir_context);
    SEND_HTTP_MSG(request->socket, buf, "</table></body></html>");
    http_server_send(request->socket, "0\r\n\r\n", 5);
    return true;
}

static bool handle_file(struct http_request *request,
                        struct file *fp,
                        const char *filename)
{
    char header[SEND_BUFFER_SIZE];
    char *chunk;
    loff_t offset = 0;
    loff_t size = fp->f_inode->i_size;

    chunk = kmalloc(FILE_CHUNK_SIZE, GFP_KERNEL);
    if (!chunk) {
        pr_err("handle_file: kmalloc(%d) failed\n", FILE_CHUNK_SIZE);
        return false;
    }

    snprintf(header, SEND_BUFFER_SIZE,
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %lld\r\n"
             "Connection: Keep-Alive\r\n\r\n",
             get_mime_str(filename), size);
    http_server_send(request->socket, header, strlen(header));

    while (offset < size) {
        int ret = kernel_read(fp, chunk, FILE_CHUNK_SIZE, &offset);
        if (ret <= 0) {
            pr_err("handle_file: kernel_read failed: %d\n", ret);
            break;
        }
        http_server_send(request->socket, chunk, ret);
    }

    kfree(chunk);
    return true;
}

static bool http_server_response(struct http_request *request, int keep_alive)
{
    char pathbuf[256];
    struct file *fp;

    /* 路徑安全:過濾 .. 防 directory traversal */
    if (strstr(request->request_url, "..")) {
        send_404(request);
        return false;
    }

    snprintf(pathbuf, sizeof(pathbuf), "%s%s", wwwroot, request->request_url);
    fp = filp_open(pathbuf, O_RDONLY, 0);
    if (IS_ERR(fp)) {
        send_404(request);
        return false;
    }

    if (S_ISDIR(fp->f_inode->i_mode)) {
        size_t url_len = strlen(request->request_url);
        if (url_len == 0 || request->request_url[url_len - 1] != '/') {
            send_301_redirect(request);
            filp_close(fp, NULL);
            return false;
        }
        handle_directory(request, fp);
    } else if (S_ISREG(fp->f_inode->i_mode)) {
        handle_file(request, fp, pathbuf);
    }

    filp_close(fp, NULL);
    return true;
}

static int http_parser_callback_message_begin(http_parser *parser)
{
    struct http_request *request = parser->data;
    request->method = 0;
    request->request_url[0] = '\0';
    request->complete = 0;
    return 0;
}

static int http_parser_callback_request_url(http_parser *parser,
                                            const char *p,
                                            size_t len)
{
    struct http_request *request = parser->data;
    strncat(request->request_url, p, len);
    return 0;
}

static int http_parser_callback_header_field(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_header_value(http_parser *parser,
                                             const char *p,
                                             size_t len)
{
    return 0;
}

static int http_parser_callback_headers_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    request->method = parser->method;
    return 0;
}

static int http_parser_callback_body(http_parser *parser,
                                     const char *p,
                                     size_t len)
{
    return 0;
}

static int http_parser_callback_message_complete(http_parser *parser)
{
    struct http_request *request = parser->data;
    http_server_response(request, http_should_keep_alive(parser));
    request->complete = 1;
    return 0;
}

static void http_server_worker(struct work_struct *work)
{
    struct http_request *worker =
        container_of(work, struct http_request, khttpd_work);
    char *buf;
    struct http_parser parser;
    struct http_parser_settings setting = {
        .on_message_begin = http_parser_callback_message_begin,
        .on_url = http_parser_callback_request_url,
        .on_header_field = http_parser_callback_header_field,
        .on_header_value = http_parser_callback_header_value,
        .on_headers_complete = http_parser_callback_headers_complete,
        .on_body = http_parser_callback_body,
        .on_message_complete = http_parser_callback_message_complete,
    };

    buf = mempool_alloc(http_buf_pool, GFP_KERNEL);
    if (!buf) {
        pr_err("can't allocate memory!\n");
        goto cleanup;
    }

    http_parser_init(&parser, HTTP_REQUEST);
    parser.data = worker;
    while (!daemon_list.is_stopped) {
        int ret = http_server_recv(worker->socket, buf, RECV_BUFFER_SIZE - 1);
        if (ret <= 0) {
            if (ret && ret != -ECONNRESET && ret != -EPIPE)
                pr_err("recv error: %d\n", ret);
            break;
        }
        http_parser_execute(&parser, &setting, buf, ret);
        if (worker->complete && !http_should_keep_alive(&parser))
            break;
        memset(buf, 0, RECV_BUFFER_SIZE);
    }
    mempool_free(buf, http_buf_pool);

cleanup:
    mutex_lock(&daemon_list.lock);
    list_del(&worker->node);
    mutex_unlock(&daemon_list.lock);

    kernel_sock_shutdown(worker->socket, SHUT_RDWR);
    sock_release(worker->socket);
    kfree(worker);
}

static struct work_struct *create_work(struct socket *sk)
{
    struct http_request *work = kmalloc(sizeof(*work), GFP_KERNEL);
    if (!work)
        return NULL;

    work->socket = sk;
    INIT_WORK(&work->khttpd_work, http_server_worker);
    mutex_lock(&daemon_list.lock);
    list_add(&work->node, &daemon_list.head);
    mutex_unlock(&daemon_list.lock);
    return &work->khttpd_work;
}

static void free_work(void)
{
    struct http_request *tar;
    mutex_lock(&daemon_list.lock);
    list_for_each_entry (tar, &daemon_list.head, node) {
        kernel_sock_shutdown(tar->socket, SHUT_RDWR);
    }
    mutex_unlock(&daemon_list.lock);
}

int http_server_daemon(void *arg)
{
    struct socket *socket;
    struct work_struct *work;
    struct http_server_param *param = (struct http_server_param *) arg;

    allow_signal(SIGKILL);
    allow_signal(SIGTERM);

    while (!kthread_should_stop()) {
        int err = kernel_accept(param->listen_socket, &socket, 0);
        if (err < 0) {
            if (signal_pending(current))
                break;
            pr_err("kernel_accept() error: %d\n", err);
            continue;
        }

        if (unlikely(!(work = create_work(socket)))) {
            pr_err("can't create more worker process\n");
            kernel_sock_shutdown(socket, SHUT_RDWR);
            sock_release(socket);
            continue;
        }

        queue_work(khttpd_wq, work);
    }

    daemon_list.is_stopped = true;
    free_work();

    return 0;
}
