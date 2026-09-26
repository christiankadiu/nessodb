#include "recovery/wal.hpp"

#include "storage/page/checksum.hpp"
#include "storage/page/codec.hpp"
#include "storage/page/page_header.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace minidb::recovery {
namespace {

inline constexpr std::uint32_t wal_magic = 0x4c41574e;
inline constexpr std::uint32_t wal_record_magic = 0x524c574e;
inline constexpr std::size_t wal_header_size = 32;
inline constexpr std::size_t record_header_size = 48;
inline constexpr std::size_t header_checksum_offset = 24;
inline constexpr std::size_t record_checksum_offset = 12;
inline constexpr std::uint16_t begin_record = 1;
inline constexpr std::uint16_t page_update_record = 2;
inline constexpr std::uint16_t commit_record = 3;
inline constexpr std::uint16_t abort_record = 4;
inline constexpr std::uint16_t checkpoint_record = 5;
inline constexpr std::uint32_t before_image_present = 1;

using WalHeader = std::array<std::byte, wal_header_size>;

struct ParsedRecord {
    std::uint16_t type;
    std::uint64_t lsn;
    common::TransactionId transaction_id;
    std::uint64_t value;
    std::optional<storage::PageBuffer> before;
    std::optional<storage::PageBuffer> after;
};

struct RecoveryTransaction {
    enum class State {
        active,
        committed,
        aborted,
    };

