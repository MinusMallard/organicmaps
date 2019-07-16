#include "routing/routing_benchmarks/helpers.hpp"

#include "testing/testing.hpp"

#include "routing/base/astar_algorithm.hpp"
#include "routing/features_road_graph.hpp"
#include "routing/router_delegate.hpp"

#include "routing_integration_tests/routing_test_tools.hpp"

#include "indexer/classificator_loader.hpp"
#include "indexer/mwm_set.hpp"

#include "platform/local_country_file.hpp"
#include "platform/local_country_file_utils.hpp"
#include "platform/platform.hpp"

#include "geometry/mercator.hpp"
#include "geometry/polyline2d.hpp"

#include "base/logging.hpp"
#include "base/math.hpp"
#include "base/timer.hpp"

#include <limits>
#include <memory>

using namespace std;

namespace
{
m2::PointD GetPointOnEdge(routing::Edge const & e, double posAlong)
{
  if (posAlong <= 0.0)
    return e.GetStartJunction().GetPoint();
  if (posAlong >= 1.0)
    return e.GetEndJunction().GetPoint();
  m2::PointD const d = e.GetEndJunction().GetPoint() - e.GetStartJunction().GetPoint();
  return e.GetStartJunction().GetPoint() + d * posAlong;
}
}  // namespace

RoutingTest::RoutingTest(routing::IRoadGraph::Mode mode, routing::VehicleType type,
                         set<string> const & neededMaps)
  : m_mode(mode), m_type(type), m_neededMaps(neededMaps), m_numMwmIds(make_unique<routing::NumMwmIds>())
{
  classificator::Load();

  Platform & platform = GetPlatform();
  m_cig = storage::CountryInfoReader::CreateCountryInfoReader(platform);

  platform::FindAllLocalMapsAndCleanup(numeric_limits<int64_t>::max(), m_localFiles);

  set<string> registeredMaps;
  for (auto const & localFile : m_localFiles)
  {
    m_numMwmIds->RegisterFile(localFile.GetCountryFile());

    auto const & name = localFile.GetCountryName();
    if (m_neededMaps.count(name) == 0)
      continue;

    UNUSED_VALUE(m_dataSource.RegisterMap(localFile));

    auto const & countryFile = localFile.GetCountryFile();
    TEST(m_dataSource.IsLoaded(countryFile), ());
    MwmSet::MwmId const id = m_dataSource.GetMwmIdByCountryFile(countryFile);
    TEST(id.IsAlive(), ());

    registeredMaps.insert(name);
  }

  if (registeredMaps != m_neededMaps)
  {
    for (auto const & file : m_neededMaps)
    {
      if (registeredMaps.count(file) == 0)
        LOG(LERROR, ("Can't find map:", file));
    }
    TEST(false, ("Some maps can't be found. See error messages above."));
  }
}

void RoutingTest::TestRouters(m2::PointD const & startPos, m2::PointD const & finalPos)
{
  // Find route by A*-bidirectional algorithm.
  routing::Route routeFoundByAstarBidirectional("", 0 /* route id */);
  {
    auto router = CreateRouter("test-astar-bidirectional");
    TestRouter(*router, startPos, finalPos, routeFoundByAstarBidirectional);
  }

  // Find route by A* algorithm.
  routing::Route routeFoundByAstar("", 0 /* route id */);
  {
    auto router = CreateRouter("test-astar");
    TestRouter(*router, startPos, finalPos, routeFoundByAstar);
  }

  double constexpr kEpsilon = 1e-6;
  TEST(base::AlmostEqualAbs(routeFoundByAstar.GetTotalDistanceMeters(),
                          routeFoundByAstarBidirectional.GetTotalDistanceMeters(), kEpsilon),
       ());
}

void RoutingTest::TestTwoPointsOnFeature(m2::PointD const & startPos, m2::PointD const & finalPos)
{
  vector<pair<routing::Edge, routing::Junction>> startEdges;
  GetNearestEdges(startPos, startEdges);
  TEST(!startEdges.empty(), ());

  vector<pair<routing::Edge, routing::Junction>> finalEdges;
  GetNearestEdges(finalPos, finalEdges);
  TEST(!finalEdges.empty(), ());

  m2::PointD const startPosOnFeature =
      GetPointOnEdge(startEdges.front().first, 0.0 /* the start point of the feature */);
  m2::PointD const finalPosOnFeature =
      GetPointOnEdge(finalEdges.front().first, 1.0 /* the end point of the feature */);

  TestRouters(startPosOnFeature, finalPosOnFeature);
}

unique_ptr<routing::IRouter> RoutingTest::CreateRouter(string const & name)
{
  vector<platform::LocalCountryFile> neededLocalFiles;
  neededLocalFiles.reserve(m_neededMaps.size());
  for (auto const & file : m_localFiles)
  {
    if (m_neededMaps.count(file.GetCountryName()) != 0)
      neededLocalFiles.push_back(file);
  }

  unique_ptr<routing::IRouter> router = integration::CreateVehicleRouter(
      m_dataSource, *m_cig, m_trafficCache, neededLocalFiles, m_type);
  return router;
}

void RoutingTest::GetNearestEdges(m2::PointD const & pt,
                                  vector<pair<routing::Edge, routing::Junction>> & edges)
{
  routing::FeaturesRoadGraph graph(m_dataSource, m_mode, CreateModelFactory());
  graph.FindClosestEdges(MercatorBounds::RectByCenterXYAndSizeInMeters(
                             pt, routing::FeaturesRoadGraph::kClosestEdgesRadiusM),
                         1 /* count */, nullptr /* isGoodFeature */, edges);
}

void TestRouter(routing::IRouter & router, m2::PointD const & startPos,
                m2::PointD const & finalPos, routing::Route & route)
{
  routing::RouterDelegate delegate;
  LOG(LINFO, ("Calculating routing ...", router.GetName()));
  base::Timer timer;
  auto const resultCode = router.CalculateRoute(routing::Checkpoints(startPos, finalPos),
                                                m2::PointD::Zero() /* startDirection */,
                                                false /* adjust */, delegate, route);
  double const elapsedSec = timer.ElapsedSeconds();
  TEST_EQUAL(routing::RouterResultCode::NoError, resultCode, ());
  TEST(route.IsValid(), ());
  m2::PolylineD const & poly = route.GetPoly();
  TEST_GREATER(poly.GetSize(), 0, ());
  TEST(base::AlmostEqualAbs(poly.Front(), startPos, routing::kPointsEqualEpsilon), ());
  TEST(base::AlmostEqualAbs(poly.Back(), finalPos, routing::kPointsEqualEpsilon), ());
  LOG(LINFO, ("Route polyline size:", route.GetPoly().GetSize()));
  LOG(LINFO, ("Route distance, meters:", route.GetTotalDistanceMeters()));
  LOG(LINFO, ("Elapsed, seconds:", elapsedSec));
}
