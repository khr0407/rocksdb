//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
#include "db/db_impl/db_impl.h"

#include <cinttypes>

#include "db/builder.h"
#include "db/error_handler.h"
#include "env/composite_env_wrapper.h"
#include "file/read_write_util.h"
#include "file/sst_file_manager_impl.h"
#include "file/writable_file_writer.h"
#include "monitoring/persistent_stats_history.h"
#include "options/options_helper.h"
#include "rocksdb/wal_filter.h"
#include "table/block_based/block_based_table_factory.h"
#include "test_util/sync_point.h"
#include "util/rate_limiter.h"

#include "lemma.h"

namespace rocksdb {
Options SanitizeOptions(const std::string& dbname, const Options& src) {
  auto db_options = SanitizeOptions(dbname, DBOptions(src));
  ImmutableDBOptions immutable_db_options(db_options);
  auto cf_options =
      SanitizeOptions(immutable_db_options, ColumnFamilyOptions(src));
  return Options(db_options, cf_options);
}

DBOptions SanitizeOptions(const std::string& dbname, const DBOptions& src) {
  DBOptions result(src);

  if (result.file_system == nullptr) {
    if (result.env == Env::Default()) {
      result.file_system = FileSystem::Default();
    } else {
      result.file_system.reset(new LegacyFileSystemWrapper(result.env));
    }
  } else {
    if (result.env == nullptr) {
      result.env = Env::Default();
    }
  }

  // result.max_open_files means an "infinite" open files.
  if (result.max_open_files != -1) {
    int max_max_open_files = port::GetMaxOpenFiles();
    if (max_max_open_files == -1) {
      max_max_open_files = 0x400000;
    }
    ClipToRange(&result.max_open_files, 20, max_max_open_files);
    TEST_SYNC_POINT_CALLBACK("SanitizeOptions::AfterChangeMaxOpenFiles",
                             &result.max_open_files);
  }

  if (result.info_log == nullptr) {
    Status s = CreateLoggerFromOptions(dbname, result, &result.info_log);
    if (!s.ok()) {
      // No place suitable for logging
      result.info_log = nullptr;
    }
  }

  if (!result.write_buffer_manager) {
    result.write_buffer_manager.reset(
        new WriteBufferManager(result.db_write_buffer_size));
  }
  auto bg_job_limits = DBImpl::GetBGJobLimits(
      result.max_background_flushes, result.max_background_compactions,
      result.max_background_jobs, true /* parallelize_compactions */);
  result.env->IncBackgroundThreadsIfNeeded(bg_job_limits.max_compactions,
                                           Env::Priority::LOW);
  result.env->IncBackgroundThreadsIfNeeded(bg_job_limits.max_flushes,
                                           Env::Priority::HIGH);

  if (result.rate_limiter.get() != nullptr) {
    if (result.bytes_per_sync == 0) {
      result.bytes_per_sync = 1024 * 1024;
    }
  }

  if (result.delayed_write_rate == 0) {
    if (result.rate_limiter.get() != nullptr) {
      result.delayed_write_rate = result.rate_limiter->GetBytesPerSecond();
    }
    if (result.delayed_write_rate == 0) {
      result.delayed_write_rate = 16 * 1024 * 1024;
    }
  }

  if (result.WAL_ttl_seconds > 0 || result.WAL_size_limit_MB > 0) {
    result.recycle_log_file_num = false;
  }

  if (result.recycle_log_file_num &&
      (result.wal_recovery_mode == WALRecoveryMode::kPointInTimeRecovery ||
       result.wal_recovery_mode == WALRecoveryMode::kAbsoluteConsistency)) {
    // kPointInTimeRecovery is inconsistent with recycle log file feature since
    // we define the "end" of the log as the first corrupt record we encounter.
    // kAbsoluteConsistency doesn't make sense because even a clean
    // shutdown leaves old junk at the end of the log file.
    result.recycle_log_file_num = 0;
  }

  if (result.wal_dir.empty()) {
    // Use dbname as default
    result.wal_dir = dbname;
  }
  if (result.wal_dir.back() == '/') {
    result.wal_dir = result.wal_dir.substr(0, result.wal_dir.size() - 1);
  }

  if (result.db_paths.size() == 0) {
    result.db_paths.emplace_back(dbname, std::numeric_limits<uint64_t>::max());
  }

  if (result.use_direct_reads && result.compaction_readahead_size == 0) {
    TEST_SYNC_POINT_CALLBACK("SanitizeOptions:direct_io", nullptr);
    result.compaction_readahead_size = 1024 * 1024 * 2;
  }

  if (result.compaction_readahead_size > 0 || result.use_direct_reads) {
    result.new_table_reader_for_compaction_inputs = true;
  }

  // Force flush on DB open if 2PC is enabled, since with 2PC we have no
  // guarantee that consecutive log files have consecutive sequence id, which
  // make recovery complicated.
  if (result.allow_2pc) {
    result.avoid_flush_during_recovery = false;
  }

#ifndef ROCKSDB_LITE
  ImmutableDBOptions immutable_db_options(result);
  if (!IsWalDirSameAsDBPath(&immutable_db_options)) {
    // Either the WAL dir and db_paths[0]/db_name are not the same, or we
    // cannot tell for sure. In either case, assume they're different and
    // explicitly cleanup the trash log files (bypass DeleteScheduler)
    // Do this first so even if we end up calling
    // DeleteScheduler::CleanupDirectory on the same dir later, it will be
    // safe
    std::vector<std::string> filenames;
    result.env->GetChildren(result.wal_dir, &filenames);
    for (std::string& filename : filenames) {
      if (filename.find(".log.trash", filename.length() -
                                          std::string(".log.trash").length()) !=
          std::string::npos) {
        std::string trash_file = result.wal_dir + "/" + filename;
        result.env->DeleteFile(trash_file);
      }
    }
  }
  // When the DB is stopped, it's possible that there are some .trash files that
  // were not deleted yet, when we open the DB we will find these .trash files
  // and schedule them to be deleted (or delete immediately if SstFileManager
  // was not used)
  auto sfm = static_cast<SstFileManagerImpl*>(result.sst_file_manager.get());
  for (size_t i = 0; i < result.db_paths.size(); i++) {
    DeleteScheduler::CleanupDirectory(result.env, sfm, result.db_paths[i].path);
  }

  // Create a default SstFileManager for purposes of tracking compaction size
  // and facilitating recovery from out of space errors.
  if (result.sst_file_manager.get() == nullptr) {
    std::shared_ptr<SstFileManager> sst_file_manager(
        NewSstFileManager(result.env, result.info_log));
    result.sst_file_manager = sst_file_manager;
  }
#endif

  if (!result.paranoid_checks) {
    result.skip_checking_sst_file_sizes_on_db_open = true;
    ROCKS_LOG_INFO(result.info_log,
                   "file size check will be skipped during open.");
  }

  return result;
}

namespace {
Status SanitizeOptionsByTable(
    const DBOptions& db_opts,
    const std::vector<ColumnFamilyDescriptor>& column_families) {
  Status s;
  for (auto cf : column_families) {
    s = cf.options.table_factory->SanitizeOptions(db_opts, cf.options);
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}
}  // namespace

Status DBImpl::ValidateOptions(
    const DBOptions& db_options,
    const std::vector<ColumnFamilyDescriptor>& column_families) {
  Status s;
  for (auto& cfd : column_families) {
    s = ColumnFamilyData::ValidateOptions(db_options, cfd.options);
    if (!s.ok()) {
      return s;
    }
  }
  s = ValidateOptions(db_options);
  return s;
}

Status DBImpl::ValidateOptions(const DBOptions& db_options) {
  if (db_options.db_paths.size() > 4) {
    return Status::NotSupported(
        "More than four DB paths are not supported yet. ");
  }

  if (db_options.allow_mmap_reads && db_options.use_direct_reads) {
    // Protect against assert in PosixMMapReadableFile constructor
    return Status::NotSupported(
        "If memory mapped reads (allow_mmap_reads) are enabled "
        "then direct I/O reads (use_direct_reads) must be disabled. ");
  }

  if (db_options.allow_mmap_writes &&
      db_options.use_direct_io_for_flush_and_compaction) {
    return Status::NotSupported(
        "If memory mapped writes (allow_mmap_writes) are enabled "
        "then direct I/O writes (use_direct_io_for_flush_and_compaction) must "
        "be disabled. ");
  }

  if (db_options.keep_log_file_num == 0) {
    return Status::InvalidArgument("keep_log_file_num must be greater than 0");
  }

  if (db_options.unordered_write &&
      !db_options.allow_concurrent_memtable_write) {
    return Status::InvalidArgument(
        "unordered_write is incompatible with !allow_concurrent_memtable_write");
  }

  if (db_options.unordered_write && db_options.enable_pipelined_write) {
    return Status::InvalidArgument(
        "unordered_write is incompatible with enable_pipelined_write");
  }

  if (db_options.atomic_flush && db_options.enable_pipelined_write) {
    return Status::InvalidArgument(
        "atomic_flush is incompatible with enable_pipelined_write");
  }

  return Status::OK();
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  Status s = SetIdentityFile(env_, dbname_);
  if (!s.ok()) {
    return s;
  }
  if (immutable_db_options_.write_dbid_to_manifest) {
    std::string temp_db_id;
    GetDbIdentityFromIdentityFile(&temp_db_id);
    new_db.SetDBId(temp_db_id);
  }
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  ROCKS_LOG_INFO(immutable_db_options_.info_log, "Creating manifest 1 \n");
  const std::string manifest = DescriptorFileName(dbname_, 1);
  {
    std::unique_ptr<FSWritableFile> file;
    FileOptions file_options = fs_->OptimizeForManifestWrite(file_options_);
    s = NewWritableFile(fs_.get(), manifest, &file, file_options);
    if (!s.ok()) {
      return s;
    }
    file->SetPreallocationBlockSize(
        immutable_db_options_.manifest_preallocation_size);
    std::unique_ptr<WritableFileWriter> file_writer(new WritableFileWriter(
        std::move(file), manifest, file_options, env_, nullptr /* stats */,
        immutable_db_options_.listeners));
    log::Writer log(std::move(file_writer), 0, false);
    std::string record;
    new_db.EncodeTo(&record);
    s = log.AddRecord(record);
    if (s.ok()) {
      s = SyncManifest(env_, &immutable_db_options_, log.file());
    }
  }
  if (s.ok()) {
    // Make "CURRENT" file that points to the new manifest file.
    s = SetCurrentFile(env_, dbname_, 1, directories_.GetDbDir());
  } else {
    fs_->DeleteFile(manifest, IOOptions(), nullptr);
  }
  return s;
}

Status DBImpl::CreateAndNewDirectory(Env* env, const std::string& dirname,
                                     std::unique_ptr<Directory>* directory) {
  // We call CreateDirIfMissing() as the directory may already exist (if we
  // are reopening a DB), when this happens we don't want creating the
  // directory to cause an error. However, we need to check if creating the
  // directory fails or else we may get an obscure message about the lock
  // file not existing. One real-world example of this occurring is if
  // env->CreateDirIfMissing() doesn't create intermediate directories, e.g.
  // when dbname_ is "dir/db" but when "dir" doesn't exist.
  Status s = env->CreateDirIfMissing(dirname);
  if (!s.ok()) {
    return s;
  }
  return env->NewDirectory(dirname, directory);
}

Status Directories::SetDirectories(Env* env, const std::string& dbname,
                                   const std::string& wal_dir,
                                   const std::vector<DbPath>& data_paths) {
  Status s = DBImpl::CreateAndNewDirectory(env, dbname, &db_dir_);
  if (!s.ok()) {
    return s;
  }
  if (!wal_dir.empty() && dbname != wal_dir) {
    s = DBImpl::CreateAndNewDirectory(env, wal_dir, &wal_dir_);
    if (!s.ok()) {
      return s;
    }
  }

  data_dirs_.clear();
  for (auto& p : data_paths) {
    const std::string db_path = p.path;
    if (db_path == dbname) {
      data_dirs_.emplace_back(nullptr);
    } else {
      std::unique_ptr<Directory> path_directory;
      s = DBImpl::CreateAndNewDirectory(env, db_path, &path_directory);
      if (!s.ok()) {
        return s;
      }
      data_dirs_.emplace_back(path_directory.release());
    }
  }
  assert(data_dirs_.size() == data_paths.size());
  return Status::OK();
}

Status DBImpl::Recover(
    const std::vector<ColumnFamilyDescriptor>& column_families, bool read_only,
    bool error_if_log_file_exist, bool error_if_data_exists_in_logs,
    uint64_t* recovered_seq) {
  mutex_.AssertHeld();

  bool is_new_db = false;
  assert(db_lock_ == nullptr);
  if (!read_only) {
    Status s = directories_.SetDirectories(env_, dbname_,
                                           immutable_db_options_.wal_dir,
                                           immutable_db_options_.db_paths);
    if (!s.ok()) {
      return s;
    }

    s = env_->LockFile(LockFileName(dbname_), &db_lock_);
    if (!s.ok()) {
      return s;
    }

    std::string current_fname = CurrentFileName(dbname_);
    s = env_->FileExists(current_fname);
    if (s.IsNotFound()) {
      if (immutable_db_options_.create_if_missing) {
        // Has to be called only after Identity File creation is successful
        // because DB ID is stored in Manifest if
        // immutable_db_options_.write_dbid_to_manifest = true
        s = NewDB();
        is_new_db = true;
        if (!s.ok()) {
          return s;
        }
      } else {
        return Status::InvalidArgument(
            current_fname, "does not exist (create_if_missing is false)");
      }
    } else if (s.ok()) {
      if (immutable_db_options_.error_if_exists) {
        return Status::InvalidArgument(dbname_,
                                       "exists (error_if_exists is true)");
      }
    } else {
      // Unexpected error reading file
      assert(s.IsIOError());
      return s;
    }
    // Verify compatibility of file_options_ and filesystem
    {
      std::unique_ptr<FSRandomAccessFile> idfile;
      FileOptions customized_fs(file_options_);
      customized_fs.use_direct_reads |=
          immutable_db_options_.use_direct_io_for_flush_and_compaction;
      s = fs_->NewRandomAccessFile(current_fname, customized_fs, &idfile,
                                   nullptr);
      if (!s.ok()) {
        std::string error_str = s.ToString();
        // Check if unsupported Direct I/O is the root cause
        customized_fs.use_direct_reads = false;
        s = fs_->NewRandomAccessFile(current_fname, customized_fs, &idfile,
                                     nullptr);
        if (s.ok()) {
          return Status::InvalidArgument(
              "Direct I/O is not supported by the specified DB.");
        } else {
          return Status::InvalidArgument(
              "Found options incompatible with filesystem", error_str.c_str());
        }
      }
    }
  }
  assert(db_id_.empty());
  Status s = versions_->Recover(column_families, read_only, &db_id_);
  if (!s.ok()) {
    return s;
  }
  // Happens when immutable_db_options_.write_dbid_to_manifest is set to true
  // the very first time.
  if (db_id_.empty()) {
    // Check for the IDENTITY file and create it if not there.
    s = fs_->FileExists(IdentityFileName(dbname_), IOOptions(), nullptr);
    // Typically Identity file is created in NewDB() and for some reason if
    // it is no longer available then at this point DB ID is not in Identity
    // file or Manifest.
    if (s.IsNotFound()) {
      s = SetIdentityFile(env_, dbname_);
      if (!s.ok()) {
        return s;
      }
    } else if (!s.ok()) {
      assert(s.IsIOError());
      return s;
    }
    GetDbIdentityFromIdentityFile(&db_id_);
    if (immutable_db_options_.write_dbid_to_manifest) {
      VersionEdit edit;
      edit.SetDBId(db_id_);
      Options options;
      MutableCFOptions mutable_cf_options(options);
      versions_->db_id_ = db_id_;
      versions_->LogAndApply(versions_->GetColumnFamilySet()->GetDefault(),
                             mutable_cf_options, &edit, &mutex_, nullptr,
                             false);
    }
  } else {
    SetIdentityFile(env_, dbname_, db_id_);
  }

  if (immutable_db_options_.paranoid_checks && s.ok()) {
    s = CheckConsistency();
  }
  if (s.ok() && !read_only) {
    std::map<std::string, std::shared_ptr<Directory>> created_dirs;
    for (auto cfd : *versions_->GetColumnFamilySet()) {
      s = cfd->AddDirectories(&created_dirs);
      if (!s.ok()) {
        return s;
      }
    }
  }
  // DB mutex is already held
  if (s.ok() && immutable_db_options_.persist_stats_to_disk) {
    s = InitPersistStatsColumnFamily();
  }

  // Initial max_total_in_memory_state_ before recovery logs. Log recovery
  // may check this value to decide whether to flush.
  max_total_in_memory_state_ = 0;
  for (auto cfd : *versions_->GetColumnFamilySet()) {
    auto* mutable_cf_options = cfd->GetLatestMutableCFOptions();
    max_total_in_memory_state_ += mutable_cf_options->write_buffer_size *
                                  mutable_cf_options->max_write_buffer_number;
  }

  if (s.ok()) {
    SequenceNumber next_sequence(kMaxSequenceNumber);
    default_cf_handle_ = new ColumnFamilyHandleImpl(
        versions_->GetColumnFamilySet()->GetDefault(), this, &mutex_);
    default_cf_internal_stats_ = default_cf_handle_->cfd()->internal_stats();
    // TODO(Zhongyi): handle single_column_family_mode_ when
    // persistent_stats is enabled
    single_column_family_mode_ =
        versions_->GetColumnFamilySet()->NumberOfColumnFamilies() == 1;

    // Recover from all newer log files than the ones named in the
    // descriptor (new log files may have been added by the previous
    // incarnation without registering them in the descriptor).
    //
    // Note that prev_log_number() is no longer used, but we pay
    // attention to it in case we are recovering a database
    // produced by an older version of rocksdb.
    std::vector<std::string> filenames;
    s = env_->GetChildren(immutable_db_options_.wal_dir, &filenames);
    if (s.IsNotFound()) {
      return Status::InvalidArgument("wal_dir not found",
                                     immutable_db_options_.wal_dir);
    } else if (!s.ok()) {
      return s;
    }

    std::vector<uint64_t> logs;
    for (size_t i = 0; i < filenames.size(); i++) {
      uint64_t number;
      FileType type;
      if (ParseFileName(filenames[i], &number, &type) && type == kLogFile) {
        if (is_new_db) {
          return Status::Corruption(
              "While creating a new Db, wal_dir contains "
              "existing log file: ",
              filenames[i]);
        } else {
          logs.push_back(number);
        }
      }
    }

    if (logs.size() > 0) {
      if (error_if_log_file_exist) {
        return Status::Corruption(
            "The db was opened in readonly mode with error_if_log_file_exist"
            "flag but a log file already exists");
      } else if (error_if_data_exists_in_logs) {
        for (auto& log : logs) {
          std::string fname = LogFileName(immutable_db_options_.wal_dir, log);
          uint64_t bytes;
          s = env_->GetFileSize(fname, &bytes);
          if (s.ok()) {
            if (bytes > 0) {
              return Status::Corruption(
                  "error_if_data_exists_in_logs is set but there are data "
                  " in log files.");
            }
          }
        }
      }
    }

    if (!logs.empty()) {
      // Recover in the order in which the logs were generated
      std::sort(logs.begin(), logs.end());
      bool corrupted_log_found = false;
      s = RecoverLogFiles(logs, &next_sequence, read_only,
                          &corrupted_log_found);
      if (corrupted_log_found && recovered_seq != nullptr) {
        *recovered_seq = next_sequence;
      }
      if (!s.ok()) {
        // Clear memtables if recovery failed
        for (auto cfd : *versions_->GetColumnFamilySet()) {
          cfd->CreateNewMemtable(*cfd->GetLatestMutableCFOptions(),
                                 kMaxSequenceNumber);
        }
      }
    }
  }

  if (read_only) {
    // If we are opening as read-only, we need to update options_file_number_
    // to reflect the most recent OPTIONS file. It does not matter for regular
    // read-write db instance because options_file_number_ will later be
    // updated to versions_->NewFileNumber() in RenameTempFileToOptionsFile.
    std::vector<std::string> file_names;
    if (s.ok()) {
      s = env_->GetChildren(GetName(), &file_names);
    }
    if (s.ok()) {
      uint64_t number = 0;
      uint64_t options_file_number = 0;
      FileType type;
      for (const auto& fname : file_names) {
        if (ParseFileName(fname, &number, &type) && type == kOptionsFile) {
          options_file_number = std::max(number, options_file_number);
        }
      }
      versions_->options_file_number_ = options_file_number;
    }
  }

  return s;
}

Status DBImpl::PersistentStatsProcessFormatVersion() {
  mutex_.AssertHeld();
  Status s;
  // persist version when stats CF doesn't exist
  bool should_persist_format_version = !persistent_stats_cfd_exists_;
  mutex_.Unlock();
  if (persistent_stats_cfd_exists_) {
    // Check persistent stats format version compatibility. Drop and recreate
    // persistent stats CF if format version is incompatible
    uint64_t format_version_recovered = 0;
    Status s_format = DecodePersistentStatsVersionNumber(
        this, StatsVersionKeyType::kFormatVersion, &format_version_recovered);
    uint64_t compatible_version_recovered = 0;
    Status s_compatible = DecodePersistentStatsVersionNumber(
        this, StatsVersionKeyType::kCompatibleVersion,
        &compatible_version_recovered);
    // abort reading from existing stats CF if any of following is true:
    // 1. failed to read format version or compatible version from disk
    // 2. sst's format version is greater than current format version, meaning
    // this sst is encoded with a newer RocksDB release, and current compatible
    // version is below the sst's compatible version
    if (!s_format.ok() || !s_compatible.ok() ||
        (kStatsCFCurrentFormatVersion < format_version_recovered &&
         kStatsCFCompatibleFormatVersion < compatible_version_recovered)) {
      if (!s_format.ok() || !s_compatible.ok()) {
        ROCKS_LOG_INFO(
            immutable_db_options_.info_log,
            "Reading persistent stats version key failed. Format key: %s, "
            "compatible key: %s",
            s_format.ToString().c_str(), s_compatible.ToString().c_str());
      } else {
        ROCKS_LOG_INFO(
            immutable_db_options_.info_log,
            "Disable persistent stats due to corrupted or incompatible format "
            "version\n");
      }
      DropColumnFamily(persist_stats_cf_handle_);
      DestroyColumnFamilyHandle(persist_stats_cf_handle_);
      ColumnFamilyHandle* handle = nullptr;
      ColumnFamilyOptions cfo;
      OptimizeForPersistentStats(&cfo);
      s = CreateColumnFamily(cfo, kPersistentStatsColumnFamilyName, &handle);
      persist_stats_cf_handle_ = static_cast<ColumnFamilyHandleImpl*>(handle);
      // should also persist version here because old stats CF is discarded
      should_persist_format_version = true;
    }
  }
  if (s.ok() && should_persist_format_version) {
    // Persistent stats CF being created for the first time, need to write
    // format version key
    WriteBatch batch;
    batch.Put(persist_stats_cf_handle_, kFormatVersionKeyString,
              ToString(kStatsCFCurrentFormatVersion));
    batch.Put(persist_stats_cf_handle_, kCompatibleVersionKeyString,
              ToString(kStatsCFCompatibleFormatVersion));
    WriteOptions wo;
    wo.low_pri = true;
    wo.no_slowdown = true;
    wo.sync = false;
    s = Write(wo, &batch);
  }
  mutex_.Lock();
  return s;
}

Status DBImpl::InitPersistStatsColumnFamily() {
  mutex_.AssertHeld();
  assert(!persist_stats_cf_handle_);
  ColumnFamilyData* persistent_stats_cfd =
      versions_->GetColumnFamilySet()->GetColumnFamily(
          kPersistentStatsColumnFamilyName);
  persistent_stats_cfd_exists_ = persistent_stats_cfd != nullptr;

  Status s;
  if (persistent_stats_cfd != nullptr) {
    // We are recovering from a DB which already contains persistent stats CF,
    // the CF is already created in VersionSet::ApplyOneVersionEdit, but
    // column family handle was not. Need to explicitly create handle here.
    persist_stats_cf_handle_ =
        new ColumnFamilyHandleImpl(persistent_stats_cfd, this, &mutex_);
  } else {
    mutex_.Unlock();
    ColumnFamilyHandle* handle = nullptr;
    ColumnFamilyOptions cfo;
    OptimizeForPersistentStats(&cfo);
    s = CreateColumnFamily(cfo, kPersistentStatsColumnFamilyName, &handle);
    persist_stats_cf_handle_ = static_cast<ColumnFamilyHandleImpl*>(handle);
    mutex_.Lock();
  }
  return s;
}

// REQUIRES: log_numbers are sorted in ascending order
Status DBImpl::RecoverLogFiles(const std::vector<uint64_t>& log_numbers,
                               SequenceNumber* next_sequence, bool read_only,
                               bool* corrupted_log_found) {
  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    const char* fname;
    Status* status;  // nullptr if immutable_db_options_.paranoid_checks==false
    void Corruption(size_t bytes, const Status& s) override {
      ROCKS_LOG_WARN(info_log, "%s%s: dropping %d bytes; %s",
                     (this->status == nullptr ? "(ignoring error) " : ""),
                     fname, static_cast<int>(bytes), s.ToString().c_str());
      if (this->status != nullptr && this->status->ok()) {
        *this->status = s;
      }
    }
  };