    std::uint64_t initial_page_count{};
    std::vector<const ParsedRecord*> updates;
    State state{State::active};
};

std::uint32_t checksum_with_zeroed_field(
    std::span<const std::byte> bytes, std::size_t offset) {
    std::vector<std::byte> copy(bytes.begin(), bytes.end());
    std::fill_n(copy.begin() + static_cast<std::ptrdiff_t>(offset),
                sizeof(std::uint32_t), std::byte{0});
    return storage::crc32c(copy);
}

WalHeader encode_header(std::uint64_t next_lsn) {
    WalHeader header{};
    (void)storage::write_u32(header, 0, wal_magic);
    (void)storage::write_u16(header, 4, wal_format_version);
    (void)storage::write_u16(
        header, 6, static_cast<std::uint16_t>(wal_header_size));
    (void)storage::write_u64(header, 8, next_lsn);
    (void)storage::write_u64(header, 16, 0);
    const auto checksum = checksum_with_zeroed_field(
        header, header_checksum_offset);
    (void)storage::write_u32(header, header_checksum_offset, checksum);
    (void)storage::write_u32(header, 28, 0);
    return header;
}

std::expected<std::uint64_t, WalError> decode_header(
    std::span<const std::byte> header) {
    if (header.size() != wal_header_size ||
        *storage::read_u32(header, 0) != wal_magic ||
        *storage::read_u16(header, 6) != wal_header_size ||
        *storage::read_u32(header, 28) != 0) {
        return std::unexpected(WalError::invalid_header);
    }
    if (*storage::read_u16(header, 4) != wal_format_version) {
        return std::unexpected(WalError::unsupported_version);
    }
    const auto stored_checksum =
        *storage::read_u32(header, header_checksum_offset);
    if (stored_checksum != checksum_with_zeroed_field(
                               header, header_checksum_offset)) {
        return std::unexpected(WalError::checksum_mismatch);
    }
    const auto next_lsn = *storage::read_u64(header, 8);
    if (next_lsn == 0) {
        return std::unexpected(WalError::invalid_header);
    }
    return next_lsn;
}

std::expected<void, WalError> flush_stream(std::fstream& stream) {
    stream.flush();
    if (!stream) {
        return std::unexpected(WalError::flush_failed);
    }
    return {};
}

std::expected<void, WalError> set_page_lsn(
    storage::PageBuffer& page, common::PageId page_id,
    std::uint64_t lsn) {
    auto header = storage::decode_page_header(page);
    if (!header || header->page_id != page_id) {
        return std::unexpected(WalError::invalid_record);
    }
    header->log_sequence_number = lsn;
    if (!storage::encode_page_header(page, *header) ||
        !storage::update_page_checksum(page)) {
        return std::unexpected(WalError::invalid_record);
    }
    return {};
}

bool page_matches_record(const storage::PageBuffer& page,
                         common::PageId page_id,
                         std::optional<std::uint64_t> lsn = std::nullopt) {
    const auto header = storage::decode_page_header(page);
    return header && header->page_id == page_id &&
           (!lsn || header->log_sequence_number == *lsn);
}

std::expected<std::vector<ParsedRecord>, WalError> read_records(
    std::fstream& stream, bool& truncated_tail,
    std::uint64_t& maximum_lsn) {
    stream.clear();
    stream.seekg(0, std::ios::end);
    if (!stream) {
        return std::unexpected(WalError::read_failed);
    }
    const auto end = stream.tellg();
    if (end < static_cast<std::streamoff>(wal_header_size)) {
        return std::unexpected(WalError::invalid_header);
    }
    const auto file_size = static_cast<std::uint64_t>(end);
    std::uint64_t offset = wal_header_size;
    std::vector<ParsedRecord> records;
    maximum_lsn = 0;

    while (offset < file_size) {
        if (file_size - offset < record_header_size) {
            truncated_tail = true;
            break;
        }
        std::array<std::byte, record_header_size> header{};
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char*>(header.data()),
                    static_cast<std::streamsize>(header.size()));
        if (!stream) {
            return std::unexpected(WalError::read_failed);
        }
        if (*storage::read_u32(header, 0) != wal_record_magic ||
            *storage::read_u16(header, 4) != wal_format_version ||
            *storage::read_u32(header, 44) != 0) {
            return std::unexpected(WalError::invalid_record);
        }
        const auto type = *storage::read_u16(header, 6);
        const auto total_size = *storage::read_u32(header, 8);
        if (total_size < record_header_size) {
            return std::unexpected(WalError::invalid_record);
        }
        if (total_size > file_size - offset) {
            truncated_tail = true;
            break;
        }
        std::vector<std::byte> encoded(total_size);
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset));
        stream.read(reinterpret_cast<char*>(encoded.data()),
                    static_cast<std::streamsize>(encoded.size()));
        if (!stream) {
            return std::unexpected(WalError::read_failed);
        }
        const auto checksum =
            *storage::read_u32(encoded, record_checksum_offset);
        if (checksum != checksum_with_zeroed_field(
                            encoded, record_checksum_offset)) {
            return std::unexpected(WalError::checksum_mismatch);
        }

        const auto lsn = *storage::read_u64(encoded, 16);
        const common::TransactionId transaction_id{
            *storage::read_u64(encoded, 24)};
        const auto value = *storage::read_u64(encoded, 32);
        const auto flags = *storage::read_u32(encoded, 40);
        if (lsn == 0 || lsn <= maximum_lsn || (flags & ~1U) != 0) {
            return std::unexpected(WalError::invalid_record);
        }
        maximum_lsn = lsn;

        ParsedRecord record{type, lsn, transaction_id, value,
                            std::nullopt, std::nullopt};
        const auto payload_size =
            static_cast<std::size_t>(total_size) - record_header_size;
        if (type == page_update_record) {
            const bool has_before = (flags & before_image_present) != 0;
            const auto expected_payload = storage::page_size *
                static_cast<std::size_t>(has_before ? 2 : 1);
            if (!transaction_id.is_valid() || !common::PageId{value}.is_valid() ||
                payload_size != expected_payload) {
                return std::unexpected(WalError::invalid_record);
            }
            std::size_t payload_offset = record_header_size;
            if (has_before) {
                storage::PageBuffer before{};
                std::memcpy(before.data(), encoded.data() + payload_offset,
                            storage::page_size);
                record.before = std::move(before);
                payload_offset += storage::page_size;
            }
            storage::PageBuffer after{};
            std::memcpy(after.data(), encoded.data() + payload_offset,
                        storage::page_size);
            record.after = std::move(after);
            const common::PageId page_id{value};
            if (!page_matches_record(*record.after, page_id, lsn) ||
                (record.before &&
                 !page_matches_record(*record.before, page_id))) {
                return std::unexpected(WalError::invalid_record);
            }
        } else if (type == begin_record &&
                   (!transaction_id.is_valid() || value == 0 ||
                    payload_size != 0 || flags != 0)) {
            return std::unexpected(WalError::invalid_record);
        } else if ((type == commit_record || type == abort_record) &&
                   (!transaction_id.is_valid() || value != 0 ||
                    payload_size != 0 || flags != 0)) {
            return std::unexpected(WalError::invalid_record);
        } else if (type == checkpoint_record &&
                   (transaction_id.is_valid() || payload_size != 0 ||
                    flags != 0 || value != 0)) {
            return std::unexpected(WalError::invalid_record);
        } else if (type < begin_record || type > checkpoint_record) {
            return std::unexpected(WalError::invalid_record);
        }
        records.push_back(std::move(record));
        offset += total_size;
    }
    return records;
}

}  // namespace

