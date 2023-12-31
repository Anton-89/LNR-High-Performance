#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fstream>
#include <functional>
#include <thread>
#include <atomic>
#include <systemd/sd-daemon.h>
#include <systemd/sd-journal.h>
#include <glog/logging.h>
#include <folly/dynamic.h>
#include <folly/json.h>
#include <folly/stop_watch.h>
#include <folly/portability/GFlags.h>
#include <folly/system/ThreadId.h>
#include <folly/String.h>
#include <folly/Format.h>

#include "CallFwd.h"
#include "PhoneMapping.h"
#include "DncMapping.h"
#include "DnoMapping.h"
#include "TollFreeMapping.h"
#include "LergMapping.h"
#include "YoumailMapping.h"
#include "GeoMapping.h"
#include "FtcMapping.h"
#include "F404Mapping.h"
#include "F606Mapping.h"
#include "ACL.h"

using folly::StringPiece;

DEFINE_uint32(status_report_period, 0,
              "How often (in seconds) long operation reports about its status");
static auto reportPeriod = std::chrono::seconds(30);

static std::atomic<PhoneMapping::Data*> mappingUS;
static std::atomic<PhoneMapping::Data*> mappingCA;
static std::atomic<DncMapping::Data*> mappingDNC;
static std::atomic<DnoMapping::Data*> mappingDNO;
static std::atomic<TollFreeMapping::Data*> mappingTollFree;
static std::atomic<ACL::Data*> currentACL;
static std::atomic<LergMapping::Data*> mappingLerg;
static std::atomic<YoumailMapping::Data*> mappingYoumail;
static std::atomic<GeoMapping::Data*> mappingGeo;
static std::atomic<FtcMapping::Data*> mappingFtc;
static std::atomic<F404Mapping::Data*> mapping404;
static std::atomic<F606Mapping::Data*> mapping606;

PhoneMapping PhoneMapping::getUS() noexcept { return { mappingUS }; }
PhoneMapping PhoneMapping::getCA() noexcept { return { mappingCA }; }
DnoMapping DnoMapping::getDNO() noexcept { return { mappingDNO }; }
DncMapping DncMapping::getDNC() noexcept { return { mappingDNC }; }
TollFreeMapping TollFreeMapping::getTollFree() noexcept { return { mappingTollFree }; }
LergMapping LergMapping::getLerg() noexcept { return { mappingLerg }; }
YoumailMapping YoumailMapping::getYoumail() noexcept { return { mappingYoumail }; }
GeoMapping GeoMapping::getGeo() noexcept { return { mappingGeo }; }
FtcMapping FtcMapping::getFtc() noexcept { return { mappingFtc }; }
F404Mapping F404Mapping::getF404() noexcept { return { mapping404 }; }
F606Mapping F606Mapping::getF606() noexcept { return { mapping606 }; }

bool PhoneMapping::isAvailable() noexcept {
  return !!mappingUS.load() && !!mappingCA.load();
}

bool DncMapping::isAvailable() noexcept {
  return !!mappingDNC.load();
}

bool TollFreeMapping::isAvailable() noexcept {
  return !!mappingTollFree.load();
}

bool DnoMapping::isAvailable() noexcept {
  return !!mappingDNO.load();
}

bool LergMapping::isAvailable() noexcept {
  return !!mappingLerg.load();
}

bool YoumailMapping::isAvailable() noexcept {
  return !!mappingYoumail.load();
}

bool GeoMapping::isAvailable() noexcept {
  return !!mappingGeo.load();
}

bool FtcMapping::isAvailable() noexcept {
  return !!mappingFtc.load();
}

bool F404Mapping::isAvailable() noexcept {
  return !!mapping404.load();
}

bool F606Mapping::isAvailable() noexcept {
  return !!mapping606.load();
}

ACL ACL::get() noexcept { return { currentACL }; }

static StringPiece osBasename(StringPiece path) {
  auto idx = path.rfind('/');
  if (idx == StringPiece::npos) {
    return path.str();
  }
  return path.subpiece(idx + 1);
}

static bool loadACLFile(const std::string &path) {
  std::unique_ptr<ACL::Data> data;
  std::ifstream in;
  size_t line = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.open(path);
    data = ACL::fromCSV(in, line);
    in.close();
  } catch (std::exception& e) {
    LOG(ERROR) << osBasename(path) << ':' << line << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Replacing ACL (" << line << " rows)...";
  ACL::commit(std::move(data), currentACL);
  folly::hazptr_cleanup();
  return true;
}