  mutex_.AssertHeld();
  Status status;
  std::unordered_map<int, VersionEdit> version_edits;
  // no need to refcount because iteration is under mutex
  for (auto cfd : *versions_->GetColumnFamilySet()) {
    VersionEdit edit;
    edit.SetColumnFamily(cfd->GetID());
    version_edits.insert({cfd->GetID(), edit});
  }
  int job_id = next_job_id_.fetch_add(1);
  {
    auto stream = event_logger_.Log();
    stream << "job" << job_id << "event"
           << "recovery_started";
    stream << "log_files";
    stream.StartArray();
    for (auto log_number : log_numbers) {
      stream << log_number;
    }
    stream.EndArray();
  }

#ifndef ROCKSDB_LITE
  if (immutable_db_options_.wal_filter != nullptr) {
    std::map<std::string, uint32_t> cf_name_id_map;
    std::map<uint32_t, uint64_t> cf_lognumber_map;
    for (auto cfd : *versions_->GetColumnFamilySet()) {
      cf_name_id_map.insert(std::make_pair(cfd->GetName(), cfd->GetID()));
      cf_lognumber_map.insert(
          std::make_pair(cfd->GetID(), cfd->GetLogNumber()));
    }

    immutable_db_options_.wal_filter->ColumnFamilyLogNumberMap(cf_lognumber_map,
                                                               cf_name_id_map);
  }
#endif

