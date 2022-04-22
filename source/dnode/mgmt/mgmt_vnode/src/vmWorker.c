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
 * along with this program. If not, see <http:www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "sync.h"
#include "syncTools.h"
#include "vmInt.h"

static inline void vmSendRsp(SMgmtWrapper *pWrapper, SNodeMsg *pMsg, int32_t code) {
  SRpcMsg rsp = {.handle = pMsg->rpcMsg.handle,
                 .ahandle = pMsg->rpcMsg.ahandle,
                 .code = code,
                 .pCont = pMsg->pRsp,
                 .contLen = pMsg->rspLen};
  tmsgSendRsp(&rsp);
}

static void vmProcessMgmtQueue(SQueueInfo *pInfo, SNodeMsg *pMsg) {
  SVnodesMgmt *pMgmt = pInfo->ahandle;

  int32_t code = -1;
  tmsg_t  msgType = pMsg->rpcMsg.msgType;
  dTrace("msg:%p, will be processed in vnode-m queue", pMsg);

  switch (msgType) {
    case TDMT_MON_VM_INFO:
      code = vmProcessGetMonVmInfoReq(pMgmt->pWrapper, pMsg);
      break;
    case TDMT_MON_VM_LOAD:
      code = vmProcessGetVnodeLoadsReq(pMgmt->pWrapper, pMsg);
      break;
    case TDMT_DND_CREATE_VNODE:
      code = vmProcessCreateVnodeReq(pMgmt, pMsg);
      break;
    case TDMT_DND_DROP_VNODE:
      code = vmProcessDropVnodeReq(pMgmt, pMsg);
      break;
    default:
      terrno = TSDB_CODE_MSG_NOT_PROCESSED;
      dError("msg:%p, not processed in vnode-mgmt queue", pMsg);
  }

  if (msgType & 1u) {
    if (code != 0 && terrno != 0) code = terrno;
    vmSendRsp(pMgmt->pWrapper, pMsg, code);
  }

  dTrace("msg:%p, is freed, result:0x%04x:%s", pMsg, code & 0XFFFF, tstrerror(code));
  rpcFreeCont(pMsg->rpcMsg.pCont);
  taosFreeQitem(pMsg);
}

static void vmProcessQueryQueue(SQueueInfo *pInfo, SNodeMsg *pMsg) {
  SVnodeObj *pVnode = pInfo->ahandle;

  dTrace("msg:%p, will be processed in vnode-query queue", pMsg);
  int32_t code = vnodeProcessQueryMsg(pVnode->pImpl, &pMsg->rpcMsg);
  if (code != 0) {
    vmSendRsp(pVnode->pWrapper, pMsg, code);
    dTrace("msg:%p, is freed, result:0x%04x:%s", pMsg, code & 0XFFFF, tstrerror(code));
    rpcFreeCont(pMsg->rpcMsg.pCont);
    taosFreeQitem(pMsg);
  }
}

static void vmProcessFetchQueue(SQueueInfo *pInfo, SNodeMsg *pMsg) {
  SVnodeObj *pVnode = pInfo->ahandle;

  dTrace("msg:%p, will be processed in vnode-fetch queue", pMsg);
  int32_t code = vnodeProcessFetchMsg(pVnode->pImpl, &pMsg->rpcMsg, pInfo);
  if (code != 0) {
    vmSendRsp(pVnode->pWrapper, pMsg, code);
    dTrace("msg:%p, is freed, result:0x%04x:%s", pMsg, code & 0XFFFF, tstrerror(code));
    rpcFreeCont(pMsg->rpcMsg.pCont);
    taosFreeQitem(pMsg);
  }
}

