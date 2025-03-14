// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "util.h"
#include <kj/encoding.h>
#include <workerd/io/io-context.h>
#include <workerd/util/thread-scopes.h>
#include <workerd/util/mimetype.h>

namespace workerd::api {

jsg::ByteString toLower(kj::StringPtr str) {
  auto buf = kj::heapArray<char>(str.size() + 1);
  for (auto i: kj::indices(str)) {
    char c = str[i];
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
    buf[i] = c;
  }
  buf.back() = '\0';
  return jsg::ByteString(kj::mv(buf));
}

namespace {

kj::ArrayPtr<const char> split(kj::ArrayPtr<const char>& text, char c) {
  // TODO(cleanup): Modified version of split() found in kj/compat/url.c++.

  for (auto i: kj::indices(text)) {
    if (text[i] == c) {
      kj::ArrayPtr<const char> result = text.slice(0, i);
      text = text.slice(i + 1, text.size());
      return result;
    }
  }
  auto result = text;
  text = {};
  return result;
}

}  // namespace

kj::String toLower(kj::String&& str) {
  for (char& c: str) {
    if ('A' <= c && c <= 'Z') c += 'a' - 'A';
  }
  return kj::mv(str);
}

kj::String toUpper(kj::String&& str) {
  for (char& c: str) {
    if ('a' <= c && c <= 'z') c -= 'a' - 'A';
  }
  return kj::mv(str);
}

void parseQueryString(kj::Vector<kj::Url::QueryParam>& query, kj::ArrayPtr<const char> text,
                      bool skipLeadingQuestionMark) {
  if (skipLeadingQuestionMark && text.size() > 0 && text[0] == '?') {
    text = text.slice(1, text.size());
  }

  while (text.size() > 0) {
    auto value = split(text, '&');
    if (value.size() == 0) continue;
    auto name = split(value, '=');
    query.add(kj::Url::QueryParam { kj::decodeWwwForm(name), kj::decodeWwwForm(value) });
  }
}

kj::Maybe<kj::String> readContentTypeParameter(kj::StringPtr contentType,
                                               kj::StringPtr param) {
  KJ_IF_MAYBE(parsed, MimeType::tryParse(contentType)) {
    return parsed->params().find(toLower(param)).map([](auto& value) {
      return kj::str(value);
    });
  }
  return nullptr;
}

kj::Maybe<kj::Exception> translateKjException(const kj::Exception& exception,
    std::initializer_list<ErrorTranslation> translations) {
  for (auto& t: translations) {
    if (strstr(exception.getDescription().cStr(), t.kjDescription.cStr()) != nullptr) {
      return kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
          kj::str(JSG_EXCEPTION(TypeError) ": ", t.jsDescription));
    }
  }

  return nullptr;
}

namespace {

template <typename Func>
auto translateTeeErrors(Func&& f) -> decltype(kj::fwd<Func>(f)()) {
  try {
    co_return co_await f();
  } catch (...) {
    auto exception = kj::getCaughtExceptionAsKj();
    KJ_IF_MAYBE(e, translateKjException(exception, {
      { "tee buffer size limit exceeded"_kj,
        "ReadableStream.tee() buffer limit exceeded. This error usually occurs when a Request or "
        "Response with a large body is cloned, then only one of the clones is read, forcing "
        "the Workers runtime to buffer the entire body in memory. To fix this issue, remove "
        "unnecessary calls to Request/Response.clone() and ReadableStream.tee(), and always read "
        "clones/tees in parallel."_kj },
    })) {
      kj::throwFatalException(kj::mv(*e));
    }
    kj::throwFatalException(kj::mv(exception));
  }
}

}

