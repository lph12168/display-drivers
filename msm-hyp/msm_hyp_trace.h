/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#if !defined(_HYP_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _HYP_TRACE_H_

#include <linux/stringify.h>
#include <linux/types.h>
#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM msm_hyp
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE msm_hyp_trace

TRACE_EVENT(tracing_mark_write,
	TP_PROTO(char trace_type, const struct task_struct *task,
		 const char *name, int value),
	TP_ARGS(trace_type, task, name, value),
	TP_STRUCT__entry(
			__field(char, trace_type)
			__field(int, pid)
			__string(trace_name, name)
			__field(int, value)
	),
	TP_fast_assign(
			__entry->trace_type = trace_type;
			__entry->pid = task ? task->tgid : 0;
			__assign_str(trace_name, name);
			__entry->value = value;
	),
	TP_printk("%c|%d|%s|%d", __entry->trace_type,
		__entry->pid, __get_str(trace_name), __entry->value)
);

#define hpy_atrace trace_tracing_mark_write

#define HYP_ATRACE_END(name) hpy_atrace('E', current, name, 0)
#define HYP_ATRACE_BEGIN(name) hpy_atrace('B', current, name, 0)

#endif /* _HYP_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#include <trace/define_trace.h>
