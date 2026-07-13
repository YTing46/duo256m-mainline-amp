#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include "rpmsg_user.h"


#define TC_TRANSFER_COUNT 16

typedef struct the_message
{
    unsigned int DATA;
} THE_MESSAGE, *THE_MESSAGE_PTR;

static int read_message(int fd, THE_MESSAGE *msg)
{
    ssize_t n;

    do {
        n = read(fd, msg, sizeof(*msg));
    } while (n < 0 && errno == EINTR);

    if (n != (ssize_t)sizeof(*msg)) {
        if (n < 0)
            perror("read rpmsg endpoint");
        else
            fprintf(stderr, "short read from rpmsg endpoint: %zd bytes\n", n);
        return -1;
    }

    return 0;
}

static int write_message(int fd, const THE_MESSAGE *msg)
{
    ssize_t n;

    do {
        n = write(fd, msg, sizeof(*msg));
    } while (n < 0 && errno == EINTR);

    if (n != (ssize_t)sizeof(*msg)) {
        if (n < 0)
            perror("write rpmsg endpoint");
        else
            fprintf(stderr, "short write to rpmsg endpoint: %zd bytes\n", n);
        return -1;
    }

    return 0;
}

int main(void)
{
/** user-space application source code
  * default channel: rpmsg-sg2002-c906l-channel [dst=0x1e]
  */

    struct rpmsg_endpoint_info ept_info;
    int fd = -1;
    int fd_ept = -1;
    int ret = 1;
    THE_MESSAGE r5_data = {.DATA = 0};

    rpmsg_init_endpoint_info(&ept_info, RPMSG_DEFAULT_EPT_NAME,
                             RPMSG_DEFAULT_SRC_ADDR, RPMSG_DEFAULT_DST_ADDR);
    if (rpmsg_open_endpoint(&ept_info, &fd, &fd_ept) != 0)
        goto out;

    /* send data to remote device */ 
    if (write_message(fd_ept, &r5_data) != 0)
        goto out;

    /* receive data from remote device */
    for (int i = 0; i < TC_TRANSFER_COUNT; i++)
    {    
        if (read_message(fd_ept, &r5_data) != 0)
            goto out;
        printf("ThreadX_data.DATA: %d\n",r5_data.DATA);
        if (write_message(fd_ept, &r5_data) != 0)
            goto out;
    }

    ret = 0;

out:
    rpmsg_close_endpoint(fd, fd_ept);
    return ret;
}
