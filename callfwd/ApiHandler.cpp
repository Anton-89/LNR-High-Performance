#include <functional>
#include <gflags/gflags.h>
#include <folly/Likely.h>
#include <folly/Range.h>
#include <folly/Conv.h>
#include <folly/Format.h>
#include <folly/small_vector.h>
#include <proxygen/lib/http/HTTPCommonHeaders.h>
#include <proxygen/lib/http/HTTPMethod.h>
#include <proxygen/lib/http/RFC2616.h>
#include <proxygen/httpserver/RequestHandler.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/httpserver/filters/DirectResponseHandler.h>

#include "PhoneMapping.h"
#include "DncMapping.h"
#include "DnoMapping.h"
#include "TollFreeMapping.h"
#include "LergMapping.h"
#include "AccessLog.h"

using namespace proxygen;
using folly::StringPiece;

DEFINE_uint32(max_query_length, 32768,
              "Maximum length of POST x-www-form-urlencoded body");


bool isJsonRequested(StringPiece accept) {
  RFC2616::TokenPairVec acceptTok;
  RFC2616::parseQvalues(accept, acceptTok);
  return acceptTok.size() > 0 && acceptTok[0].first == "application/json";
}

class TargetHandler final : public RequestHandler {
 public:
  void onRequest(std::unique_ptr<HTTPMessage> req) noexcept override {
    using namespace std::placeholders;
    req->getHeaders()
      .forEachWithCode(std::bind(&TargetHandler::sanitizeHeader,
                                 this, _1, _2, _3));

    if (req->getMethod() == HTTPMethod::GET) {
      needBody_ = false;
      onQueryString(req->getQueryStringAsStringPiece());
      onQueryComplete();
      return;
    }

    if (req->getMethod() == HTTPMethod::POST) {
      if (needBody_)
        return;
    }

    ResponseBuilder(downstream_)
      .status(400, "Bad Request")
      .sendWithEOM();
    needBody_ = false;
  }

  void sanitizeHeader(HTTPHeaderCode code, const std::string& name,
                      const std::string& value) noexcept {
    switch (code) {
    case HTTP_HEADER_CONTENT_LENGTH:
      if (folly::to<size_t>(value) > FLAGS_max_query_length)
        needBody_ = false;
      break;
    case HTTP_HEADER_CONTENT_TYPE:
      if (value != "application/x-www-form-urlencoded")
        needBody_ = false;
      break;
    case HTTP_HEADER_ACCEPT:
      json_ = isJsonRequested(value);
      break;
    default:
      break;
    }
  }