static void vmProcessWriteQueue(SQueueInfo *pInfo, STaosQall *qall, int32_t numOfMsgs) {
  SVnodeObj *pVnode = pInfo->ahandle;
  SRpcMsg    rsp;

  SArray *pArray = taosArrayInit(numOfMsgs, sizeof(SNodeMsg *));
  if (pArray == NULL) {
    dError("failed to process %d msgs in write-queue since %s", numOfMsgs, terrstr());
    return;
  }

  for (int32_t i = 0; i < numOfMsgs; ++i) {
    SNodeMsg *pMsg = NULL;
    if (taosGetQitem(qall, (void **)&pMsg) == 0) continue;

    dTrace("msg:%p, will be processed in vnode-write queue", pMsg);
    if (taosArrayPush(pArray, &pMsg) == NULL) {
      dTrace("msg:%p, failed to process since %s", pMsg, terrstr());
      vmSendRsp(pVnode->pWrapper, pMsg, TSDB_CODE_OUT_OF_MEMORY);
    }
  }

#if 0
  int64_t    version;
  vnodePreprocessWriteReqs(pVnode->pImpl, pArray, &version);

  numOfMsgs = taosArrayGetSize(pArray);
  for (int32_t i = 0; i < numOfMsgs; i++) {
    SNodeMsg *pMsg = *(SNodeMsg **)taosArrayGet(pArray, i);
    SRpcMsg  *pRpc = &pMsg->rpcMsg;
    SRpcMsg   rsp;

    rsp.pCont = NULL;
    rsp.contLen = 0;
    rsp.code = 0;
    rsp.handle = pRpc->handle;
    rsp.ahandle = pRpc->ahandle;

    int32_t code = vnodeProcessWriteReq(pVnode->pImpl, pRpc, version++, &rsp);
    tmsgSendRsp(&rsp);

#if 0
    if (pRsp != NULL) {
      pRsp->ahandle = pRpc->ahandle;
      taosMemoryFree(pRsp);
    } else {
      if (code != 0 && terrno != 0) code = terrno;
      vmSendRsp(pVnode->pWrapper, pMsg, code);
    }
#endif
  }

#else
  // sync integration response
  for (int i = 0; i < taosArrayGetSize(pArray); i++) {
    SNodeMsg *pMsg;
    SRpcMsg  *pRpc;

    pMsg = *(SNodeMsg **)taosArrayGet(pArray, i);
    pRpc = &pMsg->rpcMsg;

    rsp.ahandle = pRpc->ahandle;
    rsp.handle = pRpc->handle;
    rsp.pCont = NULL;
    rsp.contLen = 0;

    int32_t ret = syncPropose(vnodeGetSyncHandle(pVnode->pImpl), pRpc, false);
    if (ret == TAOS_SYNC_PROPOSE_NOT_LEADER) {
      rsp.code = TSDB_CODE_SYN_NOT_LEADER;
      tmsgSendRsp(&rsp);
    } else if (ret == TAOS_SYNC_PROPOSE_OTHER_ERROR) {
      rsp.code = TSDB_CODE_SYN_INTERNAL_ERROR;
      tmsgSendRsp(&rsp);
    } else if (ret == TAOS_SYNC_PROPOSE_SUCCESS) {
      // ok
      // send response in applyQ
    } else {
      assert(0);
    }
  }
#endif

  for (int32_t i = 0; i < numOfMsgs; i++) {
    SNodeMsg *pMsg = *(SNodeMsg **)taosArrayGet(pArray, i);
    dTrace("msg:%p, is freed", pMsg);
    rpcFreeCont(pMsg->rpcMsg.pCont);
    taosFreeQitem(pMsg);
  }

  taosArrayDestroy(pArray);
}