std::filesystem::path wal_path_for(
    const std::filesystem::path& database_path) {
    auto path = database_path;
    path += ".wal";
    return path;
}

std::expected<WriteAheadLog, WalError> WriteAheadLog::create(
    const std::filesystem::path& database_path) {
    if (database_path.empty()) {
        return std::unexpected(WalError::empty_path);
    }
    const auto path = wal_path_for(database_path);
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        return std::unexpected(WalError::create_failed);
    }
    if (status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected(WalError::create_failed);
    }
    std::fstream stream(path, std::ios::binary | std::ios::in |
                                  std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        return std::unexpected(WalError::create_failed);
    }
    const auto header = encode_header(1);
    stream.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    if (!stream || !flush_stream(stream)) {
        return std::unexpected(WalError::write_failed);
    }
    return WriteAheadLog{path, std::move(stream), 1};
}

std::expected<WriteAheadLog, WalError> WriteAheadLog::open(
    const std::filesystem::path& database_path) {
    if (database_path.empty()) {
        return std::unexpected(WalError::empty_path);
    }
    const auto path = wal_path_for(database_path);
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
        return create(database_path);
    }
    if (status_error || status.type() != std::filesystem::file_type::regular) {
        return std::unexpected(WalError::open_failed);
    }
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!stream.is_open()) {
        return std::unexpected(WalError::open_failed);
    }
    WalHeader header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    if (!stream) {
        return std::unexpected(WalError::read_failed);
    }
    auto next_lsn = decode_header(header);
    if (!next_lsn) {
        return std::unexpected(next_lsn.error());
    }
    return WriteAheadLog{path, std::move(stream), *next_lsn};
}

WriteAheadLog::WriteAheadLog(std::filesystem::path path,
                             std::fstream stream,
                             std::uint64_t next_lsn) noexcept
    : path_(std::move(path)), stream_(std::move(stream)),
      next_lsn_(next_lsn) {}

const std::filesystem::path& WriteAheadLog::path() const noexcept {
    return path_;
}

bool WriteAheadLog::has_active_transaction() const noexcept {
    return active_transaction_.has_value();
}

std::expected<void, WalError> WriteAheadLog::begin(
    common::TransactionId transaction_id,
    std::uint64_t initial_page_count) {
    if (active_transaction_) {
        return std::unexpected(WalError::transaction_already_active);
    }
    if (!transaction_id.is_valid() || initial_page_count == 0) {
        return std::unexpected(WalError::invalid_record);
    }
    auto appended = append_record(
        begin_record, transaction_id, initial_page_count, 0);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    active_transaction_ = ActiveTransaction{
        transaction_id, initial_page_count};
    return {};
}

