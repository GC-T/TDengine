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

#ifndef _TD_TQ_INT_H_
#define _TD_TQ_INT_H_

#include "tq.h"

#ifdef __cplusplus
extern "C" {
#endif

//implement the array index
//implement the ring buffer

//create persistent storage for meta info such as consuming offset
//return value > 0: cgId
//return value <= 0: error code
int tqCreateGroup(STQ*);
//create ring buffer in memory and load consuming offset
int tqOpenGroup(STQ*, int cgId);
//destroy ring buffer and persist consuming offset
int tqCloseGroup(STQ*, int cgId);
//delete persistent storage for meta info
int tqDropGroup(STQ*, int cgId);

#ifdef __cplusplus
}
#endif

#endif /*_TD_TQ_INT_H_*/