static void vmProcessApplyQueue(SQueueInfo *pInfo, STaosQall *qall, int32_t numOfMsgs) {
  SVnodeObj *pVnode = pInfo->ahandle;
  SNodeMsg  *pMsg = NULL;
  SRpcMsg    rsp;

  // static int64_t version = 0;

  for (int32_t i = 0; i < numOfMsgs; ++i) {
#if 1
    // sync integration

    taosGetQitem(qall, (void **)&pMsg);

    // init response rpc msg
    rsp.code = 0;
    rsp.pCont = NULL;
    rsp.contLen = 0;

    // get original rpc msg
    assert(pMsg->rpcMsg.msgType == TDMT_VND_SYNC_APPLY_MSG);
    SyncApplyMsg *pSyncApplyMsg = syncApplyMsgFromRpcMsg2(&pMsg->rpcMsg);
    syncApplyMsgLog2("==vmProcessApplyQueue==", pSyncApplyMsg);
    SRpcMsg originalRpcMsg;
    syncApplyMsg2OriginalRpcMsg(pSyncApplyMsg, &originalRpcMsg);

    // apply data into tsdb
    if (vnodeProcessWriteReq(pVnode->pImpl, &originalRpcMsg, pSyncApplyMsg->fsmMeta.index, &rsp) < 0) {
      rsp.code = terrno;
      dTrace("vnodeProcessWriteReq error, code:%d", terrno);
    }

    syncApplyMsgDestroy(pSyncApplyMsg);
    rpcFreeCont(originalRpcMsg.pCont);

    // if leader, send response
    if (pMsg->rpcMsg.handle != NULL && pMsg->rpcMsg.ahandle != NULL) {
      rsp.ahandle = pMsg->rpcMsg.ahandle;
      rsp.handle = pMsg->rpcMsg.handle;
      tmsgSendRsp(&rsp);
    }
#endif
  }
}

static void vmProcessSyncQueue(SQueueInfo *pInfo, STaosQall *qall, int32_t numOfMsgs) {
  SVnodeObj *pVnode = pInfo->ahandle;
  SNodeMsg  *pMsg = NULL;

  for (int32_t i = 0; i < numOfMsgs; ++i) {
    taosGetQitem(qall, (void **)&pMsg);

    // todo
    SRpcMsg *pRsp = NULL;
    (void)vnodeProcessSyncReq(pVnode->pImpl, &pMsg->rpcMsg, &pRsp);
  }
}

static void vmProcessMergeQueue(SQueueInfo *pInfo, STaosQall *qall, int32_t numOfMsgs) {
  SVnodeObj *pVnode = pInfo->ahandle;
  SNodeMsg  *pMsg = NULL;

  for (int32_t i = 0; i < numOfMsgs; ++i) {
    taosGetQitem(qall, (void **)&pMsg);

    dTrace("msg:%p, will be processed in vnode-merge queue", pMsg);
    int32_t code = vnodeProcessFetchMsg(pVnode->pImpl, &pMsg->rpcMsg, pInfo);
    if (code != 0) {
      vmSendRsp(pVnode->pWrapper, pMsg, code);
      dTrace("msg:%p, is freed, result:0x%04x:%s", pMsg, code & 0XFFFF, tstrerror(code));
      rpcFreeCont(pMsg->rpcMsg.pCont);
      taosFreeQitem(pMsg);
    }
  }
}

static int32_t vmPutNodeMsgToQueue(SVnodesMgmt *pMgmt, SNodeMsg *pMsg, EQueueType qtype) {
  SRpcMsg  *pRpc = &pMsg->rpcMsg;
  SMsgHead *pHead = pRpc->pCont;
  pHead->contLen = ntohl(pHead->contLen);
  pHead->vgId = ntohl(pHead->vgId);

  SVnodeObj *pVnode = vmAcquireVnode(pMgmt, pHead->vgId);
  if (pVnode == NULL) {
    dError("vgId:%d, failed to write msg:%p to vnode-queue since %s", pHead->vgId, pMsg, terrstr());
    return terrno;
  }

  int32_t code = 0;
  switch (qtype) {
    case QUERY_QUEUE:
      dTrace("msg:%p, will be written into vnode-query queue", pMsg);
      taosWriteQitem(pVnode->pQueryQ, pMsg);
      break;
    case FETCH_QUEUE:
      dTrace("msg:%p, will be written into vnode-fetch queue", pMsg);
      taosWriteQitem(pVnode->pFetchQ, pMsg);
      break;
    case WRITE_QUEUE:
      dTrace("msg:%p, will be written into vnode-write queue", pMsg);
      taosWriteQitem(pVnode->pWriteQ, pMsg);
      break;
    case SYNC_QUEUE:
      dTrace("msg:%p, will be written into vnode-sync queue", pMsg);
      taosWriteQitem(pVnode->pSyncQ, pMsg);
      break;
    case MERGE_QUEUE:
      dTrace("msg:%p, will be written into vnode-merge queue", pMsg);
      taosWriteQitem(pVnode->pMergeQ, pMsg);
      break;
    default:
      code = -1;
      terrno = TSDB_CODE_INVALID_PARA;
      break;
  }

  vmReleaseVnode(pMgmt, pVnode);
  return code;
}