std::expected<LoggedPage, WalError> WriteAheadLog::log_page_update(
    common::PageId page_id,
    std::optional<storage::PageBuffer> before,
    storage::PageBuffer after) {
    if (!active_transaction_) {
        return std::unexpected(WalError::no_active_transaction);
    }
    if (!page_id.is_valid()) {
        return std::unexpected(WalError::invalid_record);
    }
    if (next_lsn_ == 0 ||
        next_lsn_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(WalError::log_sequence_number_exhausted);
    }
    const auto lsn = next_lsn_;
    auto lsn_set = set_page_lsn(after, page_id, lsn);
    if (!lsn_set) {
        return std::unexpected(lsn_set.error());
    }

    std::vector<std::byte> payload;
    payload.reserve(storage::page_size * (before ? 2U : 1U));
    if (before) {
        payload.insert(payload.end(), before->begin(), before->end());
    }
    payload.insert(payload.end(), after.begin(), after.end());
    auto appended = append_record(
        page_update_record, active_transaction_->id, page_id.value,
        before ? before_image_present : 0, payload);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    return LoggedPage{std::move(after), *appended};
}

std::expected<void, WalError> WriteAheadLog::commit(
    common::TransactionId transaction_id) {
    if (!active_transaction_) {
        return std::unexpected(WalError::no_active_transaction);
    }
    if (active_transaction_->id != transaction_id) {
        return std::unexpected(WalError::transaction_mismatch);
    }
    auto appended = append_record(commit_record, transaction_id, 0, 0);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    active_transaction_.reset();
    return {};
}

std::expected<void, WalError> WriteAheadLog::abort(
    common::TransactionId transaction_id) {
    if (!active_transaction_) {
        return std::unexpected(WalError::no_active_transaction);
    }
    if (active_transaction_->id != transaction_id) {
        return std::unexpected(WalError::transaction_mismatch);
    }
    auto appended = append_record(abort_record, transaction_id, 0, 0);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    active_transaction_.reset();
    return {};
}

std::expected<void, WalError> WriteAheadLog::checkpoint() {
    if (active_transaction_) {
        return std::unexpected(WalError::transaction_already_active);
    }
    auto appended = append_record(
        checkpoint_record, common::TransactionId{}, 0, 0);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    return rewrite_empty();
}