  void onQueryComplete() noexcept {
    size_t N = pn_.size();
    std::string record;
    bool dnoAvailable = false;
    bool dncAvailable = false;
    bool tollfreeAvailable = false;
    bool lergAvailable = false;

    if (LIKELY(DncMapping::isAvailable())) {
      dncAvailable = true;
    } else {
      dncAvailable = false;
    }

    if (LIKELY(DnoMapping::isAvailable())) {
      dnoAvailable = true;
    } else {
      dnoAvailable = false;
    }

    if (LIKELY(TollFreeMapping::isAvailable())) {
      tollfreeAvailable = true;
    } else {
      tollfreeAvailable = false;
    }

    if (LIKELY(LergMapping::isAvailable())) {
      lergAvailable = true;
    } else {
      lergAvailable = false;
    }

    us_rn_.resize(N);
    ca_rn_.resize(N);
    if (dncAvailable)
      us_dnc_.resize(N);
    if (dnoAvailable)
      us_dno_.resize(N);
    if (tollfreeAvailable)
      us_tollfree_.resize(N);
    if (lergAvailable)
      us_lerg_.resize(N);

    PhoneMapping::getUS()
      .getRNs(N, pn_.data(), us_rn_.data());
    PhoneMapping::getCA()
      .getRNs(N, pn_.data(), ca_rn_.data());

    if (dncAvailable)
      DncMapping::getDNC()
        .getDNCs(N, pn_.data(), us_dnc_.data());

    if (dnoAvailable)
      DnoMapping::getDNO()
        .getDNOs(N, pn_.data(), us_dno_.data());

    if (tollfreeAvailable)
      TollFreeMapping::getTollFree()
        .getTollFrees(N, pn_.data(), us_tollfree_.data());

    if (lergAvailable) {
      folly::small_vector<uint64_t, 16> lerg_search_key;
      lerg_search_key.resize(N);

      for (size_t i = 0; i < N; ++i) {
        uint64_t rn = us_rn_[i];
        if (rn == PhoneNumber::NONE)
          rn = ca_rn_[i];
        
        if (rn != PhoneNumber::NONE)
          lerg_search_key[i] = rn;
        else
          lerg_search_key[i] = pn_[i];
      }

      LergMapping::getLerg()
        .getLergs(N, lerg_search_key.data(), us_lerg_.data());
    }

    ResponseBuilder(downstream_)
      .status(200, "OK")
      .header(HTTP_HEADER_CONTENT_TYPE,
              json_ ? "application/json" : "text/plain")
      .send();

    if (json_)
      record += "[\n";
    for (size_t i = 0; i < N; ++i) {
      uint64_t rn = us_rn_[i];
      if (rn == PhoneNumber::NONE)
        rn = ca_rn_[i];

      std::string lrn_str;
      std::string dno_str;
      std::string dnc_str;
      std::string tollfree_str;
      std::string lerg_str;
      
      if (json_) {
        if (rn != PhoneNumber::NONE)
          lrn_str = folly::format("\"pn\": \"{}\", \"rn\": \"{}\"", pn_[i], rn).str();
        else
          lrn_str = folly::format("\"pn\": \"{}\", \"rn\": null", pn_[i]).str();

        if (!dncAvailable || us_dnc_[i] == 0)
          dnc_str = std::string("\"is_dnc\": \"no\"");
        else
          dnc_str = std::string("\"is_dnc\": \"yes\"");
          
        if (!dnoAvailable || us_dno_[i] == 0)
          dno_str = std::string("\"is_dno\": \"no\"");
        else
          dno_str = std::string("\"is_dno\": \"yes\"");

        if (!tollfreeAvailable || us_tollfree_[i] == 0)
          tollfree_str = std::string("\"is_tollfree\": \"no\"");
        else
          tollfree_str = std::string("\"is_tollfree\": \"yes\"");

        if (!lergAvailable || us_lerg_[i].lerg_key == 0)
          lerg_str = std::string("\"ocn\":: null, \"operator\": null, \"ocn_type\": null, \"lata\": null, \"rate_center\": null, \"country\": null");
        else {
          char buffer[1024];
          std::printf(buffer, "\"ocn\": \"%s\", \"operator\": \"%s\", \"ocn_type\": \"%s\", \"lata\": \"%s\", \"rate_center\": \"%s\", \"country\": \"%s\"",
              us_lerg_[i].ocn.c_str(), us_lerg_[i].company.c_str(), us_lerg_[i].ocn_type.c_str(), 
              us_lerg_[i].lata.c_str(), us_lerg_[i].rate_center.c_str(), us_lerg_[i].country.c_str());
          lerg_str = std::string(buffer);  
        }
          

      } else {
        if (rn != PhoneNumber::NONE)
          lrn_str = folly::format("pn={},lrn={}", pn_[i], rn).str();
        else
          lrn_str = folly::format("pn={},lrn=null", pn_[i]).str();

        if (!dncAvailable || us_dnc_[i] == 0)
          dnc_str = std::string("is_dnc=no");
        else
          dnc_str = std::string("is_dnc=yes");
        
        if (!dnoAvailable || us_dno_[i] == 0)
          dno_str = std::string("is_dno=no");
        else
          dno_str = std::string("is_dno=yes");

        if (!tollfreeAvailable || us_tollfree_[i] == 0)
          tollfree_str = std::string("is_tollfree=no");
        else
          tollfree_str = std::string("is_tollfree=yes");
        
        if (!lergAvailable || us_lerg_[i].lerg_key == 0)
          lerg_str = std::string("ocn= ,operator= ,ocn_type= ,lata= ,rate_center= ,country= ");
        else {
          char buffer[1024];
          std::sprintf(buffer, "ocn=%s,operator=%s,ocn_type=%s,lata=%s,rate_center=%s,country=%s", 
              us_lerg_[i].ocn.c_str(), us_lerg_[i].company.c_str(), us_lerg_[i].ocn_type.c_str(), 
              us_lerg_[i].lata.c_str(), us_lerg_[i].rate_center.c_str(), us_lerg_[i].country.c_str()
            );
          lerg_str = std::string(buffer);
        }
          
      }

      folly::format(&record, "  {{{},{},{},{},{}}},\n", lrn_str, dno_str, dnc_str, tollfree_str, lerg_str);

      if (record.size() > 1000) {
        downstream_->sendBody(folly::IOBuf::copyBuffer(record));
        record.clear();
      }
    }
    if (json_)
      record += "]\n";

    if (!record.empty())
      downstream_->sendBody(folly::IOBuf::copyBuffer(record));
    downstream_->sendEOM();
  }

  void onQueryString(StringPiece query) {
    using namespace std::placeholders;
    auto paramFn = std::bind(&TargetHandler::onQueryParam, this, _1, _2);
    HTTPMessage::splitNameValuePieces(query, '&', '=', std::move(paramFn));
  }

  void onQueryParam(StringPiece name, StringPiece value) {
    if (name == "phone%5B%5D" || name == "phone[]") {
      uint64_t pn = PhoneNumber::fromString(value);
      if (pn != PhoneNumber::NONE)
        pn_.push_back(pn);
    }
  }

  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override {
    if (!needBody_)
      return;

    if (body_) {
      body_->prependChain(std::move(body));
    } else {
      body_ = std::move(body);
    }

    if (body_->computeChainDataLength() > FLAGS_max_query_length) {
      needBody_ = false;
      body_.release();
      ResponseBuilder(downstream_)
        .status(400, "Bad Request")
        .sendWithEOM();
    }
  }

