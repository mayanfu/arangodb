////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"
#include "common.h"

#include "StorageEngineMock.h"

#if USE_ENTERPRISE
  #include "Enterprise/Ldap/LdapFeature.h"
#endif

#include "V8/v8-globals.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"
#include "Transaction/UserTransaction.h"
#include "Transaction/StandaloneContext.h"
#include "Transaction/V8Context.h"
#include "Utils/OperationOptions.h"
#include "Utils/SingleCollectionTransaction.h"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/OptimizerRulesFeature.h"
#include "GeneralServer/AuthenticationFeature.h"
#include "IResearch/ApplicationServerHelper.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFeature.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchView.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/SystemDatabaseFeature.h"
#include "Logger/Logger.h"
#include "Logger/LogTopic.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "ApplicationFeatures/JemallocFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/ViewTypesFeature.h"
#include "RestServer/AqlFeature.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/TraverserEngineRegistryFeature.h"
#include "Basics/VelocyPackHelper.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/BasicBlocks.h"
#include "Aql/Ast.h"
#include "Aql/Query.h"
#include "ExecutionBlockMock.h"
#include "3rdParty/iresearch/tests/tests_config.hpp"

#include "IResearch/VelocyPackHelper.h"
#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "utils/utf8_path.hpp"

#include <velocypack/Iterator.h>

extern const char* ARGV0; // defined in main.cpp

namespace {

struct IResearchBlockMockSetup {
  StorageEngineMock engine;
  arangodb::application_features::ApplicationServer server;
  std::unique_ptr<TRI_vocbase_t> system;
  std::vector<std::pair<arangodb::application_features::ApplicationFeature*, bool>> features;

  IResearchBlockMockSetup(): server(nullptr, nullptr) {
    arangodb::EngineSelectorFeature::ENGINE = &engine;

    arangodb::tests::init(true);

    // suppress INFO {authentication} Authentication is turned on (system only), authentication for unix sockets is turned on
    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::WARN);

    // suppress log messages since tests check error conditions
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::ERR); // suppress WARNING DefaultCustomTypeHandler called
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(), arangodb::LogLevel::FATAL);
    irs::logger::output_le(iresearch::logger::IRL_FATAL, stderr);

    // setup required application features
    features.emplace_back(new arangodb::ViewTypesFeature(&server), true);
    features.emplace_back(new arangodb::AuthenticationFeature(&server), true); // required for FeatureCacheFeature
    features.emplace_back(new arangodb::DatabasePathFeature(&server), false);
    features.emplace_back(new arangodb::JemallocFeature(&server), false); // required for DatabasePathFeature
    features.emplace_back(new arangodb::DatabaseFeature(&server), false); // required for FeatureCacheFeature
    features.emplace_back(new arangodb::QueryRegistryFeature(&server), false); // must be first
    arangodb::application_features::ApplicationServer::server->addFeature(features.back().first);
    system = irs::memory::make_unique<TRI_vocbase_t>(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 0, TRI_VOC_SYSTEM_DATABASE);
    features.emplace_back(new arangodb::TraverserEngineRegistryFeature(&server), false); // must be before AqlFeature
    features.emplace_back(new arangodb::AqlFeature(&server), true);
    features.emplace_back(new arangodb::aql::OptimizerRulesFeature(&server), true);
    features.emplace_back(new arangodb::aql::AqlFunctionFeature(&server), true); // required for IResearchAnalyzerFeature
    features.emplace_back(new arangodb::iresearch::IResearchAnalyzerFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::IResearchFeature(&server), true);
    features.emplace_back(new arangodb::iresearch::SystemDatabaseFeature(&server, system.get()), false); // required for IResearchAnalyzerFeature

    #if USE_ENTERPRISE
      features.emplace_back(new arangodb::LdapFeature(&server), false); // required for AuthenticationFeature with USE_ENTERPRISE
    #endif

    for (auto& f : features) {
      arangodb::application_features::ApplicationServer::server->addFeature(f.first);
    }

    for (auto& f : features) {
      f.first->prepare();
    }

    for (auto& f : features) {
      if (f.second) {
        f.first->start();
      }
    }

    auto* analyzers = arangodb::iresearch::getFeature<arangodb::iresearch::IResearchAnalyzerFeature>();

    analyzers->emplace("test_analyzer", "TestAnalyzer", "abc"); // cache analyzer
    analyzers->emplace("test_csv_analyzer", "TestDelimAnalyzer", ","); // cache analyzer
  }

  ~IResearchBlockMockSetup() {
    system.reset(); // destroy before reseting the 'ENGINE'
    arangodb::AqlFeature(&server).stop(); // unset singleton instance
    arangodb::LogTopic::setLogLevel(arangodb::iresearch::TOPIC.name(), arangodb::LogLevel::DEFAULT);
    arangodb::LogTopic::setLogLevel(arangodb::Logger::FIXME.name(), arangodb::LogLevel::DEFAULT);
    arangodb::application_features::ApplicationServer::server = nullptr;
    arangodb::EngineSelectorFeature::ENGINE = nullptr;

    // destroy application features
    for (auto& f : features) {
      if (f.second) {
        f.first->stop();
      }
    }

    for (auto& f : features) {
      f.first->unprepare();
    }

    arangodb::LogTopic::setLogLevel(arangodb::Logger::AUTHENTICATION.name(), arangodb::LogLevel::DEFAULT);
  }
}; // IResearchQuerySetup

}

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

