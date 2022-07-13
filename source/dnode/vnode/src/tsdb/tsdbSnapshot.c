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

#include "tsdb.h"

// STsdbSnapReader ========================================
struct STsdbSnapReader {
  STsdb*  pTsdb;
  int64_t sver;
  int64_t ever;
  // for data file
  int8_t        dataDone;
  int32_t       fid;
  SDataFReader* pDataFReader;
  SArray*       aBlockIdx;  // SArray<SBlockIdx>
  int32_t       iBlockIdx;
  SBlockIdx*    pBlockIdx;
  SMapData      mBlock;  // SMapData<SBlock>
  int32_t       iBlock;
  SBlockData    oBlockData;
  SBlockData    nBlockData;
  // for del file
  int8_t       delDone;
  SDelFReader* pDelFReader;
  SArray*      aDelIdx;  // SArray<SDelIdx>
  int32_t      iDelIdx;
  SArray*      aDelData;  // SArray<SDelData>
};

static int32_t tsdbSnapReadData(STsdbSnapReader* pReader, uint8_t** ppData) {
  int32_t code = 0;
  STsdb*  pTsdb = pReader->pTsdb;

  while (true) {
    if (pReader->pDataFReader == NULL) {
      SDFileSet* pSet = tsdbFSStateGetDFileSet(pTsdb->fs->cState, pReader->fid, TD_GT);

      if (pSet == NULL) goto _exit;

      pReader->fid = pSet->fid;
      code = tsdbDataFReaderOpen(&pReader->pDataFReader, pReader->pTsdb, pSet);
      if (code) goto _err;

      // SBlockIdx
      code = tsdbReadBlockIdx(pReader->pDataFReader, pReader->aBlockIdx, NULL);
      if (code) goto _err;

      pReader->iBlockIdx = 0;
      pReader->pBlockIdx = NULL;

      tsdbInfo("vgId:%d vnode snapshot tsdb open data file to read, fid:%d", TD_VID(pTsdb->pVnode), pReader->fid);
    }

    while (true) {
      if (pReader->pBlockIdx == NULL) {
        if (pReader->iBlockIdx >= taosArrayGetSize(pReader->aBlockIdx)) {
          tsdbDataFReaderClose(&pReader->pDataFReader);
          break;
        }

        pReader->pBlockIdx = (SBlockIdx*)taosArrayGet(pReader->aBlockIdx, pReader->iBlockIdx);
        pReader->iBlockIdx++;

        code = tsdbReadBlock(pReader->pDataFReader, pReader->pBlockIdx, &pReader->mBlock, NULL);
        if (code) goto _err;

        pReader->iBlock = 0;
      }

      SBlock  block;
      SBlock* pBlock = &block;
      while (true) {
        if (pReader->iBlock >= pReader->mBlock.nItem) {
          pReader->pBlockIdx = NULL;
          break;
        }

        tMapDataGetItemByIdx(&pReader->mBlock, pReader->iBlock, pBlock, tGetBlock);
        pReader->iBlock++;

        if (pBlock->minVersion > pReader->ever || pBlock->maxVersion < pReader->sver) continue;

        code = tsdbReadBlockData(pReader->pDataFReader, pReader->pBlockIdx, pBlock, &pReader->oBlockData, NULL, NULL);
        if (code) goto _err;

        // filter
        tBlockDataReset(&pReader->nBlockData);
        for (int32_t iColData = 0; iColData < taosArrayGetSize(pReader->oBlockData.aIdx); iColData++) {
          SColData* pColDataO = tBlockDataGetColDataByIdx(&pReader->oBlockData, iColData);
          SColData* pColDataN = NULL;

          code = tBlockDataAddColData(&pReader->nBlockData, taosArrayGetSize(pReader->nBlockData.aIdx), &pColDataN);
          if (code) goto _err;

          tColDataInit(pColDataN, pColDataO->cid, pColDataO->type, pColDataO->smaOn);
        }

        for (int32_t iRow = 0; iRow < pReader->oBlockData.nRow; iRow++) {
          TSDBROW row = tsdbRowFromBlockData(&pReader->oBlockData, iRow);
          int64_t version = TSDBROW_VERSION(&row);

          if (version < pReader->sver || version > pReader->ever) continue;

          code = tBlockDataAppendRow(&pReader->nBlockData, &row, NULL);
          if (code) goto _err;
        }

        // org data
        // compress data (todo)
        int32_t size = sizeof(TABLEID) + tPutBlockData(NULL, &pReader->nBlockData);

        *ppData = taosMemoryMalloc(sizeof(SSnapDataHdr) + size);
        if (*ppData == NULL) {
          code = TSDB_CODE_OUT_OF_MEMORY;
          goto _err;
        }

        SSnapDataHdr* pHdr = (SSnapDataHdr*)(*ppData);
        pHdr->type = 1;
        pHdr->size = size;

        TABLEID* pId = (TABLEID*)(&pHdr[1]);
        pId->suid = pReader->pBlockIdx->suid;
        pId->uid = pReader->pBlockIdx->uid;

        tPutBlockData((uint8_t*)(&pId[1]), &pReader->nBlockData);

        tsdbInfo("vgId:%d vnode snapshot read data, fid:%d suid:%" PRId64 " uid:%" PRId64
                 " iBlock:%d minVersion:%d maxVersion:%d nRow:%d out of %d size:%d",
                 TD_VID(pTsdb->pVnode), pReader->fid, pReader->pBlockIdx->suid, pReader->pBlockIdx->uid,
                 pReader->iBlock - 1, pBlock->minVersion, pBlock->maxVersion, pReader->nBlockData.nRow, pBlock->nRow,
                 size);

        goto _exit;
      }
    }
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d vnode snapshot tsdb read data failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbSnapReadDel(STsdbSnapReader* pReader, uint8_t** ppData) {
  int32_t   code = 0;
  STsdb*    pTsdb = pReader->pTsdb;
  SDelFile* pDelFile = pTsdb->fs->cState->pDelFile;

  if (pReader->pDelFReader == NULL) {
    if (pDelFile == NULL) {
      goto _exit;
    }

    // open
    code = tsdbDelFReaderOpen(&pReader->pDelFReader, pDelFile, pTsdb, NULL);
    if (code) goto _err;

    // read index
    code = tsdbReadDelIdx(pReader->pDelFReader, pReader->aDelIdx, NULL);
    if (code) goto _err;

    pReader->iDelIdx = 0;
  }

  while (true) {
    if (pReader->iDelIdx >= taosArrayGetSize(pReader->aDelIdx)) {
      tsdbDelFReaderClose(&pReader->pDelFReader);
      break;
    }

    SDelIdx* pDelIdx = (SDelIdx*)taosArrayGet(pReader->aDelIdx, pReader->iDelIdx);

    pReader->iDelIdx++;

    code = tsdbReadDelData(pReader->pDelFReader, pDelIdx, pReader->aDelData, NULL);
    if (code) goto _err;

    int32_t size = 0;
    for (int32_t iDelData = 0; iDelData < taosArrayGetSize(pReader->aDelData); iDelData++) {
      SDelData* pDelData = (SDelData*)taosArrayGet(pReader->aDelData, iDelData);

      if (pDelData->version >= pReader->sver && pDelData->version <= pReader->ever) {
        size += tPutDelData(NULL, pDelData);
      }
    }

    if (size == 0) continue;

    // org data
    size = sizeof(TABLEID) + size;
    *ppData = taosMemoryMalloc(sizeof(SSnapDataHdr) + size);
    if (*ppData == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }

    SSnapDataHdr* pHdr = (SSnapDataHdr*)(*ppData);
    pHdr->type = 2;
    pHdr->size = size;

    TABLEID* pId = (TABLEID*)(&pHdr[1]);
    pId->suid = pDelIdx->suid;
    pId->uid = pDelIdx->uid;
    int32_t n = sizeof(SSnapDataHdr) + sizeof(TABLEID);
    for (int32_t iDelData = 0; iDelData < taosArrayGetSize(pReader->aDelData); iDelData++) {
      SDelData* pDelData = (SDelData*)taosArrayGet(pReader->aDelData, iDelData);

      if (pDelData->version < pReader->sver) continue;
      if (pDelData->version > pReader->ever) continue;

      n += tPutDelData((*ppData) + n, pDelData);
    }

    tsdbInfo("vgId:%d vnode snapshot tsdb read del data, suid:%" PRId64 " uid:%d" PRId64 " size:%d",
             TD_VID(pTsdb->pVnode), pDelIdx->suid, pDelIdx->uid, size);

    break;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d vnode snapshot tsdb read del failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbSnapReaderOpen(STsdb* pTsdb, int64_t sver, int64_t ever, STsdbSnapReader** ppReader) {
  int32_t          code = 0;
  STsdbSnapReader* pReader = NULL;

  // alloc
  pReader = (STsdbSnapReader*)taosMemoryCalloc(1, sizeof(*pReader));
  if (pReader == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pReader->pTsdb = pTsdb;
  pReader->sver = sver;
  pReader->ever = ever;

  pReader->fid = INT32_MIN;
  pReader->aBlockIdx = taosArrayInit(0, sizeof(SBlockIdx));
  if (pReader->aBlockIdx == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pReader->mBlock = tMapDataInit();
  code = tBlockDataInit(&pReader->oBlockData);
  if (code) goto _err;
  code = tBlockDataInit(&pReader->nBlockData);
  if (code) goto _err;

  pReader->aDelIdx = taosArrayInit(0, sizeof(SDelIdx));
  if (pReader->aDelIdx == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pReader->aDelData = taosArrayInit(0, sizeof(SDelData));
  if (pReader->aDelData == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  tsdbInfo("vgId:%d vnode snapshot tsdb reader opened", TD_VID(pTsdb->pVnode));
  *ppReader = pReader;
  return code;

_err:
  tsdbError("vgId:%d vnode snapshot tsdb reader open failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  *ppReader = NULL;
  return code;
}

int32_t tsdbSnapReaderClose(STsdbSnapReader** ppReader) {
  int32_t          code = 0;
  STsdbSnapReader* pReader = *ppReader;

  if (pReader->pDataFReader) {
    tsdbDataFReaderClose(&pReader->pDataFReader);
  }
  taosArrayDestroy(pReader->aBlockIdx);
  tMapDataClear(&pReader->mBlock);
  tBlockDataClear(&pReader->oBlockData, 1);
  tBlockDataClear(&pReader->nBlockData, 1);

  if (pReader->pDelFReader) {
    tsdbDelFReaderClose(&pReader->pDelFReader);
  }
  taosArrayDestroy(pReader->aDelIdx);
  taosArrayDestroy(pReader->aDelData);

  tsdbInfo("vgId:%d vnode snapshot tsdb reader closed", TD_VID(pReader->pTsdb->pVnode));

  taosMemoryFree(pReader);
  *ppReader = NULL;
  return code;
}

int32_t tsdbSnapRead(STsdbSnapReader* pReader, uint8_t** ppData) {
  int32_t code = 0;

  *ppData = NULL;

  // read data file
  if (!pReader->dataDone) {
    code = tsdbSnapReadData(pReader, ppData);
    if (code) {
      goto _err;
    } else {
      if (*ppData) {
        goto _exit;
      } else {
        pReader->dataDone = 1;
      }
    }
  }

  // read del file
  if (!pReader->delDone) {
    code = tsdbSnapReadDel(pReader, ppData);
    if (code) {
      goto _err;
    } else {
      if (*ppData) {
        goto _exit;
      } else {
        pReader->delDone = 1;
      }
    }
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d vnode snapshot tsdb read failed since %s", TD_VID(pReader->pTsdb->pVnode), tstrerror(code));
  return code;
}

// STsdbSnapWriter ========================================
struct STsdbSnapWriter {
  STsdb*  pTsdb;
  int64_t sver;
  int64_t ever;

  // config
  int32_t minutes;
  int8_t  precision;
  int32_t minRow;
  int32_t maxRow;
  int8_t  cmprAlg;
  int64_t commitID;

  // for data file
  SBlockData bData;

  int32_t       fid;
  SDataFReader* pDataFReader;
  SArray*       aBlockIdx;  // SArray<SBlockIdx>
  int32_t       iBlockIdx;
  SBlockIdx*    pBlockIdx;
  SMapData      mBlock;  // SMapData<SBlock>
  int32_t       iBlock;
  SBlock*       pBlock;
  SBlock        block;
  SBlockData    bDataR;
  int32_t       iRow;

  SDataFWriter* pDataFWriter;
  SBlockIdx*    pBlockIdxW;
  SBlockIdx     blockIdx;
  SBlock*       pBlockW;
  SBlock        blockW;
  SBlockData    bDataW;

  SMapData mBlockW;     // SMapData<SBlock>
  SArray*  aBlockIdxW;  // SArray<SBlockIdx>

  // for del file
  SDelFReader* pDelFReader;
  SDelFWriter* pDelFWriter;
  int32_t      iDelIdx;
  SArray*      aDelIdxR;
  SArray*      aDelData;
  SArray*      aDelIdxW;
};

static int32_t tsdbSnapRollback(STsdbSnapWriter* pWriter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbSnapCommit(STsdbSnapWriter* pWriter) {
  int32_t code = 0;
  // TODO
  return code;
}

static int32_t tsdbSnapWriteDataEnd(STsdbSnapWriter* pWriter) {
  int32_t code = 0;
  STsdb*  pTsdb = pWriter->pTsdb;

  if (pWriter->pDataFWriter == NULL) goto _exit;

  // TODO

  code = tsdbDataFWriterClose(&pWriter->pDataFWriter, 0);
  if (code) goto _err;

  if (pWriter->pDataFReader) {
    code = tsdbDataFReaderClose(&pWriter->pDataFReader);
    if (code) goto _err;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d tsdb snapshot writer data end failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteAppendData(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t     code = 0;
  int32_t     iRow = 0;           // todo
  int32_t     nRow = 0;           // todo
  SBlockData* pBlockData = NULL;  // todo

  while (iRow < nRow) {
    code = tBlockDataAppendRow(&pWriter->bDataW, &tsdbRowFromBlockData(pBlockData, iRow), NULL);
    if (code) goto _err;
  }

  return code;

_err:
  tsdbError("vgId:%d tsdb snapshot write append data failed since %s", TD_VID(pWriter->pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteTableDataEnd(STsdbSnapWriter* pWrite) {
  int32_t code = 0;
  // TODO
  return code;
}

#if 0
static int32_t tsdbSnapWriteTableData(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t code = 0;
  TABLEID id = {0};  // TODO

  // skip
  while (pWriter->pBlockIdx && tTABLEIDCmprFn(&id, pWriter->pBlockIdx) < 0) {
    code = tsdbSnapWriteTableDataEnd(pWriter);
    if (code) goto _err;

    pWriter->iBlockIdx++;
    if (pWriter->iBlockIdx < taosArrayGetSize(pWriter->aBlockIdx)) {
      pWriter->pBlockIdx = (SBlockIdx*)taosArrayGet(pWriter->aBlockIdx, pWriter->iBlockIdx);
    } else {
      pWriter->pBlockIdx = NULL;
    }
  }

  // new or merge
  if (pWriter->pBlockIdx == NULL || tTABLEIDCmprFn(&id, pWriter->pBlockIdx) < 0) {
    int32_t c;

    if (pWriter->pBlockIdxW && ((c = tTABLEIDCmprFn(&id, pWriter->pBlockIdxW)) != 0)) {
      ASSERT(c > 0);

      code = tsdbSnapWriteTableDataEnd(pWriter);
      if (code) goto _err;
    }

    if (pWriter->pBlockIdxW == NULL) {
      pWriter->pBlockIdx = &pWriter->blockIdx;
      pWriter->pBlockIdx->suid = id.suid;
      pWriter->pBlockIdx->uid = id.uid;
    }

    // loop to write the data
    TSDBROW*    pRow = NULL;        // todo
    int32_t     nRow = 0;           // todo
    SBlockData* pBlockData = NULL;  // todo
    for (int32_t iRow = 0; iRow < nRow; iRow++) {
      code = tBlockDataAppendRow(&pWriter->bDataW, &tsdbRowFromBlockData(pBlockData, iRow), NULL);
      if (code) goto _err;

      if (pWriter->bDataW.nRow > pWriter->maxRow * 4 / 5) {
        code = tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataW, NULL, NULL, pWriter->pBlockIdxW,
                                  pWriter->pBlockW, pWriter->cmprAlg);
        if (code) goto _err;
      }
    }
  } else {
    // skip
    while (true) {
      if (pWriter->pBlock == NULL) break;
      if (pWriter->pBlock->last) break;
      if (tBlockCmprFn(&(SBlock){.minKey = {0}, .maxKey = {0}}, pWriter->pBlock) >= 0) break;

      code = tMapDataPutItem(&pWriter->mBlockW, pWriter->pBlock, tPutBlock);
      if (code) goto _err;
    }

    if (pWriter->pBlock) {
      if (pWriter->pBlock->last) {
        // load the last block and merge with the data (todo)
      } else {
        int32_t c = tBlockCmprFn(&(SBlock){0 /*TODO*/}, pWriter->pBlock);

        if (c > 0) {
          // commit until pWriter->pBlock (todo)
        } else {
          // load the block and merge with the data (todo)
        }
      }
    } else {
      int32_t     nRow = 0;
      SBlockData* pBlockData = NULL;

      for (int32_t iRow = 0; iRow < nRow; iRow++) {
        code = tBlockDataAppendRow(&pWriter->bDataW, &tsdbRowFromBlockData(pBlockData, iRow), NULL);
        if (code) goto _err;

        if (pWriter->bDataW.nRow >= pWriter->maxRow * 4 / 5) {
          code = tsdbWriteBlockData(pWriter->pDataFWriter, &pWriter->bDataW, NULL, NULL, pWriter->pBlockIdxW,
                                    pWriter->pBlockW, pWriter->cmprAlg);
          if (code) goto _err;

          tBlockDataClearData(&pWriter->bDataW);
        }
      }
    }
  }

  return code;

_err:
  tsdbError("vgId:%d tsdb snapshot write table data failed since %s", TD_VID(pWriter->pTsdb->pVnode), tstrerror(code));
  return code;
}
#endif

static int32_t tsdbSnapWriteDataImpl(STsdbSnapWriter* pWriter, TABLEID id) {
  int32_t     code = 0;
  SBlockData* pBlockData = &pWriter->bData;

  if (pWriter->pDataFReader == NULL) {
    // no old data

    // end last table data commit if id not same
    if (pWriter->pBlockIdxW) {
      int32_t c = tTABLEIDCmprFn(pWriter->pBlockIdx, &id);
      if (c < 0) {
        // commit last table data and reset (todo)
        pWriter->pBlockIdxW = NULL;
      } else if (c > 0) {
        ASSERT(0);
      }
    }

    // start a new table data if need
    if (pWriter->pBlockIdxW == NULL) {
      pWriter->pBlockIdxW = &pWriter->blockIdx;
      pWriter->pBlockIdxW->suid = id.suid;
      pWriter->pBlockIdxW->uid = id.uid;
    }

  } else {
  }

  return code;
}

static int32_t tsdbSnapWriteData(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t       code = 0;
  STsdb*        pTsdb = pWriter->pTsdb;
  SSnapDataHdr* pHdr = (SSnapDataHdr*)pData;
  TABLEID       id = *(TABLEID*)(&pHdr[1]);
  int64_t       n;

  // decode
  SBlockData* pBlockData = &pWriter->bData;
  n = tGetBlockData(pData + sizeof(SSnapDataHdr) + sizeof(TABLEID), pBlockData);
  ASSERT(n + sizeof(SSnapDataHdr) + sizeof(TABLEID) == nData);

  // open file
  TSDBKEY keyFirst = tBlockDataFirstKey(pBlockData);
  TSDBKEY keyLast = tBlockDataLastKey(pBlockData);

  int32_t fid = tsdbKeyFid(keyFirst.ts, pWriter->minutes, pWriter->precision);
  ASSERT(fid == tsdbKeyFid(keyLast.ts, pWriter->minutes, pWriter->precision));
  if (pWriter->pDataFWriter == NULL || pWriter->fid != fid) {
    code = tsdbSnapWriteDataEnd(pWriter);  // todo
    if (code) goto _err;

    pWriter->fid = fid;

    // read
    SDFileSet* pSet = tsdbFSStateGetDFileSet(pTsdb->fs->nState, fid, TD_EQ);  // todo: check nState is valid
    if (pSet) {
      code = tsdbDataFReaderOpen(&pWriter->pDataFReader, pTsdb, pSet);
      if (code) goto _err;

      code = tsdbReadBlockIdx(pWriter->pDataFReader, pWriter->aBlockIdx, NULL);
      if (code) goto _err;
    } else {
      ASSERT(pWriter->pDataFReader == NULL);
      taosArrayClear(pWriter->aBlockIdx);
    }
    pWriter->iBlockIdx = 0;
    pWriter->pBlockIdx = NULL;
    tMapDataReset(&pWriter->mBlock);
    pWriter->iBlock = 0;
    pWriter->pBlock = NULL;
    tBlockDataReset(&pWriter->bDataR);
    pWriter->iRow = 0;

    // write
    SDFileSet wSet;

    if (pSet) {
      wSet = (SDFileSet){.diskId = pSet->diskId,
                         .fid = fid,
                         .fHead = {.commitID = pWriter->commitID, .offset = 0, .size = 0},
                         .fData = pSet->fData,
                         .fLast = {.commitID = pWriter->commitID, .size = 0},
                         .fSma = pSet->fSma};
    } else {
      wSet = (SDFileSet){.diskId = (SDiskID){.level = 0, .id = 0},
                         .fid = fid,
                         .fHead = {.commitID = pWriter->commitID, .offset = 0, .size = 0},
                         .fData = {.commitID = pWriter->commitID, .size = 0},
                         .fLast = {.commitID = pWriter->commitID, .size = 0},
                         .fSma = {.commitID = pWriter->commitID, .size = 0}};
    }

    code = tsdbDataFWriterOpen(&pWriter->pDataFWriter, pTsdb, &wSet);
    if (code) goto _err;

    taosArrayClear(pWriter->aBlockIdxW);
    pWriter->pBlockIdxW = NULL;
    tMapDataReset(&pWriter->mBlockW);
    pWriter->pBlockW = NULL;
    tBlockDataReset(&pWriter->bDataW);
  }

  code = tsdbSnapWriteDataImpl(pWriter, id);
  if (code) goto _err;

  tsdbInfo("vgId:%d vnode snapshot tsdb write data, fid:%d suid:%" PRId64 " uid:%" PRId64 " nRow:%d",
           TD_VID(pTsdb->pVnode), fid, id.suid, id.suid, pBlockData->nRow);
  return code;

_err:
  tsdbError("vgId:%d tsdb snapshot write data failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteDel(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t code = 0;
  STsdb*  pTsdb = pWriter->pTsdb;

  if (pWriter->pDelFWriter == NULL) {
    SDelFile* pDelFile = tsdbFSStateGetDelFile(pTsdb->fs->nState);

    // reader
    if (pDelFile) {
      code = tsdbDelFReaderOpen(&pWriter->pDelFReader, pDelFile, pTsdb, NULL);
      if (code) goto _err;

      code = tsdbReadDelIdx(pWriter->pDelFReader, pWriter->aDelIdxR, NULL);
      if (code) goto _err;
    }

    // writer
    SDelFile delFile = {.commitID = pTsdb->pVnode->state.commitID, .offset = 0, .size = 0};
    code = tsdbDelFWriterOpen(&pWriter->pDelFWriter, &delFile, pTsdb);
    if (code) goto _err;
  }

  // process the del data
  TABLEID id = {0};  // todo

  while (true) {
    SDelIdx* pDelIdx = NULL;
    int64_t  n = 0;
    SDelData delData;
    SDelIdx  delIdx;
    int8_t   toBreak = 0;

    if (pWriter->iDelIdx < taosArrayGetSize(pWriter->aDelIdxR)) {
      pDelIdx = taosArrayGet(pWriter->aDelIdxR, pWriter->iDelIdx);
    }

    if (pDelIdx) {
      int32_t c = tTABLEIDCmprFn(&id, pDelIdx);
      if (c < 0) {
        goto _new_del;
      } else {
        code = tsdbReadDelData(pWriter->pDelFReader, pDelIdx, pWriter->aDelData, NULL);
        if (code) goto _err;

        pWriter->iDelIdx++;
        if (c == 0) {
          toBreak = 1;
          delIdx = (SDelIdx){.suid = id.suid, .uid = id.uid};
          goto _merge_del;
        } else {
          delIdx = (SDelIdx){.suid = pDelIdx->suid, .uid = pDelIdx->uid};
          goto _write_del;
        }
      }
    }

  _new_del:
    toBreak = 1;
    delIdx = (SDelIdx){.suid = id.suid, .uid = id.uid};
    taosArrayClear(pWriter->aDelData);

  _merge_del:
    while (n < nData) {
      n += tGetDelData(pData + n, &delData);
      if (taosArrayPush(pWriter->aDelData, &delData) == NULL) {
        code = TSDB_CODE_OUT_OF_MEMORY;
        goto _err;
      }
    }

  _write_del:
    code = tsdbWriteDelData(pWriter->pDelFWriter, pWriter->aDelData, NULL, &delIdx);
    if (code) goto _err;

    if (taosArrayPush(pWriter->aDelIdxW, &delIdx) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }

    if (toBreak) break;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d tsdb snapshot write del failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

static int32_t tsdbSnapWriteDelEnd(STsdbSnapWriter* pWriter) {
  int32_t code = 0;
  STsdb*  pTsdb = pWriter->pTsdb;

  if (pWriter->pDelFWriter == NULL) goto _exit;
  for (; pWriter->iDelIdx < taosArrayGetSize(pWriter->aDelIdxR); pWriter->iDelIdx++) {
    SDelIdx* pDelIdx = (SDelIdx*)taosArrayGet(pWriter->aDelIdxR, pWriter->iDelIdx);

    code = tsdbReadDelData(pWriter->pDelFReader, pDelIdx, pWriter->aDelData, NULL);
    if (code) goto _err;

    SDelIdx delIdx = (SDelIdx){.suid = pDelIdx->suid, .uid = pDelIdx->uid};
    code = tsdbWriteDelData(pWriter->pDelFWriter, pWriter->aDelData, NULL, &delIdx);
    if (code) goto _err;

    if (taosArrayPush(pWriter->aDelIdxR, &delIdx) == NULL) {
      code = TSDB_CODE_OUT_OF_MEMORY;
      goto _err;
    }
  }

  code = tsdbUpdateDelFileHdr(pWriter->pDelFWriter);
  if (code) goto _err;

  code = tsdbFSStateUpsertDelFile(pTsdb->fs->nState, &pWriter->pDelFWriter->fDel);
  if (code) goto _err;

  code = tsdbDelFWriterClose(&pWriter->pDelFWriter, 1);
  if (code) goto _err;

  if (pWriter->pDelFReader) {
    code = tsdbDelFReaderClose(&pWriter->pDelFReader);
    if (code) goto _err;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d tsdb snapshow write del end failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbSnapWriterOpen(STsdb* pTsdb, int64_t sver, int64_t ever, STsdbSnapWriter** ppWriter) {
  int32_t          code = 0;
  STsdbSnapWriter* pWriter = NULL;

  // alloc
  pWriter = (STsdbSnapWriter*)taosMemoryCalloc(1, sizeof(*pWriter));
  if (pWriter == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pWriter->pTsdb = pTsdb;
  pWriter->sver = sver;
  pWriter->ever = ever;

  // config
  pWriter->minutes = pTsdb->keepCfg.days;
  pWriter->precision = pTsdb->keepCfg.precision;
  pWriter->minRow = pTsdb->pVnode->config.tsdbCfg.minRows;
  pWriter->maxRow = pTsdb->pVnode->config.tsdbCfg.maxRows;
  pWriter->cmprAlg = pTsdb->pVnode->config.tsdbCfg.compression;
  pWriter->commitID = pTsdb->pVnode->state.commitID;

  // for data file
  code = tBlockDataInit(&pWriter->bData);

  if (code) goto _err;
  pWriter->aBlockIdx = taosArrayInit(0, sizeof(SBlockIdx));
  if (pWriter->aBlockIdx == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  code = tBlockDataInit(&pWriter->bDataR);
  if (code) goto _err;

  pWriter->aBlockIdxW = taosArrayInit(0, sizeof(SBlockIdx));
  if (pWriter->aBlockIdxW == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  code = tBlockDataInit(&pWriter->bDataW);
  if (code) goto _err;

  // for del file
  pWriter->aDelIdxR = taosArrayInit(0, sizeof(SDelIdx));
  if (pWriter->aDelIdxR == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pWriter->aDelData = taosArrayInit(0, sizeof(SDelData));
  if (pWriter->aDelData == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }
  pWriter->aDelIdxW = taosArrayInit(0, sizeof(SDelIdx));
  if (pWriter->aDelIdxW == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _err;
  }

  *ppWriter = pWriter;
  return code;

_err:
  tsdbError("vgId:%d tsdb snapshot writer open failed since %s", TD_VID(pTsdb->pVnode), tstrerror(code));
  *ppWriter = NULL;
  return code;
}

int32_t tsdbSnapWriterClose(STsdbSnapWriter** ppWriter, int8_t rollback) {
  int32_t          code = 0;
  STsdbSnapWriter* pWriter = *ppWriter;

  if (rollback) {
    code = tsdbSnapRollback(pWriter);
    if (code) goto _err;
  } else {
    code = tsdbSnapWriteDataEnd(pWriter);
    if (code) goto _err;

    code = tsdbSnapWriteDelEnd(pWriter);
    if (code) goto _err;

    code = tsdbSnapCommit(pWriter);
    if (code) goto _err;
  }

  taosMemoryFree(pWriter);
  *ppWriter = NULL;

  return code;

_err:
  tsdbError("vgId:%d tsdb snapshot writer close failed since %s", TD_VID(pWriter->pTsdb->pVnode), tstrerror(code));
  return code;
}

int32_t tsdbSnapWrite(STsdbSnapWriter* pWriter, uint8_t* pData, uint32_t nData) {
  int32_t       code = 0;
  SSnapDataHdr* pHdr = (SSnapDataHdr*)pData;

  // ts data
  if (pHdr->type == 1) {
    code = tsdbSnapWriteData(pWriter, pData, nData);
    if (code) goto _err;

    goto _exit;
  } else {
    if (pWriter->pDataFWriter) {
      code = tsdbSnapWriteDataEnd(pWriter);
      if (code) goto _err;
    }
  }

  // del data
  if (pHdr->type == 2) {
    code = tsdbSnapWriteDel(pWriter, pData + 1, nData - 1);
    if (code) goto _err;
  }

_exit:
  return code;

_err:
  tsdbError("vgId:%d tsdb snapshow write failed since %s", TD_VID(pWriter->pTsdb->pVnode), tstrerror(code));
  return code;
}
