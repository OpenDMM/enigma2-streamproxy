#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/version.h>

#define MAX_PIDS 32
#define MAX_LINE_LENGTH 512

#define BSIZE                    32712

#if DVB_API_VERSION < 5
#define DMX_ADD_PID              _IO('o', 51)
#define DMX_REMOVE_PID           _IO('o', 52)

typedef enum {
	DMX_TAP_TS = 0,
	DMX_TAP_PES = DMX_PES_OTHER, /* for backward binary compat. */
} dmx_tap_type_t;
#endif

char response_line[MAX_LINE_LENGTH];
int response_p;

int upstream;
int upstream_state, upstream_response_code;
		/*
		 0 - response
		 1 - options
		 2 - body
		 */

int demux_fd = -1;

char *reason = "";

int active_pids[MAX_PIDS];

int handle_upstream(void);
int handle_upstream_line(void);

char authorization[MAX_LINE_LENGTH]; /* the saved Authorization:-client-header which will be forwarded to the server */
char wwwauthenticate[MAX_LINE_LENGTH]; /* the saved WWW-Authenticate:-server-header, which will be forwarded to user client */

static ssize_t safe_write(int fd, const void *buf, size_t count)
{
	const unsigned char *src = buf;
	size_t todo = count;
	ssize_t ret;

	do {
		ret = write(fd, src, todo);
		if (ret < 0) {
			if ((errno != EINTR) && (errno != EAGAIN))
				return ret;
		} else {
			src += ret;
			todo -= ret;
		}
	} while (todo > 0);

	return count;
}

int main(int argc, char **argv)
{
	char request[MAX_LINE_LENGTH], upstream_request[256];
	char xff_header[MAX_LINE_LENGTH]; /* X-Forwarded-For header */
	char *c, *service_ref;

	struct sockaddr_in s_client;
	int ret_val;
	socklen_t len = sizeof(s_client);
	s_client.sin_family = AF_INET;
	ret_val = getpeername(STDIN_FILENO, (struct sockaddr *)&s_client, &len);
	if (!ret_val) {
		snprintf(xff_header, sizeof(xff_header), "X-Forwarded-For: %s\r\n", (char *)inet_ntoa(s_client.sin_addr));
	}
	
	if (!fgets(request, MAX_LINE_LENGTH - 1, stdin))
		goto bad_request;

	if (strncmp(request, "GET /", 5))
		goto bad_request;
	
	c = strchr(request + 5, ' ');
	if (!c || strncmp(c, " HTTP/1.", 7))
		goto bad_request;
	
	*c++ = 0;
	
	c = request + 5;
	
	service_ref = c;
	
	int i;
	for (i=0; i<MAX_PIDS; ++i)
		active_pids[i] = -1;
	
	while (1)
	{
		char option[MAX_LINE_LENGTH];
		if (!fgets(option, MAX_LINE_LENGTH - 1, stdin))
			break;

		if (!strncasecmp(option, "Authorization: ", 15)) /* save authorization header */
			strcpy(authorization, option);
		
		if (option[1] && option[strlen(option)-2] == '\r')
			option[strlen(option)-2] = 0;
		else
			option[strlen(option)-1] = 0;

		if (!*option)
			break;
	}
	
		/* connect to enigma2 */
	upstream = socket(PF_INET, SOCK_STREAM, 0);
	
  struct sockaddr_in sin;
	sin.sin_family = AF_INET;
	sin.sin_port = htons(80);
	sin.sin_addr.s_addr = inet_addr("127.0.0.1");
	if (connect(upstream, (struct sockaddr*)&sin, sizeof(struct sockaddr_in)))
	{
		reason = "Upstream connect failed.";
		goto bad_gateway;
	}

	snprintf(upstream_request, sizeof(upstream_request), "GET /web/stream?StreamService=%s HTTP/1.0\r\n%s%s\r\n", service_ref, xff_header, authorization);

	if (safe_write(upstream, upstream_request, strlen(upstream_request)) != strlen(upstream_request))
		goto bad_gateway;
	
	while (1)
	{
		fd_set r;
		FD_ZERO(&r);
		FD_SET(upstream, &r);
		FD_SET(0, &r);
		if (demux_fd != -1)
			FD_SET(demux_fd, &r);
		
		if (select(5, &r, 0, 0, 0) < 0)
			break;

		if (FD_ISSET(0, &r)) /* check for client disconnect */
			if (read(0, request, sizeof(request)) <= 0)
				break;
		
				/* handle enigma responses */
		if (FD_ISSET(upstream, &r))
			if (handle_upstream())
				break;
		if (demux_fd > 0 && FD_ISSET(demux_fd, &r))
		{
			static unsigned char buffer[BSIZE];
			int r = read(demux_fd, buffer, BSIZE);
			if (r < 0) {
				if (errno == EINTR || errno == EAGAIN || errno == EBUSY || errno == EOVERFLOW)
					continue;
				break;
			}
			safe_write(1, buffer, r);
		}
	}
	
	if (upstream_state != 3)
		goto bad_gateway;
	
	return 0;
bad_request:
	printf("HTTP/1.0 400 Bad Request\r\n\r\n");
	return 1;
bad_gateway:
	printf("HTTP/1.0 %s\r\n%s\r\n%s\r\n",
		upstream_response_code == 401 ? "401 Unauthorized" : "502 Bad Gateway",
		wwwauthenticate, reason);
	return 1;
}