static bool loadMappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();
  const std::string &country = meta.getDefault("country", "US").asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  PhoneMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  if (country == "CA")
    builder.commit(mappingCA);
  else
    builder.commit(mappingUS);
  folly::hazptr_cleanup();
  return true;
}

static bool loadDNCMappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  DncMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mappingDNC);
  folly::hazptr_cleanup();
  return true;
}

static bool loadDnoMappingFile(const std::string &path, folly::dynamic meta, std::string dnotype)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  DnoMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, dnotype, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mappingDNO);
  folly::hazptr_cleanup();
  return true;
}

static bool loadTollFreeMappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  TollFreeMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mappingTollFree);
  folly::hazptr_cleanup();
  return true;
}

static bool loadLergMappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  LergMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mappingLerg);
  folly::hazptr_cleanup();
  return true;
}

static bool loadYoumailMappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  YoumailMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mappingYoumail);
  folly::hazptr_cleanup();
  return true;
}

static bool loadGeoMappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  GeoMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mappingGeo);
  folly::hazptr_cleanup();
  return true;
}

static bool loadFtcMappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  FtcMapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mappingFtc);
  folly::hazptr_cleanup();
  return true;
}

static bool load404MappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  F404Mapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mapping404);
  folly::hazptr_cleanup();
  return true;
}

