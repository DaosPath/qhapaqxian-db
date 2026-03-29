/*-------------------------------------------------------------------------
 *
 * memorycmds.h
 *	  prototypes for QhapaqXian DB memory commands
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/commands/memorycmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MEMORYCMDS_H
#define MEMORYCMDS_H

#include "access/tupdesc.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"

extern void RememberMemoryCommand(RememberStmt *stmt);
extern void FetchMemoryCommand(FetchMemoryStmt *stmt, DestReceiver *dest);
extern TupleDesc FetchMemoryResultDesc(void);

#endif							/* MEMORYCMDS_H */