  bool stop_replay_by_wal_filter = false;
  bool stop_replay_for_corruption = false;
  bool flushed = false;
  uint64_t corrupted_log_number = kMaxSequenceNumber;
  uint64_t min_log_number = MinLogNumberToKeep();
  for (auto log_number : log_numbers) {
    if (log_number < min_log_number) {
      ROCKS_LOG_INFO(immutable_db_options_.info_log,
                     "Skipping log #%" PRIu64
                     " since it is older than min log to keep #%" PRIu64,
                     log_number, min_log_number);
      continue;
    }
    // The previous incarnation may not have written any MANIFEST
    // records after allocating this log number.  So we manually
    // update the file number allocation counter in VersionSet.
    versions_->MarkFileNumberUsed(log_number);
    // Open the log file
    std::string fname = LogFileName(immutable_db_options_.wal_dir, log_number);

    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "Recovering log #%" PRIu64 " mode %d", log_number,
                   static_cast<int>(immutable_db_options_.wal_recovery_mode));
    auto logFileDropped = [this, &fname]() {
      uint64_t bytes;
      if (env_->GetFileSize(fname, &bytes).ok()) {
        auto info_log = immutable_db_options_.info_log.get();
        ROCKS_LOG_WARN(info_log, "%s: dropping %d bytes", fname.c_str(),
                       static_cast<int>(bytes));
      }
    };
    if (stop_replay_by_wal_filter) {
      logFileDropped();
      continue;
    }