int handle_upstream(void)
{
	char buffer[MAX_LINE_LENGTH];
	int n = read(upstream, buffer, MAX_LINE_LENGTH);
	if (n == 0)
		return 1;

	if (n < 0)
	{
		perror("read");
		return 1;
	}
	
	char *c = buffer;
	
	while (n)
	{
		char *next_line;
		int valid;

		next_line = memchr(c, '\n', n);
		if (!next_line)
			next_line = c + n;
		else
			next_line++;
		
		valid = next_line - c;
		if (valid > sizeof(response_line)-response_p)
			return 1;
		
		memcpy(response_line + response_p, c, valid);
		c += valid;
		response_p += valid;
		n -= valid;
		
				/* line received? */
		if (response_line[response_p - 1] == '\n')
		{
			response_line[response_p-1] = 0;
			
			if (response_p >= 2 && response_line[response_p - 2] == '\r')
				response_line[response_p-2] = 0;
			response_p = 0;
		
			if (handle_upstream_line())
				return 1;
		}
	}
	return 0;
}

int handle_upstream_line(void)
{
	switch (upstream_state)
	{
	case 0:
		if (strncmp(response_line, "HTTP/1.", 7) || strlen(response_line) < 9) {
			reason = "Invalid upstream response.";
			return 1;
		}
		upstream_response_code = atoi(response_line + 9);
		reason = strdup(response_line + 9);
		upstream_state++;
		break;
	case 1:
		if (!*response_line)
		{
			if (upstream_response_code == 200)
				upstream_state = 2;
			else
				return 1; /* reason was already set in state 0, but we need all header lines for potential WWW-Authenticate */
		} else if (!strncasecmp(response_line, "WWW-Authenticate: ", 18))
			snprintf(wwwauthenticate, MAX_LINE_LENGTH, "%s\r\n", response_line);
		break;
	case 2:
	case 3:
		if (response_line[0] == '+') {
					/* parse (and possibly open) demux */
			int demux = atoi(response_line + 1);
			

					/* parse new pids */
			const char *p = strchr(response_line, ':');
			int old_active_pids[MAX_PIDS];
			
			memcpy(old_active_pids, active_pids, sizeof(active_pids));
			
			int nr_pids = 0, i, j;
			while (p)
			{
				++p;
				int pid = strtoul(p, 0, 0x10);
				p = strchr(p, ',');
				
					/* do not add pids twice */
				for (i = 0; i < nr_pids; ++i)
					if (active_pids[i] == pid)
						break;

				if (i != nr_pids)
					continue;

				active_pids[nr_pids++] = pid;
				
				if (nr_pids == MAX_PIDS)
					break;
			}
			
			for (i = nr_pids; i < MAX_PIDS; ++i)
				active_pids[i] = -1;
				
					/* check for added pids */
			for (i = 0; i < nr_pids; ++i)
			{
				for (j = 0; j < MAX_PIDS; ++j)
					if (active_pids[i] == old_active_pids[j])
						break;
				if (j == MAX_PIDS) {
					if (demux_fd < 0) {
						struct dmx_pes_filter_params flt; 
						char demuxfn[32];
						sprintf(demuxfn, "/dev/dvb/adapter0/demux%d", demux);
						demux_fd = open(demuxfn, O_RDWR | O_NONBLOCK);
						if (demux_fd < 0) {
							reason = "DEMUX OPEN FAILED";
							return 2;
						}

						ioctl(demux_fd, DMX_SET_BUFFER_SIZE, 1024*1024);

						flt.pid = active_pids[i];
						flt.input = DMX_IN_FRONTEND;
#if DVB_API_VERSION > 3
						flt.output = DMX_OUT_TSDEMUX_TAP;
						flt.pes_type = DMX_PES_OTHER;
#else
						flt.output = DMX_OUT_TAP;
						flt.pes_type = DMX_TAP_TS;
#endif
						flt.flags = DMX_IMMEDIATE_START;

						if (ioctl(demux_fd, DMX_SET_PES_FILTER, &flt) < 0) {
							reason = "DEMUX PES FILTER SET FAILED";
							return 2;
						}
					}
					else {
						uint16_t pid = active_pids[i];
						int ret;
#if DVB_API_VERSION > 3
						ret = ioctl(demux_fd, DMX_ADD_PID, &pid);
#else
						ret = ioctl(demux_fd, DMX_ADD_PID, pid);
#endif
						if (ret < 0) {
							reason = "DMX_ADD_PID FAILED";
							return 2;
						}
					}
				}
			}
			
					/* check for removed pids */
			for (i = 0; i < MAX_PIDS; ++i)
			{
				if (old_active_pids[i] == -1)
					continue;
				for (j = 0; j < nr_pids; ++j)
					if (old_active_pids[i] == active_pids[j])
						break;
				if (j == nr_pids) {
#if DVB_API_VERSION > 3
					uint16_t pid = old_active_pids[i];
					ioctl(demux_fd, DMX_REMOVE_PID, &pid);
#else
					ioctl(demux_fd, DMX_REMOVE_PID, old_active_pids[i]);
#endif
				}
			}
			if (upstream_state == 2) {
				char *c = "HTTP/1.0 200 OK\r\nConnection: Close\r\nContent-Type: video/mpeg\r\nServer: stream_enigma2\r\n\r\n";
				safe_write(1, c, strlen(c));
				upstream_state = 3; /* HTTP response sent */
			}
		}
		else if (response_line[0] == '-') {
			reason = strdup(response_line + 1);
			return 1;
		}
				/* ignore everything not starting with + or - */
		break;
	}
	return 0;
}
