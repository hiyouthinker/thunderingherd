/* 
 * tcp server using epoll
 *		-- BigBro/2024
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <errno.h>

#define MAX_EVENTS 10

static void usage(char *cmd)
{
	printf("usage: %s\n", cmd);
	printf("\t-h\tshow this help\n");
	printf("\t-l\tlocal IP\n");
	printf("\t-p\tlocal port\n");
	printf("\t-r\tenable reuseaddr\n");
	printf("\t-R\tenable reuseport\n");
	printf("\t-w\tworker number\n");

	exit(EXIT_SUCCESS);
}

static void worker_process(int fd)
{
	int timeout = 5000;
	int epfd, listen_fd;
	int nfds, n;
	struct sockaddr_in addr;
	struct epoll_event event, events[MAX_EVENTS];

	epfd = epoll_create(1);
	if (epfd == -1) {
		printf("failed to epoll_create: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	listen_fd = fd;

	event.events = EPOLLIN | EPOLLET | EPOLLEXCLUSIVE;
	event.data.fd = fd;

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) == -1) {
		printf("failed to epoll_ctl: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	while (1) {
		nfds = epoll_wait(epfd, events, MAX_EVENTS, timeout);
		if (nfds == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				printf("failed to epoll_wait: %s\n", strerror(errno));
				exit(EXIT_FAILURE);
			}
		}

		for (n = 0; n < nfds; ++n) {
			if (events[n].events & EPOLLIN) {
				char buffer[1024] = {0};
				int len, ret, accept_fd;

				fd = events[n].data.fd;

				printf("worker %d fd %d is ready to read\n", getpid(), fd);

				len = sizeof(struct sockaddr);

				if (fd == listen_fd) {
					accept_fd = accept(listen_fd, (struct sockaddr *)&addr, (socklen_t *)&len);
					if (accept_fd < 0) {
						printf("failed to accept: %s\n", strerror(errno));
						exit(EXIT_FAILURE);
					}

					event.events = EPOLLIN | EPOLLET;
					event.data.fd = accept_fd;
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, accept_fd, &event) == -1) {
						printf("failed to epoll_ctl: %s\n", strerror(errno));
						exit(EXIT_FAILURE);
					}

					printf("worker %d accepted from %s:%d\n", getpid(), inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
				} else {
					if (getpeername(fd, (struct sockaddr *)&addr, (socklen_t *)&len) != 0) {
						printf("failed to getpeername: %s\n", strerror(errno));
						exit(EXIT_FAILURE);
					}

					len = read(fd, buffer, sizeof(buffer));
					switch (len) {
					case 0:
						printf("close connection for %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
						close(fd);
						continue;
					case -1:
						printf("failed to read: %s from %s:%d\n", strerror(errno), inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
						exit(EXIT_FAILURE);
					default:
						printf("worker %d read %d bytes from %s:%d: %s\n", getpid(), len, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), buffer);

						ret = write(fd, buffer, len);
						if (ret != len) {
							printf("failed to write: %s (%d != %d)\n", strerror(errno), ret, len);
							exit(EXIT_FAILURE);
						}
						break;
					}
				}
			}
		}
	}
}

int main(int argc, char *argv[])
{
	int opt, fd = -1, port = 80, len;
	char *local_ip = "0.0.0.0";
	struct sockaddr_in addr;
	int reuseaddr = 0, reuseport = 0, defer = 0, accept_fd;
	int keepalive_interval = 3, keepalive = 0, one = 1;
	int tcp_fast_open = -1;
	int worker = 2, i;
	struct linger linger = {0, 0};

	while ((opt = getopt(argc, argv, "l:p:rRw:h")) != -1) {
		switch (opt) {
		case 'l':
			local_ip = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			if (port <= 0)
				port = 80;
			break;
		case 'r':
			reuseaddr = 1;
			break;
		case 'R':
			reuseport = 1;
			break;
		case 'w':
			worker = atoi(optarg);
			if (worker <= 0)
				worker = 2;
			break;
		default:
		case 'h':
			usage(argv[0]);
			break;
		}
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		goto error;
	}

	if (reuseaddr && setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
		perror("setsockopt for reuseaddr");
		goto error;
	}
	if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) < 0) {
		perror("setsockopt for reuseport");
		goto error;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = inet_addr(local_ip);
	len = sizeof(struct sockaddr_in);

	printf("Prepare to bind to %s:%d\n", local_ip, port);
	if (bind(fd, (struct sockaddr *)&addr, len) < 0) {
		perror("bind");
		goto error;
	}
	printf("bind successful\n");

	if (listen(fd, 5)) {
		printf("listen: %s\n", strerror(errno));
		goto error;
	}

	for (i = 0; i < worker; i++) {
		int pid = fork();
		switch (pid) {
		case -1:
			printf("fork: %s\n", strerror(errno));
			exit(0);
		case 0:/* child */
			printf("worker %d started\n", getpid());
			worker_process(fd);
			break;
		default:/* parent */
			break;
		}
	}

	while (1) {
		sleep(5);
	}

error:
	if (fd >= 0)
		close(fd);
	return 0;
}