    std::unique_ptr<SequentialFileReader> file_reader;
    {
      std::unique_ptr<FSSequentialFile> file;
      status = fs_->NewSequentialFile(fname,
                                      fs_->OptimizeForLogRead(file_options_),
                                      &file, nullptr);
      if (!status.ok()) {
        MaybeIgnoreError(&status);
        if (!status.ok()) {
          return status;
        } else {
          // Fail with one log file, but that's ok.
          // Try next one.
          continue;
        }
      }
      file_reader.reset(new SequentialFileReader(
          std::move(file), fname, immutable_db_options_.log_readahead_size));
    }

    // Create the log reader.
    LogReporter reporter;
    reporter.env = env_;
    reporter.info_log = immutable_db_options_.info_log.get();
    reporter.fname = fname.c_str();
    if (!immutable_db_options_.paranoid_checks ||
        immutable_db_options_.wal_recovery_mode ==
            WALRecoveryMode::kSkipAnyCorruptedRecords) {
      reporter.status = nullptr;
    } else {
      reporter.status = &status;
    }
    // We intentially make log::Reader do checksumming even if
    // paranoid_checks==false so that corruptions cause entire commits
    // to be skipped instead of propagating bad information (like overly
    // large sequence numbers).
    log::Reader reader(immutable_db_options_.info_log, std::move(file_reader),
                       &reporter, true /*checksum*/, log_number);

    // Determine if we should tolerate incomplete records at the tail end of the
    // Read all the records and add to a memtable
    std::string scratch;
    Slice record;
    WriteBatch batch;