TEST_CASE("ExecutionBlockMockTestSingle", "[iresearch]") {
  IResearchBlockMockSetup s;
  UNUSED(s);

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  arangodb::aql::ResourceMonitor resMon;

  // getSome
  {
    std::string const queryString = "RETURN 1";
    arangodb::aql::Query query(
      false, &vocbase, arangodb::aql::QueryString(queryString),
      nullptr, std::make_shared<arangodb::velocypack::Builder>(),
      arangodb::aql::PART_MAIN
    );
    query.prepare(arangodb::QueryRegistryFeature::QUERY_REGISTRY,
                  arangodb::aql::Query::DontCache);

    arangodb::aql::AqlItemBlock data(&resMon, 100, 4);

    // build simple chain
    // Singleton <- MockBlock
    MockNode<arangodb::aql::SingletonNode> rootNode;
    arangodb::aql::SingletonBlock rootBlock(query.engine(), &rootNode);

    ExecutionNodeMock node;
    ExecutionBlockMock block(data, *query.engine(), node);
    block.addDependency(&rootBlock);

    // retrieve first 10 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block.getSome(10));
      CHECK(nullptr != res);
      CHECK(10 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // retrieve last 90 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block.getSome(100));
      CHECK(nullptr != res);
      CHECK(90 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // exhausted
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block.getSome(1));
      CHECK(nullptr == res);
    }
  }

  // getSome + skipSome
  {
    std::string const queryString = "RETURN 1";
    arangodb::aql::Query query(
      false, &vocbase, arangodb::aql::QueryString(queryString),
      nullptr, std::make_shared<arangodb::velocypack::Builder>(),
      arangodb::aql::PART_MAIN
    );
    query.prepare(arangodb::QueryRegistryFeature::QUERY_REGISTRY,
                  arangodb::aql::Query::DontCache);

    arangodb::aql::AqlItemBlock data(&resMon, 100, 4);

    // build simple chain
    // Singleton <- MockBlock
    MockNode<arangodb::aql::SingletonNode> rootNode;
    arangodb::aql::SingletonBlock rootBlock(query.engine(), &rootNode);

    ExecutionNodeMock node;
    ExecutionBlockMock block(data, *query.engine(), node);
    block.addDependency(&rootBlock);

    // retrieve first 10 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block.getSome(10));
      CHECK(nullptr != res);
      CHECK(10 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // skip last 90 items
    CHECK(90 == block.skipSome(90));

    // exhausted
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block.getSome(1));
      CHECK(nullptr == res);
    }
  }

  // skipSome + getSome
  {
    std::string const queryString = "RETURN 1";
    arangodb::aql::Query query(
      false, &vocbase, arangodb::aql::QueryString(queryString),
      nullptr, std::make_shared<arangodb::velocypack::Builder>(),
      arangodb::aql::PART_MAIN
    );
    query.prepare(arangodb::QueryRegistryFeature::QUERY_REGISTRY,
                  arangodb::aql::Query::DontCache);

    arangodb::aql::AqlItemBlock data(&resMon, 100, 4);

    // build simple chain
    // Singleton <- MockBlock
    MockNode<arangodb::aql::SingletonNode> rootNode;
    arangodb::aql::SingletonBlock rootBlock(query.engine(), &rootNode);

    ExecutionNodeMock node;
    ExecutionBlockMock block(data, *query.engine(), node);
    block.addDependency(&rootBlock);

    // skip last 90 items
    CHECK(90 == block.skipSome(90));

    // retrieve first 10 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block.getSome(10));
      CHECK(nullptr != res);
      CHECK(10 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // exhausted
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block.getSome(1));
      CHECK(nullptr == res);
    }
  }
}

