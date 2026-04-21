/*-------------------------------------------------------------------------
 *
 * qhapaqxian_output.c
 *	  QhapaqXian semantic logical decoding output plugin
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/qhapaqxian_output/qhapaqxian_output.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "replication/logical.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

typedef struct QhapaqXianOutputData
{
	MemoryContext context;
	char	   *prefix;
} QhapaqXianOutputData;

static const char *const qx_schema_v1 = "\"schema\":\"stage9.semantic.v1\"";
static const char *const qx_schema_v2 = "\"schema\":\"stage31.semantic.v2\"";

static void qx_decode_startup(LogicalDecodingContext *ctx,
							  OutputPluginOptions *opt, bool is_init);
static void qx_decode_shutdown(LogicalDecodingContext *ctx);
static void qx_decode_begin_txn(LogicalDecodingContext *ctx,
								ReorderBufferTXN *txn);
static void qx_decode_commit_txn(LogicalDecodingContext *ctx,
								 ReorderBufferTXN *txn,
								 XLogRecPtr commit_lsn);
static void qx_decode_change(LogicalDecodingContext *ctx,
							 ReorderBufferTXN *txn, Relation relation,
							 ReorderBufferChange *change);
static void qx_decode_truncate(LogicalDecodingContext *ctx,
							   ReorderBufferTXN *txn,
							   int nrelations, Relation relations[],
							   ReorderBufferChange *change);
static void qx_decode_message(LogicalDecodingContext *ctx,
							  ReorderBufferTXN *txn, XLogRecPtr lsn,
							  bool transactional, const char *prefix,
							  Size sz, const char *message);
static void qx_decode_stream_message(LogicalDecodingContext *ctx,
									 ReorderBufferTXN *txn, XLogRecPtr lsn,
									 bool transactional, const char *prefix,
									 Size sz, const char *message);
static void qx_validate_semantic_schema(const char *message, Size sz);
static void qx_write_message(LogicalDecodingContext *ctx, const char *prefix,
							 Size sz, const char *message);

void
_PG_init(void)
{
}

void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	cb->startup_cb = qx_decode_startup;
	cb->shutdown_cb = qx_decode_shutdown;
	cb->begin_cb = qx_decode_begin_txn;
	cb->commit_cb = qx_decode_commit_txn;
	cb->change_cb = qx_decode_change;
	cb->truncate_cb = qx_decode_truncate;
	cb->message_cb = qx_decode_message;
	cb->stream_message_cb = qx_decode_stream_message;
}

static void
qx_decode_startup(LogicalDecodingContext *ctx, OutputPluginOptions *opt,
				  bool is_init)
{
	ListCell   *option;
	QhapaqXianOutputData *data;

	data = palloc0(sizeof(QhapaqXianOutputData));
	data->context = AllocSetContextCreate(ctx->context,
										  "QhapaqXian output context",
										  ALLOCSET_DEFAULT_SIZES);
	data->prefix = MemoryContextStrdup(data->context, "qhapaqxian");

	ctx->output_plugin_private = data;

	opt->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
	opt->receive_rewrites = false;

	foreach(option, ctx->output_plugin_options)
	{
		DefElem    *elem = lfirst(option);

		Assert(elem->arg == NULL || IsA(elem->arg, String));

		if (strcmp(elem->defname, "prefix") == 0)
		{
			if (elem->arg == NULL)
				continue;

			pfree(data->prefix);
			data->prefix = MemoryContextStrdup(data->context, strVal(elem->arg));
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("option \"%s\" = \"%s\" is unknown",
							elem->defname,
							elem->arg ? strVal(elem->arg) : "(null)")));
		}
	}

	(void) is_init;
}

static void
qx_decode_shutdown(LogicalDecodingContext *ctx)
{
	QhapaqXianOutputData *data = ctx->output_plugin_private;

	MemoryContextDelete(data->context);
}

static void
qx_decode_begin_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	(void) ctx;
	(void) txn;
}

static void
qx_decode_commit_txn(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
					 XLogRecPtr commit_lsn)
{
	(void) ctx;
	(void) txn;
	(void) commit_lsn;
}

static void
qx_decode_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				 Relation relation, ReorderBufferChange *change)
{
	(void) ctx;
	(void) txn;
	(void) relation;
	(void) change;
}

static void
qx_decode_truncate(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				   int nrelations, Relation relations[],
				   ReorderBufferChange *change)
{
	(void) ctx;
	(void) txn;
	(void) nrelations;
	(void) relations;
	(void) change;
}

static void
qx_write_message(LogicalDecodingContext *ctx, const char *prefix,
				 Size sz, const char *message)
{
	QhapaqXianOutputData *data = ctx->output_plugin_private;

	if (strcmp(prefix, data->prefix) != 0)
		return;

	OutputPluginPrepareWrite(ctx, true);
	appendBinaryStringInfo(ctx->out, message, sz);
	OutputPluginWrite(ctx, true);
}

static void
qx_validate_semantic_schema(const char *message, Size sz)
{
	char	   *text;
	bool		supported;

	text = pnstrdup(message, sz);
	supported = (strstr(text, qx_schema_v1) != NULL ||
				 strstr(text, qx_schema_v2) != NULL);
	if (!supported)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported QhapaqXian semantic schema in logical message")));
	pfree(text);
}

static void
qx_decode_message(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
				  XLogRecPtr lsn, bool transactional, const char *prefix,
				  Size sz, const char *message)
{
	qx_validate_semantic_schema(message, sz);
	qx_write_message(ctx, prefix, sz, message);
	(void) txn;
	(void) lsn;
	(void) transactional;
}

static void
qx_decode_stream_message(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
						 XLogRecPtr lsn, bool transactional, const char *prefix,
						 Size sz, const char *message)
{
	qx_validate_semantic_schema(message, sz);
	qx_write_message(ctx, prefix, sz, message);
	(void) txn;
	(void) lsn;
	(void) transactional;
}
