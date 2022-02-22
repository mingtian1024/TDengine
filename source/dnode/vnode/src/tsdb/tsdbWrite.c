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

#include "tsdbDef.h"

// insert TS data
int tsdbInsertData(STsdb *pTsdb, SSubmitReq *pMsg, SSubmitRsp *pRsp) {
  // Check if mem is there. If not, create one.
  if (pTsdb->mem == NULL) {
    pTsdb->mem = tsdbNewMemTable(pTsdb);
    if (pTsdb->mem == NULL) {
      return -1;
    }
  }
  return tsdbMemTableInsert(pTsdb, pTsdb->mem, pMsg, NULL);
}

/**
 * @brief insert Time-range-wise Sma(TSma) data
 *
 * @param pTsdb
 * @param param
 * @param pData
 * @return int32_t
 * TODO: Who is responsible for resource release
 */
int32_t tsdbInsertTSmaData(STsdb *pTsdb, STimeRangeSma *param, STimeRangeData *pData) {
  // 
  return tsdbInsertTSmaDataImpl(pTsdb, param, pData);
}

// insert Time-range-wise Roll-Up Sma(RSma) data
int32_t tsdbInsertRSmaData(STsdb *pTsdb) { return TSDB_CODE_SUCCESS; }