    while (!stop_replay_by_wal_filter &&
           reader.ReadRecord(&record, &scratch,
                             immutable_db_options_.wal_recovery_mode) &&
           status.ok()) {
      if (record.size() < WriteBatchInternal::kHeader) {
        reporter.Corruption(record.size(),
                            Status::Corruption("log record too small"));
        continue;
      }
      WriteBatchInternal::SetContents(&batch, record);
      SequenceNumber sequence = WriteBatchInternal::Sequence(&batch);

      if (immutable_db_options_.wal_recovery_mode ==
          WALRecoveryMode::kPointInTimeRecovery) {
        // In point-in-time recovery mode, if sequence id of log files are
        // consecutive, we continue recovery despite corruption. This could
        // happen when we open and write to a corrupted DB, where sequence id
        // will start from the last sequence id we recovered.
        if (sequence == *next_sequence) {
          stop_replay_for_corruption = false;
        }
        if (stop_replay_for_corruption) {
          logFileDropped();
          break;
        }
      }

#ifndef ROCKSDB_LITE
      if (immutable_db_options_.wal_filter != nullptr) {
        WriteBatch new_batch;
        bool batch_changed = false;

        WalFilter::WalProcessingOption wal_processing_option =
            immutable_db_options_.wal_filter->LogRecordFound(
                log_number, fname, batch, &new_batch, &batch_changed);

        switch (wal_processing_option) {
          case WalFilter::WalProcessingOption::kContinueProcessing:
            // do nothing, proceeed normally
            break;
          case WalFilter::WalProcessingOption::kIgnoreCurrentRecord:
            // skip current record
            continue;
          case WalFilter::WalProcessingOption::kStopReplay:
            // skip current record and stop replay
            stop_replay_by_wal_filter = true;
            continue;
          case WalFilter::WalProcessingOption::kCorruptedRecord: {
            status =
                Status::Corruption("Corruption reported by Wal Filter ",
                                   immutable_db_options_.wal_filter->Name());
            MaybeIgnoreError(&status);
            if (!status.ok()) {
              reporter.Corruption(record.size(), status);
              continue;
            }
            break;
          }
          default: {
            assert(false);  // unhandled case
            status = Status::NotSupported(
                "Unknown WalProcessingOption returned"
                " by Wal Filter ",
                immutable_db_options_.wal_filter->Name());
            MaybeIgnoreError(&status);
            if (!status.ok()) {
              return status;
            } else {
              // Ignore the error with current record processing.
              continue;
            }
          }
        }

        if (batch_changed) {
          // Make sure that the count in the new batch is
          // within the orignal count.
          int new_count = WriteBatchInternal::Count(&new_batch);
          int original_count = WriteBatchInternal::Count(&batch);
          if (new_count > original_count) {
            ROCKS_LOG_FATAL(
                immutable_db_options_.info_log,
                "Recovering log #%" PRIu64
                " mode %d log filter %s returned "
                "more records (%d) than original (%d) which is not allowed. "
                "Aborting recovery.",
                log_number,
                static_cast<int>(immutable_db_options_.wal_recovery_mode),
                immutable_db_options_.wal_filter->Name(), new_count,
                original_count);
            status = Status::NotSupported(
                "More than original # of records "
                "returned by Wal Filter ",
                immutable_db_options_.wal_filter->Name());
            return status;
          }
          // Set the same sequence number in the new_batch
          // as the original batch.
          WriteBatchInternal::SetSequence(&new_batch,
                                          WriteBatchInternal::Sequence(&batch));
          batch = new_batch;
        }
      }
#endif  // ROCKSDB_LITE

      // If column family was not found, it might mean that the WAL write
      // batch references to the column family that was dropped after the
      // insert. We don't want to fail the whole write batch in that case --
      // we just ignore the update.
      // That's why we set ignore missing column families to true
      bool has_valid_writes = false;
      status = WriteBatchInternal::InsertInto(
          &batch, column_family_memtables_.get(), &flush_scheduler_,
          &trim_history_scheduler_, true, log_number, this,
          false /* concurrent_memtable_writes */, next_sequence,
          &has_valid_writes, seq_per_batch_, batch_per_txn_);
      MaybeIgnoreError(&status);
      if (!status.ok()) {
        // We are treating this as a failure while reading since we read valid
        // blocks that do not form coherent data
        reporter.Corruption(record.size(), status);
        continue;
      }

      if (has_valid_writes && !read_only) {
        // we can do this because this is called before client has access to the
        // DB and there is only a single thread operating on DB
        ColumnFamilyData* cfd;

        while ((cfd = flush_scheduler_.TakeNextColumnFamily()) != nullptr) {
          cfd->UnrefAndTryDelete();
          // If this asserts, it means that InsertInto failed in
          // filtering updates to already-flushed column families
          assert(cfd->GetLogNumber() <= log_number);
          auto iter = version_edits.find(cfd->GetID());
          assert(iter != version_edits.end());
          VersionEdit* edit = &iter->second;
          status = WriteLevel0TableForRecovery(job_id, cfd, cfd->mem(), edit);
          if (!status.ok()) {
            // Reflect errors immediately so that conditions like full
            // file-systems cause the DB::Open() to fail.
            return status;
          }
          flushed = true;

          cfd->CreateNewMemtable(*cfd->GetLatestMutableCFOptions(),
                                 *next_sequence);
        }
      }
    }

    if (!status.ok()) {
      if (status.IsNotSupported()) {
        // We should not treat NotSupported as corruption. It is rather a clear
        // sign that we are processing a WAL that is produced by an incompatible
        // version of the code.
        return status;
      }
      if (immutable_db_options_.wal_recovery_mode ==
          WALRecoveryMode::kSkipAnyCorruptedRecords) {
        // We should ignore all errors unconditionally
        status = Status::OK();
      } else if (immutable_db_options_.wal_recovery_mode ==
                 WALRecoveryMode::kPointInTimeRecovery) {
        // We should ignore the error but not continue replaying
        status = Status::OK();
        stop_replay_for_corruption = true;
        corrupted_log_number = log_number;
        if (corrupted_log_found != nullptr) {
          *corrupted_log_found = true;
        }
        ROCKS_LOG_INFO(immutable_db_options_.info_log,
                       "Point in time recovered to log #%" PRIu64
                       " seq #%" PRIu64,
                       log_number, *next_sequence);
      } else {
        assert(immutable_db_options_.wal_recovery_mode ==
                   WALRecoveryMode::kTolerateCorruptedTailRecords ||
               immutable_db_options_.wal_recovery_mode ==
                   WALRecoveryMode::kAbsoluteConsistency);
        return status;
      }
    }

    flush_scheduler_.Clear();
    trim_history_scheduler_.Clear();
    auto last_sequence = *next_sequence - 1;
    if ((*next_sequence != kMaxSequenceNumber) &&
        (versions_->LastSequence() <= last_sequence)) {
      versions_->SetLastAllocatedSequence(last_sequence);
      versions_->SetLastPublishedSequence(last_sequence);
      versions_->SetLastSequence(last_sequence);
    }
  }
  // Compare the corrupted log number to all columnfamily's current log number.
  // Abort Open() if any column family's log number is greater than
  // the corrupted log number, which means CF contains data beyond the point of
  // corruption. This could during PIT recovery when the WAL is corrupted and
  // some (but not all) CFs are flushed
  // Exclude the PIT case where no log is dropped after the corruption point.
  // This is to cover the case for empty logs after corrupted log, in which we
  // don't reset stop_replay_for_corruption.
  if (stop_replay_for_corruption == true &&
      (immutable_db_options_.wal_recovery_mode ==
           WALRecoveryMode::kPointInTimeRecovery ||
       immutable_db_options_.wal_recovery_mode ==
           WALRecoveryMode::kTolerateCorruptedTailRecords)) {
    for (auto cfd : *versions_->GetColumnFamilySet()) {
      if (cfd->GetLogNumber() > corrupted_log_number) {
        ROCKS_LOG_ERROR(immutable_db_options_.info_log,
                        "Column family inconsistency: SST file contains data"
                        " beyond the point of corruption.");
        return Status::Corruption("SST file is ahead of WALs");
      }
    }
  }

