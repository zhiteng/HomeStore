/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <sisl/fds/buffer.hpp>
#include <sisl/fds/stream_tracker.hpp>
#include <folly/Synchronized.h>
#include <nlohmann/json.hpp>

#include <homestore/logstore/log_store_internal.hpp>

namespace homestore {

class LogDev;
class LogStoreServiceMetrics;

static constexpr logstore_seq_num_t invalid_lsn() { return std::numeric_limits< logstore_seq_num_t >::min(); }
typedef std::function< void(logstore_seq_num_t) > on_rollback_cb_t;

class HomeLogStore : public std::enable_shared_from_this< HomeLogStore > {
public:
    HomeLogStore(std::shared_ptr< LogDev > logdev, logstore_id_t id, bool append_mode, logstore_seq_num_t start_lsn);
    HomeLogStore(const HomeLogStore&) = delete;
    HomeLogStore(HomeLogStore&&) noexcept = delete;
    HomeLogStore& operator=(const HomeLogStore&) = delete;
    HomeLogStore& operator=(HomeLogStore&&) noexcept = delete;
    ~HomeLogStore() = default;

    /**
     * @brief Register default request completion callback. In case every write does not carry a callback, this
     * callback will be used to report completions.
     * @param cb
     */
    void register_req_comp_cb(const log_req_comp_cb_t& cb) { m_comp_cb = cb; }

    /**
     * @brief Register callback upon a new log entry is found during recovery. Failing to register for log_found
     * callback is ok as long as log entries are not required to replayed during recovery.
     *
     * @param cb
     */
    void register_log_found_cb(const log_found_cb_t& cb) { m_found_cb = cb; }

    /**
     * @brief Register callback to indicate the replay is done during recovery. Failing to register for log_replay
     * callback is ok as long as user of the log store knows when all logs are replayed.
     *
     * @param cb
     */
    void register_log_replay_done_cb(const log_replay_done_cb_t& cb) { m_replay_done_cb = cb; }

    /**
     * @brief Register callback to indicate the replay is done during recovery. Failing to register for log_replay
     * callback is ok as long as user of the log store knows when all logs are replayed.
     *
     * @param cb
     */
    log_replay_done_cb_t get_log_replay_done_cb() const { return m_replay_done_cb; }

    /**
     * @brief Write the blob at the user specified seq number in sync manner. Under the covers it will call async
     * write and then wait for its completion. As such this is much lesser performing than async version since it
     * involves mutex/cv combination
     *
     * @param seq_num : Sequence number to insert data
     * @param b : Data blob to write to log
     *
     * @return is write completed successfully.
     */
    bool write_sync(logstore_seq_num_t seq_num, const sisl::io_blob& b);

    /**
     * @brief Write the blob at the user specified seq number - prepared as a request in async fashion.
     *
     * @param req The fully formed request which has the seqnum and data blob already prepared.
     * @param cb [OPTIONAL] Callback if caller wants specific callback as against common/default callback registed.
     * The callback returns the request back with status of execution
     */
    void write_async(logstore_req* req, const log_req_comp_cb_t& cb = nullptr);

    /**
     * @brief Write the blob at the user specified seq number
     *
     * @param seq_num: Seq number to write to
     * @param b : Blob of data
     * @param cookie : Any cookie or context which will passed back in the callback
     * @param cb Callback upon completion which is called with the status, seq_num and cookie that was passed.
     */
    void write_async(logstore_seq_num_t seq_num, const sisl::io_blob& b, void* cookie, const log_write_comp_cb_t& cb);

    /**
     * @brief This method appends the blob into the log and it returns the generated seq number
     *
     * @param b Blob of data to append
     * @return logstore_seq_num_t Returns the seqnum generated by the log
     */
    // This method is not implemented yet
    logstore_seq_num_t append_sync(const sisl::io_blob& b);