static bool load606MappingFile(const std::string &path, folly::dynamic meta)
{
  int64_t estimate = meta.getDefault("row_estimate", 0).asInt();
  const std::string &name = meta.getDefault("file_name", path).asString();

  std::ifstream in;
  std::vector<char> rbuf(1ull << 19);
  folly::stop_watch<> watch;

  F606Mapping::Builder builder;
  size_t nrows = 0;

  try {
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.rdbuf()->pubsetbuf(rbuf.data(), rbuf.size());
    in.open(path);

    builder.sizeHint(estimate + estimate / 20);
    builder.setMetadata(meta);

    LOG(INFO) << "Reading database from " << name
      << " (" << estimate << " rows estimated)";

    while (in.good()) {
      builder.fromCSV(in, nrows, 10000);
      if (watch.lap(reportPeriod)) {
        LOG_IF(INFO, estimate != 0) << nrows * 100 / estimate << "% completed";
        LOG_IF(INFO, estimate == 0) << nrows << " rows read";
      }
    }
    in.close();
  } catch (std::runtime_error &e) {
    LOG(ERROR) << osBasename(name) << ':' << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << "Building index (" << nrows << " rows)...";
  builder.commit(mapping606);
  folly::hazptr_cleanup();
  return true;
}

static bool verifyMappingFile(const std::string &path, folly::dynamic meta)
{
  std::ifstream in;
  std::string linebuf;
  std::vector<uint64_t> row;
  folly::stop_watch<> watch;
  size_t maxdiff = 100;
  size_t nrows = 0;

  bool canada = meta.getDefault("country", "US").asString() == "CA";
  PhoneMapping db = canada ? PhoneMapping::getCA() : PhoneMapping::getUS();

  try {
    LOG(INFO) << "Verifying database";
    in.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    in.open(path);
    while (in.good()) {
      for (size_t i = 0; i < 10000; ++i) {
        if (in.peek() == EOF)
          break;

        row.clear();
        std::getline(in, linebuf);
        folly::split(',', linebuf, row);
        ++nrows;

        if (db.getRN(row[0]) == row[1])
          continue;

        LOG(ERROR) << osBasename(path) << ":" << nrows
                    << ": key " << row[0] << " differs";
        if (--maxdiff == 0) {
          LOG(ERROR) << "Diff limit reached, stopping";
          return false;
        }
      }
      if (watch.lap(reportPeriod))
        LOG_IF(INFO, db.size()) << nrows * 100 / db.size() << "% completed";
    }
    in.close();
  } catch (std::runtime_error& e) {
    LOG(ERROR) << osBasename(path) << ":" << nrows << ": " << e.what();
    return false;
  }

  if (nrows != db.size()) {
    LOG(ERROR) << "Loaded DB has " << db.size() - nrows << " extra rows";
    return false;
  }

  LOG(INFO) << "Loaded database matches file";
  return true;
}

static bool dumpMappingFile(const std::string &path, folly::dynamic meta)
{
  std::ofstream out;
  folly::stop_watch<> watch;
  size_t nrows = 0;

  bool canada = meta.getDefault("country", "US").asString() == "CA";
  PhoneMapping db = canada ? PhoneMapping::getCA() : PhoneMapping::getUS();

  try {
    LOG(INFO) << "Dumping database";
    out.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    out.open(path);

    for (db.visitRows(); db.hasRow(); ) {
      for (size_t i = 0; i < 10000 && db.hasRow(); ++i) {
        out << db.currentPN() << "," << db.currentRN() << "\r\n";
        db.advance();
        ++nrows;
      }
      if (watch.lap(reportPeriod))
        LOG_IF(INFO, db.size()) << nrows * 100 / db.size() << "% completed";
    }
    out.flush();
    out.close();
  } catch (std::runtime_error& e) {
    LOG(ERROR) << osBasename(path) << ":" << nrows << ": " << e.what();
    return false;
  }

  LOG(INFO) << nrows << " rows dumped";
  return true;
}

class FdLogSink : public google::LogSink {
public:
  FdLogSink(int fd)
    : fd_(fd)
  {
    AddLogSink(this);
  }

  ~FdLogSink() override {
    RemoveLogSink(this);
  }

  void send(google::LogSeverity severity, const char* full_filename,
            const char* base_filename, int line,
            const struct ::tm* tm_time,
            const char* message, size_t message_len) override
  {
    std::string buf = ToString(severity, base_filename, line,
                               tm_time, message, message_len);
    buf += '\n';
    write(fd_, buf.data(), buf.size());
  }

private:
  int fd_;
};

class JournaldSink : public google::LogSink {
public:
  void send(google::LogSeverity severity, const char* full_filename,
            const char* base_filename, int line,
            const struct ::tm* tm_time,
            const char* message, size_t message_len) override
  {
    // This array maps Google severity levels to syslog levels
    const int SEVERITY_TO_LEVEL[] = { LOG_INFO, LOG_WARNING, LOG_ERR, LOG_EMERG };

    sd_journal_send("MESSAGE=%.*s", int(message_len), message,
                    "PRIORITY=%i", SEVERITY_TO_LEVEL[static_cast<int>(severity)],
                    "TID=%i", int(folly::getOSThreadID()),
                    "CODE_FILE=%s", full_filename,
                    "CODE_LINE=%d", line,
                    NULL);
  }
};

class ControlThread {
 public:
  ControlThread(int sockFd)
    : sock_(sockFd)
    , pktbuf_(1500, 'x')
  {}

  struct sockaddr_t {
    union {
      struct sockaddr_un addr_un;
      struct sockaddr addr;
    };
    size_t addr_len;
  };

  void operator()();
  ssize_t awaitMessage();
  void dispatch(sockaddr_t peer, folly::dynamic msg,
                std::vector<int> argfd) const noexcept;

 private:
  int sock_;
  std::string pktbuf_;
  sockaddr_t peer_;
  alignas(struct cmsghdr) char cbuf_[256];

  folly::dynamic msg_;
  std::vector<int> argfd_;
};

ssize_t ControlThread::awaitMessage()
{
  struct iovec iov;
  struct msghdr io;

  iov.iov_base = pktbuf_.data();
  iov.iov_len = pktbuf_.size();
  io.msg_iov = &iov;
  io.msg_iovlen = 1;
  io.msg_name = &peer_.addr_un;
  io.msg_namelen = sizeof(peer_.addr_un);
  io.msg_flags = 0;
  io.msg_control = cbuf_;
  io.msg_controllen = sizeof(cbuf_);

  ssize_t ret = recvmsg(sock_, &io, MSG_CMSG_CLOEXEC);
  if (ret < 0) {
    PLOG(WARNING) << "recvmsg";
    return ret;
  }

  if (io.msg_namelen <= 0) {
    LOG(WARNING) << "no reply address on a message";
    return -1;
  }

  for (struct cmsghdr *c = CMSG_FIRSTHDR(&io); c != NULL; c = CMSG_NXTHDR(&io, c)) {
    if (c->cmsg_len == 0)
      continue;
    if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
      argfd_.resize((c->cmsg_len - sizeof(struct cmsghdr)) / sizeof(int));
      memcpy(argfd_.data(), CMSG_DATA(c), argfd_.size()*sizeof(int));
      break;
    }
  }

  peer_.addr_len = io.msg_namelen;
  return ret;
}