kj::Own<kj::AsyncInputStream> newTeeErrorAdapter(kj::Own<kj::AsyncInputStream> inner) {
  class Adapter final: public kj::AsyncInputStream {
  public:
    explicit Adapter(kj::Own<AsyncInputStream> inner): inner(kj::mv(inner)) {}

    kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override {
      return translateTeeErrors([&] {
        return inner->tryRead(buffer, minBytes, maxBytes);
      });
    }

    kj::Maybe<uint64_t> tryGetLength() override { return inner->tryGetLength(); };

    kj::Promise<uint64_t> pumpTo(kj::AsyncOutputStream& output, uint64_t amount) override {
      return translateTeeErrors([&] {
        return inner->pumpTo(output, amount);
      });
    }

    kj::Maybe<kj::Own<kj::AsyncInputStream>> tryTee(uint64_t limit) override {
      return inner->tryTee(limit);
    }

  private:
    kj::Own<AsyncInputStream> inner;
  };

  if (dynamic_cast<Adapter*>(inner.get()) != nullptr) {
    // HACK: Don't double-wrap. This can otherwise happen if we tee a tee.
    return kj::mv(inner);
  } else {
    return kj::heap<Adapter>(kj::mv(inner));
  }
}

kj::String redactUrl(kj::StringPtr url) {
  kj::Vector<char> redacted(url.size() + 1);
  const char* spanStart = url.begin();
  bool sawNonHexChar = false;
  uint digitCount = 0;
  uint upperCount = 0;
  uint lowerCount = 0;
  uint hexDigitCount = 0;

  auto maybeRedactSpan = [&](kj::ArrayPtr<const char> span) {
    bool isHexId = (hexDigitCount >= 32 && !sawNonHexChar);
    bool probablyBase64Id = (span.size() >= 21 &&
                             digitCount >= 2 &&
                             upperCount >= 2 &&
                             lowerCount >= 2);

    if (isHexId || probablyBase64Id) {
      redacted.addAll("REDACTED"_kj);
    } else {
      redacted.addAll(span);
    }
  };

  for (const char& c: url) {
    bool isUpper = ('A' <= c && c <= 'Z');
    bool isLower = ('a' <= c && c <= 'z');
    bool isDigit = ('0' <= c && c <= '9');
    bool isHexDigit = (isDigit || ('A' <= c && c <= 'F') || ('a' <= c && c <= 'f'));
    bool isSep = (c == '+' || c == '-' || c == '_');
    // These extra characters are used in the regular and url-safe versions of
    // base64, but might also be used for GUID-style separators in hex ids.
    // Regular base64 also includes '/', which we don't try to match here due
    // to its prevalence in URLs.  Likewise, we ignore the base64 "=" padding
    // character.

    if (isUpper || isLower || isDigit || isSep) {
      if (isHexDigit) { hexDigitCount++; }
      if (!isHexDigit && !isSep) { sawNonHexChar = true; }
      if (isUpper) { upperCount++; }
      if (isLower) { lowerCount++; }
      if (isDigit) { digitCount++; }
    } else {
      maybeRedactSpan(kj::ArrayPtr<const char>(spanStart, &c));
      redacted.add(c);
      spanStart = &c + 1;
      hexDigitCount = 0;
      digitCount = 0;
      upperCount = 0;
      lowerCount = 0;
      sawNonHexChar = false;
    }
  }
  maybeRedactSpan(kj::ArrayPtr<const char>(spanStart, url.end()));
  redacted.add('\0');

  return kj::String(redacted.releaseAsArray());
}

double dateNow() {
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    return (ioContext.now() - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }

  return 0.0;
}

kj::Maybe<jsg::V8Ref<v8::Object>> cloneRequestCf(
    jsg::Lock& js,
    kj::Maybe<jsg::V8Ref<v8::Object>> maybeCf) {
  KJ_IF_MAYBE(cf, maybeCf) {
    return cf->deepClone(js);
  }
  return nullptr;
}

void maybeWarnIfNotText(kj::StringPtr str) {
  // A common mistake is to call .text() on non-text content, e.g. because you're implementing a
  // search-and-replace across your whole site and you forgot that it'll apply to images too.
  // When running in the fiddle, let's warn the developer if they do this.
  if (!str.startsWith("text/") &&
      !str.endsWith("charset=UTF-8") &&
      !str.endsWith("charset=utf-8") &&
      !str.endsWith("xml") &&
      !str.endsWith("json") &&
      !str.endsWith("javascript")) {
    IoContext::current().logWarning(kj::str(
        "Called .text() on an HTTP body which does not appear to be text. The body's "
        "Content-Type is \"", str, "\". The result will probably be corrupted. Consider "
        "checking the Content-Type header before interpreting entities as text."));
  }
}
}  // namespace workerd::api