  // True if there's any data in the WALs; if not, we can skip re-processing
  // them later
  bool data_seen = false;
  if (!read_only) {
    // no need to refcount since client still doesn't have access
    // to the DB and can not drop column families while we iterate
    auto max_log_number = log_numbers.back();
    for (auto cfd : *versions_->GetColumnFamilySet()) {
      auto iter = version_edits.find(cfd->GetID());
      assert(iter != version_edits.end());
      VersionEdit* edit = &iter->second;

      if (cfd->GetLogNumber() > max_log_number) {
        // Column family cfd has already flushed the data
        // from all logs. Memtable has to be empty because
        // we filter the updates based on log_number
        // (in WriteBatch::InsertInto)
        assert(cfd->mem()->GetFirstSequenceNumber() == 0);
        assert(edit->NumEntries() == 0);
        continue;
      }

      TEST_SYNC_POINT_CALLBACK(
          "DBImpl::RecoverLogFiles:BeforeFlushFinalMemtable", /*arg=*/nullptr);

      // flush the final memtable (if non-empty)
      if (cfd->mem()->GetFirstSequenceNumber() != 0) {
        // If flush happened in the middle of recovery (e.g. due to memtable
        // being full), we flush at the end. Otherwise we'll need to record
        // where we were on last flush, which make the logic complicated.
        if (flushed || !immutable_db_options_.avoid_flush_during_recovery) {
          status = WriteLevel0TableForRecovery(job_id, cfd, cfd->mem(), edit);
          if (!status.ok()) {
            // Recovery failed
            break;
          }
          flushed = true;

          cfd->CreateNewMemtable(*cfd->GetLatestMutableCFOptions(),
                                 versions_->LastSequence());
        }
        data_seen = true;
      }

      // Update the log number info in the version edit corresponding to this
      // column family. Note that the version edits will be written to MANIFEST
      // together later.
      // writing log_number in the manifest means that any log file
      // with number strongly less than (log_number + 1) is already
      // recovered and should be ignored on next reincarnation.
      // Since we already recovered max_log_number, we want all logs
      // with numbers `<= max_log_number` (includes this one) to be ignored
      if (flushed || cfd->mem()->GetFirstSequenceNumber() == 0) {
        edit->SetLogNumber(max_log_number + 1);
      }
    }
    if (status.ok()) {
      // we must mark the next log number as used, even though it's
      // not actually used. that is because VersionSet assumes
      // VersionSet::next_file_number_ always to be strictly greater than any
      // log number
      versions_->MarkFileNumberUsed(max_log_number + 1);

      autovector<ColumnFamilyData*> cfds;
      autovector<const MutableCFOptions*> cf_opts;
      autovector<autovector<VersionEdit*>> edit_lists;
      for (auto* cfd : *versions_->GetColumnFamilySet()) {
        cfds.push_back(cfd);
        cf_opts.push_back(cfd->GetLatestMutableCFOptions());
        auto iter = version_edits.find(cfd->GetID());
        assert(iter != version_edits.end());
        edit_lists.push_back({&iter->second});
      }
      // write MANIFEST with update
      status = versions_->LogAndApply(cfds, cf_opts, edit_lists, &mutex_,
                                      directories_.GetDbDir(),
                                      /*new_descriptor_log=*/true);
    }
  }

  if (status.ok() && data_seen && !flushed) {
    status = RestoreAliveLogFiles(log_numbers);
  }

  event_logger_.Log() << "job" << job_id << "event"
                      << "recovery_finished";

  return status;
}

Status DBImpl::RestoreAliveLogFiles(const std::vector<uint64_t>& log_numbers) {
  if (log_numbers.empty()) {
    return Status::OK();
  }
  Status s;
  mutex_.AssertHeld();
  assert(immutable_db_options_.avoid_flush_during_recovery);
  if (two_write_queues_) {
    log_write_mutex_.Lock();
  }
  // Mark these as alive so they'll be considered for deletion later by
  // FindObsoleteFiles()
  total_log_size_ = 0;
  log_empty_ = false;
  for (auto log_number : log_numbers) {
    LogFileNumberSize log(log_number);
    std::string fname = LogFileName(immutable_db_options_.wal_dir, log_number);
    // This gets the appear size of the logs, not including preallocated space.
    s = env_->GetFileSize(fname, &log.size);
    if (!s.ok()) {
      break;
    }
    total_log_size_ += log.size;
    alive_log_files_.push_back(log);
    // We preallocate space for logs, but then after a crash and restart, those
    // preallocated space are not needed anymore. It is likely only the last
    // log has such preallocated space, so we only truncate for the last log.
    if (log_number == log_numbers.back()) {
      std::unique_ptr<FSWritableFile> last_log;
      Status truncate_status = fs_->ReopenWritableFile(
          fname,
          fs_->OptimizeForLogWrite(
              file_options_,
              BuildDBOptions(immutable_db_options_, mutable_db_options_)),
          &last_log, nullptr);
      if (truncate_status.ok()) {
        truncate_status = last_log->Truncate(log.size, IOOptions(), nullptr);
      }
      if (truncate_status.ok()) {
        truncate_status = last_log->Close(IOOptions(), nullptr);
      }
      // Not a critical error if fail to truncate.
      if (!truncate_status.ok()) {
        ROCKS_LOG_WARN(immutable_db_options_.info_log,
                       "Failed to truncate log #%" PRIu64 ": %s", log_number,
                       truncate_status.ToString().c_str());
      }
    }
  }
  if (two_write_queues_) {
    log_write_mutex_.Unlock();
  }
  return s;
}

Status DBImpl::WriteLevel0TableForRecovery(int job_id, ColumnFamilyData* cfd,
                                           MemTable* mem, VersionEdit* edit) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();
  FileMetaData meta;
  std::unique_ptr<std::list<uint64_t>::iterator> pending_outputs_inserted_elem(
      new std::list<uint64_t>::iterator(
          CaptureCurrentFileNumberInPendingOutputs()));
  meta.fd = FileDescriptor(versions_->NewFileNumber(), 0, 0);
  ReadOptions ro;
  ro.total_order_seek = true;
  Arena arena;
  Status s;
  TableProperties table_properties;
  {
    ScopedArenaIterator iter(mem->NewIterator(ro, &arena));
    ROCKS_LOG_DEBUG(immutable_db_options_.info_log,
                    "[%s] [WriteLevel0TableForRecovery]"
                    " Level-0 table #%" PRIu64 ": started",
                    cfd->GetName().c_str(), meta.fd.GetNumber());

    // Get the latest mutable cf options while the mutex is still locked
    const MutableCFOptions mutable_cf_options =
        *cfd->GetLatestMutableCFOptions();
    bool paranoid_file_checks =
        cfd->GetLatestMutableCFOptions()->paranoid_file_checks;

    int64_t _current_time = 0;
    env_->GetCurrentTime(&_current_time);  // ignore error
    const uint64_t current_time = static_cast<uint64_t>(_current_time);
    meta.oldest_ancester_time = current_time;

    {
      auto write_hint = cfd->CalculateSSTWriteHint(0);
      mutex_.Unlock();

      SequenceNumber earliest_write_conflict_snapshot;
      std::vector<SequenceNumber> snapshot_seqs =
          snapshots_.GetAll(&earliest_write_conflict_snapshot);
      auto snapshot_checker = snapshot_checker_.get();
      if (use_custom_gc_ && snapshot_checker == nullptr) {
        snapshot_checker = DisableGCSnapshotChecker::Instance();
      }
      std::vector<std::unique_ptr<FragmentedRangeTombstoneIterator>>
          range_del_iters;
      auto range_del_iter =
          mem->NewRangeTombstoneIterator(ro, kMaxSequenceNumber);
      if (range_del_iter != nullptr) {
        range_del_iters.emplace_back(range_del_iter);
      }
      s = BuildTable(
          dbname_, env_, fs_.get(), *cfd->ioptions(), mutable_cf_options,
          file_options_for_compaction_, cfd->table_cache(), iter.get(),
          std::move(range_del_iters), &meta, cfd->internal_comparator(),
          cfd->int_tbl_prop_collector_factories(), cfd->GetID(), cfd->GetName(),
          snapshot_seqs, earliest_write_conflict_snapshot, snapshot_checker,
          GetCompressionFlush(*cfd->ioptions(), mutable_cf_options),
          mutable_cf_options.sample_for_compression,
          cfd->ioptions()->compression_opts, paranoid_file_checks,
          cfd->internal_stats(), TableFileCreationReason::kRecovery,
          &event_logger_, job_id, Env::IO_HIGH, nullptr /* table_properties */,
          -1 /* level */, current_time, write_hint);
      LogFlush(immutable_db_options_.info_log);
      ROCKS_LOG_DEBUG(immutable_db_options_.info_log,
                      "[%s] [WriteLevel0TableForRecovery]"
                      " Level-0 table #%" PRIu64 ": %" PRIu64 " bytes %s",
                      cfd->GetName().c_str(), meta.fd.GetNumber(),
                      meta.fd.GetFileSize(), s.ToString().c_str());
      mutex_.Lock();
    }
  }
  ReleaseFileNumberFromPendingOutputs(pending_outputs_inserted_elem);

  // Note that if file_size is zero, the file has been deleted and
  // should not be added to the manifest.
  int level = 0;
  if (s.ok() && meta.fd.GetFileSize() > 0) {
    edit->AddFile(level, meta.fd.GetNumber(), meta.fd.GetPathId(),
                  meta.fd.GetFileSize(), meta.smallest, meta.largest,
                  meta.fd.smallest_seqno, meta.fd.largest_seqno,
                  meta.marked_for_compaction, meta.oldest_blob_file_number,
                  meta.oldest_ancester_time, meta.file_creation_time);
  }

  InternalStats::CompactionStats stats(CompactionReason::kFlush, 1);
  stats.micros = env_->NowMicros() - start_micros;
  stats.bytes_written = meta.fd.GetFileSize();
  stats.num_output_files = 1;
  cfd->internal_stats()->AddCompactionStats(level, Env::Priority::USER, stats);
  cfd->internal_stats()->AddCFStats(InternalStats::BYTES_FLUSHED,
                                    meta.fd.GetFileSize());
  RecordTick(stats_, COMPACT_WRITE_BYTES, meta.fd.GetFileSize());
  return s;
}

