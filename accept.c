/* 
 * tcp server using accept
 *		-- BigBro/2024
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdbool.h>

#define FLOCK_FILE "/tmp/.file_lock_123"

static void usage(char *cmd)
{
	printf("usage: %s\n", cmd);
	printf("\t-h\tshow this help\n");
	printf("\t-l\tlocal IP\n");
	printf("\t-p\tlocal port\n");
	printf("\t-r\tenable reuseaddr\n");
	printf("\t-R\tenable reuseport\n");
	printf("\t-w\tworker number\n");
	printf("\t-f\tuse flock for thunderingherd\n");
	printf("\t-n\tuse nonblock attribute for thunderingherd\n");

	exit(EXIT_SUCCESS);
}

static void set_socket_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0) {
		perror("fcntl for F_GETFL");
		exit(EXIT_FAILURE);
	}

	flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	if (flags < 0) {
		perror("fcntl for F_SETFL");
		exit(EXIT_FAILURE);
	}
}

static int open_flock_file()
{
	int fd = open(FLOCK_FILE, O_RDWR | O_CREAT, 0666);
	if (fd == -1) {
		printf("failed to open file: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	return fd;
}

static bool fd_is_readable(int fd)
{
	struct timeval tv = {
		.tv_sec = 0,
		.tv_usec = 0,
	};
	fd_set rfds;
	int nfds = 0, ret;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	if (nfds <= fd)
		nfds = fd + 1;

	ret = select(nfds, &rfds, NULL, NULL, &tv);
	
	switch(ret) {
	case 0:
	//	printf("fd %d is not readable\n", fd);
		return false;
	case 1:
	//	printf("fd %d is readable\n", fd);
		return true;
	default:
		printf("failed to select fd %d: %s\n", fd, strerror(errno));
		break;
	}

	return false;
}
static void worker_process_accept(int fd)
{
	int nfds = 0;
	fd_set rfds, rfds_orig;

	FD_ZERO(&rfds);
	FD_SET(fd, &rfds);

	rfds_orig = rfds;

	if (nfds <= fd)
		nfds = fd + 1;

	while (1) {
		struct sockaddr_in addr;
		struct timeval tv = {
			.tv_sec = 5,
			.tv_usec = 0,
		};
		int ret, i, len;

		rfds = rfds_orig;

		ret = select(nfds, &rfds, NULL, NULL, &tv);
		switch (ret) {
		case 0:
		//	printf("Timeout...\n");
			continue;
		case -1:
			printf("select failed(%s) and will exit\n", strerror(errno));
			exit(EXIT_FAILURE);
		default:
			if (FD_ISSET(fd, &rfds)) {
				char buffer[1024] = {0};
				struct sockaddr_in addr;
				len = sizeof(struct sockaddr);

				if (getpeername(fd, (struct sockaddr *)&addr, (socklen_t *)&len) != 0) {
					printf("failed to getpeername: %s\n", strerror(errno));
					exit(EXIT_FAILURE);
				}

				ret = read(fd, buffer, sizeof(buffer) - 1);

				switch (ret) {
				case 0:
					printf("close connection for %s:%d\n", inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
					close(fd);
					FD_CLR(fd, &rfds_orig);
					continue;
				case -1:
					printf("failed to read: %s from %s:%d\n", strerror(errno), inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
					exit(EXIT_FAILURE);
				default:
					printf("worker %d read %d bytes from %s:%d: %s\n", getpid(), ret, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port), buffer);

					len = write(fd, buffer, ret);
					if (ret != len) {
						printf("failed to write: %s (%d != %d)\n", strerror(errno), ret, len);
						exit(EXIT_FAILURE);
					}
				}
			} else {
				printf("worker %d: no fd is readable, ignore\n", getpid());
			}
			break;
		}
	}
}

static void worker_process_listen(int fd)
{
	while (1) {
		struct sockaddr_in addr;
		int len = sizeof(struct sockaddr);
		int pid;

		/*
		 * https://elixir.bootlin.com/linux/v2.6.32/source/kernel/wait.c#L86
		 * 		use WQ_FLAG_EXCLUSIVE flag to avoid thundering herd
		 * 		 	wait->flags |= WQ_FLAG_EXCLUSIVE;
		 *			__add_wait_queue_tail(q, wait);
		 */
		fd = accept(fd, (struct sockaddr *)&addr, (socklen_t *)&len);
		if (fd < 0) {
			printf("failed to accept: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		}

		printf("worker %d accepted from %s:%d\n", getpid(), inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

		pid = fork();
		switch (pid) {
		case -1:
			printf("fork: %s\n", strerror(errno));
			exit(EXIT_FAILURE);
		case 0:/* child */
			printf("worker %d started for accept socket\n", getpid());
			worker_process_accept(fd);
			break;
		default:/* parent */
			break;
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
	bool use_flock = false, accept_noblock = false;

	while ((opt = getopt(argc, argv, "l:p:rRw:fnh")) != -1) {
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
		case 'f':
			use_flock = true;
			if (accept_noblock) {
				printf("-f and -n are in conflict\n");
				usage(argv[0]);
			}
			break;
		case 'n':
			accept_noblock = true;
			if (use_flock) {
				printf("-f and -n are in conflict\n");
				usage(argv[0]);
			}
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

	if (accept_noblock) {
		set_socket_nonblock(fd);
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
			exit(EXIT_FAILURE);
		case 0:/* child */
			printf("worker %d started for listen socket\n", getpid());
			worker_process_listen(fd);
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