int32_t vmProcessSyncMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SVnodesMgmt *pMgmt = pWrapper->pMgmt;
  return vmPutNodeMsgToQueue(pMgmt, pMsg, SYNC_QUEUE);
}

int32_t vmProcessWriteMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SVnodesMgmt *pMgmt = pWrapper->pMgmt;
  return vmPutNodeMsgToQueue(pMgmt, pMsg, WRITE_QUEUE);
}

int32_t vmProcessQueryMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SVnodesMgmt *pMgmt = pWrapper->pMgmt;
  return vmPutNodeMsgToQueue(pMgmt, pMsg, QUERY_QUEUE);
}

int32_t vmProcessFetchMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SVnodesMgmt *pMgmt = pWrapper->pMgmt;
  return vmPutNodeMsgToQueue(pMgmt, pMsg, FETCH_QUEUE);
}

int32_t vmProcessMergeMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SVnodesMgmt *pMgmt = pWrapper->pMgmt;
  return vmPutNodeMsgToQueue(pMgmt, pMsg, MERGE_QUEUE);
}

int32_t vmProcessMgmtMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SVnodesMgmt   *pMgmt = pWrapper->pMgmt;
  SSingleWorker *pWorker = &pMgmt->mgmtWorker;
  dTrace("msg:%p, will be written to vnode-mgmt queue, worker:%s", pMsg, pWorker->name);
  taosWriteQitem(pWorker->queue, pMsg);
  return 0;
}

int32_t vmProcessMonitorMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SVnodesMgmt   *pMgmt = pWrapper->pMgmt;
  SSingleWorker *pWorker = &pMgmt->monitorWorker;

  dTrace("msg:%p, put into worker:%s", pMsg, pWorker->name);
  taosWriteQitem(pWorker->queue, pMsg);
  return 0;
}

static int32_t vmPutRpcMsgToQueue(SMgmtWrapper *pWrapper, SRpcMsg *pRpc, EQueueType qtype) {
  SVnodesMgmt *pMgmt = pWrapper->pMgmt;
  SMsgHead    *pHead = pRpc->pCont;

  SVnodeObj *pVnode = vmAcquireVnode(pMgmt, pHead->vgId);
  if (pVnode == NULL) return -1;

  SNodeMsg *pMsg = taosAllocateQitem(sizeof(SNodeMsg));
  int32_t   code = 0;

  if (pMsg == NULL) {
    code = -1;
  } else {
    dTrace("msg:%p, is created, type:%s", pMsg, TMSG_INFO(pRpc->msgType));
    pMsg->rpcMsg = *pRpc;
    switch (qtype) {
      case QUERY_QUEUE:
        dTrace("msg:%p, will be put into vnode-query queue", pMsg);
        taosWriteQitem(pVnode->pQueryQ, pMsg);
        break;
      case FETCH_QUEUE:
        dTrace("msg:%p, will be put into vnode-fetch queue", pMsg);
        taosWriteQitem(pVnode->pFetchQ, pMsg);
        break;
      case APPLY_QUEUE:
        dTrace("msg:%p, will be put into vnode-apply queue", pMsg);
        taosWriteQitem(pVnode->pApplyQ, pMsg);
        break;
      case MERGE_QUEUE:
        dTrace("msg:%p, will be put into vnode-merge queue", pMsg);
        taosWriteQitem(pVnode->pMergeQ, pMsg);
        break;
      default:
        code = -1;
        terrno = TSDB_CODE_INVALID_PARA;
        break;
    }
  }
  vmReleaseVnode(pMgmt, pVnode);
  return code;
}