void ControlThread::dispatch(sockaddr_t peer, folly::dynamic msg,
                             std::vector<int> argfd) const noexcept
try {
  std::unique_ptr<google::LogSink> sink;

  auto mapfd = [&](int index) { return (index >= 0) ? argfd.at(index) : index; };

  const std::string &cmd = msg["cmd"].asString();
  int stdin = mapfd(msg.getDefault("stdin", -1).asInt());
  std::string stdinPath = folly::sformat("/proc/self/fd/{}", stdin);
  int stdout = mapfd(msg.getDefault("stdout", -1).asInt());
  std::string stdoutPath = folly::sformat("/proc/self/fd/{}", stdout);
  int stderr = mapfd(msg.getDefault("stderr", -1).asInt());

  msg.erase("stdin");
  msg.erase("stdout");
  msg.erase("stderr");
  msg.erase("cmd");

  if (stderr >= 0)
    sink.reset(new FdLogSink(stderr));

  char status = 'F';
  if (cmd == "reload") {
    if (loadMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "dnc_reload") {
    if (loadDNCMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "tollfree_reload") {
    if (loadTollFreeMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "dno_npa_reload") {
    if (loadDnoMappingFile(stdinPath, msg, std::string("dno_npa")))
      status = 'S';
  } else if (cmd == "dno_reload") {
    if (loadDnoMappingFile(stdinPath, msg, std::string("dno")))
      status = 'S';
  } else if (cmd == "dno_npa_nxx_reload") {
    if (loadDnoMappingFile(stdinPath, msg, std::string("dno_npa_nxx")))
      status = 'S';
  } else if (cmd == "dno_npa_nxx_x_reload") {
    if (loadDnoMappingFile(stdinPath, msg, std::string("dno_npa_nxx_x")))
      status = 'S';
  } else if (cmd == "lerg_reload") {
    if (loadLergMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "youmail_reload") {
    if (loadYoumailMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "geo_reload") {
    if (loadGeoMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "ftc_reload") {
    if (loadFtcMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "404_reload") {
    if (load404MappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "606_reload") {
    if (load606MappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "verify") {
    if (verifyMappingFile(stdinPath, msg))
      status = 'S';
  } else if (cmd == "dump") {
    if (dumpMappingFile(stdoutPath, msg))
      status = 'S';
  } else if (cmd == "acl") {
    if (loadACLFile(stdinPath))
      status = 'S';
  } else if (cmd == "meta") {
    PhoneMapping::getUS().printMetadata();
    PhoneMapping::getCA().printMetadata();
    DncMapping::getDNC().printMetadata();
    TollFreeMapping::getTollFree().printMetadata();
    LergMapping::getLerg().printMetadata();
    status = 'S';
  } else {
    LOG(WARNING) << "Unrecognized command: " << cmd << "(fds: " << argfd.size() << ")";
  }

  if (sendto(sock_, &status, 1, 0, &peer.addr, peer.addr_len) < 0)
    PLOG(WARNING) << "sendto";

  for (int fd : argfd)
    close(fd);
 } catch (std::exception& e) {
  LOG(ERROR) << "Bad argument: " << e.what();
 }

void ControlThread::operator()() {
  while (true) try {
    ssize_t bytes = awaitMessage();
    if (bytes < 0) {
      sleep(0.1);
      continue;
    }

    StringPiece body{pktbuf_.data(), (size_t)bytes};
    folly::dynamic msg = folly::parseJson(body);
    std::thread([this, peer = peer_, msg = std::move(msg), argfd = argfd_]() {
      dispatch(peer, msg, argfd);
    }).detach();

  } catch (std::exception& e) {
    LOG(ERROR) << "Bad message: " << e.what();
  }
}

void startControlSocket() {
  static JournaldSink journalSink;
  if (sd_listen_fds(0) != 1) {
    LOG(WARNING) << "launched without systemd, control socket disabled";
    return;
  }

  // Replace default log sinks with systemd
  google::AddLogSink(&journalSink);
  for ( int i = 0; i < google::NUM_SEVERITIES; ++i ) {
    google::SetLogDestination(i, "");     // "" turns off logging to a logfile
  }
  google::SetStderrLogging(google::FATAL);

  if (FLAGS_status_report_period)
    reportPeriod = std::chrono::seconds(FLAGS_status_report_period);
  int sockFd = SD_LISTEN_FDS_START + 0;
  std::thread(ControlThread(sockFd)).detach();
}