TEST_CASE("ExecutionBlockMockTestChain", "[iresearch]") {
  IResearchBlockMockSetup s;
  UNUSED(s);

  TRI_vocbase_t vocbase(TRI_vocbase_type_e::TRI_VOCBASE_TYPE_NORMAL, 1, "testVocbase");
  arangodb::aql::ResourceMonitor resMon;

  // getSome
  {
    std::string const queryString = "RETURN 1";
    arangodb::aql::Query query(
      false, &vocbase, arangodb::aql::QueryString(queryString),
      nullptr, std::make_shared<arangodb::velocypack::Builder>(),
      arangodb::aql::PART_MAIN
    );
    query.prepare(arangodb::QueryRegistryFeature::QUERY_REGISTRY,
                  arangodb::aql::Query::DontCache);

    // build chain:
    // Singleton <- MockBlock0 <- MockBlock1
    MockNode<arangodb::aql::SingletonNode> rootNode;
    arangodb::aql::SingletonBlock rootBlock(query.engine(), &rootNode);

    arangodb::aql::AqlItemBlock data0(&resMon, 2, 2);
    ExecutionNodeMock node0;
    ExecutionBlockMock block0(data0, *query.engine(), node0);
    block0.addDependency(&rootBlock);

    arangodb::aql::AqlItemBlock data1(&resMon, 100, 4);
    ExecutionNodeMock node1;
    ExecutionBlockMock block1(data1, *query.engine(), node1);
    block1.addDependency(&block0);

    // retrieve first 10 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(10));
      CHECK(nullptr != res);
      CHECK(10 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // retrieve 90 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(100));
      CHECK(nullptr != res);
      CHECK(90 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // retrieve last 100 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(100));
      CHECK(nullptr != res);
      CHECK(100 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // exhausted
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(1));
      CHECK(nullptr == res);
    }
  }

  // getSome + skip
  {
    std::string const queryString = "RETURN 1";
    arangodb::aql::Query query(
      false, &vocbase, arangodb::aql::QueryString(queryString),
      nullptr, std::make_shared<arangodb::velocypack::Builder>(),
      arangodb::aql::PART_MAIN
    );
    query.prepare(arangodb::QueryRegistryFeature::QUERY_REGISTRY,
                  arangodb::aql::Query::DontCache);

    // build chain:
    // Singleton <- MockBlock0 <- MockBlock1
    MockNode<arangodb::aql::SingletonNode> rootNode;
    arangodb::aql::SingletonBlock rootBlock(query.engine(), &rootNode);

    arangodb::aql::AqlItemBlock data0(&resMon, 2, 2);
    ExecutionNodeMock node0;
    ExecutionBlockMock block0(data0, *query.engine(), node0);
    block0.addDependency(&rootBlock);

    arangodb::aql::AqlItemBlock data1(&resMon, 100, 4);
    ExecutionNodeMock node1;
    ExecutionBlockMock block1(data1, *query.engine(), node1);
    block1.addDependency(&block0);

    // retrieve first 10 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(10));
      CHECK(nullptr != res);
      CHECK(10 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // skip 90 items
    CHECK(90 == block1.skipSome(90));

    // retrieve last 100 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(100));
      CHECK(nullptr != res);
      CHECK(100 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // exhausted
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(1));
      CHECK(nullptr == res);
    }
  }

  // skip + getSome
  {
    std::string const queryString = "RETURN 1";
    arangodb::aql::Query query(
      false, &vocbase, arangodb::aql::QueryString(queryString),
      nullptr, std::make_shared<arangodb::velocypack::Builder>(),
      arangodb::aql::PART_MAIN
    );
    query.prepare(arangodb::QueryRegistryFeature::QUERY_REGISTRY,
                  arangodb::aql::Query::DontCache);

    // build chain:
    // Singleton <- MockBlock0 <- MockBlock1
    MockNode<arangodb::aql::SingletonNode> rootNode;
    arangodb::aql::SingletonBlock rootBlock(query.engine(), &rootNode);

    arangodb::aql::AqlItemBlock data0(&resMon, 2, 2);
    ExecutionNodeMock node0;
    ExecutionBlockMock block0(data0, *query.engine(), node0);
    block0.addDependency(&rootBlock);

    arangodb::aql::AqlItemBlock data1(&resMon, 100, 4);
    ExecutionNodeMock node1;
    ExecutionBlockMock block1(data1, *query.engine(), node1);
    block1.addDependency(&block0);

    // skip 90 items
    CHECK(90 == block1.skipSome(90));

    // retrieve 10 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(10));
      CHECK(nullptr != res);
      CHECK(10 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // retrieve last 100 items
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(100));
      CHECK(nullptr != res);
      CHECK(100 == res->size());
      CHECK(4 == res->getNrRegs());
    }

    // exhausted
    {
      std::unique_ptr<arangodb::aql::AqlItemBlock> res(block1.getSome(1));
      CHECK(nullptr == res);
    }
  }
}