int32_t vmPutMsgToQueryQueue(SMgmtWrapper *pWrapper, SRpcMsg *pRpc) {
  return vmPutRpcMsgToQueue(pWrapper, pRpc, QUERY_QUEUE);
}

int32_t vmPutMsgToFetchQueue(SMgmtWrapper *pWrapper, SRpcMsg *pRpc) {
  return vmPutRpcMsgToQueue(pWrapper, pRpc, FETCH_QUEUE);
}

int32_t vmPutMsgToApplyQueue(SMgmtWrapper *pWrapper, SRpcMsg *pRpc) {
  return vmPutRpcMsgToQueue(pWrapper, pRpc, APPLY_QUEUE);
}

int32_t vmPutMsgToMergeQueue(SMgmtWrapper *pWrapper, SRpcMsg *pRpc) {
  return vmPutRpcMsgToQueue(pWrapper, pRpc, MERGE_QUEUE);
}

// sync integration
int32_t vmPutMsgToSyncQueue(SMgmtWrapper *pWrapper, SRpcMsg *pRpc) {
  return vmPutRpcMsgToQueue(pWrapper, pRpc, SYNC_QUEUE);
}

int32_t vmGetQueueSize(SMgmtWrapper *pWrapper, int32_t vgId, EQueueType qtype) {
  int32_t    size = -1;
  SVnodeObj *pVnode = vmAcquireVnode(pWrapper->pMgmt, vgId);
  if (pVnode != NULL) {
    switch (qtype) {
      case QUERY_QUEUE:
        size = taosQueueSize(pVnode->pQueryQ);
        break;
      case FETCH_QUEUE:
        size = taosQueueSize(pVnode->pFetchQ);
        break;
      case WRITE_QUEUE:
        size = taosQueueSize(pVnode->pWriteQ);
        break;
      case SYNC_QUEUE:
        size = taosQueueSize(pVnode->pSyncQ);
        break;
      case APPLY_QUEUE:
        size = taosQueueSize(pVnode->pApplyQ);
        break;
      case MERGE_QUEUE:
        size = taosQueueSize(pVnode->pMergeQ);
        break;
      default:
        break;
    }
  }
  vmReleaseVnode(pWrapper->pMgmt, pVnode);
  return size;
}

