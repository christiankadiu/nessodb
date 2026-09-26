#include "transaction/transaction_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

using nessodb::common::TransactionId;
using nessodb::common::TableId;
using nessodb::transaction::TransactionErrorCode;
using nessodb::transaction::TransactionHandle;
using nessodb::transaction::TransactionManager;
using nessodb::transaction::TransactionState;
using nessodb::transaction::LockError;
using nessodb::transaction::LockErrorCode;
using nessodb::transaction::LockMode;
using nessodb::transaction::TransactionError;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void test_manager_assigns_ids_and_finalizes_transactions() {
    TransactionManager manager;
    auto first = manager.begin();
    auto second = manager.begin();
    expect(first.has_value() && second.has_value(),
           "the manager begins transactions");
    if (!first || !second) {
        return;
    }

    expect((*first)->id() == TransactionId{1} &&
               (*second)->id() == TransactionId{2},
           "transaction IDs are monotonic");
    expect(manager.active_transaction_count() == 2,
           "the manager tracks active transactions");

    expect(manager.commit(*first).has_value(),
           "the manager commits an active transaction");
    expect((*first)->state() == TransactionState::committed &&
               manager.active_transaction_count() == 1,
           "commit finalizes and removes the transaction from the active set");

    const auto repeated_commit = manager.commit(*first);
    expect(!repeated_commit && repeated_commit.error().code ==
                                   TransactionErrorCode::transaction_not_active,
           "a committed transaction cannot commit again");

    expect(manager.rollback(*second).has_value(),
           "the manager rolls back an active transaction");
    expect((*second)->state() == TransactionState::aborted &&
               manager.active_transaction_count() == 0,
           "rollback aborts and removes the transaction from the active set");
}

void test_manager_rejects_foreign_handles() {
    TransactionManager owner;
    TransactionManager other;
    auto started = owner.begin();
    expect(started.has_value(), "the owner begins a transaction");
    if (!started) {
        return;
    }

    const auto foreign_commit = other.commit(*started);
    expect(!foreign_commit && foreign_commit.error().code ==
                                  TransactionErrorCode::transaction_not_owned,
           "a manager rejects another manager's transaction");

    const TransactionHandle empty;
    const auto empty_rollback = owner.rollback(empty);
    expect(!empty_rollback && empty_rollback.error().code ==
                                  TransactionErrorCode::transaction_not_owned,
           "a manager rejects an empty transaction handle");
    expect(owner.rollback(*started).has_value(),
           "the owning manager can still roll back its transaction");
}

void test_manager_aborts_active_transactions_on_destruction() {
    TransactionHandle transaction;
    {
        TransactionManager manager;
        auto started = manager.begin();
        expect(started.has_value(), "a transaction starts before shutdown");
        if (!started) {
            return;
        }
        transaction = *started;
    }

    expect(transaction->state() == TransactionState::aborted,
           "manager destruction aborts transactions that remain active");
}

void test_manager_is_safe_for_concurrent_connections() {
    constexpr std::size_t connection_count = 8;
    TransactionManager manager;
    std::mutex handles_mutex;
    std::vector<TransactionHandle> handles;
    std::vector<std::thread> connections;
    handles.reserve(connection_count);
    connections.reserve(connection_count);

    for (std::size_t index = 0; index < connection_count; ++index) {
        connections.emplace_back([&] {
            auto started = manager.begin();
            if (started) {
                const std::scoped_lock lock{handles_mutex};
                handles.push_back(*started);
            }
        });
    }
    for (auto& connection : connections) {
        connection.join();
    }

    std::vector<std::uint64_t> ids;
    ids.reserve(handles.size());
    for (const auto& handle : handles) {
        ids.push_back(handle->id().value);
    }
    std::ranges::sort(ids);
    bool ids_are_monotonic = ids.size() == connection_count;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        ids_are_monotonic =
            ids_are_monotonic &&
            ids[index] == static_cast<std::uint64_t>(index + 1);
    }
    expect(ids_are_monotonic,
           "concurrent connections receive distinct monotonic IDs");

    connections.clear();
    std::atomic<std::size_t> rollback_failures{};
    for (const auto& handle : handles) {
        connections.emplace_back([&manager, &rollback_failures, handle] {
            if (!manager.rollback(handle)) {
                ++rollback_failures;
            }
        });
    }
    for (auto& connection : connections) {
        connection.join();
    }
    expect(rollback_failures == 0 && manager.active_transaction_count() == 0,
           "concurrent rollback finalizes every active transaction");
}

