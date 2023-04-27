// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/habmm.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include "msm_hyp_trace.h"
#include "user_os_utils.h"
#include "wire_user.h"

#define USE_HAB

/*
 * ---------------------------------------------------------------------------
 * Defines
 * ---------------------------------------------------------------------------
 */
//#define DEBUG_USER_HAB_UTILS
#define CHANNEL_OPENWFD		0
#define CHANNEL_EVENTS		1
#define CHANNEL_BUFFERS		2
#define MAX_CHANNELS		3
#define MAX_RECV_PACKET_RETRY	10
#define WFD_MAX_NUM_OF_CLIENTS	10
#define WFD_CLIENT_ID_BASE	WFD_CLIENT_ID_CLUSTER
#define WFD_CLIENT_ID_LA_CONTAINER	0x7818
#define WFD_CLIENT_ID_LV_CONTAINER	0x7819

#define DO_NOT_LOCK_CHANNEL		0x01
#define HAB_NO_TIMEOUT_VAL		-1
#define HAB_BUF_CHANNEL_TIMEOUT_VAL	500

#if !defined(__QNXNTO__) && !defined(__linux__)
#define CLOCK_MONOTONIC		CLOCK_REALTIME
#endif

#if defined(__linux__)
#define USER_OS_UTILS_LOG_MODULE_NAME "LV_FE"
#elif defined(__ANDROID__)
#define USER_OS_UTILS_LOG_MODULE_NAME "LA_FE"
#endif

#define USER_OS_UTILS_LOG_MODULE_ID       10256

