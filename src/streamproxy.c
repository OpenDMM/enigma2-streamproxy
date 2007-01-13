#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/dvb/dmx.h>

#define BSIZE                    1024*16
#define DMX_ADD_PID              _IO('o', 51)
#define DMX_REMOVE_PID           _IO('o', 52)

typedef enum {
	DMX_TAP_TS = 0,
	DMX_TAP_PES = DMX_PES_OTHER, /* for backward binary compat. */
} dmx_tap_type_t;


char response_line[128];
int response_p;

int upstream;
int upstream_state;
		/*
		 0 - response
		 1 - options
		 2 - body
		 */

int demux_fd = -1;

char *reason = "";

#define MAX_PIDS 16

int active_pids[MAX_PIDS];

int handle_upstream(void);
int handle_upstream_line(void);

int main(int argc, char **argv)
{
	char request[128], upstream_request[128];
	char *c, *service_ref;
	if (!fgets(request, 128, stdin))
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
		char option[128];
		if (!fgets(option, 128, stdin))
			break;
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

	snprintf(upstream_request, sizeof(upstream_request), "GET /web/stream?StreamService=%s HTTP/1.0\r\n\r\n", service_ref);
	if (write(upstream, upstream_request, strlen(upstream_request)) != strlen(upstream_request))
		goto bad_gateway;
	
	while (1)
	{
		fd_set r;
		FD_ZERO(&r);
		FD_SET(upstream, &r);
		if (demux_fd != -1)
			FD_SET(demux_fd, &r);
		
		if (select(5, &r, 0, 0, 0) < 0)
			break;

				/* handle enigma responses */
		if (FD_ISSET(upstream, &r))
			if (handle_upstream())
				break;
		if (demux_fd > 0 && FD_ISSET(demux_fd, &r))
		{
			static unsigned char buffer[BSIZE];
			int r = read(demux_fd, buffer, BSIZE);
			if (r < 0)
				break;
			write(1, buffer, r);
		}
	}
	
	if (upstream_state == 0)
		goto bad_gateway;
	
	return 0;
bad_request:
	printf("HTTP/1.0 400 Bad Request\r\n\r\n");
	return 1;
bad_gateway:
	printf("HTTP/1.0 502 Bad Gateway\r\n\r\n%s\r\n", reason);
	return 1;
}

int handle_upstream(void)
{
	char buffer[128];
	int n = read(upstream, buffer, 128);
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
		if (strncmp(response_line, "HTTP/1.", 7))
		{
			reason = "Invalid upstream response.";
			return 1;
		}
		if (strncmp(response_line + 9, "200", 3))
		{
			reason = "Upstream streaming extensions not found.";
			return 1;
		}
		upstream_state++;
		break;
	case 1:
		if (!*response_line)
		{
			char *c = "HTTP/1.0 200 OK\r\nConnection: Close\r\nContent-Type: video/mpeg\r\nServer: stream_enigma2\r\n\r\n";
			write(1, c, strlen(c));
			upstream_state = 2;
		}
		break;
	case 2:
		if (response_line[0] == '+')
		{
					/* parse (and possibly open) demux */
			int demux = atoi(response_line + 1);
			
			if (demux_fd < 0)
			{
			  struct dmx_pes_filter_params flt; 
				char demuxfn[32];
				sprintf(demuxfn, "/dev/dvb/adapter0/demux%d", demux);
				demux_fd = open(demuxfn, O_RDWR);
				if (demux_fd < 0)
					return 2;

		    flt.pid = -1;
		    flt.input = DMX_IN_FRONTEND;
		    flt.output = DMX_OUT_TAP;
		    flt.pes_type = DMX_TAP_TS;
		    flt.flags = 0;

		    if (ioctl(demux_fd, DMX_SET_PES_FILTER, &flt) < 0)
		    	return 2;

				ioctl(demux_fd, DMX_SET_BUFFER_SIZE, 1024*1024);
				fcntl(demux_fd, F_SETFL, O_NONBLOCK);

		    if (ioctl(demux_fd, DMX_START, 0) < 0)
		    	return 2;
			}

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
				if (j == MAX_PIDS)
					ioctl(demux_fd, DMX_ADD_PID, active_pids[i]);
			}
			
					/* check for removed pids */
			for (i = 0; i < MAX_PIDS; ++i)
			{
				if (old_active_pids[i] == -1)
					continue;
				for (j = 0; j < nr_pids; ++j)
					if (old_active_pids[i] == active_pids[j])
						break;
				if (j == nr_pids)
					ioctl(demux_fd, DMX_REMOVE_PID, old_active_pids[i]);
			}
		} else if (response_line[1] == '-')
			return 1;
				/* ignore everything not starting with + or - */
		break;
	}
	return 0;
}