  void onEOM() noexcept override {
    if (needBody_) {
      onQueryString(body_ ? StringPiece(body_->coalesce()) : "");
      onQueryComplete();
    }
  }

  void onUpgrade(UpgradeProtocol proto) noexcept override {
    // handler doesn't support upgrades
  }

  void requestComplete() noexcept override {
    delete this;
  }

  void onError(ProxygenError err) noexcept override {
    delete this;
  }

 private:
  bool needBody_ = true;
  bool json_ = false;
  std::unique_ptr<folly::IOBuf> body_;
  folly::small_vector<uint64_t, 16> pn_;
  folly::small_vector<uint64_t, 16> us_rn_;
  folly::small_vector<uint64_t, 16> ca_rn_;
  folly::small_vector<uint64_t, 16> us_dnc_;
  folly::small_vector<uint64_t, 16> us_dno_;
  folly::small_vector<uint64_t, 16> us_tollfree_;
  folly::small_vector<LergData, 16> us_lerg_;
};

class ReverseHandler final : public RequestHandler {
 public:
  void onRequest(std::unique_ptr<HTTPMessage> req) noexcept override {
    using namespace std::placeholders;

    if (req->getMethod() != HTTPMethod::GET) {
      ResponseBuilder(downstream_)
        .status(400, "Bad Request")
        .sendWithEOM();
      return;
    }

    HTTPMessage::splitNameValuePieces(req->getQueryStringAsStringPiece(), '&', '=',
                                      std::bind(&ReverseHandler::onQueryParam,
                                                this, _1, _2));

    const std::string &accept = req->getHeaders()
      .getSingleOrEmpty(HTTP_HEADER_ACCEPT);
    bool json = isJsonRequested(accept);

    ResponseBuilder(downstream_)
      .status(200, "OK")
      .header(HTTP_HEADER_CONTENT_TYPE,
              json ? "application/json" : "text/plain")
      .send();

    PhoneMapping us = PhoneMapping::getUS();
    PhoneMapping ca = PhoneMapping::getCA();

    if (json)
      record_ += "[\n";
    for (std::pair<uint64_t, uint64_t> range : query_) {
      us.inverseRNs(range.first, range.second);
      sendBody(us, json);
      ca.inverseRNs(range.first, range.second);
      sendBody(ca, json);
    }
    if (json)
      record_ += "]\n";

    if (!record_.empty())
      downstream_->sendBody(folly::IOBuf::copyBuffer(record_));
    downstream_->sendEOM();
  }

  void sendBody(PhoneMapping &db, bool json) {
    for (; db.hasRow(); db.advance()) {
      if (json) {
        folly::format(&record_, "  {{\"pn\": \"{}\", \"rn\": \"{}\"}},\n",
                      db.currentPN(), db.currentRN());
      } else {
        folly::format(&record_, "{},{}\n", db.currentPN(), db.currentRN());
      }

      if (record_.size() > 1000) {
        downstream_->sendBody(folly::IOBuf::copyBuffer(record_));
        record_.clear();
      }
    }
  }

  void onQueryParam(StringPiece name, StringPiece value) {
    if (name == "prefix%5B%5D" || name == "prefix[]") {
      uint64_t from, to;

      if (value.size() > 10)
        return;

      if (auto asInt = folly::tryTo<uint64_t>(value))
        from = asInt.value();
      else
        return;

      to = from + 1;
      for (size_t i = 0; i < 10 - value.size(); ++i) {
        from *= 10;
        to *= 10;
      }
      query_.emplace_back(from, to);
    }
  }

  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override {
  }

  void onEOM() noexcept override {
  }

  void onUpgrade(UpgradeProtocol proto) noexcept override {
    // handler doesn't support upgrades
  }

  void requestComplete() noexcept override {
    delete this;
  }

  void onError(ProxygenError err) noexcept override {
    delete this;
  }

 private:
  std::vector<std::pair<uint64_t, uint64_t>> query_;
  std::string record_;
};

class ApiHandlerFactory : public RequestHandlerFactory {
 public:
  void onServerStart(folly::EventBase* /*evb*/) noexcept override {
  }

  void onServerStop() noexcept override {
  }

  template<class H, class... Args>
  RequestHandler* makeHandler(Args&&... args)
  {
    if (LIKELY(PhoneMapping::isAvailable())) {
      return new H(std::forward(args)...);
    } else {
      return new DirectResponseHandler(503, "Service Unavailable", "");
    }
  }

  RequestHandler* onRequest(RequestHandler *upstream, HTTPMessage *msg) noexcept override {
    const StringPiece path = msg->getPathAsStringPiece();

    if (path == "/target") {
      return this->makeHandler<TargetHandler>();
    } else if (path == "/reverse") {
      return this->makeHandler<ReverseHandler>();
    } else {
      return new DirectResponseHandler(404, "Not found", "");
    }
  }
};

std::unique_ptr<RequestHandlerFactory> makeApiHandlerFactory()
{
  return std::make_unique<ApiHandlerFactory>();
}
