/*
 * Copyright (c) 2016, Linaro Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <arm.h>
#include <kernel/misc.h>
#include <kernel/pseudo_ta.h>
#include <kernel/user_ta.h>
#include <kernel/thread.h>
#include <mm/core_memprot.h>
#include <mm/tee_mmu.h>
#include <optee_msg_supplicant.h>
#include <pta_gprof.h>
#include <string.h>

static TEE_Result gprof_send_rpc(TEE_UUID *uuid, void *buf, size_t len,
				 uint32_t *id)
{
	struct optee_msg_param params[3];
	TEE_Result res = TEE_ERROR_GENERIC;
	uint64_t c = 0;
	paddr_t pa;
	char *va;

	thread_rpc_alloc_payload(sizeof(*uuid) + len, &pa, &c);
	if (!pa)
		return TEE_ERROR_OUT_OF_MEMORY;

	va = phys_to_virt(pa, MEM_AREA_NSEC_SHM);
	if (!va)
		goto exit;

	memcpy(va, uuid, sizeof(*uuid));
	memcpy(va + sizeof(*uuid), buf, len);

	memset(params, 0, sizeof(params));
	params[0].attr = OPTEE_MSG_ATTR_TYPE_VALUE_INOUT;
	params[0].u.value.a = *id;

	params[1].attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
	params[1].u.tmem.buf_ptr = pa;
	params[1].u.tmem.size = sizeof(*uuid);
	params[1].u.tmem.shm_ref = c;

	params[2].attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
	params[2].u.tmem.buf_ptr = pa + sizeof(*uuid);
	params[2].u.tmem.size = len;
	params[2].u.tmem.shm_ref = c;

	res = thread_rpc_cmd(OPTEE_MSG_RPC_CMD_GPROF, 3, params);
	if (res != TEE_SUCCESS)
		goto exit;

	*id = (uint32_t)params[0].u.value.a;
exit:
	thread_rpc_free_payload(c);
	return res;
}

static TEE_Result gprof_send(struct tee_ta_session *s,
			     uint32_t param_types,
			     TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_INOUT,
					  TEE_PARAM_TYPE_MEMREF_INPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);

	if (exp_pt != param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	return gprof_send_rpc(&s->ctx->uuid, params[1].memref.buffer,
			      params[1].memref.size, &params[0].value.a);
}

static TEE_Result gprof_start_pc_sampling(struct tee_ta_session *s,
					  uint32_t param_types,
					  TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_MEMREF_INOUT,
					  TEE_PARAM_TYPE_VALUE_INPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);
	struct sample_buf *sbuf;
	uint32_t offset;
	uint32_t scale;
	TEE_Result res;
	uint32_t len;
	uaddr_t buf;

	if (exp_pt != param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	buf = (uaddr_t)params[0].memref.buffer;
	len = params[0].memref.size;
	offset = params[1].value.a;
	scale = params[1].value.b;

	res = tee_mmu_check_access_rights(to_user_ta_ctx(s->ctx),
					  TEE_MEMORY_ACCESS_WRITE |
					  TEE_MEMORY_ACCESS_ANY_OWNER,
					  buf, len);
	if (res != TEE_SUCCESS)
		return res;
	sbuf = calloc(1, sizeof(*sbuf));
	if (!sbuf)
		return TEE_ERROR_OUT_OF_MEMORY;

	sbuf->samples = (uint16_t *)buf;
	sbuf->nsamples = len / sizeof(*sbuf->samples);
	sbuf->offset = offset;
	sbuf->scale = scale;
	sbuf->freq = read_cntfrq();
	sbuf->enabled = true;
	s->sbuf = sbuf;

	return TEE_SUCCESS;
}

static TEE_Result gprof_stop_pc_sampling(struct tee_ta_session *s,
					 uint32_t param_types,
					 TEE_Param params[TEE_NUM_PARAMS])
{
	uint32_t exp_pt = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE,
					  TEE_PARAM_TYPE_NONE);
	struct sample_buf *sbuf;
	uint32_t rate;

	if (exp_pt != param_types)
		return TEE_ERROR_BAD_PARAMETERS;

	sbuf = s->sbuf;
	if (!sbuf)
		return TEE_ERROR_BAD_STATE;
	assert(sbuf->samples);

	/* Stop sampling */
	if (sbuf->enabled)
		sbuf->enabled = false;

	rate = ((uint64_t)sbuf->count * sbuf->freq) / sbuf->usr;
	params[0].value.a = rate;

	DMSG("TA sampling stats: sample count=%" PRIu32 " user time=%" PRIu64
	     " cntfrq=%" PRIu32 " rate=%" PRIu32, sbuf->count, sbuf->usr,
	     sbuf->freq, rate);

	free(sbuf);
	s->sbuf = NULL;

	return TEE_SUCCESS;
}

/*
 * Trusted Application Entry Points
 */

static TEE_Result open_session(uint32_t param_types __unused,
			       TEE_Param params[TEE_NUM_PARAMS] __unused,
			       void **sess_ctx __unused)
{
	struct tee_ta_session *s;

	/* Check that we're called from a user TA */
	s = tee_ta_get_calling_session();
	if (!s)
		return TEE_ERROR_ACCESS_DENIED;
	if (is_pseudo_ta_ctx(s->ctx))
		return TEE_ERROR_ACCESS_DENIED;

	return TEE_SUCCESS;
}

static TEE_Result invoke_command(void *sess_ctx __unused, uint32_t cmd_id,
				 uint32_t param_types,
				 TEE_Param params[TEE_NUM_PARAMS])
{
	struct tee_ta_session *s = tee_ta_get_calling_session();

	switch (cmd_id) {
	case PTA_GPROF_SEND:
		return gprof_send(s, param_types, params);
	case PTA_GPROF_START_PC_SAMPLING:
		return gprof_start_pc_sampling(s, param_types, params);
	case PTA_GPROF_STOP_PC_SAMPLING:
		return gprof_stop_pc_sampling(s, param_types, params);
	default:
		break;
	}
	return TEE_ERROR_NOT_IMPLEMENTED;
}

pseudo_ta_register(.uuid = PTA_GPROF_UUID, .name = "gprof",
		   .flags = PTA_DEFAULT_FLAGS,
		   .open_session_entry_point = open_session,
		   .invoke_command_entry_point = invoke_command);
