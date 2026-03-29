/*-------------------------------------------------------------------------
 *
 * sessioncmds.h
 *	  prototypes for QhapaqXian DB session commands
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/commands/sessioncmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SESSIONCMDS_H
#define SESSIONCMDS_H

#include "nodes/parsenodes.h"

extern void StartSessionCommand(StartSessionStmt *stmt);

#endif							/* SESSIONCMDS_H */