    /**
     * @brief This method appends the blob into the log and makes a callback at the end of the append.
     *
     * @param b Blob of data to append
     * @param cookie Passed as is to the completion callback
     * @param completion_cb Completion callback which contains the seqnum, status and cookie
     * @return internally generated sequence number
     */
    logstore_seq_num_t append_async(const sisl::io_blob& b, void* cookie, const log_write_comp_cb_t& completion_cb);

    /**
     * @brief Read the log provided the sequence number synchronously. This is not the most efficient way to read
     * as reader will be blocked until read is completed. In addition, it is built on-top of async system by doing
     * a single mutex/cv pair (which adds some cost)
     *
     * Throws: std::out_of_range exception if seq_num is already truncated or never inserted before
     *
     * @param seq_num
     * @return log_buffer Returned log_buffer (which is a safe smart ptr) that contains the data blob.
     */
    log_buffer read_sync(logstore_seq_num_t seq_num);

    /**
     * @brief Read the log based on the logstore_req prepared. In case callback is supplied, it uses the callback
     * to provide the data it has read. If not overridden, use default callback registered during initialization.
     *
     * @param req Request containing seq_num
     * @param cb [OPTIONAL] Callback to get the data back, if it needs to be different from the default registered
     * one.
     */
    void read_async(logstore_req* req, const log_found_cb_t& cb = nullptr);

    /**
     * @brief Read the log for the seq_num and make the callback with the data
     *
     * @param seq_num Seqnumber to read the log from
     * @param cookie Any cookie or context which will passed back in the callback
     * @param cb Callback which contains seq_num, cookie and
     */
    void read_async(logstore_seq_num_t seq_num, void* cookie, const log_found_cb_t& cb);

    /**
     * @brief Truncate the logs for this log store upto the seq_num provided (inclusive). Once truncated, the reads
     * on seq_num <= upto_seq_num will return an error. The truncation in general is a 2 step process, where first
     * in-memory structure of the logs are truncated and then logdevice actual space is truncated.
     *
     * @param upto_seq_num: Seq num upto which logs are to be truncated
     * @param in_memory_truncate_only If set to false, it will force to truncate the device right away. Its better
     * to set this to true on cases where there are multiple log stores, so that once all in-memory truncation is
     * completed, a device truncation can be triggered for all the logstores. The device truncation is more
     * expensive and grouping them together yields better results.
     * @return number of records to truncate
     */
    void truncate(logstore_seq_num_t upto_seq_num, bool in_memory_truncate_only = true);

    /**
     * @brief Fill the gap in the seq_num with a dummy value. This ensures that get_contiguous_issued and completed
     * seq_num methods move forward. The filled data is not readable and any attempt to read this seq_num will
     * result in out_of_range exception.
     *
     * @param seq_num: Seq_num to fill to.
     */
    void fill_gap(logstore_seq_num_t seq_num);

    /**
     * @brief Get the safe truncation log dev key from this log store perspective. Please note that the safe idx is
     * not globally safe, but it is safe from this log store perspective only. To get global safe id, one should
     * access all log stores and get the minimum of them before truncating.
     *
     * It could return invalid logdev_key which indicates that this log store does not have any valid logdev key
     * to truncate. This could happen when there were no ios on this logstore since last truncation or at least no
     * ios are flushed yet. The caller should simply ignore this return value.
     *
     * @return truncation_entry_t: Which contains the logdev key and its corresponding seq_num to truncate and also
     * is that entry represents the entire log store.
     */
    // truncation_entry_t get_safe_truncation_boundary() const;

    /**
     * @brief Get the last truncated seqnum upto which we have truncated. If called after recovery, it returns the
     * first seq_num it has seen-1.
     *
     * @return logstore_seq_num_t
     */
    logstore_seq_num_t truncated_upto() const {
        const auto ts{m_safe_truncation_boundary.seq_num.load(std::memory_order_acquire)};
        return (ts == std::numeric_limits< logstore_seq_num_t >::max()) ? -1 : ts;
    }

    /**
     * @brief iterator to get all the log buffers;
     *
     * @param start_idx  idx to start with;
     * @param cb called with current idx and log buffer.
     * Return value of the cb: true means proceed, false means stop;
     */
    void foreach (int64_t start_idx, const std::function< bool(logstore_seq_num_t, log_buffer) >& cb);

