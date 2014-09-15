//
// Copyleft RIME Developers
// License: GPLv3
//
// 2011-11-02 GONG Chen <chen.sst@gmail.com>
//
#include <cstdlib>
#include <vector>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <rime/service.h>
#include <rime/algo/dynamics.h>
#include <rime/dict/text_db.h>
#include <rime/dict/user_db.h>

namespace rime {

UserDbValue::UserDbValue(const std::string& value) {
  Unpack(value);
}

std::string UserDbValue::Pack() const {
  return boost::str(boost::format("c=%1% d=%2% t=%3%") %
                    commits % dee % tick);
}

bool UserDbValue::Unpack(const std::string& value) {
  std::vector<std::string> kv;
  boost::split(kv, value, boost::is_any_of(" "));
  for (const std::string& k_eq_v : kv) {
    size_t eq = k_eq_v.find('=');
    if (eq == std::string::npos)
      continue;
    std::string k(k_eq_v.substr(0, eq));
    std::string v(k_eq_v.substr(eq + 1));
    try {
      if (k == "c") {
        commits = boost::lexical_cast<int>(v);
      }
      else if (k == "d") {
        dee = (std::min)(10000.0, boost::lexical_cast<double>(v));
      }
      else if (k == "t") {
        tick = boost::lexical_cast<TickCount>(v);
      }
    }
    catch (...) {
      LOG(ERROR) << "failed in parsing key-value from userdb entry '"
                 << k_eq_v << "'.";
      return false;
    }
  }
  return true;
}

template class UserDb<TextDb>;

template <>
const std::string UserDb<TextDb>::extension(".userdb.txt");

template <>
const std::string UserDb<TextDb>::snapshot_extension(".userdb.txt");

// key ::= code <space> <Tab> phrase

static bool userdb_entry_parser(const Tsv& row,
                                std::string* key,
                                std::string* value) {
  if (row.size() < 2 ||
      row[0].empty() || row[1].empty()) {
    return false;
  }
  std::string code(row[0]);
  // fix invalid keys created by a buggy version
  if (code[code.length() - 1] != ' ')
    code += ' ';
  *key = code + "\t" + row[1];
  if (row.size() >= 3)
    *value = row[2];
  else
    value->clear();
  return true;
}

static bool userdb_entry_formatter(const std::string& key,
                                   const std::string& value,
                                   Tsv* tsv) {
  Tsv& row(*tsv);
  boost::algorithm::split(row, key,
                          boost::algorithm::is_any_of("\t"));
  if (row.size() != 2 ||
      row[0].empty() || row[1].empty())
    return false;
  row.push_back(value);
  return true;
}

static TextFormat plain_userdb_format = {
  userdb_entry_parser,
  userdb_entry_formatter,
  "Rime user dictionary",
};

template <>
UserDb<TextDb>::UserDb(const std::string& name)
    : TextDb(name + extension, "userdb", plain_userdb_format) {
}

template <class BaseDb>
bool UserDb<BaseDb>::CreateMetadata() {
  Deployer& deployer(Service::instance().deployer());
  return BaseDb::CreateMetadata() &&
      BaseDb::MetaUpdate("/user_id", deployer.user_id);
}

template <class BaseDb>
bool UserDb<BaseDb>::Backup(const std::string& snapshot_file) {
  // plain userdb format
  if (boost::ends_with(snapshot_file, UserDb<TextDb>::snapshot_extension)) {
    LOG(INFO) << "backing up db '" << BaseDb::name() << "' to "
              << snapshot_file;
    TsvWriter writer(snapshot_file, plain_userdb_format.formatter);
    writer.file_description = plain_userdb_format.file_description;
    DbSource source(this);
    try {
      writer << source;
    }
    catch (std::exception& ex) {
      LOG(ERROR) << ex.what();
      return false;
    }
    return true;
  }
  // base db format
  return BaseDb::Backup(snapshot_file);
}

template <class BaseDb>
bool UserDb<BaseDb>::Restore(const std::string& snapshot_file) {
  // plain userdb format
  if (boost::ends_with(snapshot_file, UserDb<TextDb>::snapshot_extension)) {
    LOG(INFO) << "restoring db '" << BaseDb::name() << "' from "
              << snapshot_file;
    TsvReader reader(snapshot_file, plain_userdb_format.parser);
    DbSink sink(this);
    try {
      reader >> sink;
    }
    catch (std::exception& ex) {
      LOG(ERROR) << ex.what();
      return false;
    }
    return true;
  }
  // base db format
  return BaseDb::Restore(snapshot_file);
}

bool UserDbUtil::IsUserDb(Db* db) {
  std::string db_type;
  return db->MetaFetch("/db_type", &db_type) && (db_type == "userdb");
}

std::string UserDbUtil::GetDbName(Db* db) {
  std::string name;
  if (!db->MetaFetch("/db_name", &name))
    return name;
  boost::erase_last(name, extension);
  return name;
}

std::string UserDbUtil::GetUserId(Db* db) {
  std::string user_id("unknown");
  db->MetaFetch("/user_id", &user_id);
  return user_id;
}

std::string UserDbUtil::GetRimeVersion(Db* db) {
  std::string version;
  db->MetaFetch("/rime_version", &version);
  return version;
}

static TickCount get_tick_count(Db* db) {
  std::string tick;
  if (db && db->MetaFetch("/tick", &tick)) {
    try {
      return boost::lexical_cast<TickCount>(tick);
    }
    catch (...) {
    }
  }
  return 1;
}

UserDbMerger::UserDbMerger(Db* db) : db_(db) {
  our_tick_ = get_tick_count(db);
  their_tick_ = 0;
  max_tick_ = our_tick_;
}

UserDbMerger::~UserDbMerger() {
  CloseMerge();
}

bool UserDbMerger::MetaPut(const std::string& key, const std::string& value) {
  if (key == "/tick") {
    try {
      their_tick_ = boost::lexical_cast<TickCount>(value);
      max_tick_ = (std::max)(our_tick_, their_tick_);
    }
    catch (...) {
    }
  }
  return true;
}

bool UserDbMerger::Put(const std::string& key, const std::string& value) {
  if (!db_) return false;
  UserDbValue v(value);
  if (v.tick < their_tick_) {
    v.dee = algo::formula_d(0, (double)their_tick_, v.dee, (double)v.tick);
  }
  UserDbValue o;
  std::string our_value;
  if (db_->Fetch(key, &our_value)) {
    o.Unpack(our_value);
  }
  if (o.tick < our_tick_) {
    o.dee = algo::formula_d(0, (double)our_tick_, o.dee, (double)o.tick);
  }
  if (std::abs(o.commits) < std::abs(v.commits))
      o.commits = v.commits;
  o.dee = (std::max)(o.dee, v.dee);
  o.tick = max_tick_;
  return db_->Update(key, o.Pack()) && ++merged_entries_;
}

void UserDbMerger::CloseMerge() {
  if (!db_ || !merged_entries_)
    return;
  Deployer& deployer(Service::instance().deployer());
  try {
    db_->MetaUpdate("/tick", boost::lexical_cast<std::string>(max_tick_));
    db_->MetaUpdate("/user_id", deployer.user_id);
  }
  catch (...) {
    LOG(ERROR) << "failed to update tick count.";
    return;
  }
  LOG(INFO) << "total " << merged_entries_ << " entries merged, tick = "
            << max_tick_;
  merged_entries_ = 0;
}

UserDbImporter::UserDbImporter(Db* db)
    : db_(db) {
}

bool UserDbImporter::MetaPut(const std::string& key, const std::string& value) {
  return true;
}

bool UserDbImporter::Put(const std::string& key, const std::string& value) {
  if (!db_) return false;
  UserDbValue v(value);
  UserDbValue o;
  std::string old_value;
  if (db_->Fetch(key, &old_value)) {
    o.Unpack(old_value);
  }
  if (v.commits > 0) {
    o.commits = (std::max)(o.commits, v.commits);
    o.dee = (std::max)(o.dee, v.dee);
  }
  else if (v.commits < 0) {  // mark as deleted
    o.commits = (std::min)(v.commits, -std::abs(o.commits));
  }
  return db_->Update(key, o.Pack());
}

}  // namespace rime
