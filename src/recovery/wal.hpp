#pragma once

#include "common/page_id.hpp"
#include "common/transaction_id.hpp"
#include "storage/io/page_file.hpp"
#include "storage/page/page.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>

namespace nessodb::recovery {

inline constexpr std::uint16_t wal_format_version = 1;

enum class WalError {
    empty_path,
    create_failed,
    open_failed,
    read_failed,
    write_failed,
    flush_failed,
    invalid_header,
    unsupported_version,
    invalid_record,
    checksum_mismatch,
    transaction_already_active,
    no_active_transaction,
    transaction_mismatch,
    log_sequence_number_exhausted,
    data_file_recovery_failed,
};

struct LoggedPage {
    storage::PageBuffer page;
    std::uint64_t log_sequence_number;
};

struct RecoveryResult {
    std::size_t records_scanned{};
    std::size_t pages_redone{};
    std::size_t pages_undone{};
    bool truncated_tail{};
};

class WriteAheadLog {
public:
    [[nodiscard]] static std::expected<WriteAheadLog, WalError> create(
        const std::filesystem::path& database_path);
    [[nodiscard]] static std::expected<WriteAheadLog, WalError> open(
        const std::filesystem::path& database_path);

    WriteAheadLog(const WriteAheadLog&) = delete;
    WriteAheadLog& operator=(const WriteAheadLog&) = delete;
    WriteAheadLog(WriteAheadLog&&) noexcept = default;
    WriteAheadLog& operator=(WriteAheadLog&&) noexcept = default;
    ~WriteAheadLog() = default;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] bool has_active_transaction() const noexcept;
    [[nodiscard]] std::expected<void, WalError> begin(
        common::TransactionId transaction_id,
        std::uint64_t initial_page_count);
    [[nodiscard]] std::expected<LoggedPage, WalError> log_page_update(
        common::PageId page_id,
        std::optional<storage::PageBuffer> before,
        storage::PageBuffer after);
    [[nodiscard]] std::expected<void, WalError> commit(
        common::TransactionId transaction_id);
    [[nodiscard]] std::expected<void, WalError> abort(
        common::TransactionId transaction_id);
    [[nodiscard]] std::expected<void, WalError> checkpoint();
    [[nodiscard]] std::expected<RecoveryResult, WalError> recover(
        storage::PageFile& page_file);

private:
    struct ActiveTransaction {
        common::TransactionId id;
        std::uint64_t initial_page_count;
    };

    WriteAheadLog(std::filesystem::path path, std::fstream stream,
                  std::uint64_t next_lsn) noexcept;

    [[nodiscard]] std::expected<std::uint64_t, WalError> append_record(
        std::uint16_t type, common::TransactionId transaction_id,
        std::uint64_t value, std::uint32_t flags,
        std::span<const std::byte> payload = {});
    [[nodiscard]] std::expected<void, WalError> rewrite_empty();

    std::filesystem::path path_;
    std::fstream stream_;
    std::uint64_t next_lsn_{1};
    std::optional<ActiveTransaction> active_transaction_;
};

[[nodiscard]] std::filesystem::path wal_path_for(
    const std::filesystem::path& database_path);

}  // namespace nessodb::recovery
