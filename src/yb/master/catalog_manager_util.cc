// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include "yb/master/catalog_manager_util.h"

#include <gflags/gflags.h>

#include "yb/util/flag_tags.h"
#include "yb/util/logging.h"
#include "yb/util/math_util.h"

DEFINE_double(balancer_load_max_standard_deviation, 2.0,
              "The standard deviation among the tserver load, above which that distribution "
              "is considered not balanced.");
TAG_FLAG(balancer_load_max_standard_deviation, advanced);

namespace yb {
namespace master {

using strings::Substitute;

Status CatalogManagerUtil::IsLoadBalanced(const master::TSDescriptorVector& ts_descs) {
  ZoneToDescMap zone_to_ts;
  RETURN_NOT_OK(GetPerZoneTSDesc(ts_descs, &zone_to_ts));

  for (const auto& zone : zone_to_ts) {
    if (zone.second.size() <= 1) {
      continue;
    }

    // Map from placement uuid to tserver load vector.
    std::map<string, vector<double>> load;
    for (const auto &ts_desc : zone.second) {
      (load[ts_desc->placement_uuid()]).push_back(ts_desc->num_live_replicas());
    }

    for (const auto& entry : load) {
      double std_dev = yb::standard_deviation(entry.second);
      LOG(INFO) << "Load standard deviation is " << std_dev << " for "
                << entry.second.size() << " tservers in placement " << zone.first
                << " for placement uuid " << entry.first;

      if (std_dev >= FLAGS_balancer_load_max_standard_deviation) {
        return STATUS(IllegalState, Substitute("Load not balanced: deviation=$0 in $1 for "
                                               "placement uuid $2.",
                                               std_dev, zone.first, entry.first));
      }
    }
  }
  return Status::OK();
}

Status CatalogManagerUtil::AreLeadersOnPreferredOnly(
    const TSDescriptorVector& ts_descs,
    const ReplicationInfoPB& replication_info,
    const vector<scoped_refptr<TableInfo>>& tables) {
  if (PREDICT_FALSE(ts_descs.empty())) {
    return Status::OK();
  }

  // Variables for checking transaction leader spread.
  auto num_servers = ts_descs.size();
  std::map<std::string, int> txn_map;
  int num_txn_tablets = 0;
  int max_txn_leaders_per_node = 0;
  int min_txn_leaders_per_node = 0;

  if (!FLAGS_transaction_tables_use_preferred_zones) {
    CalculateTxnLeaderMap(&txn_map, &num_txn_tablets, tables);
    max_txn_leaders_per_node = num_txn_tablets / num_servers;
    min_txn_leaders_per_node = max_txn_leaders_per_node;
    if (num_txn_tablets % num_servers) {
      ++max_txn_leaders_per_node;
    }
  }

  // If transaction_tables_use_preferred_zones = true, don't check for transaction leader spread.
  // This results in txn_map being empty, num_txn_tablets = 0, max_txn_leaders_per_node = 0, and
  // system_tablets_leaders = 0.
  // Thus all comparisons for transaction leader spread will be ignored (all 0 < 0, etc).

  for (const auto& ts_desc : ts_descs) {
    auto tserver = txn_map.find(ts_desc->permanent_uuid());
    int system_tablets_leaders = 0;
    if (!(tserver == txn_map.end())) {
      system_tablets_leaders = tserver->second;
    }

    // If enabled, check if transaction tablet leaders are evenly spread.
    if (system_tablets_leaders > max_txn_leaders_per_node) {
      return STATUS(
          IllegalState,
          Substitute("Too many txn status leaders found on tserver $0. Found $1, Expected $2.",
                      ts_desc->permanent_uuid(),
                      system_tablets_leaders,
                      max_txn_leaders_per_node));
    }
    if (system_tablets_leaders < min_txn_leaders_per_node) {
      return STATUS(
          IllegalState,
          Substitute("Tserver $0 expected to have at least $1 txn status leader(s), but has $2.",
                      ts_desc->permanent_uuid(),
                      min_txn_leaders_per_node,
                      system_tablets_leaders));
    }

    // Check that leaders are on preferred ts only.
    // If transaction tables follow preferred nodes, then we verify that there are 0 leaders.
    // Otherwise, we need to check that there are 0 non-txn leaders on the ts.
    if (!ts_desc->IsAcceptingLeaderLoad(replication_info) &&
        ts_desc->leader_count() > system_tablets_leaders) {
      // This is a ts that shouldn't have leader load (asides from txn leaders) but does.
      return STATUS(
          IllegalState,
          Substitute("Expected no leader load on tserver $0, found $1.",
                     ts_desc->permanent_uuid(), ts_desc->leader_count() - system_tablets_leaders));
    }
  }
  return Status::OK();
}

void CatalogManagerUtil::CalculateTxnLeaderMap(std::map<std::string, int>* txn_map,
                                               int* num_txn_tablets,
                                               vector<scoped_refptr<TableInfo>> tables) {
  for (const auto& table : tables) {
    bool is_txn_table = table->GetTableType() == TRANSACTION_STATUS_TABLE_TYPE;
    if (!is_txn_table) {
      continue;
    }
    TabletInfos tablets;
    table->GetAllTablets(&tablets);
    (*num_txn_tablets) += tablets.size();
    for (const auto& tablet : tablets) {
      TabletInfo::ReplicaMap replication_locations;
      tablet->GetReplicaLocations(&replication_locations);
      for (const auto& replica : replication_locations) {
        if (replica.second.role == consensus::RaftPeerPB_Role_LEADER) {
          (*txn_map)[replica.first]++;
        }
      }
    }
  }
}

Status CatalogManagerUtil::GetPerZoneTSDesc(const TSDescriptorVector& ts_descs,
                                            ZoneToDescMap* zone_to_ts) {
  if (zone_to_ts == nullptr) {
    return STATUS(InvalidArgument, "Need a non-null zone to tsdesc map that will be filled in.");
  }
  zone_to_ts->clear();
  for (const auto& ts_desc : ts_descs) {
    string placement_id = ts_desc->placement_id();
    auto iter = zone_to_ts->find(placement_id);
    if (iter == zone_to_ts->end()) {
      (*zone_to_ts)[placement_id] = {ts_desc};
    } else {
      iter->second.push_back(ts_desc);
    }
  }
  return Status::OK();
}

Status CatalogManagerUtil::DoesPlacementInfoContainCloudInfo(const PlacementInfoPB& placement_info,
                                                             const CloudInfoPB& cloud_info) {
  const string& cloud_info_string = TSDescriptor::generate_placement_id(cloud_info);
  for (const auto& placement_block : placement_info.placement_blocks()) {
    if (TSDescriptor::generate_placement_id(placement_block.cloud_info()) == cloud_info_string) {
      return Status::OK();
    }
  }
  return STATUS_SUBSTITUTE(InvalidArgument, "Placement info $0 does not contain cloud info $1",
                           placement_info.DebugString(), cloud_info_string);
}

} // namespace master
} // namespace yb