void test_table_lock_compatibility_and_release() {
    TransactionManager manager;
    auto first = manager.begin();
    auto second = manager.begin();
    expect(first.has_value() && second.has_value(),
           "transactions start for table locking");
    if (!first || !second) {
        return;
    }

    const TableId table_id{9};
    expect(manager.acquire_table_lock(*first, table_id, LockMode::shared)
                   .has_value() &&
               manager.acquire_table_lock(*second, table_id, LockMode::shared)
                   .has_value(),
           "multiple transactions can share a table lock");

    const auto conflicting_upgrade =
        manager.acquire_table_lock(*first, table_id, LockMode::exclusive);
    const auto* conflict = conflicting_upgrade
                               ? nullptr
                               : std::get_if<LockError>(
                                     &conflicting_upgrade.error());
    expect(conflict != nullptr &&
               conflict->code == LockErrorCode::lock_conflict &&
               conflict->table_id == table_id,
           "an exclusive upgrade conflicts with another shared holder");

    expect(manager.commit(*second).has_value(),
           "committing releases the second transaction's locks");
    expect(manager.acquire_table_lock(*first, table_id, LockMode::exclusive)
               .has_value(),
           "a sole shared holder can upgrade to exclusive");

    auto third = manager.begin();
    expect(third.has_value(), "a third transaction starts");
    if (!third) {
        return;
    }
    const auto blocked_reader =
        manager.acquire_table_lock(*third, table_id, LockMode::shared);
    expect(!blocked_reader &&
               std::holds_alternative<LockError>(blocked_reader.error()),
           "an exclusive holder blocks other readers");

    expect(manager.rollback(*first).has_value(),
           "rollback releases the exclusive table lock");
    expect(manager.acquire_table_lock(*third, table_id, LockMode::shared)
               .has_value(),
           "a table can be locked after the previous holder rolls back");
    expect(manager.rollback(*third).has_value(),
           "the remaining transaction rolls back");
}

void test_table_lock_validation() {
    TransactionManager owner;
    TransactionManager other;
    auto started = owner.begin();
    expect(started.has_value(), "a transaction starts for lock validation");
    if (!started) {
        return;
    }

    const auto foreign = other.acquire_table_lock(
        *started, TableId{1}, LockMode::shared);
    expect(!foreign &&
               std::holds_alternative<TransactionError>(foreign.error()),
           "a manager rejects locks for a foreign transaction");

    const auto invalid_table = owner.acquire_table_lock(
        *started, TableId{}, LockMode::shared);
    const auto* error = invalid_table
                            ? nullptr
                            : std::get_if<LockError>(&invalid_table.error());
    expect(error != nullptr &&
               error->code == LockErrorCode::invalid_table_id,
           "a lock requires a valid table ID");
    expect(owner.rollback(*started).has_value(),
           "the validation transaction rolls back");
}

}  // namespace

int main() {
    test_manager_assigns_ids_and_finalizes_transactions();
    test_manager_rejects_foreign_handles();
    test_manager_aborts_active_transactions_on_destruction();
    test_manager_is_safe_for_concurrent_connections();
    test_table_lock_compatibility_and_release();
    test_table_lock_validation();

    if (failures != 0) {
        std::cerr << failures << " transaction assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
