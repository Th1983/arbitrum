/*
 * Copyright 2020-2021, Offchain Labs, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef arbcore_hpp
#define arbcore_hpp

#include <avm/machine.hpp>
#include <avm/machinethread.hpp>
#include <avm_values/bigint.hpp>
#include <avm_values/valueloader.hpp>
#include <data_storage/combinedmachinecache.hpp>
#include <data_storage/datacursor.hpp>
#include <data_storage/datastorage.hpp>
#include <data_storage/executioncursor.hpp>
#include <data_storage/messageentry.hpp>
#include <data_storage/readsnapshottransaction.hpp>
#include <data_storage/storageresultfwd.hpp>
#include <data_storage/util.hpp>
#include <data_storage/value/code.hpp>
#include <data_storage/value/valuecache.hpp>

#include <map>
#include <memory>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <utility>
#include <vector>
#ifdef __linux__
#include <pthread.h>
#include <filesystem>
#endif

namespace rocksdb {
class TransactionDB;
class Status;
struct Slice;
class ColumnFamilyHandle;
}  // namespace rocksdb

namespace boost {
namespace filesystem {
class path;
}  // namespace filesystem
}  // namespace boost

struct InitializeResult {
    rocksdb::Status status;
    bool finished;
};

struct RawMessageInfo {
    std::vector<unsigned char> message;
    uint256_t sequence_number;
    uint256_t accumulator;

    RawMessageInfo(std::vector<unsigned char> message_,
                   uint256_t sequence_number_,
                   uint256_t accumulator_)
        : message(std::move(message_)),
          sequence_number(sequence_number_),
          accumulator(accumulator_) {}
};

struct DebugPrintCollectionOptions {
    uint256_t log_number_begin;
    uint256_t log_number_end;
};

class ArbCore {
   public:
    typedef enum {
        MESSAGES_EMPTY,    // Out: Ready to receive messages
        MESSAGES_LOADING,  // In:  Messages are being loading into vector
        MESSAGES_READY,    // In:  Messages in vector
        MESSAGES_ERROR,    // Out: Error receiving messages
    } message_status_enum;

    struct logscursor_logs {
        uint256_t first_log_index;
        std::vector<MachineEmission<Value>> logs;
        std::vector<MachineEmission<Value>> deleted_logs;
    };

   private:
    struct message_data_struct {
        uint256_t previous_message_count;
        uint256_t previous_batch_acc;
        std::vector<std::vector<unsigned char>> sequencer_batch_items;
        std::vector<std::vector<unsigned char>> delayed_messages;
        std::optional<uint256_t> reorg_batch_items;
    };

    struct ThreadDataStruct {
        ValueCache cache;
        MachineExecutionConfig execConfig;
        uint256_t begin_message;
        bool perform_pruning;
        bool perform_save_rocksdb_checkpoint;
        uint256_t next_checkpoint_gas;
        uint256_t next_basic_cache_gas;
        uint32_t add_messages_failure_count;
        uint32_t thread_failure_count;
        std::chrono::time_point<std::chrono::steady_clock>
            next_rocksdb_save_timepoint;
        std::chrono::time_point<std::chrono::steady_clock>
            profiling_begin_timepoint;
        std::chrono::time_point<std::chrono::steady_clock>
            last_messages_ready_check_timepoint;
        std::chrono::time_point<std::chrono::steady_clock>
            last_run_machine_check_timepoint;
        std::chrono::time_point<std::chrono::steady_clock>
            last_restart_machine_check_timepoint;

        ThreadDataStruct(const uint256_t& _begin_message,
                         const uint256_t& _next_checkpoint_gas,
                         const uint256_t& _next_basic_cache_gas)
            : cache(5, 0),
              execConfig(),
              begin_message(_begin_message),
              perform_pruning(false),
              perform_save_rocksdb_checkpoint(false),
              next_checkpoint_gas(_next_checkpoint_gas),
              next_basic_cache_gas(_next_basic_cache_gas),
              add_messages_failure_count(0),
              thread_failure_count(0),
              next_rocksdb_save_timepoint(),
              profiling_begin_timepoint(std::chrono::steady_clock::now()),
              last_messages_ready_check_timepoint(profiling_begin_timepoint),
              last_run_machine_check_timepoint(profiling_begin_timepoint),
              last_restart_machine_check_timepoint(profiling_begin_timepoint) {}
    };

   private:
    std::unique_ptr<std::thread> core_thread;

    ArbCoreConfig coreConfig{};

    // Core thread input
    std::mutex checkpoint_pruning_mutex;
    uint256_t unsafe_checkpoint_pruning_gas_used;

    // Core thread input
    std::atomic<bool> trigger_save_rocksdb_checkpoint{false};

    // Core thread holds mutex only during reorg.
    // Routines accessing database for log entries will need to acquire mutex
    // because obsolete log entries have `Value` references removed causing
    // reference counts to be decremented and possibly deleted.
    // No mutex required to access Sends or Messages because obsolete entries
    // are not deleted.
    std::shared_ptr<DataStorage> data_storage;

    std::unique_ptr<MachineThread> core_machine;
    std::shared_ptr<CoreCode> core_code{};

    // Machine caches
    CombinedMachineCache combined_machine_cache;

    // Core thread inbox status input/output. Core thread will update if and
    // only if set to MESSAGES_READY
    std::atomic<message_status_enum> message_data_status{MESSAGES_EMPTY};
    std::string message_data_error_string;

    // Core thread inbox input
    message_data_struct message_data;

    // Core thread inbox output
    std::atomic<bool> core_error{false};
    std::string core_error_string;

    // Core thread logs output
    std::vector<DataCursor> logs_cursors{1};

    // Core thread machine state output
    std::atomic<bool> machine_idle{false};

    std::shared_mutex last_machine_mutex;
    std::unique_ptr<Machine> last_machine;

#ifdef __linux__
    std::atomic<std::optional<pthread_t>> core_pthread;
#endif

   public:
    ArbCore() = delete;
    ArbCore(std::shared_ptr<DataStorage> data_storage_,
            ArbCoreConfig coreConfig);

    ~ArbCore() { abortThread(); }
    void printDatabaseMetadata();
    InitializeResult initialize(const LoadedExecutable& executable);
    InitializeResult applyConfig();

    [[nodiscard]] bool initialized() const;
    void operator()();

    void printCoreThreadBacktrace();

   private:
    rocksdb::Status initializePruningMode(
        bool database_exists,
        ValueResult<std::string>& pruning_mode_result);

   public:
    // Public Thread interaction
    bool startThread();
    void abortThread();

   private:
    // Private database interaction
    [[nodiscard]] ValueResult<uint256_t> schemaVersion(
        ReadTransaction& tx) const;
    rocksdb::Status updateSchemaVersion(ReadWriteTransaction& tx,
                                        const uint256_t& schema_version);
    ValueResult<std::string> pruningMode(ReadTransaction& tx) const;
    rocksdb::Status updatePruningMode(ReadWriteTransaction& tx,
                                      const std::string& pruning_mode);
    rocksdb::Status saveAssertion(ReadWriteTransaction& tx,
                                  const Assertion& assertion,
                                  uint256_t arb_gas_used);
    std::variant<rocksdb::Status, MachineStateKeys> getCheckpointUsingGas(
        ReadTransaction& tx,
        const uint256_t& total_gas);
    rocksdb::Status reorgToLastCheckpoint(ValueCache& cache);
    rocksdb::Status reorgToPenultimateCheckpoint(ValueCache& cache);
    rocksdb::Status reorgToL1Block(const uint256_t& l1_block_number,
                                   bool initial_start,
                                   ValueCache& cache);
    rocksdb::Status reorgToL2Block(const uint256_t& l2_block_number,
                                   bool initial_start,
                                   ValueCache& cache);
    rocksdb::Status reorgToLogCountOrBefore(const uint256_t& log_count,
                                            bool initial_start,
                                            ValueCache& cache);
    rocksdb::Status reorgToMessageCountOrBefore(const uint256_t& message_count,
                                                bool initial_start,
                                                ValueCache& cache);
    rocksdb::Status reorgToTimestampOrBefore(const uint256_t& timestamp,
                                             bool initial_start,
                                             ValueCache& cache);
    rocksdb::Status reorgCheckpoints(
        const std::function<bool(const MachineOutput&)>& check_output,
        bool initial_start,
        ValueCache& value_cache);

    rocksdb::Status reorgDatabaseToMachineOutput(const MachineOutput& output,
                                                 ValueCache& value_cache);
    rocksdb::Status advanceCoreToTarget(const MachineOutput& target_output,
                                        bool cache_sideloads,
                                        ValueCache& cache);
    std::variant<CheckpointVariant, rocksdb::Status>
    reorgToLastMatchingCheckpoint(
        const std::function<bool(const MachineOutput&)>& check_output,
        ReadWriteTransaction& tx,
        std::unique_ptr<rocksdb::Iterator>& checkpoint_it);
    std::variant<std::unique_ptr<MachineThread>, rocksdb::Status>
    loadLastMatchingMachine(
        const CheckpointVariant& last_matching_database_checkpoint,
        const std::function<bool(const MachineOutput&)>& check_output,
        ReadWriteTransaction& tx,
        std::unique_ptr<rocksdb::Iterator>& checkpoint_it,
        ValueCache& value_cache);

    template <class T>
    std::unique_ptr<T> getMachineUsingStateKeys(
        const ReadTransaction& transaction,
        const MachineStateKeys& state_data,
        ValueCache& value_cache,
        bool lazy_load) const;

   public:
    [[nodiscard]] ValueLoader makeValueLoader() const;

    // To be deprecated, use checkpoints instead
    template <class T>
    std::unique_ptr<T> getMachine(uint256_t machineHash,
                                  ValueCache& value_cache);

   public:
    // To trigger saving database copy
    void triggerSaveFullRocksdbCheckpointToDisk();

   private:
    template <class T>
    std::unique_ptr<T> getMachineImpl(ReadTransaction& tx,
                                      uint256_t machineHash,
                                      ValueCache& value_cache,
                                      bool lazy_load);
    rocksdb::Status saveCheckpoint(ReadWriteTransaction& tx);
    void deleteCheckpoint(ReadWriteTransaction& tx,
                          const CheckpointVariant& checkpoint_variant);
    std::unique_ptr<MachineThread> getMachineThreadFromCheckpoint(
        const CheckpointVariant& current_database_checkpoint,
        const std::function<bool(const MachineOutput&)>& check_output,
        ReadWriteTransaction& tx,
        ValueCache& value_cache);

   public:
    // Useful for unit tests
    uint256_t maxCheckpointGas();

   public:
    // Controlling checkpoint pruning
    void updateCheckpointPruningGas(uint256_t gas);

   private:
    uint256_t getCheckpointPruningGas();

   public:
    // Managing machine state
    bool machineIdle();
    std::unique_ptr<Machine> getLastMachine();
    MachineOutput getLastMachineOutput();
    uint256_t machineMessagesRead();

   public:
    // Sending messages to core thread
    bool deliverMessages(
        const uint256_t& previous_message_count,
        const uint256_t& previous_inbox_acc,
        std::vector<std::vector<unsigned char>> sequencer_batch_items,
        std::vector<std::vector<unsigned char>> delayed_messages,
        const std::optional<uint256_t>& reorg_batch_items);
    message_status_enum messagesStatus();
    std::string messagesGetError();
    bool checkError();
    std::string getErrorString();

   public:
    // Logs Cursor interaction
    bool logsCursorRequest(size_t cursor_index, uint256_t count);
    ValueResult<logscursor_logs> logsCursorGetLogs(size_t cursor_index);
    bool logsCursorConfirmReceived(size_t cursor_index);
    [[nodiscard]] ValueResult<uint256_t> logsCursorPosition(
        size_t cursor_index) const;

   private:
    // Logs cursor internal functions
    bool handleLogsCursorRequested(ReadTransaction& tx,
                                   size_t cursor_index,
                                   ValueCache& cache);
    rocksdb::Status handleLogsCursorReorg(size_t cursor_index,
                                          uint256_t log_count,
                                          ValueCache& cache);

   public:
    // Execution Cursor interaction
    ValueResult<std::unique_ptr<ExecutionCursor>> getExecutionCursor(
        uint256_t total_gas_used,
        bool allow_slow_lookup,
        uint32_t yield_instruction_count = BASE_YIELD_INSTRUCTION_COUNT);
    rocksdb::Status advanceExecutionCursor(
        ExecutionCursor& execution_cursor,
        uint256_t max_gas,
        bool go_over_gas,
        bool allow_slow_lookup,
        uint32_t yield_instruction_count = BASE_YIELD_INSTRUCTION_COUNT);
    ValueResult<std::vector<MachineEmission<Value>>>
    advanceExecutionCursorWithTracing(
        ExecutionCursor& execution_cursor,
        uint256_t max_gas,
        bool go_over_gas,
        bool allow_slow_lookup,
        const DebugPrintCollectionOptions& options,
        uint32_t yield_instruction_count = BASE_YIELD_INSTRUCTION_COUNT);

    std::unique_ptr<Machine> takeExecutionCursorMachine(
        ExecutionCursor& execution_cursor);

   private:
    // Execution cursor internal functions
    ValueResult<std::vector<MachineEmission<Value>>> advanceExecutionCursorImpl(
        ExecutionCursor& execution_cursor,
        uint256_t total_gas_used,
        bool go_over_gas,
        size_t message_group_size,
        bool allow_slow_lookup,
        const std::optional<DebugPrintCollectionOptions>& collectionOptions,
        uint32_t yield_instruction_count);

    std::unique_ptr<Machine>& resolveExecutionCursorMachine(
        const ReadTransaction& tx,
        ExecutionCursor& execution_cursor);
    std::unique_ptr<Machine> takeExecutionCursorMachineImpl(
        const ReadTransaction& tx,
        ExecutionCursor& execution_cursor);

   public:
    [[nodiscard]] ValueResult<uint256_t> logInsertedCount() const;
    [[nodiscard]] ValueResult<uint256_t> sendInsertedCount() const;
    [[nodiscard]] ValueResult<uint256_t> messageEntryInsertedCount() const;
    [[nodiscard]] ValueResult<uint256_t> delayedMessageEntryInsertedCount()
        const;
    [[nodiscard]] ValueResult<uint256_t> totalDelayedMessagesSequenced() const;
    ValueResult<std::vector<MachineEmission<Value>>>
    getLogs(uint256_t index, uint256_t count, ValueCache& valueCache);
    [[nodiscard]] ValueResult<std::vector<std::vector<unsigned char>>> getSends(
        uint256_t index,
        uint256_t count) const;

    [[nodiscard]] ValueResult<std::vector<std::vector<unsigned char>>>
    getMessages(uint256_t index, uint256_t count) const;
    [[nodiscard]] ValueResult<std::vector<std::vector<unsigned char>>>
    getSequencerBatchItems(uint256_t index) const;
    [[nodiscard]] ValueResult<uint256_t> getSequencerBlockNumberAt(
        uint256_t sequence_number) const;
    [[nodiscard]] ValueResult<std::vector<unsigned char>> genInboxProof(
        uint256_t seq_num,
        uint256_t batch_index,
        uint256_t batch_end_count) const;

    ValueResult<uint256_t> getInboxAcc(uint256_t index);
    ValueResult<uint256_t> getDelayedInboxAcc(uint256_t index);
    ValueResult<uint256_t> getDelayedInboxAccImpl(const ReadTransaction& tx,
                                                  uint256_t index);
    ValueResult<std::pair<uint256_t, uint256_t>> getInboxAccPair(
        uint256_t index1,
        uint256_t index2);

    [[nodiscard]] ValueResult<size_t> countMatchingBatchAccs(
        std::vector<std::pair<uint256_t, uint256_t>> seq_nums_and_accs) const;

    [[nodiscard]] ValueResult<uint256_t> getDelayedMessagesToSequence(
        uint256_t max_block_number) const;

   private:
    [[nodiscard]] ValueResult<std::vector<RawMessageInfo>> getMessagesImpl(
        const ReadConsistentTransaction& tx,
        uint256_t index,
        uint256_t count,
        std::optional<uint256_t> start_acc) const;
    [[nodiscard]] ValueResult<uint256_t> getNextSequencerBatchItemAccumulator(
        const ReadTransaction& tx,
        uint256_t sequence_number) const;

    // Private database interaction
    [[nodiscard]] ValueResult<uint256_t> logInsertedCountImpl(
        const ReadTransaction& tx) const;

    [[nodiscard]] ValueResult<uint256_t> sendInsertedCountImpl(
        const ReadTransaction& tx) const;

    [[nodiscard]] ValueResult<uint256_t> messageEntryInsertedCountImpl(
        const ReadTransaction& tx) const;
    [[nodiscard]] ValueResult<uint256_t> delayedMessageEntryInsertedCountImpl(
        const ReadTransaction& tx) const;
    [[nodiscard]] ValueResult<uint256_t> totalDelayedMessagesSequencedImpl(
        const ReadTransaction& tx) const;

    rocksdb::Status saveLogs(ReadWriteTransaction& tx,
                             const std::vector<MachineEmission<Value>>& val);
    rocksdb::Status saveSends(
        ReadWriteTransaction& tx,
        const std::vector<MachineEmission<std::vector<unsigned char>>>& sends);

   private:
    ValueResult<std::optional<uint256_t>> addMessages(
        const message_data_struct& data,
        ValueCache& cache);
    ValueResult<std::vector<MachineEmission<Value>>> getLogsNoLock(
        ReadTransaction& tx,
        uint256_t index,
        uint256_t count,
        ValueCache& valueCache);

    [[nodiscard]] ValueResult<std::vector<MachineMessage>> readNextMessages(
        const ReadConsistentTransaction& tx,
        const InboxState& fully_processed_inbox,
        size_t count) const;

    [[nodiscard]] bool isValid(const ReadTransaction& tx,
                               const InboxState& fully_processed_inbox) const;
    std::variant<rocksdb::Status, ExecutionCursor> findCloserExecutionCursor(
        ReadTransaction& tx,
        std::optional<ExecutionCursor> execution_cursor,
        uint256_t& total_gas_used,
        bool allow_slow_lookup);

    rocksdb::Status updateLogInsertedCount(ReadWriteTransaction& tx,
                                           const uint256_t& log_index);
    rocksdb::Status updateSendInsertedCount(ReadWriteTransaction& tx,
                                            const uint256_t& send_index);
    bool runCoreMachineWithMessages(MachineExecutionConfig& execConfig,
                                    size_t max_message_batch_size,
                                    bool asynchronous);

   public:
    // Public sideload interaction
    std::variant<rocksdb::Status, ExecutionCursor>
    getExecutionCursorAtEndOfBlock(
        const uint256_t& block_number,
        bool allow_slow_lookup,
        uint32_t yield_instruction_count = BASE_YIELD_INSTRUCTION_COUNT);

    ValueResult<uint256_t> getGasAtBlock(ReadTransaction& tx,
                                         const uint256_t& block_number);

   private:
    // Private sideload interaction
    rocksdb::Status saveSideloadPosition(ReadWriteTransaction& tx,
                                         const uint256_t& block_number,
                                         const uint256_t& arb_gas_used);

    rocksdb::Status deleteSideloadsStartingAt(ReadWriteTransaction& tx,
                                              const uint256_t& block_number);
    rocksdb::Status logsCursorSaveCurrentTotalCount(ReadWriteTransaction& tx,
                                                    size_t cursor_index,
                                                    uint256_t count);
    [[nodiscard]] ValueResult<uint256_t> logsCursorGetCurrentTotalCount(
        const ReadTransaction& tx,
        size_t cursor_index) const;
    rocksdb::Status pruneCheckpoints(
        const std::function<bool(const MachineOutput&)>& check_output,
        uint64_t checkpoint_max_to_prune);
    rocksdb::Status pruneToTimestampOrBefore(const uint256_t& timestamp,
                                             uint64_t checkpoint_max_to_prune);
    rocksdb::Status pruneToGasOrBefore(const uint256_t& gas,
                                       uint64_t checkpoint_max_to_prune);

   private:
    void printElapsed(const std::chrono::time_point<std::chrono::steady_clock>&
                          begin_timepoint,
                      const std::string& message) const;
    uint64_t countCheckpoints(ReadTransaction& tx);
    std::variant<rocksdb::Status, CheckpointVariant> getCheckpointNumber(
        ReadTransaction& tx,
        uint256_t& number);
    std::variant<rocksdb::Status, CheckpointVariant> getLastCheckpoint(
        ReadTransaction& tx);
    void saveRocksdbCheckpoint(const boost::filesystem::path& save_rocksdb_path,
                               ReadTransaction& tx);
    void setCoreError(const std::string& message);
    bool threadBody(ThreadDataStruct& thread_data);
    bool reorgIfInvalidMachine(uint32_t& thread_failure_count,
                               uint256_t& next_checkpoint_gas,
                               ValueCache& cache);
};

uint64_t seconds_since_epoch();

std::optional<rocksdb::Status> deleteLogsStartingAt(ReadWriteTransaction& tx,
                                                    uint256_t log_index);

std::string optionalUint256ToString(std::optional<uint256_t>& value);

#endif /* arbcore_hpp */