    /**
     * @brief Get the store id of this HomeLogStore
     *
     * @return logstore_id_t
     */
    logstore_id_t get_store_id() const { return m_store_id; }

    /**
     * @brief Get the next contiguous seq num which are already issued from the given start seq number.
     *
     * @param from The seqnum from which contiguous search begins (exclusive). In other words, if from is say 5, it
     * looks for contiguous seq number from 6 and ignores 5.
     * @return logstore_seq_num_t Returns upto the seqnum upto which contiguous number is issued (inclusive). If it
     * is same as input `from`, then there are no more new contiguous issued.
     */
    logstore_seq_num_t get_contiguous_issued_seq_num(logstore_seq_num_t from) const;

    /**
     * @brief Get the next contiguous seq num which are already completed from the given start seq number.
     *
     * @param from The seqnum from which contiguous search begins (exclusive). In other words, if from is say 5, it
     * looks for contiguous seq number from 6 and ignores 5.
     * @return logstore_seq_num_t Returns upto the seqnum upto which contiguous number is completed (inclusive). If
     * it is same as input `from`, then there are no more new contiguous completed.
     */
    logstore_seq_num_t get_contiguous_completed_seq_num(logstore_seq_num_t from) const;

    /**
     * @brief Flush this log store (write/sync to disk) up to the sequence number
     *
     * @param seq_num Sequence number upto which logs are to be flushed. If not provided, will wait to flush all seq
     * numbers issued prior.
     * @return True on success
     */
    void flush_sync(logstore_seq_num_t upto_seq_num = invalid_lsn());

    /**
     * @brief Rollback the given instance to the given sequence number
     *
     * @param seq_num Sequence number back which logs are to be rollbacked
     * @return True on success
     */
    uint64_t rollback_async(logstore_seq_num_t to_lsn, on_rollback_cb_t cb);

    auto seq_num() const { return m_seq_num.load(std::memory_order_acquire); }

    std::shared_ptr< LogDev > get_logdev() { return m_logdev; }

    nlohmann::json dump_log_store(const log_dump_req& dump_req = log_dump_req());

    nlohmann::json get_status(int verbosity) const;

    const truncation_info& pre_device_truncation();
    void post_device_truncation(const logdev_key& trunc_upto_key);
    void on_write_completion(logstore_req* req, const logdev_key& ld_key);
    void on_read_completion(logstore_req* req, const logdev_key& ld_key);
    void on_log_found(logstore_seq_num_t seq_num, const logdev_key& ld_key, const logdev_key& flush_ld_key,
                      log_buffer buf);
    void on_batch_completion(const logdev_key& flush_batch_ld_key);

private:
    void do_truncate(logstore_seq_num_t upto_seq_num);
    int search_max_le(logstore_seq_num_t input_sn);

    logstore_id_t m_store_id;
    std::shared_ptr< LogDev > m_logdev;
    sisl::StreamTracker< logstore_record > m_records;
    bool m_append_mode{false};
    log_req_comp_cb_t m_comp_cb;
    log_found_cb_t m_found_cb;
    log_replay_done_cb_t m_replay_done_cb;
    std::atomic< logstore_seq_num_t > m_seq_num;
    std::string m_fq_name;
    LogStoreServiceMetrics& m_metrics;

    // seq_ld_key_pair m_flush_batch_max = {-1, {0, 0}}; // The maximum seqnum we have seen in the prev flushed
    // batch
    logstore_seq_num_t m_flush_batch_max_lsn{std::numeric_limits< logstore_seq_num_t >::min()};

    // Sync flush sections
    std::atomic< logstore_seq_num_t > m_sync_flush_waiter_lsn{invalid_lsn()};
    std::mutex m_sync_flush_mtx;
    std::condition_variable m_sync_flush_cv;

    std::vector< seq_ld_key_pair > m_truncation_barriers; // List of truncation barriers
    truncation_info m_safe_truncation_boundary;
};
} // namespace homestore
