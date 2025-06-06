/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * RDP session stream dump interface
 *
 * Copyright 2022 Armin Novak
 * Copyright 2022 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <time.h>

#include <winpr/sysinfo.h>
#include <winpr/path.h>
#include <winpr/string.h>

#include <freerdp/freerdp.h>
#include <freerdp/streamdump.h>
#include <freerdp/transport_io.h>

#include "streamdump.h"

#define TAG FREERDP_TAG("streamdump")

struct stream_dump_context
{
	rdpTransportIo io;
	size_t writeDumpOffset;
	size_t readDumpOffset;
	size_t replayOffset;
	UINT64 replayTime;
	CONNECTION_STATE state;
	BOOL isServer;
	BOOL nodelay;
	wLog* log;
};

static UINT32 crc32b(const BYTE* data, size_t length)
{
	UINT32 crc = 0xFFFFFFFF;

	for (size_t x = 0; x < length; x++)
	{
		const UINT32 d = data[x] & 0xFF;
		crc = crc ^ d;
		for (int j = 7; j >= 0; j--)
		{
			UINT32 mask = ~(crc & 1);
			crc = (crc >> 1) ^ (0xEDB88320 & mask);
		}
	}
	return ~crc;
}

#if !defined(BUILD_TESTING_INTERNAL)
static
#endif
    BOOL
    stream_dump_read_line(FILE* fp, wStream* s, UINT64* pts, size_t* pOffset, UINT32* flags)
{
	BOOL rc = FALSE;
	UINT64 ts = 0;
	UINT64 size = 0;
	size_t r = 0;
	UINT32 crc32 = 0;
	BYTE received = 0;

	if (!fp || !s || !flags)
		return FALSE;

	if (pOffset)
		(void)_fseeki64(fp, WINPR_ASSERTING_INT_CAST(int64_t, *pOffset), SEEK_SET);

	r = fread(&ts, 1, sizeof(ts), fp);
	if (r != sizeof(ts))
		goto fail;
	r = fread(&received, 1, sizeof(received), fp);
	if (r != sizeof(received))
		goto fail;
	r = fread(&crc32, 1, sizeof(crc32), fp);
	if (r != sizeof(crc32))
		goto fail;
	r = fread(&size, 1, sizeof(size), fp);
	if (r != sizeof(size))
		goto fail;
	if (received)
		*flags = STREAM_MSG_SRV_RX;
	else
		*flags = STREAM_MSG_SRV_TX;

	const size_t usize = WINPR_ASSERTING_INT_CAST(size_t, size);
	if (!Stream_EnsureRemainingCapacity(s, usize))
		goto fail;
	r = fread(Stream_Pointer(s), 1, usize, fp);
	if (r != size)
		goto fail;
	if (crc32 != crc32b(Stream_ConstPointer(s), usize))
		goto fail;
	Stream_Seek(s, usize);

	if (pOffset)
	{
		INT64 tmp = _ftelli64(fp);
		if (tmp < 0)
			goto fail;
		*pOffset = (size_t)tmp;
	}

	if (pts)
		*pts = ts;
	rc = TRUE;

fail:
	Stream_SealLength(s);
	return rc;
}

#if !defined(BUILD_TESTING_INTERNAL)
static
#endif
    BOOL
    stream_dump_write_line(FILE* fp, UINT32 flags, wStream* s)
{
	BOOL rc = FALSE;
	const UINT64 t = GetTickCount64();
	const BYTE* data = Stream_Buffer(s);
	const size_t usize = Stream_Length(s);
	const uint64_t size = (uint64_t)usize;

	if (!fp || !s)
		return FALSE;

	{
		const UINT32 crc32 = crc32b(data, usize);
		const BYTE received = flags & STREAM_MSG_SRV_RX;
		size_t r = fwrite(&t, 1, sizeof(t), fp);
		if (r != sizeof(t))
			goto fail;
		r = fwrite(&received, 1, sizeof(received), fp);
		if (r != sizeof(received))
			goto fail;
		r = fwrite(&crc32, 1, sizeof(crc32), fp);
		if (r != sizeof(crc32))
			goto fail;
		r = fwrite(&size, 1, sizeof(size), fp);
		if (r != sizeof(size))
			goto fail;
		r = fwrite(data, 1, usize, fp);
		if (r != usize)
			goto fail;
	}

	rc = TRUE;
fail:
	return rc;
}

static FILE* stream_dump_get_file(const rdpSettings* settings, const char* mode)
{
	const char* cfolder = NULL;
	char* file = NULL;
	FILE* fp = NULL;

	if (!settings || !mode)
		return NULL;

	cfolder = freerdp_settings_get_string(settings, FreeRDP_TransportDumpFile);
	if (!cfolder)
		file = GetKnownSubPath(KNOWN_PATH_TEMP, "freerdp-transport-dump");
	else
		file = _strdup(cfolder);

	if (!file)
		goto fail;

	fp = winpr_fopen(file, mode);
fail:
	free(file);
	return fp;
}

