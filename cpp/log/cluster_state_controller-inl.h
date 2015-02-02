#ifndef CERT_TRANS_LOG_CLUSTER_STATE_CONTROLLER_INL_H_
#define CERT_TRANS_LOG_CLUSTER_STATE_CONTROLLER_INL_H_

#include "log/cluster_state_controller.h"

#include <functional>

#include "log/etcd_consistent_store-inl.h"
#include "proto/ct.pb.h"

namespace cert_trans {


template <class Logged>
ClusterStateController<Logged>::ClusterStateController(
    util::Executor* executor, ConsistentStore<Logged>* store,
    MasterElection* election)
    : store_(CHECK_NOTNULL(store)),
      election_(CHECK_NOTNULL(election)),
      watch_config_task_(CHECK_NOTNULL(executor)),
      watch_node_states_task_(CHECK_NOTNULL(executor)),
      watch_serving_sth_task_(CHECK_NOTNULL(executor)),
      exiting_(false),
      update_required_(false),
      cluster_serving_sth_update_thread_(
          std::bind(&ClusterStateController<Logged>::ClusterServingSTHUpdater,
                    this)) {
  store_->WatchClusterNodeStates(
      std::bind(&ClusterStateController::OnClusterStateUpdated, this,
                std::placeholders::_1),
      watch_node_states_task_.task());
  store_->WatchClusterConfig(
      std::bind(&ClusterStateController::OnClusterConfigUpdated, this,
                std::placeholders::_1),
      watch_config_task_.task());
  store_->WatchServingSTH(
      std::bind(&ClusterStateController::OnServingSthUpdated, this,
                std::placeholders::_1),
      watch_serving_sth_task_.task());
}


template <class Logged>
ClusterStateController<Logged>::~ClusterStateController() {
  watch_config_task_.Cancel();
  watch_node_states_task_.Cancel();
  watch_serving_sth_task_.Cancel();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    exiting_ = true;
  }
  update_required_cv_.notify_all();
  cluster_serving_sth_update_thread_.join();
  watch_config_task_.Wait();
  watch_node_states_task_.Wait();
  watch_serving_sth_task_.Wait();
}


template <class Logged>
void ClusterStateController<Logged>::NewTreeHead(
    const ct::SignedTreeHead& sth) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (local_node_state_.has_newest_sth()) {
    CHECK_GE(sth.timestamp(), local_node_state_.newest_sth().timestamp());
  }
  local_node_state_.mutable_newest_sth()->CopyFrom(sth);
  PushLocalNodeState(lock);
}


template <class Logged>
void ClusterStateController<Logged>::ContiguousTreeSizeUpdated(
    const int64_t new_contiguous_tree_size) {
  CHECK_GE(new_contiguous_tree_size, 0);
  std::unique_lock<std::mutex> lock(mutex_);
  CHECK_GE(new_contiguous_tree_size, local_node_state_.contiguous_tree_size());
  local_node_state_.set_contiguous_tree_size(new_contiguous_tree_size);
  PushLocalNodeState(lock);
}


template <class Logged>
util::StatusOr<ct::SignedTreeHead>
ClusterStateController<Logged>::GetCalculatedServingSTH() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!calculated_serving_sth_) {
    return util::StatusOr<ct::SignedTreeHead>(
        util::Status(util::error::NOT_FOUND, "No calculated STH"));
  }
  return util::StatusOr<ct::SignedTreeHead>(*calculated_serving_sth_);
}


template <class Logged>
void ClusterStateController<Logged>::GetLocalNodeState(
    ct::ClusterNodeState* state) const {
  CHECK_NOTNULL(state);
  std::lock_guard<std::mutex> lock(mutex_);
  *state = local_node_state_;
}


template <class Logged>
void ClusterStateController<Logged>::SetNodeHostPort(const std::string& host,
                                                     const uint16_t port) {
  std::unique_lock<std::mutex> lock(mutex_);
  local_node_state_.set_hostname(host);
  local_node_state_.set_log_port(port);
  PushLocalNodeState(lock);
}


template <class Logged>
void ClusterStateController<Logged>::PushLocalNodeState(
    const std::unique_lock<std::mutex>& lock) {
  CHECK(lock.owns_lock());
  // Our new node state may affect our ability to become master (e.g. perhaps
  // we've caught up on our replication), so check and join if appropriate:
  DetermineElectionParticipation(lock);

  const util::Status status(store_->SetClusterNodeState(local_node_state_));
  if (!status.ok()) {
    LOG(WARNING) << status;
  }
}


template <class Logged>
void ClusterStateController<Logged>::OnClusterStateUpdated(
    const std::vector<Update<ct::ClusterNodeState>>& updates) {
  std::unique_lock<std::mutex> lock(mutex_);
  for (const auto& update : updates) {
    const std::string& node_id(update.handle_.Entry().node_id());
    if (update.exists_) {
      all_node_states_[node_id] = update.handle_.Entry();
    } else {
      CHECK_EQ(1, all_node_states_.erase(node_id));
    }
  }

  CalculateServingSTH(lock);
}


template <class Logged>
void ClusterStateController<Logged>::OnClusterConfigUpdated(
    const Update<ct::ClusterConfig>& update) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!update.exists_) {
    LOG(WARNING) << "No ClusterConfig exists.";
    return;
  }

  cluster_config_ = update.handle_.Entry();
  LOG(INFO) << "Received new ClusterConfig:\n"
            << cluster_config_.DebugString();

  // May need to re-calculate the servingSTH since the ClusterConfig has
  // changed:
  CalculateServingSTH(lock);
}


