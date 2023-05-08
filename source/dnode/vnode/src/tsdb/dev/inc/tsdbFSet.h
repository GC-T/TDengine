/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tsdbFile.h"

#ifndef _TSDB_FILE_SET_H
#define _TSDB_FILE_SET_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct STFileSet STFileSet;
typedef struct SFileOp   SFileOp;

typedef enum {
  TSDB_FOP_NONE = 0,
  TSDB_FOP_EXTEND,
  TSDB_FOP_CREATE,
  TSDB_FOP_DELETE,
  TSDB_FOP_TRUNCATE,
} tsdb_fop_t;

int32_t tsdbFileSetCreate(int32_t fid, STFileSet **ppSet);
int32_t tsdbFileSetEdit(STFileSet *pSet, SFileOp *pOp);
int32_t tsdbFileSetToJson(SJson *pJson, const STFileSet *pSet);
int32_t tsdbEditFileSet(STFileSet *pFileSet, const SFileOp *pOp);

struct SFileOp {
  tsdb_fop_t op;
  int32_t    fid;
  STFile     oState;  // old file state
  STFile     nState;  // new file state
};

typedef struct SSttLvl {
  LISTD(struct SSttLvl) listNode;
  int32_t lvl;   // level
  int32_t nStt;  // number of .stt files on this level
  STFile *fStt;  // .stt files
} SSttLvl;

struct STFileSet {
  int32_t fid;
  int64_t nextid;
  STFile *farr[TSDB_FTYPE_MAX];  // file array
  SSttLvl lvl0;                  // level 0 of .stt
};

#ifdef __cplusplus
}
#endif

#endif /*_TSDB_FILE_SET_H*/