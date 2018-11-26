/******************************************************************************
 * IntelME Interface Example Application
 *
 * A simple test program to send messages to the ME
 *
 * MIT License
 * 
 * Copyright (c) 2018 Intel
 * 
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#include <linux/mei.h>

#define mei_msg(_me, fmt, ARGS...) do {			\
		if (_me->verbose)                       \
			fprintf(stderr, fmt, ##ARGS);	\
	} while (0)

#define mei_err(_me, fmt, ARGS...) do {			\
		fprintf(stderr, "Error: " fmt, ##ARGS); \
	} while (0)

/* Struct containing info on mei client connection */
struct mei {
	uuid_le guid;
	bool initialized;
	bool verbose;
	unsigned int buf_size;
	unsigned char prot_ver;
	int fd;
};

/* MKHI header */
typedef struct {
	uint32_t GroupId :8;
	uint32_t Command :7;
	uint32_t IsResponse :1;
	uint32_t Reserved :8;
	uint32_t Result :8;
} MKHI_MESSAGE_HEADER;

/* MKHI commands */
MKHI_MESSAGE_HEADER  GEN_GET_FW_VERSION = {
	.GroupId = 0xff,
	.Command = 0x2,
	.IsResponse =0,
	.Reserved =0,
	.Result=0
};

typedef struct {
	MKHI_MESSAGE_HEADER                Header;
	uint16_t                             CodeMinor;
	uint16_t                             CodeMajor;
	uint16_t                             CodeBuild;
	uint16_t                             CodeHotfix;
	uint16_t                             NFTPMinor;
	uint16_t                             NFTPMajor;
	uint16_t                             NFTPBuild;
	uint16_t                             NFTPHotfix;
}  GEN_GET_FW_VERSION_Response;

/* Struct containing info on MKHI connection */
struct mkhi_host_if {
	struct mei mei_cl;
	unsigned long send_timeout;
	bool initialized;
};

static void mei_deinit(struct mei *cl)
{
	if (cl->fd != -1)
		close(cl->fd);
	cl->fd = -1;
	cl->buf_size = 0;
	cl->prot_ver = 0;
	cl->initialized = false;
}

static bool mei_init(struct mei *me, const uuid_le *guid,
		     unsigned char req_protocol_version, bool verbose)
{
	int result;
	struct mei_client *cl;
	struct mei_connect_client_data data;

	me->verbose = verbose;
	me->fd = open("/dev/mei0", O_RDWR);
    
	if (me->fd == -1) {
		if (!geteuid()) {
			mei_err(me, "Cannot establish a handle to the Intel(R) MEI driver. \n");
			exit(-1);
		} else {
			mei_err(me, "Please run this program with root privilege.\n");
			mei_deinit(me);
			exit(-1);
		}

		goto err;
	}

	memcpy(&me->guid, guid, sizeof(*guid));
	memset(&data, 0, sizeof(data));
	me->initialized = true;

	memcpy(&data.in_client_uuid, &me->guid, sizeof(me->guid));
	result = ioctl(me->fd, IOCTL_MEI_CONNECT_CLIENT, &data);

	if (result) {
		mei_err(me, "IOCTL_MEI_CONNECT_CLIENT receive message. err=%d\n",result);
		goto err;
	}

	cl = &data.out_client_properties;
	mei_msg(me, "max_message_length %d\n", cl->max_msg_length);
	mei_msg(me, "protocol_version %d\n", cl->protocol_version);

	if ((req_protocol_version > 0)
            && (cl->protocol_version != req_protocol_version)) {
		mei_err(me, "Intel(R) MEI protocol version not supported\n");
		goto err;
	}

	me->buf_size = cl->max_msg_length;
	me->prot_ver = cl->protocol_version;

	return true;

 err: mei_deinit(me);

	return false;
}

static ssize_t mei_recv_msg(struct mei *me, unsigned char *buffer, ssize_t len,
			    unsigned long timeout) {
	ssize_t rc;

	mei_msg(me, "call read length = %zd\n", len);

	rc = read(me->fd, buffer, len);
	if (rc < 0) {
		mei_err(me, "read failed with status %zd %s\n",
			rc, strerror(errno));
		mei_deinit(me);
	} else {
		mei_msg(me, "read succeeded with result %zd\n", rc);
	}

	return rc;
}

static ssize_t mei_send_msg(struct mei *me, const unsigned char *buffer,
			    ssize_t len, unsigned long timeout) {
	struct timeval tv;
	ssize_t written;
	ssize_t rc;
	fd_set set;

	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000000;

	mei_msg(me, "call write length = %zd\n", len);

	written = write(me->fd, buffer, len);

	if (written < 0) {
		rc = -errno;
		mei_err(me, "write failed with status %zd %s\n", written,
			strerror(errno));
		goto out;
	}

	FD_ZERO(&set);
	FD_SET(me->fd, &set);
	rc = select(me->fd + 1, &set, NULL, NULL, &tv);

	if (rc > 0 && FD_ISSET(me->fd, &set)) {
		mei_msg(me, "write success\n");
	} else if (rc == 0) {
		mei_err(me, "write failed on timeout with status\n");
		goto out;
	} else { /* rc < 0 */
		mei_err(me, "write failed on select with status %zd\n", rc);
		goto out;
	}

	rc = written;

 out: if (rc < 0)
		mei_deinit(me);

	return rc;
}



const uuid_le MEI_MKHI_HIF_FIX =
	UUID_LE(0x55213584, 0x9a29, 0x4916, 0xba, 0xdf, 0xf, 
		0xb7, 0xed, 0x68, 0x2a, 0xeb);

static bool mkhi_host_if_fix_init(struct mkhi_host_if *acmd,
        			  unsigned long send_timeout, 
				  bool verbose) 
{
	acmd->send_timeout = (send_timeout) ? send_timeout : 20000;
	acmd->initialized =
		mei_init(&acmd->mei_cl, &MEI_MKHI_HIF_FIX, 0, verbose);

	return acmd->initialized;
}

int main(int argc, char **argv) 
{
	/* Enable fixed address interface */
	FILE *fp = fopen("/sys/kernel/debug/mei0/allow_fixed_address", "w");
	int ret = (fwrite("Y", sizeof(char), 1, fp) == 1) ? 0 : -1;    
	fclose(fp);

    	struct mkhi_host_if mkhi_cmd;

        /* Open MKHI comms with mei driver */
    	bool heci_init_call = mkhi_host_if_fix_init(&mkhi_cmd, 5000, true);

       	if (!heci_init_call) {
    		printf( "MKHI fixed i/f failed to initialise \n");
       		mei_deinit(&mkhi_cmd.mei_cl);
       		return false;
      	} else {
    		printf( "MKHI fixed i/f initialised \n");
        }         

 	/* Send simple MKHI  command to get f/w version number */
	GEN_GET_FW_VERSION_Response resp;

	mei_send_msg(&mkhi_cmd.mei_cl,
		     (const unsigned char *) &GEN_GET_FW_VERSION,
		     sizeof(GEN_GET_FW_VERSION),5000);

	mei_recv_msg(&mkhi_cmd.mei_cl,
		     (unsigned char *)&resp ,
		     sizeof(GEN_GET_FW_VERSION_Response), 5000);

	mei_deinit(&mkhi_cmd.mei_cl);
	
	printf("Build Maj Min Hotfix : %x, %x, %x, %x \n",
	       resp.CodeBuild, resp.CodeMajor, resp.CodeMinor, resp.CodeHotfix);

	return 0;
}