#define UTILS_LOG_ERROR(fmt, ...)		\
	USER_OS_UTILS_LOG_ERROR(		\
		USER_OS_UTILS_LOG_MODULE_NAME,	\
		fmt, ##__VA_ARGS__)

#define UTILS_LOG_WARNING(fmt, ...)		\
	USER_OS_UTILS_LOG_WARNING(		\
		USER_OS_UTILS_LOG_MODULE_NAME,	\
		fmt, ##__VA_ARGS__)

#define UTILS_LOG_CRITICAL_INFO(fmt, ...)	\
	USER_OS_UTILS_LOG_CRITICAL_INFO(	\
		USER_OS_UTILS_LOG_MODULE_NAME,	\
		fmt, ##__VA_ARGS__)

#define UTILS_LOG_INFO(fmt, ...)		\
	USER_OS_UTILS_LOG_INFO(			\
		USER_OS_UTILS_LOG_MODULE_NAME,	\
		fmt, ##__VA_ARGS__)

/*
 * ---------------------------------------------------------------------------
 * Structure/Enumeration/Union Definitions
 * ---------------------------------------------------------------------------
 */
static u32 channel_map[WFD_MAX_NUM_OF_CLIENTS][3] = {
	/* Each MM ID translates to a physical channel per VM.
	 * Different clients on the same VM should use different MM IDs.
	 */
	[WFD_CLIENT_ID_TELLTALE - WFD_CLIENT_ID_BASE] /* Tell-Tale App */
	{
		MM_DISP_5
	},
	[WFD_CLIENT_ID_QNX_GVM - WFD_CLIENT_ID_BASE] /* QNX GVM */
	{
		MM_DISP_3,
		MM_DISP_4
	},
	[WFD_CLIENT_ID_LA_GVM - WFD_CLIENT_ID_BASE] /* LA GVM */
	{
		MM_DISP_1,
		MM_DISP_2,
		MM_DISP_3
	},
	[WFD_CLIENT_ID_LV_GVM - WFD_CLIENT_ID_BASE] /* LV GVM */
	{
		MM_DISP_1,
		MM_DISP_2,
		MM_DISP_3
	},
	[WFD_CLIENT_ID_LA_CONTAINER - WFD_CLIENT_ID_BASE] /* LA Container */
	{
		MM_DISP_1,
		MM_DISP_2,
		MM_DISP_3
	},
	[WFD_CLIENT_ID_LV_CONTAINER - WFD_CLIENT_ID_BASE] /* LV Container */
	{
		MM_DISP_1,
		MM_DISP_2,
		MM_DISP_3
	},
};

struct user_os_utils_context {
	u32 client_id;
	int32_t hyp_hdl_disp[MAX_CHANNELS];
	spinlock_t hyp_cmdchl_lock;
	struct mutex hyp_cbchl_lock;
	struct mutex hyp_bufchl_lock;
	unsigned long cmdchl_lock_flags[MAX_CHANNELS];
	struct task_struct *buffer_thread;
	int client_idx;
};

/*
 * ---------------------------------------------------------------------------
 * Private Functions
 * ---------------------------------------------------------------------------
 */
static int buffer_channel_task(void *arg)
{
	struct user_os_utils_context *ctx = arg;
	int client_idx = ctx->client_idx;
	int rc = 0;

	if ((channel_map[client_idx][CHANNEL_BUFFERS]) != 0x00) {
		/* open hab channel for events handling */
#ifdef USE_HAB
		rc = habmm_socket_open(
#else
		rc = habmm_socket_open_dummy(
#endif
					&ctx->hyp_hdl_disp[CHANNEL_BUFFERS],
					channel_map[client_idx][CHANNEL_BUFFERS],
					HAB_BUF_CHANNEL_TIMEOUT_VAL,
					0x00);
		if (rc) {
			UTILS_LOG_ERROR("habmm_socket_open(HAB_CHNL_BUFFERS) failed");
		} else {
			/* create lock for buffer handling hab channel */
			mutex_init(&ctx->hyp_bufchl_lock);

			ctx->cmdchl_lock_flags[CHANNEL_BUFFERS] = 0;
			UTILS_LOG_CRITICAL_INFO("Buffer channel open success, hnd=%d",
					ctx->hyp_hdl_disp[CHANNEL_BUFFERS]);
		}
	}

	return 0;
}
static inline int32_t
get_hab_handle(
	struct user_os_utils_context *ctx,
	u32 *chl_id,
	u32 flags)
{
	u32 client_id = 0;
	int32_t chl_hdl = 0;

	if (chl_id) {
		client_id = *chl_id;

		if (client_id < MAX_CHANNELS) {
			/* Check if Buffer channel is created.
			 * Otherwise use the OPENWFD channel
			 */
			if ((client_id == CHANNEL_BUFFERS) &&
					(!ctx->hyp_hdl_disp[client_id])) {
				flags = DO_NOT_LOCK_CHANNEL;
				client_id = CHANNEL_OPENWFD;
			}

			if (!(DO_NOT_LOCK_CHANNEL & flags)) {
				if (client_id == CHANNEL_OPENWFD)
					spin_lock_irqsave(&ctx->hyp_cmdchl_lock,
						ctx->cmdchl_lock_flags[client_id]);
				else if (client_id == CHANNEL_EVENTS)
					mutex_lock(&ctx->hyp_cbchl_lock);
				else if (client_id == CHANNEL_BUFFERS)
					mutex_lock(&ctx->hyp_bufchl_lock);
			}
			chl_hdl = ctx->hyp_hdl_disp[client_id];
		}
	}

	return chl_hdl;
}

static inline int
rel_hab_handle(
	struct user_os_utils_context *ctx,
	u32 chl_id,
	u32 flags)
{

	if (chl_id < MAX_CHANNELS) {
		/* Check if Buffer channel is created.
		 * Otherwise use the OPENWFD channel
		 */
		if ((chl_id == CHANNEL_BUFFERS) && (!ctx->hyp_hdl_disp[chl_id])) {
			flags = DO_NOT_LOCK_CHANNEL;
			chl_id = CHANNEL_OPENWFD;
		}

		if ((!flags)) {
			if (chl_id == CHANNEL_OPENWFD)
				spin_unlock_irqrestore(&ctx->hyp_cmdchl_lock,
						ctx->cmdchl_lock_flags[chl_id]);
			else if (chl_id == CHANNEL_EVENTS)
				mutex_unlock(&ctx->hyp_cbchl_lock);
			else if (chl_id == CHANNEL_BUFFERS)
				mutex_unlock(&ctx->hyp_bufchl_lock);
		}
	}

	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * Public Functions
 * ---------------------------------------------------------------------------
 */
int
user_os_utils_init(
	struct user_os_utils_init_info *init_info,
	u32 flags)
{
	struct user_os_utils_context *ctx;
	int rc = 0;
	int client_id = init_info->client_id;
	int client_idx = 0;

	if ((client_id < WFD_CLIENT_ID_CLUSTER) ||
		(client_id > WFD_CLIENT_ID_LV_CONTAINER))
		return -EINVAL;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	init_info->clock_id = CLOCK_MONOTONIC;
	init_info->enable_event_handling = (flags & WIRE_INIT_EVENT_SUPPORT) ?
			true : false;

	client_idx = client_id - WFD_CLIENT_ID_BASE;

	if (channel_map[client_idx][CHANNEL_OPENWFD] != 0x00) {
	/* open hab channel for openwfd commands */
#ifdef USE_HAB
		rc = habmm_socket_open(
#else
		rc = habmm_socket_open_dummy(
#endif
			&ctx->hyp_hdl_disp[CHANNEL_OPENWFD],
			channel_map[client_idx][CHANNEL_OPENWFD],
			(uint32_t)HAB_NO_TIMEOUT_VAL,
			0x00);
		if (rc) {
			UTILS_LOG_ERROR("habmm_socket_open(HAB_CHNL_OPENWFD) failed");
			goto fail;
		}
	} else {
		UTILS_LOG_ERROR("invalid hab channel id");
		rc = -EINVAL;
		goto fail;
	}
	/* create lock for openwfd commands hab channel */
	spin_lock_init(&ctx->hyp_cmdchl_lock);

	UTILS_LOG_CRITICAL_INFO("OpenWFD channel open successful, handle=%d",
			ctx->hyp_hdl_disp[CHANNEL_OPENWFD]);

	/* Initialize the flag */
	 ctx->cmdchl_lock_flags[CHANNEL_OPENWFD] = 0;

	if ((init_info->enable_event_handling) &&
		(channel_map[client_idx][CHANNEL_EVENTS]) != 0x00) {
		/* open hab channel for events handling */
#ifdef USE_HAB
		rc = habmm_socket_open(
#else
		rc = habmm_socket_open_dummy(
#endif
			&ctx->hyp_hdl_disp[CHANNEL_EVENTS],
			channel_map[client_idx][CHANNEL_EVENTS],
			(uint32_t)HAB_NO_TIMEOUT_VAL,
			0x00);
		if (rc) {
			UTILS_LOG_ERROR("habmm_socket_open(HAB_CHNL_EVENTS) failed");
			goto fail;
		}
		/* create lock for events handling hab channel */
		mutex_init(&ctx->hyp_cbchl_lock);
		UTILS_LOG_CRITICAL_INFO("Events channel open successful, handle=%d",
			ctx->hyp_hdl_disp[CHANNEL_EVENTS]);

		/* Initialize the flag */
		ctx->cmdchl_lock_flags[CHANNEL_EVENTS] = 0;

	}

	/* create a thread buffer channel */
	ctx->client_idx = client_idx;
	ctx->buffer_thread = kthread_run(buffer_channel_task, ctx,
							"buffer channel task");

	ctx->client_id = client_id;
	init_info->context = ctx;
	return 0;

fail:
	kfree(ctx);
	return rc;
}

int
user_os_utils_deinit(
	void *handle,
	u32 flags)
{
	struct user_os_utils_context *ctx = handle;
	int rc = 0;

	if (!ctx)
		return -EINVAL;

	/* close hab channel for openwfd commands */
	if (ctx->hyp_hdl_disp[CHANNEL_OPENWFD]) {
#ifdef USE_HAB
		rc = habmm_socket_close(ctx->hyp_hdl_disp[CHANNEL_OPENWFD]);
#else
		rc = habmm_socket_close_dummy(ctx->hyp_hdl_disp[CHANNEL_OPENWFD]);
#endif
		if (rc)
			UTILS_LOG_ERROR("habmm_socket_close (CHANNEL_OPENWFD) failed");
	}

	/* close hab channel for events handling */
	if (ctx->hyp_hdl_disp[CHANNEL_EVENTS]) {
		/* destroy lock for events handling hab channel */
		mutex_destroy(&ctx->hyp_cbchl_lock);
#ifdef USE_HAB
		rc = habmm_socket_close(ctx->hyp_hdl_disp[CHANNEL_EVENTS]);
#else
		rc = habmm_socket_close_dummy(ctx->hyp_hdl_disp[CHANNEL_EVENTS]);
#endif
		if (rc)
			UTILS_LOG_ERROR("habmm_socket_close (CHANNEL_EVENTS) failed");
	}

	/* close hab channel for buffer handling */
	if (ctx->hyp_hdl_disp[CHANNEL_BUFFERS]) {
		/* destroy lock for buffer handling hab channel */
		mutex_destroy(&ctx->hyp_bufchl_lock);
#ifdef USE_HAB
		rc = habmm_socket_close(ctx->hyp_hdl_disp[CHANNEL_BUFFERS]);
#else
		rc = habmm_socket_close_dummy(ctx->hyp_hdl_disp[CHANNEL_BUFFERS]);
#endif
		if (rc)
			UTILS_LOG_ERROR("habmm_socket_close (CHANNEL_BUFFERS) failed");
	}

	/* stop buffer channel thread */
	if (ctx->buffer_thread)
		kthread_stop(ctx->buffer_thread);

	kfree(ctx);

	return 0;
}

void
user_os_utils_get_id(
	void *handle,
	u32 *id,
	u32 flags)
{
	struct user_os_utils_context *ctx = handle;

	*id = ctx->client_id;
}

int
user_os_utils_send_recv(
	void *context,
	struct wire_packet *req,
	struct wire_packet *resp,
	u32 flags)
{
	struct user_os_utils_context *ctx = context;
	int rc = 0;
	int32_t handle = 0;
	u32 chl_id = 0;
	u32 req_size = 0;
	u32 resp_size = 0;

	u32 version = 0;
	enum payload_types payload_type;
	i64 timestamp = 0;
	u32 req_flags = 0;
	int retry_times = 0;

	u32 num_of_wfd_cmds = 0;
	enum openwfd_cmd_type wfd_cmd_type = OPENWFD_CMD_MAX;
	char marker_buff[MARKER_BUFF_LENGTH] = {0};
	/*
	 * Hold this CPU for 0.25s since here we call spin_lock_irqsave().
	 * Normally it will be 100us to get reply, 250ms is enough.
	 *
	 * Be careful if hoping to increase such duration to be longer
	 * since actually inside this duration, it is possible that the
	 * watchdog pet procedure could not move ahead.
	 */
	unsigned long delay = jiffies + (HZ / 4);

	if (!req || !resp) {
		UTILS_LOG_ERROR("NULL req(0x%p) or resp(0x%p)",
			req, resp);
		rc = -1;
		goto end;
	}

	/* store header data for validating response packet */
	version = req->hdr.version;
	payload_type = req->hdr.payload_type;
	timestamp = req->hdr.timestamp;
	req_flags = req->hdr.flags;

	if (payload_type == OPENWFD_CMD) {
		num_of_wfd_cmds = req->payload.wfd_req.num_of_cmds;
		wfd_cmd_type = req->payload.wfd_req.reqs[0].type;
		chl_id = CHANNEL_OPENWFD;
	} else if (payload_type == EVENT_REGISTRATION) {
		chl_id = CHANNEL_OPENWFD;
	} else if (payload_type == EVENT_NOTIFICATION) {
		chl_id = CHANNEL_EVENTS;
	} else {
		UTILS_LOG_ERROR("Invalid payload type(%d)",
			payload_type);
		rc = -1;
		goto end;
	}

	handle = get_hab_handle(ctx, &chl_id, 0x00);
	if (!handle) {
		UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d", chl_id);
		rc = -1;
		goto end;
	}

	snprintf(marker_buff, sizeof(marker_buff), "hab_send %d\n", wfd_cmd_type);
	HYP_ATRACE_BEGIN(marker_buff);

	req_size = sizeof(struct wire_header) + req->hdr.payload_size;
#ifdef USE_HAB
	rc = habmm_socket_send(
#else
	rc = habmm_socket_send_dummy(
#endif
		handle,
		(void *)req,
		(uint32_t)req_size,
		(uint32_t)0x00);

	HYP_ATRACE_END(marker_buff);

	if (rc) {
		UTILS_LOG_ERROR("habmm_socket_send(payload type(%d)) failed",
			payload_type);
		rc = -1;
		goto end;
	}

	if (req_flags & WIRE_RESP_NOACK_FLAG)
		goto end;

	snprintf(marker_buff, sizeof(marker_buff), "hab_recv %d\n", wfd_cmd_type);
	HYP_ATRACE_BEGIN(marker_buff);

retry_recv_packet:
	do {
		/* TODO: Need handle exit hab_receive during deinit */
		resp_size = sizeof(struct wire_packet);
#ifdef USE_HAB
		rc = habmm_socket_recv(
#else
		rc = habmm_socket_recv_dummy(
#endif
			handle,
			(void *)resp,
			(uint32_t *)&resp_size,
			(uint32_t)HAB_NO_TIMEOUT_VAL,
			HABMM_SOCKET_RECV_FLAGS_NON_BLOCKING);
		if (rc) {
			if (-ENODEV == rc)
				UTILS_LOG_CRITICAL_INFO("OpenWFD channel broken - no device");
			else if (-EINTR == rc) {
				/*
				 * system is closed or suspend a interrupted
				 * system call is happening on hab channel.
				 * We should try it again
				 */
				UTILS_LOG_CRITICAL_INFO(
					"habmm_socket_recv - interrupted system call - retry");
			}
		}
	} while ((time_before(jiffies, delay)) && (-EAGAIN == rc) && (resp_size == 0));

	HYP_ATRACE_END(marker_buff);

	if (rc) {
		UTILS_LOG_ERROR("habmm_socket_recv(payload type(%d)) failed, resp_size=%d, rc=%d",
			payload_type, resp_size, rc);
		if ((rc == -EAGAIN) && (retry_times < MAX_RECV_PACKET_RETRY))
		{
			if (handle) {
				if (rel_hab_handle(ctx, chl_id, 0x00))
					UTILS_LOG_ERROR("rel_hab_handle failed");
			}
			/*
			 * Add this msleep to let watch dog thread can be feed
			 * need release lock fisrt
			 */
			msleep(1);
			handle = get_hab_handle(ctx, &chl_id, 0x00);
			if (!handle) {
				UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d", chl_id);
				rc = -1;
				goto end;
			}

			retry_times++;
			UTILS_LOG_ERROR("recv packet retry %d", retry_times);
			goto retry_recv_packet;
		}
		rc = -1;
		goto end;
	}

	/* validate response */
	if (resp->hdr.magic_num != WIRE_FORMAT_MAGIC) {
		UTILS_LOG_ERROR("Invalid magic_num=0x%x",
			resp->hdr.magic_num);
		rc = -1;
		goto end;
	}
	if (version != resp->hdr.version) {
		UTILS_LOG_ERROR("version mismatch req=0x%x resp=0x%x",
			version, resp->hdr.version);
		rc = -1;
		goto end;
	}
	if (payload_type != resp->hdr.payload_type) {
		UTILS_LOG_ERROR("wrong payload type %d",
			resp->hdr.payload_type);
		rc = -1;
		goto end;
	}
	if (timestamp > resp->hdr.timestamp) {
		UTILS_LOG_ERROR("wrong packet timestamp");
		UTILS_LOG_ERROR("req packet timestamp : %lu\n", timestamp);
		UTILS_LOG_ERROR("resp packet timestamp : %lu\n",
					resp->hdr.timestamp);

		/*
		 * Drm fe try 10 times to get the correct packet
		 */
		if (retry_times > MAX_RECV_PACKET_RETRY) {
			UTILS_LOG_ERROR("recv packet retry limit exceeded");
#ifdef DEBUG_USER_HAB_UTILS
			panic("user_os_utils_send_recv : wrong packet received");
#endif
			rc = -1;
		} else {
			if (handle) {
				if (rel_hab_handle(ctx, chl_id, 0x00))
					UTILS_LOG_ERROR("rel_hab_handle failed");
			}
			/*
			 * Add this msleep to let the watchdog thread can be feed
			 * need release lock first
			 */
			msleep(1);
			handle = get_hab_handle(ctx, &chl_id, 0x00);
			if (!handle) {
				UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d", chl_id);
				rc = -1;
				goto end;
			}

			retry_times++;
			UTILS_LOG_ERROR("recv packet retry %d", retry_times);
			goto retry_recv_packet;
		}
	}
	else if (timestamp < resp->hdr.timestamp) {
		UTILS_LOG_ERROR(" Wrong packet timestamp req : %lu res : %lu \n", timestamp, resp->hdr.timestamp);
		rc = -1;
		goto end;

	}
	if (payload_type == OPENWFD_CMD) {
		if (num_of_wfd_cmds != resp->payload.wfd_resp.num_of_cmds) {
			UTILS_LOG_ERROR("num_of_wfd_cmds mismatch req=%d resp=%d",
				num_of_wfd_cmds,
				resp->payload.wfd_resp.num_of_cmds);
			rc = -1;
			goto end;
		}
		if (wfd_cmd_type != resp->payload.wfd_resp.resps[0].type) {
			UTILS_LOG_ERROR("wrong cmd type %d",
				resp->payload.wfd_resp.resps[0].type);
			rc = -1;
			goto end;
		}
	}

end:
	if (handle) {
		if (rel_hab_handle(ctx, chl_id, 0x00))
			UTILS_LOG_ERROR("rel_hab_handle failed");
	}

	if (((rc == -1) || (retry_times > 0)) && (req != NULL))
	{
		UTILS_LOG_ERROR("packet receive error\n");
		print_hex_dump(KERN_INFO, "hdr: ", DUMP_PREFIX_NONE, 16, 1,
				&req->hdr, sizeof(req->hdr), false);
		print_hex_dump(KERN_INFO, "req: ", DUMP_PREFIX_NONE, 16, 1,
				&req->payload, req->hdr.payload_size, false);
	}

	return rc;
}

int
user_os_utils_recv(
	void *context,
	struct wire_packet *req,
	u32 flags)
{
	struct user_os_utils_context *ctx = context;
	int rc = 0;
	int32_t handle = 0;
	u32 chl_id = 0;
	u32 req_size = 0;
	enum payload_types payload_type;

	if (!req) {
		UTILS_LOG_ERROR("NULL req");
		rc = -1;
		goto end;
	}

	payload_type = req->hdr.payload_type;

	if (payload_type == OPENWFD_CMD) {
		chl_id = CHANNEL_OPENWFD;
	} else if (payload_type == EVENT_REGISTRATION) {
		chl_id = CHANNEL_OPENWFD;
	} else if (payload_type == EVENT_NOTIFICATION) {
		chl_id = CHANNEL_EVENTS;
	} else {
		UTILS_LOG_ERROR("Invalid payload type(%d)",
			payload_type);
		rc = -1;
		goto end;
	}

	handle = get_hab_handle(ctx, &chl_id, 0x00);
	if (!handle) {
		UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d", chl_id);
		rc = -1;
		goto end;
	}

	do {
		/* TODO: Need handle exit hab_receive during deinit */
		req_size = sizeof(struct wire_packet);
#ifdef USE_HAB
		rc = habmm_socket_recv(
#else
		rc = habmm_socket_recv_dummy(
#endif
			handle,
			(void *)req,
			(uint32_t *)&req_size,
			(uint32_t)HAB_NO_TIMEOUT_VAL,
			(uint32_t)0x00);
		if (rc) {
			if (rc == -ENODEV) {
				UTILS_LOG_CRITICAL_INFO("OpenWFD channel broken - no device");
			} else if (rc == -EINTR) {
				/*
				 * system is closed or suspend a interrupted system call is
				 * happening on hab channel. we should try it again
				 */
				UTILS_LOG_CRITICAL_INFO("channel broken interrupted system call");
			} else {
				UTILS_LOG_ERROR("habmm_socket_recv(payload type(%d)) failed",
						payload_type);
			}
		}
	} while (-EINTR == rc);

end:
	if (handle) {
		if (rel_hab_handle(ctx, chl_id, 0x00))
			UTILS_LOG_ERROR("rel_hab_handle failed");
	}

	return rc;
}

int
user_os_utils_shmem_export(
	void *context,
	struct user_os_utils_mem_info *mem,
	u32 flags)
{
	struct user_os_utils_context *ctx = context;
	int32_t rc = 0;
	int32_t handle = 0;
	u32 chl_id = CHANNEL_BUFFERS;
	u32 export_id = 0;
	u32 export_flags = 0;

	if (!mem) {
		UTILS_LOG_ERROR("NULL mem");
		rc = -1;
		goto end;
	}

	handle = get_hab_handle(ctx, &chl_id, 0x00);
	if (!handle) {
		UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d",
			chl_id);
		rc = -1;
		goto end;
	}

	export_flags |= HABMM_EXPIMP_FLAGS_DMABUF;

	mem->shmem_type = HAB_EXPORT_ID;
#ifdef USE_HAB
	rc = habmm_export(
#else
	rc = habmm_export_dummy(
#endif
		handle,
		mem->buffer,
		(uint32_t)mem->size,
		&export_id,
		export_flags);
	if (rc) {
		UTILS_LOG_ERROR("habmm_export(va=%p) failed",
			mem->buffer);
		goto end;
	}

	mem->shmem_id = export_id;

end:
	if (handle) {
		if (rel_hab_handle(ctx, chl_id, 0x00))
			UTILS_LOG_ERROR("rel_hab_handle failed");
	}

	return rc;
}

int
user_os_utils_shmem_import(
	void *context,
	struct user_os_utils_mem_info *mem,
	u32 flags)
{
	struct user_os_utils_context *ctx = context;
	int32_t rc = 0;
	int32_t handle = 0;
	u32 chl_id = CHANNEL_BUFFERS;
	u32 import_flags = 0;

	if (!mem) {
		UTILS_LOG_ERROR("NULL mem");
		rc = -1;
		goto end;
	}

	handle = get_hab_handle(ctx, &chl_id, 0x00);
	if (!handle) {
		UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d",
			chl_id);
		goto end;
	}

#if !defined(__linux__)
	import_flags |= HABMM_EXPIMP_FLAGS_FD;
#endif

	if (mem->shmem_type == HAB_EXPORT_ID) {
#ifdef USE_HAB
		rc = habmm_import(
#else
		rc = habmm_import_dummy(
#endif
			handle,
			&mem->buffer,
			(uint32_t)mem->size,
			(uint32_t)mem->shmem_id,
			(uint32_t)import_flags);
		if (rc) {
			UTILS_LOG_ERROR("habmm_import(id=%lu) failed",
				mem->shmem_id);
			rc = -1;
			goto end;
		}
	} else {
		rc = -1;
		goto end;
	}

end:
	if (handle) {
		if (rel_hab_handle(ctx, chl_id, 0x00))
			UTILS_LOG_ERROR("rel_hab_handle failed");
	}

	return rc;
}

int
user_os_utils_shmem_unexport(
	void *context,
	struct user_os_utils_mem_info *mem,
	u32 flags)
{
	struct user_os_utils_context *ctx = context;
	int32_t rc = 0;
	int32_t handle = 0;
	u32 chl_id = CHANNEL_BUFFERS;
	u32 unexport_flags = 0;

	if (!mem) {
		UTILS_LOG_ERROR("NULL mem");
		rc = -1;
		goto end;
	}

	handle = get_hab_handle(ctx, &chl_id, 0x00);
	if (!handle) {
		UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d", chl_id);
		rc = -1;
		goto end;
	}

	unexport_flags |= HABMM_EXPIMP_FLAGS_FD;

	if (mem->shmem_type == HAB_EXPORT_ID) {
#ifdef USE_HAB
		rc = habmm_unexport(
#else
		rc = habmm_unexport_dummy(
#endif
			handle,
			(uint32_t)mem->shmem_id,
			(uint32_t)unexport_flags);
		if (rc) {
			UTILS_LOG_ERROR("habmm_unexport(id=%lu) failed",
				mem->shmem_id);
			goto end;
		}
	} else {
		rc = -1;
		goto end;
	}

end:
	if (handle) {
		if (rel_hab_handle(ctx, chl_id, 0x00))
			UTILS_LOG_ERROR("rel_hab_handle failed");
	}

	return rc;
}

int
user_os_utils_shmem_unimport(
	void *context,
	struct user_os_utils_mem_info *mem,
	u32 flags)
{
	struct user_os_utils_context *ctx = context;
	int32_t rc = 0;
	int32_t handle = 0;
	u32 chl_id = CHANNEL_BUFFERS;
	u32 unimport_flags = 0;

	if (!mem) {
		UTILS_LOG_ERROR("NULL mem");
		rc = -1;
		goto end;
	}

	handle = get_hab_handle(ctx, &chl_id, 0x00);
	if (!handle) {
		UTILS_LOG_ERROR("get_hab_handle failed for chl_id=%d", chl_id);
		rc = -1;
		goto end;
	}

#if !defined(__linux__)
	unimport_flags |= HABMM_EXPIMP_FLAGS_FD;
#endif

	if (mem->shmem_type == HAB_EXPORT_ID) {
#ifdef USE_HAB
		rc = habmm_unimport(
#else
		rc = habmm_unimport_dummy(
#endif
			handle,
			(uint32_t)mem->shmem_id,
			mem->buffer,
			(uint32_t)unimport_flags);
		if (rc) {
			UTILS_LOG_ERROR("habmm_unimport(id=%lu) failed",
				mem->shmem_id);
			goto end;
		}
	} else {
		rc = -1;
		goto end;
	}

end:
	if (handle) {
		if (rel_hab_handle(ctx, chl_id, 0x00))
			UTILS_LOG_ERROR("rel_hab_handle failed");
	}

	return rc;
}