int32_t vmAllocQueue(SVnodesMgmt *pMgmt, SVnodeObj *pVnode) {
  pVnode->pWriteQ = tWWorkerAllocQueue(&pMgmt->writePool, pVnode, (FItems)vmProcessWriteQueue);
  pVnode->pApplyQ = tWWorkerAllocQueue(&pMgmt->writePool, pVnode, (FItems)vmProcessApplyQueue);
  pVnode->pMergeQ = tWWorkerAllocQueue(&pMgmt->mergePool, pVnode, (FItems)vmProcessMergeQueue);
  pVnode->pSyncQ = tWWorkerAllocQueue(&pMgmt->syncPool, pVnode, (FItems)vmProcessSyncQueue);
  pVnode->pFetchQ = tQWorkerAllocQueue(&pMgmt->fetchPool, pVnode, (FItem)vmProcessFetchQueue);
  pVnode->pQueryQ = tQWorkerAllocQueue(&pMgmt->queryPool, pVnode, (FItem)vmProcessQueryQueue);

  if (pVnode->pApplyQ == NULL || pVnode->pWriteQ == NULL || pVnode->pSyncQ == NULL || pVnode->pFetchQ == NULL ||
      pVnode->pQueryQ == NULL || pVnode->pMergeQ == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  dDebug("vgId:%d, vnode queue is alloced", pVnode->vgId);
  return 0;
}

void vmFreeQueue(SVnodesMgmt *pMgmt, SVnodeObj *pVnode) {
  tQWorkerFreeQueue(&pMgmt->queryPool, pVnode->pQueryQ);
  tQWorkerFreeQueue(&pMgmt->fetchPool, pVnode->pFetchQ);
  tWWorkerFreeQueue(&pMgmt->writePool, pVnode->pWriteQ);
  tWWorkerFreeQueue(&pMgmt->writePool, pVnode->pApplyQ);
  tWWorkerFreeQueue(&pMgmt->mergePool, pVnode->pMergeQ);
  tWWorkerFreeQueue(&pMgmt->syncPool, pVnode->pSyncQ);
  pVnode->pWriteQ = NULL;
  pVnode->pApplyQ = NULL;
  pVnode->pSyncQ = NULL;
  pVnode->pFetchQ = NULL;
  pVnode->pQueryQ = NULL;
  pVnode->pMergeQ = NULL;
  dDebug("vgId:%d, vnode queue is freed", pVnode->vgId);
}

int32_t vmStartWorker(SVnodesMgmt *pMgmt) {
  SQWorkerPool *pQPool = &pMgmt->queryPool;
  pQPool->name = "vnode-query";
  pQPool->min = tsNumOfVnodeQueryThreads;
  pQPool->max = tsNumOfVnodeQueryThreads;
  if (tQWorkerInit(pQPool) != 0) return -1;

  SQWorkerPool *pFPool = &pMgmt->fetchPool;
  pFPool->name = "vnode-fetch";
  pFPool->min = tsNumOfVnodeFetchThreads;
  pFPool->max = tsNumOfVnodeFetchThreads;
  if (tQWorkerInit(pFPool) != 0) return -1;

  SWWorkerPool *pWPool = &pMgmt->writePool;
  pWPool->name = "vnode-write";
  pWPool->max = tsNumOfVnodeWriteThreads;
  if (tWWorkerInit(pWPool) != 0) return -1;

  pWPool = &pMgmt->syncPool;
  pWPool->name = "vnode-sync";
  pWPool->max = tsNumOfVnodeSyncThreads;
  if (tWWorkerInit(pWPool) != 0) return -1;

  pWPool = &pMgmt->mergePool;
  pWPool->name = "vnode-merge";
  pWPool->max = tsNumOfVnodeMergeThreads;
  if (tWWorkerInit(pWPool) != 0) return -1;

  SSingleWorkerCfg cfg = {.min = 1, .max = 1, .name = "vnode-mgmt", .fp = (FItem)vmProcessMgmtQueue, .param = pMgmt};
  if (tSingleWorkerInit(&pMgmt->mgmtWorker, &cfg) != 0) {
    dError("failed to start vnode-mgmt worker since %s", terrstr());
    return -1;
  }

  if (tsMultiProcess) {
    SSingleWorkerCfg mCfg = {
        .min = 1, .max = 1, .name = "vnode-monitor", .fp = (FItem)vmProcessMgmtQueue, .param = pMgmt};
    if (tSingleWorkerInit(&pMgmt->monitorWorker, &mCfg) != 0) {
      dError("failed to start mnode vnode-monitor worker since %s", terrstr());
      return -1;
    }
  }

  dDebug("vnode workers are initialized");
  return 0;
}

void vmStopWorker(SVnodesMgmt *pMgmt) {
  tSingleWorkerCleanup(&pMgmt->monitorWorker);
  tSingleWorkerCleanup(&pMgmt->mgmtWorker);
  tQWorkerCleanup(&pMgmt->fetchPool);
  tQWorkerCleanup(&pMgmt->queryPool);
  tWWorkerCleanup(&pMgmt->writePool);
  tWWorkerCleanup(&pMgmt->syncPool);
  tWWorkerCleanup(&pMgmt->mergePool);
  dDebug("vnode workers are closed");
}