SSIZE_T stream_dump_append(const rdpContext* context, UINT32 flags, wStream* s, size_t* offset)
{
	SSIZE_T rc = -1;
	FILE* fp = NULL;
	const UINT32 mask = STREAM_MSG_SRV_RX | STREAM_MSG_SRV_TX;
	CONNECTION_STATE state = freerdp_get_state(context);
	int r = 0;

	if (!context || !s || !offset)
		return -1;

	if ((flags & STREAM_MSG_SRV_RX) && (flags & STREAM_MSG_SRV_TX))
		return -1;

	if ((flags & mask) == 0)
		return -1;

	if (state < context->dump->state)
		return 0;

	fp = stream_dump_get_file(context->settings, "ab");
	if (!fp)
		return -1;

	r = _fseeki64(fp, WINPR_ASSERTING_INT_CAST(int64_t, *offset), SEEK_SET);
	if (r < 0)
		goto fail;

	if (!stream_dump_write_line(fp, flags, s))
		goto fail;
	{
		const int64_t rt = _ftelli64(fp);
		if (rt < 0)
		{
			rc = -1;
			goto fail;
		}
		rc = WINPR_ASSERTING_INT_CAST(SSIZE_T, rt);
	}
	*offset = (size_t)rc;

fail:
	if (fp)
		(void)fclose(fp);
	return rc;
}

SSIZE_T stream_dump_get(const rdpContext* context, UINT32* flags, wStream* s, size_t* offset,
                        UINT64* pts)
{
	SSIZE_T rc = -1;
	FILE* fp = NULL;
	int r = 0;

	if (!context || !s || !offset)
		return -1;
	fp = stream_dump_get_file(context->settings, "rb");
	if (!fp)
		return -1;
	r = _fseeki64(fp, WINPR_ASSERTING_INT_CAST(int64_t, *offset), SEEK_SET);
	if (r < 0)
		goto fail;

	if (!stream_dump_read_line(fp, s, pts, offset, flags))
		goto fail;

	const int64_t rt = _ftelli64(fp);
	if (rt < 0)
		goto fail;
	rc = WINPR_ASSERTING_INT_CAST(SSIZE_T, rt);
fail:
	if (fp)
		(void)fclose(fp);
	return rc;
}

static int stream_dump_transport_write(rdpTransport* transport, wStream* s)
{
	SSIZE_T r = 0;
	rdpContext* ctx = transport_get_context(transport);

	WINPR_ASSERT(ctx);
	WINPR_ASSERT(ctx->dump);
	WINPR_ASSERT(s);

	r = stream_dump_append(ctx, ctx->dump->isServer ? STREAM_MSG_SRV_TX : STREAM_MSG_SRV_RX, s,
	                       &ctx->dump->writeDumpOffset);
	if (r < 0)
		return -1;

	WINPR_ASSERT(ctx->dump->io.WritePdu);
	return ctx->dump->io.WritePdu(transport, s);
}

static int stream_dump_transport_read(rdpTransport* transport, wStream* s)
{
	int rc = 0;
	rdpContext* ctx = transport_get_context(transport);

	WINPR_ASSERT(ctx);
	WINPR_ASSERT(ctx->dump);
	WINPR_ASSERT(s);

	WINPR_ASSERT(ctx->dump->io.ReadPdu);
	rc = ctx->dump->io.ReadPdu(transport, s);
	if (rc > 0)
	{
		SSIZE_T r =
		    stream_dump_append(ctx, ctx->dump->isServer ? STREAM_MSG_SRV_RX : STREAM_MSG_SRV_TX, s,
		                       &ctx->dump->readDumpOffset);
		if (r < 0)
			return -1;
	}
	return rc;
}

static BOOL stream_dump_register_write_handlers(rdpContext* context)
{
	rdpTransportIo dump = { 0 };
	const rdpTransportIo* dfl = freerdp_get_io_callbacks(context);

	if (!freerdp_settings_get_bool(context->settings, FreeRDP_TransportDump))
		return TRUE;

	WINPR_ASSERT(dfl);
	dump = *dfl;

	/* Remember original callbacks for later */
	WINPR_ASSERT(context->dump);
	context->dump->io.ReadPdu = dfl->ReadPdu;
	context->dump->io.WritePdu = dfl->WritePdu;

	/* Set our dump wrappers */
	dump.WritePdu = stream_dump_transport_write;
	dump.ReadPdu = stream_dump_transport_read;
	return freerdp_set_io_callbacks(context, &dump);
}

static int stream_dump_replay_transport_write(rdpTransport* transport, wStream* s)
{
	rdpContext* ctx = transport_get_context(transport);
	size_t size = 0;

	WINPR_ASSERT(ctx);
	WINPR_ASSERT(s);

	size = Stream_Length(s);
	WLog_Print(ctx->dump->log, WLOG_TRACE, "replay write %" PRIuz, size);
	// TODO: Compare with write file

	return 1;
}

