////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "ShortestPathBlock.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/Query.h"
#include "Graph/ShortestPathResult.h"
#include "Transaction/Methods.h"
#include "Utils/OperationCursor.h"
#include "VocBase/EdgeCollectionInfo.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/ManagedDocumentResult.h"

#include <velocypack/Iterator.h>
#include <velocypack/velocypack-aliases.h>

/// @brief typedef the template instantiation of the PathFinder
typedef arangodb::graph::AttributeWeightShortestPathFinder ArangoDBPathFinder;

using namespace arangodb::aql;

ShortestPathBlock::ShortestPathBlock(ExecutionEngine* engine,
                                     ShortestPathNode const* ep)
    : ExecutionBlock(engine, ep),
      _vertexVar(nullptr),
      _vertexReg(ExecutionNode::MaxRegisterId),
      _edgeVar(nullptr),
      _edgeReg(ExecutionNode::MaxRegisterId),
      _opts(nullptr),
      _posInPath(0),
      _pathLength(0),
      _path(nullptr),
      _startReg(ExecutionNode::MaxRegisterId),
      _useStartRegister(false),
      _targetReg(ExecutionNode::MaxRegisterId),
      _useTargetRegister(false),
      _usedConstant(false) {
  _opts = ep->options();
  _mmdr.reset(new ManagedDocumentResult);

  size_t count = ep->_edgeColls.size();
  TRI_ASSERT(ep->_directions.size());
  _collectionInfos.reserve(count);

  for (size_t j = 0; j < count; ++j) {
    auto info = std::make_unique<arangodb::traverser::EdgeCollectionInfo>(
        _trx, ep->_edgeColls[j], ep->_directions[j], _opts->weightAttribute,
        _opts->defaultWeight);
    _collectionInfos.emplace_back(info.get());
    info.release();
  }

  if (!ep->usesStartInVariable()) {
    _startVertexId = ep->getStartVertex();
  } else {
    auto it = ep->getRegisterPlan()->varInfo.find(ep->startInVariable()->id);
    TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
    _startReg = it->second.registerId;
    _useStartRegister = true;
  }

  if (!ep->usesTargetInVariable()) {
    _targetVertexId = ep->getTargetVertex();
  } else {
    auto it = ep->getRegisterPlan()->varInfo.find(ep->targetInVariable()->id);
    TRI_ASSERT(it != ep->getRegisterPlan()->varInfo.end());
    _targetReg = it->second.registerId;
    _useTargetRegister = true;
  }

  if (ep->usesVertexOutVariable()) {
    _vertexVar = ep->vertexOutVariable();
  }

  if (ep->usesEdgeOutVariable()) {
    _edgeVar = ep->edgeOutVariable();
  }
  _path = std::make_unique<arangodb::graph::ShortestPathResult>();

  if (arangodb::ServerState::instance()->isCoordinator()) {
    if (_opts->useWeight()) {
      _finder.reset(
          new arangodb::graph::AttributeWeightShortestPathFinder(_opts));
    } else {
      _finder.reset(
          new arangodb::graph::ConstantWeightShortestPathFinder(_opts));
    }
  } else {
    if (_opts->useWeight()) {
      _finder.reset(
          new arangodb::graph::AttributeWeightShortestPathFinder(_opts));
    } else {
      _finder.reset(
          new arangodb::graph::ConstantWeightShortestPathFinder(_opts));
    }
  }
}

ShortestPathBlock::~ShortestPathBlock() {
  for (auto& it : _collectionInfos) {
    delete it;
  }
}