std::expected<std::uint64_t, WalError> WriteAheadLog::append_record(
    std::uint16_t type, common::TransactionId transaction_id,
    std::uint64_t value, std::uint32_t flags,
    std::span<const std::byte> payload) {
    if (next_lsn_ == 0 ||
        next_lsn_ == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(WalError::log_sequence_number_exhausted);
    }
    if (payload.size() >
        std::numeric_limits<std::uint32_t>::max() - record_header_size) {
        return std::unexpected(WalError::invalid_record);
    }
    const auto total_size = record_header_size + payload.size();
    std::vector<std::byte> encoded(total_size);
    (void)storage::write_u32(encoded, 0, wal_record_magic);
    (void)storage::write_u16(encoded, 4, wal_format_version);
    (void)storage::write_u16(encoded, 6, type);
    (void)storage::write_u32(
        encoded, 8, static_cast<std::uint32_t>(total_size));
    (void)storage::write_u64(encoded, 16, next_lsn_);
    (void)storage::write_u64(encoded, 24, transaction_id.value);
    (void)storage::write_u64(encoded, 32, value);
    (void)storage::write_u32(encoded, 40, flags);
    (void)storage::write_u32(encoded, 44, 0);
    std::copy(payload.begin(), payload.end(),
              encoded.begin() + static_cast<std::ptrdiff_t>(record_header_size));
    const auto checksum = checksum_with_zeroed_field(
        encoded, record_checksum_offset);
    (void)storage::write_u32(encoded, record_checksum_offset, checksum);

    stream_.clear();
    stream_.seekp(0, std::ios::end);
    stream_.write(reinterpret_cast<const char*>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
    if (!stream_) {
        return std::unexpected(WalError::write_failed);
    }
    auto flushed = flush_stream(stream_);
    if (!flushed) {
        return std::unexpected(flushed.error());
    }
    const auto appended_lsn = next_lsn_;
    ++next_lsn_;
    return appended_lsn;
}

std::expected<RecoveryResult, WalError> WriteAheadLog::recover(
    storage::PageFile& page_file) {
    if (active_transaction_) {
        return std::unexpected(WalError::transaction_already_active);
    }
    bool truncated_tail = false;
    std::uint64_t maximum_lsn = 0;
    auto records = read_records(
        stream_, truncated_tail, maximum_lsn);
    if (!records) {
        return std::unexpected(records.error());
    }
    if (maximum_lsn >= next_lsn_) {
        if (maximum_lsn == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(
                WalError::log_sequence_number_exhausted);
        }
        next_lsn_ = maximum_lsn + 1;
    }

    std::unordered_map<std::uint64_t, RecoveryTransaction> transactions;
    for (const auto& record : *records) {
        if (record.type == checkpoint_record) {
            transactions.clear();
            continue;
        }
        if (record.type == begin_record) {
            if (transactions.contains(record.transaction_id.value)) {
                return std::unexpected(WalError::invalid_record);
            }
            transactions.emplace(
                record.transaction_id.value,
                RecoveryTransaction{
                    record.value, {}, RecoveryTransaction::State::active});
            continue;
        }
        const auto transaction =
            transactions.find(record.transaction_id.value);
        if (transaction == transactions.end() ||
            transaction->second.state != RecoveryTransaction::State::active) {
            return std::unexpected(WalError::invalid_record);
        }
        if (record.type == page_update_record) {
            transaction->second.updates.push_back(&record);
        } else if (record.type == commit_record) {
            transaction->second.state =
                RecoveryTransaction::State::committed;
        } else {
            transaction->second.state =
                RecoveryTransaction::State::aborted;
        }
    }

    RecoveryResult result;
    result.records_scanned = records->size();
    result.truncated_tail = truncated_tail;
    for (const auto& record : *records) {
        if (record.type != page_update_record) {
            continue;
        }
        const auto transaction = transactions.find(
            record.transaction_id.value);
        if (transaction == transactions.end() ||
            transaction->second.state !=
                RecoveryTransaction::State::committed) {
            continue;
        }
        auto written = page_file.write_page(
            common::PageId{record.value}, *record.after);
        if (!written) {
            return std::unexpected(WalError::data_file_recovery_failed);
        }
        ++result.pages_redone;
    }
    for (const auto& [transaction_id, transaction] : transactions) {
        (void)transaction_id;
        if (transaction.state != RecoveryTransaction::State::active) {
            continue;
        }
        for (auto update = transaction.updates.rbegin();
             update != transaction.updates.rend(); ++update) {
            if (!(*update)->before) {
                continue;
            }
            auto written = page_file.write_page(
                common::PageId{(*update)->value}, *(*update)->before);
            if (!written) {
                return std::unexpected(
                    WalError::data_file_recovery_failed);
            }
            ++result.pages_undone;
        }
        auto truncated = page_file.truncate_pages(
            transaction.initial_page_count);
        if (!truncated) {
            return std::unexpected(WalError::data_file_recovery_failed);
        }
    }
    if (!page_file.flush()) {
        return std::unexpected(WalError::data_file_recovery_failed);
    }
    auto emptied = rewrite_empty();
    if (!emptied) {
        return std::unexpected(emptied.error());
    }
    return result;
}

std::expected<void, WalError> WriteAheadLog::rewrite_empty() {
    stream_.close();
    std::fstream stream(path_, std::ios::binary | std::ios::in |
                                  std::ios::out | std::ios::trunc);
    if (!stream.is_open()) {
        return std::unexpected(WalError::open_failed);
    }
    const auto header = encode_header(next_lsn_);
    stream.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    if (!stream) {
        return std::unexpected(WalError::write_failed);
    }
    auto flushed = flush_stream(stream);
    if (!flushed) {
        return std::unexpected(flushed.error());
    }
    stream_ = std::move(stream);
    return {};
}

}  // namespace minidb::recovery