#if RANDOM_PATH_1
Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr, std::vector<DbPath> cf_paths) {
	DBOptions db_options(options);
	ColumnFamilyOptions cf_options(options);
	if (cf_options.cf_paths.size() == 0)
		cf_options.cf_paths = cf_paths;
	std::vector<ColumnFamilyDescriptor> column_families;
	column_families.push_back(ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_options));
	if (db_options.persist_stats_to_disk)
		column_families.push_back(ColumnFamilyDescriptor(kPersistentStatsColumnFamilyName, cf_options));
	std::vector<ColumnFamilyHandle*> handles;
	
	Status s = DB::Open(db_options, dbname, column_families, &handles, dbptr);
	
	if (s.ok()) {
		if (db_options.persist_stats_to_disk) assert(handles.size() == 2);
		else assert(handles.size() == 1);
		
		if (db_options.persist_stats_to_disk && handles[1] != nullptr)
			delete handles[1];
		delete handles[0];
	}
	return s;
}
#endif

Status DB::Open(const Options& options, const std::string& dbname, DB** dbptr) {
  DBOptions db_options(options);
  ColumnFamilyOptions cf_options(options);

#if RANDOM_PATH
  cf_options.cf_paths.push_back(DbPath({"/rocksdb_tests/path_test/path1", 100llu<<30}));
  cf_options.cf_paths.push_back(DbPath({"/rocksdb_tests/path_test/path2", 100llu<<30}));
#endif

  std::vector<ColumnFamilyDescriptor> column_families;
  column_families.push_back(
      ColumnFamilyDescriptor(kDefaultColumnFamilyName, cf_options));
  if (db_options.persist_stats_to_disk) {
    column_families.push_back(
        ColumnFamilyDescriptor(kPersistentStatsColumnFamilyName, cf_options));
  }
  std::vector<ColumnFamilyHandle*> handles;
  Status s = DB::Open(db_options, dbname, column_families, &handles, dbptr);
  if (s.ok()) {
    if (db_options.persist_stats_to_disk) {
      assert(handles.size() == 2);
    } else {
      assert(handles.size() == 1);
    }
    // i can delete the handle since DBImpl is always holding a reference to
    // default column family
    if (db_options.persist_stats_to_disk && handles[1] != nullptr) {
      delete handles[1];
    }
    delete handles[0];
  }
  return s;
}

Status DB::Open(const DBOptions& db_options, const std::string& dbname,
                const std::vector<ColumnFamilyDescriptor>& column_families,
                std::vector<ColumnFamilyHandle*>* handles, DB** dbptr) {
  const bool kSeqPerBatch = true;
  const bool kBatchPerTxn = true;
  return DBImpl::Open(db_options, dbname, column_families, handles, dbptr,
                      !kSeqPerBatch, kBatchPerTxn);
}

Status DBImpl::CreateWAL(uint64_t log_file_num, uint64_t recycle_log_number,
                         size_t preallocate_block_size, log::Writer** new_log) {
  Status s;
  std::unique_ptr<FSWritableFile> lfile;

  DBOptions db_options =
      BuildDBOptions(immutable_db_options_, mutable_db_options_);
  FileOptions opt_file_options =
      fs_->OptimizeForLogWrite(file_options_, db_options);
  std::string log_fname =
      LogFileName(immutable_db_options_.wal_dir, log_file_num);

  if (recycle_log_number) {
    ROCKS_LOG_INFO(immutable_db_options_.info_log,
                   "reusing log %" PRIu64 " from recycle list\n",
                   recycle_log_number);
    std::string old_log_fname =
        LogFileName(immutable_db_options_.wal_dir, recycle_log_number);
    s = fs_->ReuseWritableFile(log_fname, old_log_fname, opt_file_options,
                               &lfile, /*dbg=*/nullptr);
  } else {
    s = NewWritableFile(fs_.get(), log_fname, &lfile, opt_file_options);
  }

  if (s.ok()) {
    lfile->SetWriteLifeTimeHint(CalculateWALWriteHint());
    lfile->SetPreallocationBlockSize(preallocate_block_size);

    const auto& listeners = immutable_db_options_.listeners;
    std::unique_ptr<WritableFileWriter> file_writer(
        new WritableFileWriter(std::move(lfile), log_fname, opt_file_options,
                               env_, nullptr /* stats */, listeners));
    *new_log = new log::Writer(std::move(file_writer), log_file_num,
                               immutable_db_options_.recycle_log_file_num > 0,
                               immutable_db_options_.manual_wal_flush);
  }
  return s;
}