int ShortestPathBlock::initialize() {
  DEBUG_BEGIN_BLOCK();
  int res = ExecutionBlock::initialize();
  auto varInfo = getPlanNode()->getRegisterPlan()->varInfo;

  if (usesVertexOutput()) {
    TRI_ASSERT(_vertexVar != nullptr);
    auto it = varInfo.find(_vertexVar->id);
    TRI_ASSERT(it != varInfo.end());
    TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
    _vertexReg = it->second.registerId;
  }
  if (usesEdgeOutput()) {
    TRI_ASSERT(_edgeVar != nullptr);
    auto it = varInfo.find(_edgeVar->id);
    TRI_ASSERT(it != varInfo.end());
    TRI_ASSERT(it->second.registerId < ExecutionNode::MaxRegisterId);
    _edgeReg = it->second.registerId;
  }

  return res;

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

int ShortestPathBlock::initializeCursor(AqlItemBlock* items, size_t pos) {
  _posInPath = 0;
  _pathLength = 0;
  _usedConstant = false;
  return ExecutionBlock::initializeCursor(items, pos);
}

bool ShortestPathBlock::nextPath(AqlItemBlock const* items) {
  if (_usedConstant) {
    // Both source and target are constant.
    // Just one path to compute
    return false;
  }
  _path->clear();
  if (!_useStartRegister && !_useTargetRegister) {
    // Both are constant, after this computation we are done
    _usedConstant = true;
  }
  if (!_useStartRegister) {
    auto pos = _startVertexId.find('/');
    if (pos == std::string::npos) {
      _engine->getQuery()->registerWarning(TRI_ERROR_BAD_PARAMETER,
                                           "Invalid input for Shortest Path: "
                                           "Only id strings or objects with "
                                           "_id are allowed");
      return false;
    } else {
      _opts->setStart(_startVertexId);
    }
  } else {
    AqlValue const& in = items->getValueReference(_pos, _startReg);
    if (in.isObject()) {
      try {
        _opts->setStart(_trx->extractIdString(in.slice()));
      } catch (...) {
        // _id or _key not present... ignore this error and fall through
        // returning no path
        return false;
      }
    } else if (in.isString()) {
      _startVertexId = in.slice().copyString();
      _opts->setStart(_startVertexId);
    } else {
      _engine->getQuery()->registerWarning(
          TRI_ERROR_BAD_PARAMETER,
          "Invalid input for Shortest Path: Only "
          "id strings or objects with _id are "
          "allowed");
      return false;
    }
  }

  if (!_useTargetRegister) {
    auto pos = _targetVertexId.find('/');
    if (pos == std::string::npos) {
      _engine->getQuery()->registerWarning(TRI_ERROR_BAD_PARAMETER,
                                           "Invalid input for Shortest Path: "
                                           "Only id strings or objects with "
                                           "_id are allowed");
      return false;
    } else {
      _opts->setEnd(_targetVertexId);
    }
  } else {
    AqlValue const& in = items->getValueReference(_pos, _targetReg);
    if (in.isObject()) {
      try {
        std::string idString = _trx->extractIdString(in.slice());
        _opts->setEnd(idString);
      } catch (...) {
        // _id or _key not present... ignore this error and fall through
        // returning no path
        return false;
      }
    } else if (in.isString()) {
      _targetVertexId = in.slice().copyString();
      _opts->setEnd(_targetVertexId);
    } else {
      _engine->getQuery()->registerWarning(
          TRI_ERROR_BAD_PARAMETER,
          "Invalid input for Shortest Path: Only "
          "id strings or objects with _id are "
          "allowed");
      return false;
    }
  }

  VPackSlice start = _opts->getStart();
  VPackSlice end = _opts->getEnd();
  TRI_ASSERT(_finder != nullptr);
  // We do not need this data anymore. Result has been processed.
  // Save some memory.
  _coordinatorCache.clear();
  bool hasPath =
      _finder->shortestPath(start, end, *_path, [this]() { throwIfKilled(); });

  if (hasPath) {
    _posInPath = 0;
    _pathLength = _path->length();
  }

  return hasPath;
}

AqlItemBlock* ShortestPathBlock::getSome(size_t, size_t atMost) {
  DEBUG_BEGIN_BLOCK();
  traceGetSomeBegin();
  if (_done) {
    traceGetSomeEnd(nullptr);
    return nullptr;
  }

  if (_buffer.empty()) {
    size_t toFetch = (std::min)(DefaultBatchSize(), atMost);
    if (!ExecutionBlock::getBlock(toFetch, toFetch)) {
      _done = true;
      traceGetSomeEnd(nullptr);
      return nullptr;
    }
    _pos = 0;  // this is in the first block
  }

  // If we get here, we do have _buffer.front()
  AqlItemBlock* cur = _buffer.front();
  size_t const curRegs = cur->getNrRegs();

  // Collect the next path:
  if (_posInPath >= _pathLength) {
    if (!nextPath(cur)) {
      // This input does not have any path. maybe the next one has.
      // we can only return nullptr iff the buffer is empty.
      if (++_pos >= cur->size()) {
        _buffer.pop_front();  // does not throw
        returnBlock(cur);
        _pos = 0;
      }
      auto r = getSome(atMost, atMost);
      traceGetSomeEnd(r);
      return r;
    }
  }

  size_t available = _pathLength - _posInPath;
  size_t toSend = (std::min)(atMost, available);

  RegisterId nrRegs =
      getPlanNode()->getRegisterPlan()->nrRegs[getPlanNode()->getDepth()];
  std::unique_ptr<AqlItemBlock> res(requestBlock(toSend, nrRegs));
  // automatically freed if we throw
  TRI_ASSERT(curRegs <= res->getNrRegs());

  // only copy 1st row of registers inherited from previous frame(s)
  inheritRegisters(cur, res.get(), _pos);

  for (size_t j = 0; j < toSend; j++) {
    if (usesVertexOutput()) {
      res->setValue(j, _vertexReg,
                    _path->vertexToAqlValue(_opts->cache(), _posInPath));
    }
    if (usesEdgeOutput()) {
      res->setValue(j, _edgeReg,
                    _path->edgeToAqlValue(_opts->cache(), _posInPath));
    }
    if (j > 0) {
      // re-use already copied aqlvalues
      res->copyValuesFromFirstRow(j, static_cast<RegisterId>(curRegs));
    }
    ++_posInPath;
  }

  if (_posInPath >= _pathLength) {
    // Advance read position for next call
    if (++_pos >= cur->size()) {
      _buffer.pop_front();  // does not throw
      returnBlock(cur);
      _pos = 0;
    }
  }

  // Clear out registers no longer needed later:
  clearRegisters(res.get());
  traceGetSomeEnd(res.get());
  return res.release();

  // cppcheck-suppress style
  DEBUG_END_BLOCK();
}

size_t ShortestPathBlock::skipSome(size_t, size_t atMost) { return 0; }