template <class Logged>
void ClusterStateController<Logged>::OnServingSthUpdated(
    const Update<ct::SignedTreeHead>& update) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!update.exists_) {
    LOG(WARNING) << "Cluster has no Serving STH!";
    actual_serving_sth_.reset();
  } else {
    actual_serving_sth_.reset(new ct::SignedTreeHead(update.handle_.Entry()));
    LOG(INFO) << "Received new Serving STH:\n"
              << actual_serving_sth_->DebugString();
  }

  // This could affect our ability to produce new STHs, so better check
  // whether we should leave the election for now:
  DetermineElectionParticipation(lock);
}


template <class Logged>
void ClusterStateController<Logged>::CalculateServingSTH(
    const std::unique_lock<std::mutex>& lock) {
  VLOG(1) << "Calculating new ServingSTH...";
  CHECK(lock.owns_lock());

  // First, create a mapping of tree size to number of nodes at that size, and
  // a mapping of the newst STH for any given size:
  std::map<int64_t, ct::SignedTreeHead> sth_by_size;
  std::map<int64_t, int> num_nodes_by_sth_size;
  for (const auto& node : all_node_states_) {
    if (node.second.has_newest_sth()) {
      const int64_t tree_size(node.second.newest_sth().tree_size());
      CHECK_LE(0, tree_size);
      num_nodes_by_sth_size[tree_size]++;
      // Default timestamp (first call in here) will be 0
      if (node.second.newest_sth().timestamp() >
          sth_by_size[tree_size].timestamp()) {
        sth_by_size[tree_size] = node.second.newest_sth();
      }
    }
  }

  // Next calculate the newest STH we've seen which satisfies the following
  // criteria:
  //   - at least minimum_serving_nodes have an STH at least as large
  //   - at least minimum_serving_fraction have an STH at least as large
  //   - not smaller than the current serving STH
  int num_nodes_seen(0);
  const int current_tree_size(
      calculated_serving_sth_ ? calculated_serving_sth_->tree_size() : 0);
  CHECK_LE(0, current_tree_size);

  // Work backwards (from largest STH size) until we see that there's enough
  // coverage (according to the criteria above) to serve an STH (or determine
  // that there are insufficient nodes to serve anything.)
  for (auto it = num_nodes_by_sth_size.rbegin();
       it != num_nodes_by_sth_size.rend() && it->first >= current_tree_size;
       ++it) {
    // num_nodes_seen keeps track of the number of nodes we've seen so far (and
    // since we're working from larger to smaller size STH, they should all be
    // able to serve this [and smaller] STHs.)
    num_nodes_seen += it->second;
    const double serving_fraction(static_cast<double>(num_nodes_seen) /
                                  all_node_states_.size());
    if (serving_fraction >= cluster_config_.minimum_serving_fraction() &&
        num_nodes_seen >= cluster_config_.minimum_serving_nodes()) {
      LOG(INFO) << "Can serve @" << it->first << " with " << num_nodes_seen
                << " nodes (" << (serving_fraction * 100) << "% of cluster)";
      calculated_serving_sth_.reset(
          new ct::SignedTreeHead(sth_by_size[it->first]));
      // Push this STH out to the cluster if we're master:
      if (election_->IsMaster()) {
        update_required_ = true;
        update_required_cv_.notify_all();
      }
      return;
    }
  }
  // TODO(alcutter): Add a mechanism to take the cluster off-line until we have
  // sufficient nodes able to serve.
  LOG(WARNING) << "Failed to determine suitable serving STH.";
}


template <class Logged>
void ClusterStateController<Logged>::DetermineElectionParticipation(
    const std::unique_lock<std::mutex>& lock) {
  CHECK(lock.owns_lock());
  // Can't be in the election if the cluster isn't properly initialised
  if (!actual_serving_sth_) {
    LOG(WARNING) << "Cluster has no Serving STH - leaving election.";
    election_->StopElection();
    return;
  }

  // Don't want to be the master if we don't yet have the data to be able to
  // issue new STHs
  if (actual_serving_sth_->tree_size() >
      local_node_state_.contiguous_tree_size()) {
    LOG(INFO) << "Serving STH tree_size (" << actual_serving_sth_->tree_size()
              << " < Local contiguous_tree_size ("
              << local_node_state_.contiguous_tree_size() << ")";
    LOG(INFO) << "Local replication too far behind to be master - "
                 "leaving election.";
    election_->StopElection();
    return;
  }

  // Otherwise, make sure we're joining in the election.
  election_->StartElection();
}


// Thread entry point for cluster_serving_sth_update_thread_.
template <class Logged>
void ClusterStateController<Logged>::ClusterServingSTHUpdater() {
  while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    update_required_cv_.wait(lock, [this]() {
      return update_required_ || exiting_;
    });
    if (exiting_) {
      return;
    }
    CHECK(update_required_);
    CHECK_NOTNULL(calculated_serving_sth_.get());
    const ct::SignedTreeHead local_sth(*calculated_serving_sth_);

    update_required_ = false;

    // And then release it before we send the update.
    // This allows any other code to get on with modifying
    // calculated_serving_sth_ in response to cluster state changes
    lock.unlock();

    if (election_->IsMaster()) {
      store_->SetServingSTH(local_sth);
    }
  }
}


}  // namespace cert_trans


#endif  // CERT_TRANS_LOG_CLUSTER_STATE_CONTROLLER_INL_H_
