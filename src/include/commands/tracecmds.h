/*-------------------------------------------------------------------------
 *
 * tracecmds.h
 *	  prototypes for QhapaqXian DB trace commands
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/commands/tracecmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRACECMDS_H
#define TRACECMDS_H

#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"

extern void ShowTraceCommand(ShowTraceStmt *stmt, DestReceiver *dest);
extern TupleDesc ShowTraceResultDesc(void);

#endif							/* TRACECMDS_H */