Status DBImpl::Open(const DBOptions& db_options, const std::string& dbname,
                    const std::vector<ColumnFamilyDescriptor>& column_families,
                    std::vector<ColumnFamilyHandle*>* handles, DB** dbptr,
                    const bool seq_per_batch, const bool batch_per_txn) {
  Status s = SanitizeOptionsByTable(db_options, column_families);
  if (!s.ok()) {
    return s;
  }

  s = ValidateOptions(db_options, column_families);
  if (!s.ok()) {
    return s;
  }

  *dbptr = nullptr;
  handles->clear();

  size_t max_write_buffer_size = 0;
  for (auto cf : column_families) {
    max_write_buffer_size =
        std::max(max_write_buffer_size, cf.options.write_buffer_size);
  }

  DBImpl* impl = new DBImpl(db_options, dbname, seq_per_batch, batch_per_txn);
  s = impl->env_->CreateDirIfMissing(impl->immutable_db_options_.wal_dir);
  if (s.ok()) {
    std::vector<std::string> paths;
    for (auto& db_path : impl->immutable_db_options_.db_paths) {
      paths.emplace_back(db_path.path);
    }
    for (auto& cf : column_families) {
      for (auto& cf_path : cf.options.cf_paths) {
        paths.emplace_back(cf_path.path);
      }
    }
    for (auto& path : paths) {
      s = impl->env_->CreateDirIfMissing(path);
      if (!s.ok()) {
        break;
      }
    }

    // For recovery from NoSpace() error, we can only handle
    // the case where the database is stored in a single path
    if (paths.size() <= 1) {
      impl->error_handler_.EnableAutoRecovery();
    }
  }

  if (!s.ok()) {
    delete impl;
    return s;
  }

  s = impl->CreateArchivalDirectory();
  if (!s.ok()) {
    delete impl;
    return s;
  }

  impl->wal_in_db_path_ = IsWalDirSameAsDBPath(&impl->immutable_db_options_);

  impl->mutex_.Lock();
  // Handles create_if_missing, error_if_exists
  uint64_t recovered_seq(kMaxSequenceNumber);
  s = impl->Recover(column_families, false, false, false, &recovered_seq);
  if (s.ok()) {
    uint64_t new_log_number = impl->versions_->NewFileNumber();
    log::Writer* new_log = nullptr;
    const size_t preallocate_block_size =
        impl->GetWalPreallocateBlockSize(max_write_buffer_size);
    s = impl->CreateWAL(new_log_number, 0 /*recycle_log_number*/,
                        preallocate_block_size, &new_log);
    if (s.ok()) {
      InstrumentedMutexLock wl(&impl->log_write_mutex_);
      impl->logfile_number_ = new_log_number;
      assert(new_log != nullptr);
      impl->logs_.emplace_back(new_log_number, new_log);
    }

    if (s.ok()) {
      // set column family handles
      for (auto cf : column_families) {
        auto cfd =
            impl->versions_->GetColumnFamilySet()->GetColumnFamily(cf.name);
        if (cfd != nullptr) {
          handles->push_back(
              new ColumnFamilyHandleImpl(cfd, impl, &impl->mutex_));
          impl->NewThreadStatusCfInfo(cfd);
        } else {
          if (db_options.create_missing_column_families) {
            // missing column family, create it
            ColumnFamilyHandle* handle;
            impl->mutex_.Unlock();
            s = impl->CreateColumnFamily(cf.options, cf.name, &handle);
            impl->mutex_.Lock();
            if (s.ok()) {
              handles->push_back(handle);
            } else {
              break;
            }
          } else {
            s = Status::InvalidArgument("Column family not found: ", cf.name);
            break;
          }
        }
      }
    }
    if (s.ok()) {
      SuperVersionContext sv_context(/* create_superversion */ true);
      for (auto cfd : *impl->versions_->GetColumnFamilySet()) {
        impl->InstallSuperVersionAndScheduleWork(
            cfd, &sv_context, *cfd->GetLatestMutableCFOptions());
      }
      sv_context.Clean();
      if (impl->two_write_queues_) {
        impl->log_write_mutex_.Lock();
      }
      impl->alive_log_files_.push_back(
          DBImpl::LogFileNumberSize(impl->logfile_number_));
      if (impl->two_write_queues_) {
        impl->log_write_mutex_.Unlock();
      }

      impl->DeleteObsoleteFiles();
      s = impl->directories_.GetDbDir()->Fsync();
    }
    if (s.ok()) {
      // In WritePrepared there could be gap in sequence numbers. This breaks
      // the trick we use in kPointInTimeRecovery which assumes the first seq in
      // the log right after the corrupted log is one larger than the last seq
      // we read from the logs. To let this trick keep working, we add a dummy
      // entry with the expected sequence to the first log right after recovery.
      // In non-WritePrepared case also the new log after recovery could be
      // empty, and thus missing the consecutive seq hint to distinguish
      // middle-log corruption to corrupted-log-remained-after-recovery. This
      // case also will be addressed by a dummy write.
      if (recovered_seq != kMaxSequenceNumber) {
        WriteBatch empty_batch;
        WriteBatchInternal::SetSequence(&empty_batch, recovered_seq);
        WriteOptions write_options;
        uint64_t log_used, log_size;
        log::Writer* log_writer = impl->logs_.back().writer;
        s = impl->WriteToWAL(empty_batch, log_writer, &log_used, &log_size);
      }
    }
  }
  if (s.ok() && impl->immutable_db_options_.persist_stats_to_disk) {
    // try to read format version but no need to fail Open() even if it fails
    s = impl->PersistentStatsProcessFormatVersion();
  }

  if (s.ok()) {
    for (auto cfd : *impl->versions_->GetColumnFamilySet()) {
      if (cfd->ioptions()->compaction_style == kCompactionStyleFIFO) {
        auto* vstorage = cfd->current()->storage_info();
        for (int i = 1; i < vstorage->num_levels(); ++i) {
          int num_files = vstorage->NumLevelFiles(i);
          if (num_files > 0) {
            s = Status::InvalidArgument(
                "Not all files are at level 0. Cannot "
                "open with FIFO compaction style.");
            break;
          }
        }
      }
      if (!cfd->mem()->IsSnapshotSupported()) {
        impl->is_snapshot_supported_ = false;
      }
      if (cfd->ioptions()->merge_operator != nullptr &&
          !cfd->mem()->IsMergeOperatorSupported()) {
        s = Status::InvalidArgument(
            "The memtable of column family %s does not support merge operator "
            "its options.merge_operator is non-null",
            cfd->GetName().c_str());
      }
      if (!s.ok()) {
        break;
      }
    }
  }
  TEST_SYNC_POINT("DBImpl::Open:Opened");
  Status persist_options_status;
  if (s.ok()) {
    // Persist RocksDB Options before scheduling the compaction.
    // The WriteOptionsFile() will release and lock the mutex internally.
    persist_options_status = impl->WriteOptionsFile(
        false /*need_mutex_lock*/, false /*need_enter_write_thread*/);

    *dbptr = impl;
    impl->opened_successfully_ = true;
    impl->MaybeScheduleFlushOrCompaction();
  }
  impl->mutex_.Unlock();

#ifndef ROCKSDB_LITE
  auto sfm = static_cast<SstFileManagerImpl*>(
      impl->immutable_db_options_.sst_file_manager.get());
  if (s.ok() && sfm) {
    // Notify SstFileManager about all sst files that already exist in
    // db_paths[0] and cf_paths[0] when the DB is opened.

    // SstFileManagerImpl needs to know sizes of the files. For files whose size
    // we already know (sst files that appear in manifest - typically that's the
    // vast majority of all files), we'll pass the size to SstFileManager.
    // For all other files SstFileManager will query the size from filesystem.

    std::vector<LiveFileMetaData> metadata;

    impl->mutex_.Lock();
    impl->versions_->GetLiveFilesMetaData(&metadata);
    impl->mutex_.Unlock();

    std::unordered_map<std::string, uint64_t> known_file_sizes;
    for (const auto& md : metadata) {
      std::string name = md.name;
      if (!name.empty() && name[0] == '/') {
        name = name.substr(1);
      }
      known_file_sizes[name] = md.size;
    }

    std::vector<std::string> paths;
    paths.emplace_back(impl->immutable_db_options_.db_paths[0].path);
    for (auto& cf : column_families) {
      if (!cf.options.cf_paths.empty()) {
        paths.emplace_back(cf.options.cf_paths[0].path);
      }
    }
    // Remove duplicate paths.
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    for (auto& path : paths) {
      std::vector<std::string> existing_files;
      impl->immutable_db_options_.env->GetChildren(path, &existing_files);
      for (auto& file_name : existing_files) {
        uint64_t file_number;
        FileType file_type;
        std::string file_path = path + "/" + file_name;
        if (ParseFileName(file_name, &file_number, &file_type) &&
            file_type == kTableFile) {
          if (known_file_sizes.count(file_name)) {
            // We're assuming that each sst file name exists in at most one of
            // the paths.
            sfm->OnAddFile(file_path, known_file_sizes.at(file_name),
                           /* compaction */ false);
          } else {
            sfm->OnAddFile(file_path);
          }
        }
      }
    }

    // Reserve some disk buffer space. This is a heuristic - when we run out
    // of disk space, this ensures that there is atleast write_buffer_size
    // amount of free space before we resume DB writes. In low disk space
    // conditions, we want to avoid a lot of small L0 files due to frequent
    // WAL write failures and resultant forced flushes
    sfm->ReserveDiskBuffer(max_write_buffer_size,
                           impl->immutable_db_options_.db_paths[0].path);
  }
#endif  // !ROCKSDB_LITE

  if (s.ok()) {
    ROCKS_LOG_HEADER(impl->immutable_db_options_.info_log, "DB pointer %p",
                     impl);
    LogFlush(impl->immutable_db_options_.info_log);
    assert(impl->TEST_WALBufferIsEmpty());
    // If the assert above fails then we need to FlushWAL before returning
    // control back to the user.
    if (!persist_options_status.ok()) {
      s = Status::IOError(
          "DB::Open() failed --- Unable to persist Options file",
          persist_options_status.ToString());
    }
  }
  if (s.ok()) {
    impl->StartTimedTasks();
  }
  if (!s.ok()) {
    for (auto* h : *handles) {
      delete h;
    }
    handles->clear();
    delete impl;
    *dbptr = nullptr;
  }
  return s;
}
}  // namespace rocksdb