static int stream_dump_replay_transport_read(rdpTransport* transport, wStream* s)
{
	rdpContext* ctx = transport_get_context(transport);

	size_t size = 0;
	UINT64 slp = 0;
	UINT64 ts = 0;
	UINT32 flags = 0;

	WINPR_ASSERT(ctx);
	WINPR_ASSERT(ctx->dump);
	WINPR_ASSERT(s);

	const size_t start = Stream_GetPosition(s);
	do
	{
		Stream_SetPosition(s, start);
		if (stream_dump_get(ctx, &flags, s, &ctx->dump->replayOffset, &ts) < 0)
			return -1;
	} while (flags & STREAM_MSG_SRV_RX);

	if (!ctx->dump->nodelay)
	{
		if ((ctx->dump->replayTime > 0) && (ts > ctx->dump->replayTime))
			slp = ts - ctx->dump->replayTime;
	}
	ctx->dump->replayTime = ts;

	size = Stream_Length(s);
	Stream_SetPosition(s, 0);
	WLog_Print(ctx->dump->log, WLOG_TRACE, "replay read %" PRIuz, size);

	if (slp > 0)
	{
		uint64_t duration = slp;
		do
		{
			const DWORD actual = (DWORD)MIN(duration, UINT32_MAX);
			Sleep(actual);
			duration -= actual;
		} while (duration > 0);
	}

	return 1;
}

static int stream_dump_replay_transport_tcp_connect(WINPR_ATTR_UNUSED rdpContext* context,
                                                    WINPR_ATTR_UNUSED rdpSettings* settings,
                                                    WINPR_ATTR_UNUSED const char* hostname,
                                                    WINPR_ATTR_UNUSED int port,
                                                    WINPR_ATTR_UNUSED DWORD timeout)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(settings);
	WINPR_ASSERT(hostname);

	return 42;
}

static rdpTransportLayer* stream_dump_replay_transport_connect_layer(
    WINPR_ATTR_UNUSED rdpTransport* transport, WINPR_ATTR_UNUSED const char* hostname,
    WINPR_ATTR_UNUSED int port, WINPR_ATTR_UNUSED DWORD timeout)
{
	WINPR_ASSERT(transport);
	WINPR_ASSERT(hostname);

	return NULL;
}

static BOOL stream_dump_replay_transport_tls_connect(WINPR_ATTR_UNUSED rdpTransport* transport)
{
	WINPR_ASSERT(transport);
	return TRUE;
}

static BOOL stream_dump_replay_transport_accept(WINPR_ATTR_UNUSED rdpTransport* transport)
{
	WINPR_ASSERT(transport);
	return TRUE;
}

static BOOL stream_dump_register_read_handlers(rdpContext* context)
{
	const rdpTransportIo* dfl = freerdp_get_io_callbacks(context);

	if (!freerdp_settings_get_bool(context->settings, FreeRDP_TransportDumpReplay))
		return TRUE;

	WINPR_ASSERT(dfl);
	rdpTransportIo dump = *dfl;

	/* Remember original callbacks for later */
	WINPR_ASSERT(context->dump);
	context->dump->nodelay =
	    freerdp_settings_get_bool(context->settings, FreeRDP_TransportDumpReplayNodelay);
	context->dump->io.ReadPdu = dfl->ReadPdu;
	context->dump->io.WritePdu = dfl->WritePdu;

	/* Set our dump wrappers */
	dump.WritePdu = stream_dump_transport_write;
	dump.ReadPdu = stream_dump_transport_read;

	/* Set our dump wrappers */
	dump.WritePdu = stream_dump_replay_transport_write;
	dump.ReadPdu = stream_dump_replay_transport_read;
	dump.TCPConnect = stream_dump_replay_transport_tcp_connect;
	dump.TLSAccept = stream_dump_replay_transport_accept;
	dump.TLSConnect = stream_dump_replay_transport_tls_connect;
	dump.ConnectLayer = stream_dump_replay_transport_connect_layer;
	if (!freerdp_set_io_callbacks(context, &dump))
		return FALSE;
	return freerdp_io_callback_set_event(context, TRUE);
}

BOOL stream_dump_register_handlers(rdpContext* context, CONNECTION_STATE state, BOOL isServer)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(context->dump);
	context->dump->state = state;
	context->dump->isServer = isServer;
	if (!stream_dump_register_write_handlers(context))
		return FALSE;
	return stream_dump_register_read_handlers(context);
}

void stream_dump_free(rdpStreamDumpContext* dump)
{
	free(dump);
}

rdpStreamDumpContext* stream_dump_new(void)
{
	rdpStreamDumpContext* dump = calloc(1, sizeof(rdpStreamDumpContext));
	if (!dump)
		return NULL;
	dump->log = WLog_Get(TAG);

	return dump;
}
